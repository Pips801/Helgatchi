#include "devices_screen.h"
#include "foxhunting_screen.h"
#include "scan_service.h"
#include "scan_types.h"
#include "vendor_lookup.h"
#include "vibe_service.h"
#include "event_ids.h"
#include "UI/screens.h"
#include "UI/styles.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

DevicesScreen g_devices_screen;

// ---------------------------------------------------------------------------
// Device list — recycler over objects.devices_container.
//
// The old approach (one live card per seen device) died by O(N): every frame
// of a scroll walked every card's objects through the PSRAM-resident LVGL
// pool (~240 ms/frame at 140 devices). Instead, a fixed pool of POOL_SIZE
// cards binds to a sliding window of a data-only row list, and two style-less
// spacer objects keep the flex column's total height — and therefore every
// scroll position, snap point, and scrollbar extent — identical to a full
// one-card-per-device list. LVGL's per-frame cost is constant regardless of
// how many devices are seen, and geometry stays continuous, so scrolling
// never snaps the way a push/pop virtualization did.
//
// EEZ still owns the visuals (Device user widget + "Device Card" style +
// container flex/scroll config); C owns the data flow and the recycling.
//
// Selection is a ROW INDEX, not LVGL focus. The nav group stays empty on this
// screen (EEZ's screen-load handler clears it; we add nothing), so
// UIController's keypad-indev routing no-ops here and the LEFT/RIGHT/CENTER
// events this service already subscribes to drive selection directly. The
// selected row's bound card gets LV_STATE_FOCUS_KEY so the EEZ focus style
// renders exactly as before.
//
// Refresh cadence: rows rebuild on EV_SCAN_COMPLETE only — so RSSI (dBm)
// values move only when a scan lands. A 5 s timer re-renders the "Ns ago"
// age on the bound cards; between scans only that text advances.
// ---------------------------------------------------------------------------

// Snapshot of one listed device. `name` is copied because the seen-map slot
// can be evicted/rewritten between scans; `mfg` points into the flash vendor
// tables, which never move.
struct DeviceRow {
    uint8_t     domain;
    uint8_t     mac[6];
    int8_t      rssi;
    uint32_t    last_seen_ms;
    char        name[32];
    const char* mfg;
};

static DeviceRow _rows[ScanService::SEEN_CAPACITY];
static uint16_t  _row_count = 0;

// Fixed widget pool. 12 covers the ~5 visible rows plus the 2-row lookback
// kept above the selection and the extra row a scroll glide can expose.
struct PoolCard {
    lv_obj_t* card       = nullptr;
    lv_obj_t* name_label = nullptr;   // combined "<domain symbol> name"
    lv_obj_t* mfg_label  = nullptr;
    lv_obj_t* rssi_label = nullptr;   // combined "-43dBm 32s ago"
    lv_obj_t* mac_label  = nullptr;
    int32_t   row        = -1;        // bound row index; -1 = hidden/unbound
};
static constexpr int32_t POOL_SIZE = 12;
static PoolCard  _pool[POOL_SIZE];
static lv_obj_t* _spacer_top = nullptr;
static lv_obj_t* _spacer_bot = nullptr;
static bool      _pool_built = false;
static int32_t   _first      = 0;    // row currently bound to _pool[0]
static int32_t   _pitch      = 0;    // row-to-row Y distance: card height + pad_row
static int32_t   _pad_row    = 0;    // container's flex row gap (negative today)

// Selection: a row index plus a (domain, MAC) pin so the same device stays
// selected across re-sorts. If the pinned device ages out of the list, the
// selection holds its list position instead of jumping to the top.
static int32_t _sel        = 0;
static bool    _has_pin    = false;
static uint8_t _pin_domain = 0;
static uint8_t _pin_mac[6] = {0};

static bool        _dirty       = false;   // seen-map changed; rows need a rebuild
static lv_timer_t* _retry_timer = nullptr; // refresh deferred while a scroll glide runs
static lv_timer_t* _age_timer   = nullptr;

// C-owned top-bar title ("<N> Devices"). The EEZ top bar's center label is
// bound to the literal Title="Devices" and tick_screen_devices re-evaluates it
// every frame, so a set_text on that label would be clobbered next tick. Instead
// we hide the flow-driven label (it keeps writing "Devices" to itself, unseen)
// and render our own label in its place — font/align copied from the EEZ one so
// the visuals still come from the project, only the text is ours.
static lv_obj_t* _title_label = nullptr;

// Devices unseen for this long fall off the list. (BLE MAC randomization
// means new "devices" appear constantly — without this the list only grows.)
static constexpr uint32_t DEVICE_LIST_AGE_MS = 5UL * 60UL * 1000UL;   // 5 min

// "Ns ago" re-render cadence for bound cards. The dBm half of that label is
// frozen row data (updates on scan complete only), so this tick advances just
// the age text.
static constexpr uint32_t AGE_TICK_MS = 5000;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",     (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago", (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

// BT SIG company name for BLE with a mfg id, else the OUI org for the MAC
// (covers BLE-without-mfg and WiFi). Empty when nothing resolves (e.g. a
// random/private BLE address whose OUI isn't in the IEEE table).
static const char* _resolveMfg(const ScanResult& r) {
    const char* org = nullptr;
    if (r.domain == SCAN_BLE && r.mfg_id != 0) org = vendor_mfg_lookup(r.mfg_id);
    if (!org) org = vendor_for_mac(r.mac);
    return org ? org : "";
}

static void _fmtMac(char* buf, size_t sz, const uint8_t mac[6]) {
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void _fmtRssi(char* buf, size_t sz, int8_t rssi, uint32_t last_seen_ms) {
    char ago[24];
    _formatTimeAgo(ago, sizeof(ago), millis() - last_seen_ms);
    snprintf(buf, sz, "%ddBm %s", (int)rssi, ago);
}

// set_text is never free, even with identical text: it reallocs in the LVGL
// pool, re-measures, and invalidates the label. Rebinds hit all five labels
// of a card, so skipping unchanged text keeps window shifts and age ticks
// down to the labels that actually differ.
static void _setLabelIfChanged(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text) == 0) return;
    lv_label_set_text(label, text);
}

// Long-text handling: DOTS everywhere, SCROLL_CIRCULAR only on the selected
// card — the marquee is a continuous animation that invalidates its label
// every tick, fine on one card and a standing tax on twelve. LVGL only
// starts the anim when text actually overflows the label's width (widths
// come from the EEZ widget), so short names cost nothing even selected.
// set_long_mode has no same-value early-out (it kills and restarts the anim
// and re-measures), so guard it — otherwise every rebind would restart the
// selected card's marquee from the beginning.
static void _setLongModeIfChanged(lv_obj_t* label, lv_label_long_mode_t mode) {
    if (lv_label_get_long_mode(label) == mode) return;
    lv_label_set_long_mode(label, mode);
}

// ---------------------------------------------------------------------------
// Device detail popup (lv_msgbox modal on the top layer)
//
// NOTE: this is the one place UI is created in code, by explicit request —
// EEZ Studio can't build this popup. Opened by pressing center on a card;
// closed by press-and-hold center (handled generically in UIController, which
// closes any open msgbox backdrop). While open, the two footer buttons own
// the button events (see onEvent).
// ---------------------------------------------------------------------------

static lv_obj_t* _msgbox     = nullptr;
static int8_t    _mb_focus   = -1;   // -1 = scrolling content; 0/1 = footer button index
static lv_obj_t* _mb_content = nullptr;
static lv_obj_t* _mb_btn[2]  = {nullptr, nullptr};
static constexpr int MB_SCROLL_STEP = 30;   // px scrolled per left/right in content

// Highlight the focused footer button (none while scrolling content). The
// "Focused - Button" style renders LV_STATE_FOCUS_KEY.
static void _mbHighlight() {
    for (int i = 0; i < 2; i++) {
        if (!_mb_btn[i]) continue;
        if (_mb_focus == i) lv_obj_add_state   (_mb_btn[i], LV_STATE_FOCUS_KEY);
        else                lv_obj_remove_state(_mb_btn[i], LV_STATE_FOCUS_KEY);
    }
}

// Right/down: scroll the content until its bottom, then step onto the buttons.
static void _mbNavRight() {
    if (_mb_focus < 0) {
        if (lv_obj_get_scroll_bottom(_mb_content) > 0)
            lv_obj_scroll_by(_mb_content, 0, -MB_SCROLL_STEP, LV_ANIM_ON);
        else { _mb_focus = 0; _mbHighlight(); }
    } else if (_mb_focus == 0) {
        _mb_focus = 1; _mbHighlight();
    }
    // on the last button: stay
}

// Left/up: step back through the buttons, then scroll the content up.
static void _mbNavLeft() {
    if (_mb_focus == 1)      { _mb_focus = 0;  _mbHighlight(); }
    else if (_mb_focus == 0) { _mb_focus = -1; _mbHighlight(); }
    else if (lv_obj_get_scroll_top(_mb_content) > 0) {
        lv_obj_scroll_by(_mb_content, 0, MB_SCROLL_STEP, LV_ANIM_ON);
    }
}

// Center: activate the focused button. Hunt hands the selected device to the
// foxhunt controller (which closes this popup's world by navigating away); Scan
// is not built yet.
static void _mbEnter() {
    if (_mb_focus == 0) {                       // Hunt
        if (_sel < 0 || _sel >= (int32_t)_row_count) return;
        const uint8_t domain = _rows[_sel].domain;
        uint8_t mac[6];
        memcpy(mac, _rows[_sel].mac, 6);
        lv_msgbox_close(_msgbox);               // drop the modal before we leave the screen
        g_foxhunting_screen.startHunt(domain, mac);
    } else if (_mb_focus == 1) {                // Scan
        // TODO: scan option — not implemented yet.
    }
}

static void _msgboxDeleteCb(lv_event_t* /*e*/) {
    // Nothing to restore: the card list doesn't use the nav group, and the
    // pooled cards were untouched under the modal.
    _msgbox     = nullptr;
    _mb_content = nullptr;
    _mb_btn[0]  = _mb_btn[1] = nullptr;
    _mb_focus   = -1;
}

static void _appendUuid(char* buf, size_t sz, const uint8_t uuid[16]) {
    size_t p = strlen(buf);
    for (int b = 0; b < 16 && p + 2 < sz; b++) p += (size_t)snprintf(buf + p, sz - p, "%02X", uuid[b]);
}

// BLE base UUID (0000xxxx-0000-1000-8000-00805F9B34FB) in the byte order the
// scan engine stores (NimBLE value order, little-endian), with the 16-bit slot
// (bytes 12-13) zeroed.
static const uint8_t BLE_BASE_UUID_LE[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// If `uuid` is a 16-bit assigned UUID promoted onto the BLE base, return its
// short value (0..0xFFFF); otherwise -1 (a 32/128-bit UUID with no short form).
static int _uuid16(const uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) {
        if (i == 12 || i == 13) continue;   // the 16-bit value slot
        if (uuid[i] != BLE_BASE_UUID_LE[i]) return -1;
    }
    return uuid[12] | (uuid[13] << 8);
}

static void _openMsgbox(uint8_t domain, const uint8_t mac[6]) {
    if (_msgbox) return;
    const ScanResult* r = g_scan_service.findSeen(domain, mac);
    if (!r) return;

    lv_obj_t* mb = lv_msgbox_create(nullptr);   // NULL parent → modal on top layer
    lv_obj_set_size(mb, LV_PCT(85), LV_PCT(85));
    lv_obj_set_style_radius(mb, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    char macbuf[24];
    snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    lv_msgbox_add_title(mb, r->name[0] ? r->name : macbuf);

    // Content labels (compact font so the summary fits the 85% box).
    lv_obj_t* content = lv_msgbox_get_content(mb);
    lv_obj_set_style_text_font(content, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

    const uint32_t now = millis();
    char first_ago[24], last_ago[24];
    _formatTimeAgo(first_ago, sizeof(first_ago), now - r->first_seen_ms);
    _formatTimeAgo(last_ago,  sizeof(last_ago),  now - r->timestamp_ms);
    const char* bt  = (r->mfg_id != 0) ? vendor_mfg_lookup(r->mfg_id) : nullptr;
    const char* oui = vendor_for_mac(r->mac);

    char line[128];
    snprintf(line, sizeof(line), "Name: %s",        r->name[0] ? r->name : "(none)");  lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "MAC: %s",         macbuf);                            lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Address type: %s", macTypeName(r->mac_type));         lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "First seen: %s",  first_ago);                         lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Last seen: %s",   last_ago);                          lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "RSSI: %d dBm",    (int)r->rssi);                      lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "MFG: %s",         bt  ? bt  : "-");                   lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "OUI MFG: %s",     oui ? oui : "-");                   lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Services: %u",    (unsigned)r->service_count);        lv_msgbox_add_text(mb, line);
    for (uint8_t s = 0; s < r->service_count; s++) {
        const int u16 = _uuid16(r->service_uuids[s]);
        if (u16 >= 0) {
            snprintf(line, sizeof(line), "  0x%04X", (unsigned)u16);
        } else {
            // 32/128-bit UUID with no short form — show it in full.
            line[0] = ' '; line[1] = ' '; line[2] = '\0';
            _appendUuid(line, sizeof(line), r->service_uuids[s]);
        }
        lv_msgbox_add_text(mb, line);
    }

    // Footer actions: Hunt → foxhunt lock-on this device; Scan → TBD.
    lv_obj_t* b_hunt = lv_msgbox_add_footer_button(mb, "Hunt");
    lv_obj_t* b_scan = lv_msgbox_add_footer_button(mb, "Scan");
    add_style_focused___button(b_hunt);
    add_style_focused___button(b_scan);

    // Custom keypad nav (see _mbNav*): left/right scrolls the content to the
    // bottom, then steps through the buttons. The nav group is already empty
    // on this screen; clear defensively in case the popup outlives a screen
    // change so UIController's key routing stays a no-op while it's up.
    lv_group_remove_all_objs(groups.UINavigation);
    _mb_content = content;
    _mb_btn[0]  = b_hunt;
    _mb_btn[1]  = b_scan;
    _mb_focus   = -1;                       // start in content-scroll mode
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    _mbHighlight();

    lv_obj_add_event_cb(mb, _msgboxDeleteCb, LV_EVENT_DELETE, nullptr);
    _msgbox = mb;
}

// ---------------------------------------------------------------------------
// Pool construction
// ---------------------------------------------------------------------------

// Pure geometry: no styles (so no theme bg/border), no interaction. Height is
// driven by _applyWindow to stand in for the unbound rows above/below the
// window.
static lv_obj_t* _makeSpacer(lv_obj_t* parent) {
    lv_obj_t* sp = lv_obj_create(parent);
    lv_obj_remove_style_all(sp);
    lv_obj_remove_flag(sp, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(sp, 1, 1);
    return sp;
}

// Instantiate the EEZ-designed Device user widget so its layout, fonts, and
// styles come straight from create_user_widget_device() in src/UI/screens.c —
// the single place to edit them. The generated builder stores its child
// pointers into objects[startWidgetIndex + 0..4]; the widget isn't placed on
// a screen (so it has no reserved slots) and is never ticked by EEZ, so we
// lend it slots [0..4] for the call, read the created objects back, then
// restore the originals. Index/field mapping matches the assignments in
// create_user_widget_device(): +0 panel, +1 name, +2 mfg, +3 rssi, +4 mac.
static void _buildPoolCard(lv_obj_t* parent, PoolCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[5];
    for (int i = 0; i < 5; i++) saved[i] = slots[i];

    create_user_widget_device(parent, nullptr, 0);

    out->card       = slots[0];
    out->name_label = slots[1];
    out->mfg_label  = slots[2];
    out->rssi_label = slots[3];
    out->mac_label  = slots[4];

    for (int i = 0; i < 5; i++) slots[i] = saved[i];

    lv_obj_add_flag(out->card, LV_OBJ_FLAG_HIDDEN);   // unbound until a row claims it
    out->row = -1;
}

// Child order after this: [top spacer, card 0..POOL_SIZE-1, bottom spacer] —
// the flex column relies on creation order, nothing is ever reordered.
static void _buildPool() {
    lv_obj_t* parent = objects.devices_container;
    if (!parent || _pool_built) return;

    _spacer_top = _makeSpacer(parent);
    for (int32_t i = 0; i < POOL_SIZE; i++) _buildPoolCard(parent, &_pool[i]);
    _spacer_bot = _makeSpacer(parent);

    // Row pitch from the EEZ widget + container styles, so an EEZ-side card
    // height or gap change can't silently desync the spacer math.
    _pad_row = lv_obj_get_style_pad_row(parent, LV_PART_MAIN);
    _pitch   = lv_obj_get_style_height(_pool[0].card, LV_PART_MAIN) + _pad_row;

    _pool_built = true;
}

// ---------------------------------------------------------------------------
// Window binding
// ---------------------------------------------------------------------------

static void _bindCard(PoolCard* p, int32_t row_idx) {
    const DeviceRow& row = _rows[row_idx];

    lv_obj_remove_flag(p->card, LV_OBJ_FLAG_HIDDEN);

    // Domain icon + name share one label: the LV_SYMBOL_* glyphs are baked
    // into montserrat_16 like any other character, and both halves rendered
    // at 16 px anyway — one fewer object and draw task per card. A nameless
    // device shows just the glyph.
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%s%s",
             row.domain == SCAN_BLE ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI,
             row.name[0] ? " " : "", row.name);
    _setLabelIfChanged(p->name_label, buf);
    _setLabelIfChanged(p->mfg_label,  row.mfg);

    _fmtRssi(buf, sizeof(buf), row.rssi, row.last_seen_ms);
    _setLabelIfChanged(p->rssi_label, buf);
    _fmtMac(buf, sizeof(buf), row.mac);
    _setLabelIfChanged(p->mac_label, buf);

    p->row = row_idx;

    // Selection visual — same EEZ FOCUS_KEY style the group focus used to
    // drive — plus the marquee swap: only the selected card's overflowing
    // name/mfg scroll; everyone else truncates with dots.
    const bool selected = (row_idx == _sel);
    if (selected) lv_obj_add_state   (p->card, LV_STATE_FOCUS_KEY);
    else          lv_obj_remove_state(p->card, LV_STATE_FOCUS_KEY);
    const lv_label_long_mode_t lm = selected ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                             : LV_LABEL_LONG_MODE_DOTS;
    _setLongModeIfChanged(p->name_label, lm);
    _setLongModeIfChanged(p->mfg_label,  lm);
}

static void _unbindCard(PoolCard* p) {
    lv_obj_add_flag(p->card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(p->card, LV_STATE_FOCUS_KEY);
    _setLongModeIfChanged(p->name_label, LV_LABEL_LONG_MODE_DOTS);   // kill any marquee
    _setLongModeIfChanged(p->mfg_label,  LV_LABEL_LONG_MODE_DOTS);
    p->row = -1;
}

// Bind the pool to the window around the selection and size the spacers so
// the flex column's geometry matches a full one-card-per-device list exactly.
static void _applyWindow() {
    if (!_pool_built) return;

    // Keep 2 rows of lookback above the selection so a scroll glide never
    // exposes an unbound row; clamp to the list ends.
    int32_t first     = _sel - 2;
    int32_t max_first = (int32_t)_row_count - POOL_SIZE;
    if (max_first < 0)     max_first = 0;
    if (first < 0)         first = 0;
    if (first > max_first) first = max_first;
    _first = first;

    int32_t bound = (int32_t)_row_count - first;
    if (bound > POOL_SIZE) bound = POOL_SIZE;
    for (int32_t i = 0; i < POOL_SIZE; i++) {
        if (i < bound) _bindCard(&_pool[i], first + i);
        else           _unbindCard(&_pool[i]);
    }

    // rows_outside × pitch, minus the one pad_row gap the spacer itself adds
    // to the flex column — with that correction every row's Y (and the total
    // content height) is pixel-identical to the full list for any row count.
    const int32_t below = (int32_t)_row_count - first - bound;
    lv_obj_set_height(_spacer_top, first * _pitch - _pad_row);
    lv_obj_set_height(_spacer_bot, below * _pitch - _pad_row);
}

static lv_obj_t* _cardForRow(int32_t row) {
    if (row < _first || row >= _first + POOL_SIZE) return nullptr;
    PoolCard& p = _pool[row - _first];
    return (p.row == row) ? p.card : nullptr;
}

// Scroll so the selected card sits where SCROLL_ON_FOCUS used to put it: the
// container's SNAP_START top-aligns the target of scroll_to_view, preserving
// the old "selected card rides the top" feel. Glide pacing comes from LVGL's
// global scroll-anim clamp (SCROLL_ANIM_TIME_MAX build flag — a per-page
// custom lv_anim was tried and reverted; it saved nothing and felt worse).
static void _scrollToSel(lv_anim_enable_t anim) {
    lv_obj_t* card = _cardForRow(_sel);
    if (!card) return;
    lv_obj_update_layout(objects.devices_container);   // spacer heights just changed
    lv_obj_scroll_to_view(card, anim);
}

// ---------------------------------------------------------------------------
// Rows rebuild + refresh driver
// ---------------------------------------------------------------------------

// Sort the first `n` entries of `order` (seen-map indices) RSSI-strongest-first
// (insertion sort). Caller fills `order` with the indices to sort.
static void _sortByRssi(uint16_t* order, uint16_t n) {
    for (uint16_t i = 1; i < n; i++) {
        const uint16_t key = order[i];
        const int8_t   kr  = g_scan_service.seenAt(key).rssi;
        int j = (int)i - 1;
        while (j >= 0 && g_scan_service.seenAt(order[j]).rssi < kr) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

// Snapshot the seen-map into _rows (age-filtered, strongest-first) and
// re-locate the selection. Data only — no LVGL work here.
static void _rebuildRows() {
    const uint16_t n   = (uint16_t)g_scan_service.seenCount();
    const uint32_t now = millis();

    static uint16_t order[ScanService::SEEN_CAPACITY];
    uint16_t m = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (now - g_scan_service.seenAt(i).timestamp_ms <= DEVICE_LIST_AGE_MS) order[m++] = i;
    }
    _sortByRssi(order, m);

    for (uint16_t k = 0; k < m; k++) {
        const ScanResult& r   = g_scan_service.seenAt(order[k]);
        DeviceRow&        row = _rows[k];
        row.domain       = r.domain;
        memcpy(row.mac, r.mac, sizeof(row.mac));
        row.rssi         = r.rssi;
        row.last_seen_ms = r.timestamp_ms;
        memcpy(row.name, r.name, sizeof(row.name));
        row.name[sizeof(row.name) - 1] = '\0';
        row.mfg          = _resolveMfg(r);
    }
    _row_count = m;

    // Follow the pinned device through the re-sort; if it aged out, hold the
    // same list position (clamped) rather than jumping to the top.
    int32_t sel = -1;
    if (_has_pin) {
        for (uint16_t k = 0; k < m; k++) {
            if (_rows[k].domain == _pin_domain && memcmp(_rows[k].mac, _pin_mac, 6) == 0) {
                sel = (int32_t)k;
                break;
            }
        }
    }
    if (sel < 0) {
        sel = _sel;
        if (sel >= (int32_t)m) sel = (int32_t)m - 1;
        if (sel < 0) sel = 0;
        if (m > 0) {
            _pin_domain = _rows[sel].domain;
            memcpy(_pin_mac, _rows[sel].mac, 6);
            _has_pin = true;
        }
    }
    _sel = sel;
}

// Take over the top bar's center title. Structure (creation order, stable):
// devices screen → child 0 = top-bar wrapper → child 0 = Top Bar container →
// children [Left, Center, Right]; index 1 is the flow-driven center label.
static void _setupTitle() {
    if (_title_label || !objects.devices) return;
    lv_obj_t* wrapper = lv_obj_get_child(objects.devices, 0);
    lv_obj_t* topbar  = wrapper ? lv_obj_get_child(wrapper, 0) : nullptr;
    lv_obj_t* center  = topbar  ? lv_obj_get_child(topbar, 1)  : nullptr;
    if (!center) return;

    lv_obj_add_flag(center, LV_OBJ_FLAG_HIDDEN);   // flow still writes "Devices" here, unseen

    _title_label = lv_label_create(topbar);
    lv_obj_set_style_align(_title_label, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(_title_label,
                               lv_obj_get_style_text_font(center, LV_PART_MAIN),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(_title_label, "Devices");
}

static void _updateTitle() {
    if (!_title_label) return;
    char buf[24];
    if (_row_count == 1) snprintf(buf, sizeof(buf), "1 Device");
    else                 snprintf(buf, sizeof(buf), "%u Devices", (unsigned)_row_count);
    _setLabelIfChanged(_title_label, buf);
}

// One full refresh: rows + window + viewport. Returns false when deferred
// because a scroll glide is running — a rebind mid-animation would swap the
// labels under the moving viewport, and the ANIM_OFF reposition would cancel
// the glide.
static bool _tryRefresh() {
    if (!objects.devices_container || !_pool_built) return true;   // nothing to refresh into
    if (lv_anim_get(objects.devices_container, nullptr)) return false;

    _rebuildRows();
    _applyWindow();
    _updateTitle();              // "<N> Devices" — count follows the age-filtered list
    _scrollToSel(LV_ANIM_OFF);   // hold the selected device in place through reorders
    _dirty = false;
    return true;
}

static void _retryTimerCb(lv_timer_t* t) {
    if (lv_screen_active() != objects.devices || _tryRefresh()) {
        lv_timer_delete(t);
        _retry_timer = nullptr;
    }
}

static void _kickRefresh() {
    if (_tryRefresh()) return;
    if (!_retry_timer) _retry_timer = lv_timer_create(_retryTimerCb, 50, nullptr);
}

// ---------------------------------------------------------------------------
// Selection movement (button-driven)
// ---------------------------------------------------------------------------

static void _moveSel(int32_t dir) {
    const int32_t next = _sel + dir;
    if (next < 0 || next >= (int32_t)_row_count) return;   // hard end — silent, like group nav
    _sel = next;
    _pin_domain = _rows[_sel].domain;
    memcpy(_pin_mac, _rows[_sel].mac, 6);
    _has_pin = true;

    _applyWindow();              // window follows the selection; sel is always bound after this
    _scrollToSel(LV_ANIM_ON);
    g_vibe.play(HAPTIC_TICK_LIGHT);   // group nav's per-step tick, now fired here
}

// ---------------------------------------------------------------------------
// Timers + lifecycle
// ---------------------------------------------------------------------------

static void _ageTimerCb(lv_timer_t* /*t*/) {
    if (lv_screen_active() != objects.devices) return;
    char buf[40];
    for (int32_t i = 0; i < POOL_SIZE; i++) {
        PoolCard& p = _pool[i];
        if (p.row < 0) continue;
        _fmtRssi(buf, sizeof(buf), _rows[p.row].rssi, _rows[p.row].last_seen_ms);
        _setLabelIfChanged(p.rssi_label, buf);
    }
}

uint16_t DevicesScreen::cardCount() const { return _row_count; }

void DevicesScreen::begin(EventBus& bus) {
    bus.subscribe(EV_SCAN_COMPLETE, this);
    // Buttons drive selection directly (and the detail popup's nav while it's
    // open) — the nav group stays empty on this screen, so UIController's
    // keypad routing doesn't compete.
    bus.subscribe(EV_BTN_LEFT,         this);
    bus.subscribe(EV_BTN_RIGHT,        this);
    bus.subscribe(EV_BTN_CENTER_SHORT, this);

    _buildPool();
    _setupTitle();
    _age_timer = lv_timer_create(_ageTimerCb, AGE_TICK_MS, nullptr);

    // Don't populate at boot — mark dirty so the first screen load snapshots
    // whatever the seen map holds by then.
    _dirty = true;

    if (objects.devices) {
        lv_obj_add_event_cb(objects.devices, [](lv_event_t* /*e*/) {
            if (!_pool_built) _buildPool();
            _setupTitle();                             // idempotent; ensures title even if begin() ran early
            if (_dirty) _kickRefresh();                // stale — rebuild for this view
            else        _scrollToSel(LV_ANIM_OFF);     // re-assert viewport on re-entry
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
}

void DevicesScreen::onEvent(const Event& e) {
    switch (e.id) {
        case EV_SCAN_COMPLETE:
            // Mark stale; refresh only if the list is actually showing.
            // Off-screen we stay dirty and the next screen load rebuilds.
            _dirty = true;
            if (lv_screen_active() == objects.devices) _kickRefresh();
            break;

        case EV_BTN_LEFT:
            if      (_msgbox)                               _mbNavLeft();
            else if (lv_screen_active() == objects.devices) _moveSel(-1);
            break;

        case EV_BTN_RIGHT:
            if      (_msgbox)                               _mbNavRight();
            else if (lv_screen_active() == objects.devices) _moveSel(+1);
            break;

        case EV_BTN_CENTER_SHORT:
            if      (_msgbox)                               _mbEnter();
            else if (lv_screen_active() == objects.devices && _row_count > 0) {
                g_vibe.play(HAPTIC_TICK);   // the "clickable ENTER" bump group nav gave
                _openMsgbox(_rows[_sel].domain, _rows[_sel].mac);
            }
            break;

        default:
            break;
    }
}
