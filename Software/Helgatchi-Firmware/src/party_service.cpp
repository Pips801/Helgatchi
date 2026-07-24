#include "party_service.h"
#include "event_ids.h"
#include "led_service.h"
#include "vibe_service.h"
#include "overview_screen.h"
#include "display_service.h"
#include "hal.h"
#include "UI/screens.h"     // objects, SCREEN_ID_OVERVIEW
#include "UI/eez-flow.h"    // eez_flow_set_screen
#include <Arduino.h>        // millis
#include <lvgl.h>

PartyService g_party;

// --- Tunables ----------------------------------------------------------------
// Haptics have no looping primitive (every pattern self-terminates at {0,0}),
// so party mode re-fires a short pulse on an interval for a rhythmic buzz.
static constexpr HapticPatternId PARTY_VIBE      = HAPTIC_DOUBLE_TAP;
static constexpr uint32_t        VIBE_INTERVAL_MS = 700;   // ~1.4 pulses/s
static constexpr uint32_t        TEXT_INTERVAL_MS = 60;    // banner recolour ~16 Hz
static constexpr uint16_t        HUE_STEP         = 10;    // degrees/step (~1 lap/2s)
static constexpr uint32_t        AWAKE_INTERVAL_MS = 1000; // keep-awake heartbeat
static const char* const         BANNER_TEXT      = "Party!";

// The overview screen's top-bar centre title. Positional name from the EEZ
// generator (startWidgetIndex 58 → obj9). Its parent (the top-bar container)
// is where we hang our banner so alignment/lifetime match the real title.
#define OVERVIEW_TITLE objects.obj9__top_bar_center_text

// -----------------------------------------------------------------------------

void PartyService::begin(EventBus& bus) {
    _bus = &bus;
    // No bus subscriptions: the long-press exit is handled in UIController (it
    // checks g_party.active() and calls stop(), staying on the status page)
    // rather than by party swallowing the button here — that avoids a race on
    // the active flag between two handlers in one dispatch. tick() only ends the
    // party if the screen leaves the overview by some other route (safety net).
}

void PartyService::onEvent(const Event&) {}   // unused; kept for IEventHandler

uint32_t PartyService::remainingMs() const {
    if (!_active) return 0;
    uint32_t now = millis();
    return (now < _until_ms) ? (_until_ms - now) : 0;
}

// Create the banner label once, parented to the real title's container so it
// inherits the same position/alignment context. Hidden until party starts.
void PartyService::_ensureBanner() {
    if (_banner) return;
    lv_obj_t* title = OVERVIEW_TITLE;
    if (!title) return;                       // objects.* not built yet
    lv_obj_t* parent = lv_obj_get_parent(title);
    if (!parent) return;

    lv_obj_t* b = lv_label_create(parent);
    lv_obj_set_style_align(b, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(b, BANNER_TEXT);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    _banner = b;
}

void PartyService::_refreshColors() {
    _hue = (uint16_t)((_hue + HUE_STEP) % 360);
    lv_color_t c = lv_color_hsv_to_rgb(_hue, 100, 100);   // LVGL takes S/V as 0..100 %, not 0..255
    if (_banner) lv_obj_set_style_text_color((lv_obj_t*)_banner, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    // Same hue drives the top-bar status icons so title + icons cycle in unison.
    uint32_t rgb = ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | (uint32_t)c.blue;
    g_display.setIconTint(rgb);
}

void PartyService::start(uint32_t duration_ms, bool from_rule) {
    uint32_t now = millis();

    // A persistent party beacon re-fires the rule every scan. After a manual
    // dismiss we ignore those for COOLDOWN_MS so the user gets a real break; an
    // explicit serial start overrides and clears the cooldown.
    if (from_rule) {
        if (_cooldown_until_ms != 0 && now < _cooldown_until_ms) return;
    } else {
        _cooldown_until_ms = 0;
    }

    if (duration_ms == 0) duration_ms = DEFAULT_DURATION_MS;
    _until_ms = now + duration_ms;

    if (_active) return;   // already running — just extended the timer above
    _active  = true;
    _settled = false;
    _last_vibe_ms = _last_text_ms = _last_awake_ms = now;

    // Always bring up the status page. A rule can fire party during a headless
    // scan window with the screen off, so wake the panel too. set_screen clears
    // the nav stack; backing out of the status page (long-press) is handled in
    // UIController and always goes to the main menu. Skip the reload if the
    // overview is already showing.
    g_hal.wakeDisplay();
    if (lv_screen_active() != objects.overview)
        eez_flow_set_screen(SCREEN_ID_OVERVIEW, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);

    // Hold the dance loop against the scan/alert cycle, which keeps firing
    // CMD_SCAN_START/STOP during party and would otherwise knock Helga to
    // sniff/idle mid-party.
    g_overview_screen.hold(true);
    g_overview_screen.play(HELGA_PARTY);

    g_leds.playAlertPattern(LED_PATTERN_RAINBOW_FAST, 0);   // 0 = until cleared

    _ensureBanner();
    if (_banner) lv_obj_clear_flag((lv_obj_t*)_banner, LV_OBJ_FLAG_HIDDEN);   // show our banner
    if (OVERVIEW_TITLE) lv_obj_add_flag(OVERVIEW_TITLE, LV_OBJ_FLAG_HIDDEN);   // hide the bound title
    _refreshColors();   // paint banner + tint icons for frame 0
}

void PartyService::stop(bool arm_cooldown) {
    _end(arm_cooldown);   // manual dismiss arms the cooldown; admin stop passes false
}

// Tear down all party effects and return the status page to normal. Never
// navigates — exiting party leaves the user on the status page (UIController
// handles backing out of it). set_cooldown arms the re-trigger lockout for
// manual dismissals; a natural timeout passes false (the beacon is gone by then).
void PartyService::_end(bool set_cooldown) {
    if (!_active) return;
    _active = false;

    g_leds.playAlertPattern(LED_PATTERN_OFF, 0);
    g_vibe.stop();
    g_display.clearIconTint();
    g_overview_screen.hold(false);
    g_overview_screen.play(HELGA_IDLE);

    if (_banner)        lv_obj_add_flag((lv_obj_t*)_banner, LV_OBJ_FLAG_HIDDEN);
    if (OVERVIEW_TITLE) lv_obj_clear_flag(OVERVIEW_TITLE, LV_OBJ_FLAG_HIDDEN);

    if (set_cooldown) _cooldown_until_ms = millis() + COOLDOWN_MS;
}

void PartyService::tick() {
    if (!_active) return;
    uint32_t now = millis();

    const bool on_overview = (lv_screen_active() == objects.overview);
    if (on_overview) _settled = true;

    // Screen changed away from the overview by some other route — treat as a
    // manual dismiss: end and arm cooldown. (Long-press exits are intercepted in
    // UIController and keep us on the overview, so this is a safety net.)
    if (_settled && !on_overview) { _end(true); return; }

    // Duration elapsed — the beacon is gone (a present one refreshes the timer),
    // so end without cooldown. Stays on the status page.
    if (now >= _until_ms) { _end(false); return; }

    if (now - _last_vibe_ms >= VIBE_INTERVAL_MS) {
        _last_vibe_ms = now;
        g_vibe.play(PARTY_VIBE);
    }
    if (now - _last_text_ms >= TEXT_INTERVAL_MS) {
        _last_text_ms = now;
        _refreshColors();
    }
    // Keep the interactive session alive so PowerManager doesn't sleep mid-party.
    if (now - _last_awake_ms >= AWAKE_INTERVAL_MS) {
        _last_awake_ms = now;
        _bus->post(EV_UI_ACTIVITY);
    }
}
