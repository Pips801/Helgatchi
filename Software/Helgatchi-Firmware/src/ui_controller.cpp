#include "ui_controller.h"
#include "hal.h"
#include "settings_service.h"
#include "power_manager.h"
#include "vibe_service.h"
#include "party_service.h"
#include "version.h"
#include "UI/ui.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"          // eez_flow_pop_screen
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <esp32s3/rom/cache.h>   // Cache_WriteBack_Addr — IDF 4.4 ESP32-S3 ROM cache API

UIController g_ui;

static EventBus* _ui_bus = nullptr;

// ---------------------------------------------------------------------------
// LVGL display driver — LVGL 9.x API, flushed via LovyanGFX
// ---------------------------------------------------------------------------

// Two equal-sized partial-render buffers let LVGL pipeline render-vs-flush
// across strips. 80 rows × 280 px = 3 strips per 240-row screen; at 2 B/px
// (RGB565) each buffer is 44,800 B — together ~90 KB.
//
// Buffers are lv_color16_t, NOT lv_color_t: in LVGL 9 lv_color_t is the
// 3-byte RGB888 struct regardless of LV_COLOR_DEPTH, so sizing RGB565
// buffers with sizeof(lv_color_t) silently over-allocates them by 50%.
//
// Buffers live in INTERNAL SRAM (DMA-capable) by choice: the SW renderer
// read-modify-writes the strip for every blend (text AA, rounded corners,
// borders), so buffer bandwidth dominates render time. The strips were in
// PSRAM once — each strip overflows the data cache, forcing rasterization
// to quad-SPI PSRAM speed; that alone held the devices screen to ~1 FPS.
// More strips can mean more tear seams during scrolls — if seams get bad
// and heap allows, raise DISP_BUF_ROWS (240 must split into whole strips).
//
// Flush strategy: writePixelsDMA + deferred endWrite.
// Each strip's DMA runs in the background while the CPU renders the next
// strip into the OTHER buffer. The bus stays "held" with a pending DMA
// between strips and between frames; endWrite is called at the START of the
// next flush to drain the previous transfer before reusing the bus.
//
// PSRAM cache caveat (fallback path only): the CPU writes pixels through its
// data cache, but GDMA reads from physical PSRAM. Dirty cache lines must be
// written back before each DMA — otherwise DMA pulls stale bytes (the
// green-glitch symptom). Cache_WriteBack_Addr handles this; the address/size
// are rounded out to the 32-byte cache line. Internal-SRAM DMA is coherent,
// so the writeback is skipped there.
//
// FULL mode was tried (one flush per frame, atomic full-screen write); it
// reduced strip seams from 2 → 1 but the remaining seam was still visible
// during scrolls, while costing a brief FPS dip on small isolated updates.
// Not worth the trade — reverted.
static constexpr size_t DISP_BUF_ROWS  = 80;
static constexpr size_t DISP_BUF_PX    = 280 * DISP_BUF_ROWS;
static constexpr size_t DISP_BUF_BYTES = DISP_BUF_PX * sizeof(lv_color16_t);
static lv_color16_t* _disp_buf1 = nullptr;
static lv_color16_t* _disp_buf2 = nullptr;
static bool        _bufs_in_psram = false; // PSRAM bufs need cache writeback pre-DMA
static bool        _dma_pending = false;   // an endWrite is owed at next flush

// Ground-truth flush counter. Incremented once per flush_cb call.
// Counts strips, not frames — in PARTIAL mode that's 1-3 calls/frame.
static uint32_t    _flush_count          = 0;
static uint32_t    _flush_last_sample_ms = 0;

// Render/flush split of the UI phase (see UIController::getRenderSplit).
// _flush_us_total accumulates wall-time spent inside _flush_cb (SPI/DMA drain
// + PSRAM cache writeback); tick() diffs it per frame to separate flush from
// rasterization and keeps the worst frame of each for the window.
static uint32_t    _flush_us_total = 0;
static uint32_t    _render_us_max  = 0;   // worst per-frame render micros (reset on read)
static uint32_t    _flush_us_max   = 0;   // worst per-frame flush micros  (reset on read)

// Rendered-frame counter — incremented on LV_EVENT_REFR_READY, which LVGL
// fires once per completed display-refresh cycle (the same event its perf
// monitor counts for FPS). Cumulative; consumers delta it against elapsed time.
static uint32_t    _refr_count = 0;

static uint32_t _tick_cb() {
    return millis();
}

static void _flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint32_t t_flush0 = micros();
    auto& tft = g_hal.tft();
    const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    const size_t bytes = (size_t)w * h * sizeof(lv_color16_t);

    // Drain the previous DMA (and release its transaction) before grabbing
    // the bus for this strip. No-op on the very first flush.
    if (_dma_pending) {
        tft.endWrite();
        _dma_pending = false;
    }

    // Write back any dirty PSRAM cache lines covering px_map so GDMA reads
    // the pixels the CPU just rendered. 32-byte cache line on ESP32-S3 — we
    // align the address down and the size up so partial-line writes flush.
    // Only needed on the PSRAM fallback path; internal SRAM is DMA-coherent.
    if (_bufs_in_psram) {
        const uintptr_t aligned_addr = (uintptr_t)px_map & ~31U;
        const size_t    aligned_size = (((uintptr_t)px_map + bytes + 31U) & ~31U) - aligned_addr;
        Cache_WriteBack_Addr((uint32_t)aligned_addr, (uint32_t)aligned_size);
    }

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // swap=false: the display renders LV_COLOR_FORMAT_RGB565_SWAPPED — the
    // bytes are already MSB-first as the ST7789 wants — so LovyanGFX DMAs
    // the LVGL buffer as-is. (With CPU-native RGB565 output this needed
    // swap=true, which routed every strip through a CPU swap-copy into
    // LovyanGFX's own DMA buffer before the transfer could start.)
    tft.writePixelsDMA((uint16_t*)px_map, (int32_t)(w * h), false);
    _dma_pending = true;
    _flush_count++;
    // Intentionally NOT endWrite()-ing — let DMA run in background. The next
    // flush_cb call drains it before re-using the bus.
    lv_display_flush_ready(disp);
    _flush_us_total += micros() - t_flush0;
}

// Counts completed display-refresh cycles for the FPS metric (see frameCount).
static void _refr_ready_cb(lv_event_t* /*e*/) {
    _refr_count++;
}

// ---------------------------------------------------------------------------
// Keypad input device
//
// Physical buttons fire EV_BTN_* once per press. LVGL's keypad indev expects
// PRESSED then RELEASED to register a key tap, so we queue keys and emit a
// press/release pair across two indev reads.
// ---------------------------------------------------------------------------

static constexpr uint8_t  KEY_QUEUE_SIZE = 8;
static uint32_t           _key_queue[KEY_QUEUE_SIZE];
static uint8_t            _key_head     = 0;
static uint8_t            _key_tail     = 0;
static bool               _key_pressed  = false;
static uint32_t           _current_key  = 0;
static lv_indev_t*        _indev_kbd    = nullptr;

static void _enqueueKey(uint32_t key) {
    uint8_t next = (_key_tail + 1) % KEY_QUEUE_SIZE;
    if (next == _key_head) return;          // queue full — drop
    _key_queue[_key_tail] = key;
    _key_tail = next;
}

static void _kbd_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    if (_key_pressed) {
        _key_pressed = false;
        data->key   = _current_key;
        data->state = LV_INDEV_STATE_RELEASED;
    } else if (_key_head != _key_tail) {
        _current_key = _key_queue[_key_head];
        _key_head    = (_key_head + 1) % KEY_QUEUE_SIZE;
        _key_pressed = true;
        data->key   = _current_key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key   = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------------------------------------------------------------------------
// Per-screen focus save/restore
//
// EEZ's SCREEN_LOAD_START handler calls lv_group_remove_all_objs then
// re-adds widgets, which resets focus to the first widget and scrolls to it.
// Our handlers are registered after ui_init() so they fire after EEZ's in
// LVGL's dispatch order — we save focus on unload and restore it on load-start
// before the first frame renders.
// ---------------------------------------------------------------------------

static lv_obj_t* _saved_main_menu_focus = nullptr;

static void _on_screen_unload(lv_event_t* /*e*/) {
    _saved_main_menu_focus = lv_group_get_focused(groups.UINavigation);
}

static void _on_screen_load_start(lv_event_t* /*e*/) {
    if (_saved_main_menu_focus) lv_group_focus_obj(_saved_main_menu_focus);
}

// Mark the tutorial complete only when the user reaches the end and presses
// "Start Scanning!". Persist immediately (SET then SAVE) so a sleep/wake or
// power loss mid-tutorial can't dismiss it — only finishing does. PowerManager
// resets SKEY_TUTORIAL_SHOWN to 0 on shipping-mode sleep.
static void _on_end_tutorial_click(lv_event_t* /*e*/) {
    if (!_ui_bus) return;
    EventPayload p{};
    p.settings_set.key   = SKEY_TUTORIAL_SHOWN;
    p.settings_set.value = 1;
    _ui_bus->post(CMD_SETTINGS_SET, p);
    _ui_bus->post(CMD_SETTINGS_SAVE);   // commit now — survives power loss
}

void UIController::begin(EventBus& bus) {
    _bus    = &bus;
    _ui_bus = &bus;

    lv_init();
    lv_tick_set_cb(_tick_cb);

    lv_display_t* disp = lv_display_create(280, 240);
    lv_display_set_flush_cb(disp, _flush_cb);

    // Render in the panel's byte order (RGB565, MSB-first). The SW renderer
    // handles RGB565_SWAPPED natively, and it lets _flush_cb hand the LVGL
    // buffer to DMA untouched — no per-strip swap copy inside LovyanGFX.
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    // Render buffers go to internal DMA-capable SRAM — see the block comment
    // above DISP_BUF_ROWS for why (render bandwidth). If internal heap can't
    // fit both strips, fall back to PSRAM (slow but functional); the flush
    // path then re-enables the cache writeback via _bufs_in_psram.
    _disp_buf1 = (lv_color16_t*)heap_caps_malloc(DISP_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    _disp_buf2 = (lv_color16_t*)heap_caps_malloc(DISP_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!_disp_buf1 || !_disp_buf2) {
        if (_disp_buf1) free(_disp_buf1);
        if (_disp_buf2) free(_disp_buf2);
        _disp_buf1 = (lv_color16_t*)ps_malloc(DISP_BUF_BYTES);
        _disp_buf2 = (lv_color16_t*)ps_malloc(DISP_BUF_BYTES);
        _bufs_in_psram = true;
    }
    lv_display_set_buffers(disp, _disp_buf1, _disp_buf2,
                           DISP_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Count completed display-refresh cycles for a true FPS metric — the same
    // event LVGL's perf monitor counts. Public API, no sysmon private headers.
    lv_display_add_event_cb(disp, _refr_ready_cb, LV_EVENT_REFR_READY, nullptr);

    // EEZ: ui_init() runs eez_flow_init which calls create_screens() and then
    // replacePageHook(1,...) — page 1 is the first entry in screen_names[],
    // which is Main Menu. So Main Menu is the default screen after ui_init().
    ui_init();

    // Replace the EEZ-baked version strings with live values from version.h.
    // BUILD_DATE_STR and UI_VERSION_STR are regenerated each build by
    // scripts/build_info.py.
    lv_label_set_text_fmt(objects.version_info,
                          "%s\n%s\n%s\n%s\n%s",
                          HW_REV_STR, FW_VERSION_STR, UI_VERSION_STR,
                          GAME_VERSION_STR, BUILD_DATE_STR);

    // Mark the tutorial complete only when the user finishes it and presses the
    // end-tutorial button — showing the tutorial no longer clears the flag, so
    // a sleep/wake or power loss mid-tutorial re-shows it on next boot.
    lv_obj_add_event_cb(objects.end_tutorial_button, _on_end_tutorial_click,
                        LV_EVENT_CLICKED, nullptr);

    // Show the tutorial on first flash or after shipping-mode exit.
    // PowerManager resets SKEY_TUTORIAL_SHOWN to 0 before shipping sleep;
    // blank/old NVS also defaults to 0.
    if (!g_settings.getBool(SKEY_TUTORIAL_SHOWN)) {
        lv_scr_load(objects.tutorial_splash_screen);
    }

    lv_obj_add_event_cb(objects.main_menu, _on_screen_unload,     LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(objects.main_menu, _on_screen_load_start, LV_EVENT_SCREEN_LOAD_START,   nullptr);

    // Keypad indev wired to EEZ's single nav group. Each screen's
    // SCREEN_LOAD_START handler in screens.c repopulates groups.UINavigation
    // with that screen's focusable widgets, so the indev follows screen
    // changes without any work on our side.
    _indev_kbd = lv_indev_create();
    lv_indev_set_type(_indev_kbd, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(_indev_kbd, _kbd_read_cb);
    lv_indev_set_group(_indev_kbd, groups.UINavigation);

    bus.subscribe(EV_BTN_LEFT,          this);
    bus.subscribe(EV_BTN_RIGHT,         this);
    bus.subscribe(EV_BTN_CENTER_SHORT,  this);
    bus.subscribe(EV_BTN_CENTER_LONG,   this);
    bus.subscribe(EV_BTN_CENTER_HOLD,   this);
}

void UIController::tick() {
    if (!_render_enabled) return;

    // Time the whole UI phase and diff the flush accumulator so we can split it
    // into rasterization vs SPI flush (see getRenderSplit). Keep the worst
    // frame of each across the window.
    const uint32_t flush0 = _flush_us_total;
    const uint32_t t0     = micros();
    ui_tick();          // EEZ Flow runtime + per-screen tick handlers
    lv_timer_handler();
    const uint32_t ui_us     = micros() - t0;
    const uint32_t flush_us  = _flush_us_total - flush0;
    const uint32_t render_us = (ui_us > flush_us) ? (ui_us - flush_us) : 0;
    if (flush_us  > _flush_us_max)  _flush_us_max  = flush_us;
    if (render_us > _render_us_max) _render_us_max = render_us;
}

void UIController::getDisplayStats(uint32_t& flushes_out, uint32_t& elapsed_ms_out) {
    uint32_t now = millis();
    flushes_out    = _flush_count;
    elapsed_ms_out = now - _flush_last_sample_ms;
    _flush_count          = 0;
    _flush_last_sample_ms = now;
}

void UIController::getRenderSplit(uint32_t& render_max_us, uint32_t& flush_max_us) {
    render_max_us = _render_us_max;
    flush_max_us  = _flush_us_max;
    _render_us_max = 0;
    _flush_us_max  = 0;
}

uint32_t UIController::frameCount() const {
    return _refr_count;
}

void UIController::showUpdatingScreen() {
    // Load the EEZ "device updating" screen, then force render+flush now so the
    // panel actually shows it before the web flasher resets us into the
    // bootloader (tick()/lv_timer_handler won't run again in time). The ST7789
    // retains this framebuffer through the flash until the new firmware boots.
    lv_screen_load(objects.device_updating);
    lv_refr_now(nullptr);
}

// ---------------------------------------------------------------------------
// IEventHandler — button routing
// ---------------------------------------------------------------------------

void UIController::onEvent(const Event& e) {
    lv_group_t* g = _indev_kbd ? lv_indev_get_group(_indev_kbd) : nullptr;

    switch (e.id) {

        case EV_BTN_LEFT:
        case EV_BTN_RIGHT: {
            const bool is_left = (e.id == EV_BTN_LEFT);
            lv_obj_t* focused = g ? lv_group_get_focused(g) : nullptr;

            // A dropdown's open-list state is a property of the widget, not the
            // group — pressing ENTER on a dropdown opens the list but DOESN'T
            // flip lv_group_get_editing(). Check the dropdown directly.
            const bool dropdown_open = focused
                && lv_obj_check_type(focused, &lv_dropdown_class)
                && lv_dropdown_is_open(focused);

            // Haptic only when the press actually moves something. Left/right
            // reflect UI state, not the raw button: at a hard end (nav group
            // doesn't wrap) or at a dropdown option limit, pressing further
            // changes nothing and stays silent.
            bool haptic = false;

            if (dropdown_open) {
                _enqueueKey(is_left ? LV_KEY_UP : LV_KEY_DOWN);
                // Dropdown clamps at both ends (see lv_dropdown.c key handler),
                // so bump only if the highlighted option can still move.
                const uint32_t sel = lv_dropdown_get_selected(focused);
                const uint32_t cnt = lv_dropdown_get_option_cnt(focused);
                haptic = is_left ? (sel > 0) : (sel + 1 < cnt);
            } else if (g && lv_group_get_editing(g)) {
                _enqueueKey(is_left ? LV_KEY_LEFT : LV_KEY_RIGHT);
                // No value-editable widgets (slider/roller/arc) are in the nav
                // group today, so a value edit always confirms. If one is added,
                // gate this on its min/max the same way the dropdown does.
                haptic = true;
            } else {
                _enqueueKey(is_left ? LV_KEY_PREV : LV_KEY_NEXT);
                // Nav mode: bump only if focus can move that way. Group doesn't
                // wrap (disabled in EEZ), so first/last is a hard end. Assumes
                // no hidden members mid-group — none exist; if that changes,
                // this count-based bound would need to skip them.
                if (focused && g) {
                    const uint32_t cnt = lv_group_get_obj_count(g);
                    for (uint32_t i = 0; i < cnt; i++) {
                        if (lv_group_get_obj_by_index(g, i) == focused) {
                            haptic = is_left ? (i > 0) : (i + 1 < cnt);
                            break;
                        }
                    }
                }
            }

            if (haptic) g_vibe.play(HAPTIC_TICK_LIGHT);
            break;
        }

        case EV_BTN_CENTER_SHORT: {
            // ENTER always goes to LVGL; the bump only fires if the focused
            // object can act on it. Scroll-only cards (tutorial/info/debug)
            // have LV_OBJ_FLAG_CLICKABLE removed in EEZ so they stay silent;
            // buttons/switches/dropdowns/menu panels keep it and bump.
            lv_obj_t* focused = g ? lv_group_get_focused(g) : nullptr;
            if (focused && lv_obj_has_flag(focused, LV_OBJ_FLAG_CLICKABLE)) {
                g_vibe.play(HAPTIC_TICK);
            }
            _enqueueKey(LV_KEY_ENTER);
            break;
        }

        case EV_BTN_CENTER_LONG: {
            // Party mode owns the long-press: it exits party and STAYS on the
            // status page (a second long-press then backs out to the main menu).
            // Must come first so the same press doesn't also navigate away.
            if (g_party.active()) {
                g_vibe.play(HAPTIC_BUMP);
                g_party.stop();
                break;
            }

            // A modal popup (e.g. the device-detail msgbox) takes precedence:
            // long-press closes it instead of navigating back a screen. Match
            // only the msgbox backdrop so other top-layer content is untouched.
            lv_obj_t* top = lv_layer_top();
            for (uint32_t i = 0, n = lv_obj_get_child_count(top); i < n; i++) {
                lv_obj_t* c = lv_obj_get_child(top, i);
                if (lv_obj_check_type(c, &lv_msgbox_backdrop_class)) {
                    g_vibe.play(HAPTIC_BUMP);
                    lv_obj_delete(c);   // cascades to the msgbox → its owner restores nav
                    return;
                }
            }
            // Main menu has no "previous" — ignore the regular long-press
            // there. Sleep is reached via the longer EV_BTN_CENTER_HOLD.
            lv_obj_t* active = lv_screen_active();
            if (active == objects.tutorial) {
                // In the tutorial, back-nav returns to the splash instead of out
                // to the main menu — it demos the "back a screen" gesture while
                // keeping the user inside the tutorial flow. Leaving any other way
                // never sets SKEY_TUTORIAL_SHOWN, so the tutorial would re-show
                // after the next sleep/wake; only the End Tutorial button
                // completes it. eez_flow_set_screen (not pop) because begin()
                // loads the splash via raw lv_scr_load, so it isn't on the EEZ
                // page stack — a pop would fall through to the main menu.
                g_vibe.play(HAPTIC_BUMP);
                eez_flow_set_screen(SCREEN_ID_TUTORIAL_SPLASH_SCREEN,
                                    LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            } else if (active == objects.overview) {
                // The status page always backs out to the main menu — not
                // whatever was open when party mode fired. Party navigates here
                // via set_screen (clearing the stack), so there's nothing to pop
                // anyway; go to the menu explicitly.
                g_vibe.play(HAPTIC_BUMP);
                eez_flow_set_screen(SCREEN_ID_MAIN_MENU, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            } else if (active != objects.main_menu
                       && active != objects.tutorial_splash_screen) {
                // The splash traps back-nav (no exit to the main menu, which
                // would end the tutorial early); every other screen pops normally.
                g_vibe.play(HAPTIC_BUMP);
                eez_flow_pop_screen(LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            }
            break;
        }

        case EV_BTN_CENTER_HOLD:
            // Sleep / screen-off only triggers on main menu (matches the
            // shipping-wake hold duration). PowerManager fires its own
            // confirmation haptic synchronously — an async pattern would be
            // cut short by the sleep teardown.
            if (lv_screen_active() == objects.main_menu) {
                g_power.requestSleepOrScreenOff();
            }
            break;

        default:
            break;
    }
}
