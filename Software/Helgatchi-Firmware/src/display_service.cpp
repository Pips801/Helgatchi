#include "display_service.h"
#include "hal.h"
#include "power_manager.h"
#include "settings_service.h"
#include "alerts_service.h"
#include "admin_service.h"   // admin-mode indicator icon
#include "scan_engine.h"     // g_scan_engine.scanInhibited()
#include "scan_types.h"      // ScanDomain (SCAN_BLE / SCAN_WIFI)
#include "UI/vars.h"
#include "UI/screens.h"      // enum Colors (COLOR_ID_*), generated from theme colors
#include "UI/eez-flow.h"
#include <lvgl.h>
#include <stdio.h>
#include <Arduino.h>   // millis() for the prefix-icon debounce

DisplayService g_display;

// Top-bar icon colors. Each glyph is emitted wrapped in LVGL recolor markup
// ("#RRGGBB glyph#"), so every icon is colored independently within one label.
// This requires the Left/Right Text labels to have recolor enabled — set in
// the EEZ project (label property "recolor"), which makes the generator emit
// lv_label_set_recolor(obj, true). Without it the markup renders literally.
//
// Scan / serial / USB colors come from EEZ theme colors so they track the
// selected theme; the rest stay fixed.
static constexpr uint32_t COLOR_IDLE     = 0xFFFFFF;  // inactive icon (white)
static constexpr uint32_t COLOR_CHARGE   = 0xFFB300;  // charging off a dumb charger (amber)
static constexpr uint32_t COLOR_WARN     = 0xF44336;  // battery missing / fault (red)
static constexpr uint32_t COLOR_DISABLED = 0x606060;  // scan suspended (grey)
static constexpr uint32_t COLOR_ADMIN_TX = 0xFFFF00;  // admin actively broadcasting (yellow)

// Party-mode icon tint. When on, every glyph is emitted in s_tint_rgb instead
// of its normal status colour (set via setIconTint(); see header).
static bool     s_tint_on  = false;
static uint32_t s_tint_rgb = 0xFFFFFF;
static inline uint32_t _iconColor(uint32_t normal) { return s_tint_on ? s_tint_rgb : normal; }

// Current theme's color for a generated COLOR_ID_* as 0xRRGGBB (theme_colors
// carry a high alpha byte the recolor markup doesn't use).
static inline uint32_t _themeColor(uint32_t color_id) {
    return eez_flow_get_theme_color(color_id) & 0xFFFFFFu;
}

// Battery fill color: green at full, sweeping through yellow to red as it
// drains. Two-segment RGB lerp (red↔yellow↔green) keeps it a plain hex compute
// with no float/HSV. `level` is 0–100.
static uint32_t _batteryColor(uint8_t level) {
    if (level > 100) level = 100;
    uint8_t r, g;
    if (level < 50) { r = 255;                          g = (uint8_t)(level * 255 / 50); }
    else            { r = (uint8_t)((100 - level) * 255 / 50); g = 255; }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8);
}

// Sets the EEZ global variable that every top bar's right_text expression
// references. EEZ Flow propagates it to all screens automatically. The prefix
// (serial/usb/charge) and the battery glyph are colored independently via
// recolor markup: prefix by connection type, battery by charge level.
static void _refreshBatteryStatus(uint16_t mv, uint8_t pct) {
    if (pct == 0xFF) {
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS, eez::StringValue(""));
        return;
    }

    // Bucket hysteresis picks the battery *glyph shape*. Climbing is immediate;
    // dropping requires HYST_PP below the current bucket's lower edge to avoid
    // flicker at boundaries. (The fill *color* below tracks `level` smoothly and
    // is independent of the bucket.)
    static uint8_t _last_bucket = 0xFF;
    constexpr uint8_t HYST_PP = 5;

    if (pct == BATT_PCT_MISSING) {
        _last_bucket = 0xFF;
        char buf[32];
        snprintf(buf, sizeof(buf), "#%06X %s#", (unsigned)_iconColor(COLOR_WARN),
                 LV_SYMBOL_WARNING LV_SYMBOL_BATTERY_EMPTY);
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS, eez::StringValue(buf));
        return;
    }

    uint8_t level = (pct > 100) ? pmBattPctFromVsenseMv((uint16_t)(mv / 2)) : pct;

    uint8_t cand;
    if      (level >= 80) cand = 4;
    else if (level >= 60) cand = 3;
    else if (level >= 40) cand = 2;
    else if (level >= 20) cand = 1;
    else                  cand = 0;

    uint8_t bucket;
    if (_last_bucket == 0xFF || cand >= _last_bucket) {
        bucket = cand;
    } else {
        uint8_t lower_edge = (uint8_t)(_last_bucket * 20);
        bucket = ((uint16_t)level + HYST_PP < lower_edge) ? cand : _last_bucket;
    }
    _last_bucket = bucket;

    const char* batt_sym;
    switch (bucket) {
        case 4:  batt_sym = LV_SYMBOL_BATTERY_FULL;  break;
        case 3:  batt_sym = LV_SYMBOL_BATTERY_3;     break;
        case 2:  batt_sym = LV_SYMBOL_BATTERY_2;     break;
        case 1:  batt_sym = LV_SYMBOL_BATTERY_1;     break;
        default: batt_sym = LV_SYMBOL_BATTERY_EMPTY; break;
    }

    // Prefix icon (left of the battery glyph). Priority high→low:
    //   3 serial console open · 2 USB host attached · 1 charging · 0 none.
    // On UNPLUG these three signals fall away at slightly different times
    // (DTR drop vs the 100 ms SOF timeout vs the battery-sentinel resample), so
    // a naive recompute cascades keyboard→USB→charge→blank — a visible flash.
    // Debounce it: a HIGHER level shows immediately, but we only step DOWN once
    // the lower level has held for PREFIX_CLEAR_MS. The 1 s tick re-drives this,
    // so the transient collapses into one clean transition to the settled level.
    uint8_t prefix_cand;
    if      ((bool)Serial)                                        prefix_cand = 3;
    else if (g_hal.usbAttached())                                 prefix_cand = 2;
    else if (pct == BATT_PCT_CHARGING || pct == BATT_PCT_CHARGED) prefix_cand = 1;
    else                                                          prefix_cand = 0;

    static constexpr uint32_t PREFIX_CLEAR_MS = 500;
    static uint8_t  shown_level = 0;
    static uint32_t below_since = 0;
    uint32_t nowm = millis();
    if (prefix_cand >= shown_level) {
        shown_level = prefix_cand; // appear / stay at a higher level immediately
        below_since = 0;
    } else {
        if (below_since == 0) below_since = nowm;
        if (nowm - below_since >= PREFIX_CLEAR_MS) { shown_level = prefix_cand; below_since = 0; }
    }

    const char* prefix       = "";
    uint32_t    prefix_color = COLOR_IDLE;
    switch (shown_level) {
        case 3: prefix = LV_SYMBOL_KEYBOARD; prefix_color = _themeColor(COLOR_ID_SERIAL_ICON_COLOR); break;
        case 2: prefix = LV_SYMBOL_USB;      prefix_color = _themeColor(COLOR_ID_USB_ICON_COLOR);    break;
        case 1: prefix = LV_SYMBOL_CHARGE;   prefix_color = COLOR_CHARGE;                            break;
        default: break;
    }

    char buf[64];
    char* p   = buf;
    char* end = buf + sizeof(buf);
    if (*prefix) p += snprintf(p, end - p, "#%06X %s#", (unsigned)_iconColor(prefix_color), prefix);
    snprintf(p, end - p, "#%06X %s#", (unsigned)_iconColor(_batteryColor(level)), batt_sym);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS, eez::StringValue(buf));
}

void DisplayService::refreshStatusIcons() {
    // Left-side status icons, in Settings-screen order: Bluetooth, WiFi, then
    // Bell when any alert is active. Each is wrapped in recolor markup so its
    // color is set independently — BT/WiFi blue while their radio is scanning,
    // white otherwise. ScanMode is a bitmask: bit 0 = BLE, bit 1 = WiFi; an
    // icon only appears when its scan domain is enabled.
    char buf[96] = {0};
    char* p      = buf;
    char* end    = buf + sizeof(buf);

    const uint32_t scan_col = _themeColor(COLOR_ID_SCAN_ICON_COLOR);

    if (_hunting) {
        // Foxhunt override: a GPS glyph then ONLY the hunted radio's icon, both
        // in the active-scan colour to signal the mode. The normal per-domain
        // scan icons are suppressed — lock-on tracks a single target.
        const char* dom = (_hunt_domain == SCAN_WIFI) ? LV_SYMBOL_WIFI : LV_SYMBOL_BLUETOOTH;
        p += snprintf(p, end - p, "#%06X %s#", (unsigned)scan_col, LV_SYMBOL_GPS);
        p += snprintf(p, end - p, "#%06X %s#", (unsigned)scan_col, dom);
    } else {
        // BT/WiFi are greyed out while scanning is suspended (an admin broadcast owns
        // the radio) even though their domain is still enabled; otherwise blue while
        // that radio is actively scanning, white when idle between windows.
        const uint32_t mode      = g_settings.get(SKEY_SCAN_MODE);
        const bool     inhibited = g_scan_engine.scanInhibited();
        if (mode & 1u) {
            const uint32_t c = inhibited ? COLOR_DISABLED
                                         : _iconColor(_ble_scanning ? scan_col : COLOR_IDLE);
            p += snprintf(p, end - p, "#%06X %s#", (unsigned)c, LV_SYMBOL_BLUETOOTH);
        }
        if (mode & 2u) {
            const uint32_t c = inhibited ? COLOR_DISABLED
                                         : _iconColor(_wifi_scanning ? scan_col : COLOR_IDLE);
            p += snprintf(p, end - p, "#%06X %s#", (unsigned)c, LV_SYMBOL_WIFI);
        }
    }

    // Bell appears whenever there's at least one active alert. AlertsScreen
    // calls refreshStatusIcons() on alert raise/clear so this stays current
    // without DisplayService subscribing to alert events itself.
    if (g_alerts.count() > 0)
        p += snprintf(p, end - p, "#%06X %s#", (unsigned)_iconColor(COLOR_IDLE), LV_SYMBOL_BELL);

    // Admin-mode indicator (last, so it sits at the end of the icon list): shown
    // only while admin mode is enabled — white when idle, yellow while actively
    // broadcasting. Literal colours (not _iconColor) so the state stays readable.
    if (g_admin.unlocked())
        p += snprintf(p, end - p, "#%06X %s#",
                      (unsigned)(g_admin.broadcasting() ? COLOR_ADMIN_TX : COLOR_IDLE),
                      LV_SYMBOL_WARNING);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_STATUS_ICONS, eez::StringValue(buf));
}

// Party-mode icon tint control. setIconTint repaints both top-bar globals in the
// given colour; clearIconTint restores normal status colouring. Both repaint
// immediately so the change lands without waiting for the next 1 Hz tick.
void DisplayService::setIconTint(uint32_t rgb) {
    s_tint_on  = true;
    s_tint_rgb = rgb & 0xFFFFFFu;
    refreshStatusIcons();
    _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
}

void DisplayService::clearIconTint() {
    if (!s_tint_on) return;
    s_tint_on = false;
    refreshStatusIcons();
    _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
}

void DisplayService::setHunt(bool on, uint8_t domain) {
    _hunting     = on;
    _hunt_domain = domain;
    refreshStatusIcons();   // repaint now; the 1 Hz tick keeps it consistent thereafter
}

// ---------------------------------------------------------------------------
// Lifecycle — must be called AFTER g_ui.begin() so EEZ Flow assets are loaded.
// ---------------------------------------------------------------------------

void DisplayService::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_BATTERY_UPDATED,      this);
    bus.subscribe(EV_TICK_1S,              this);
    bus.subscribe(EV_SETTINGS_CHANGED,     this);
    bus.subscribe(EV_SCAN_STATE_CHANGED,   this);   // per-domain BT/WiFi scan color
    bus.subscribe(EV_USB_CONNECTED,        this);   // right-side prefix color
    bus.subscribe(EV_USB_DISCONNECTED,     this);
    bus.subscribe(EV_SERIAL_CONNECTED,     this);
    bus.subscribe(EV_SERIAL_DISCONNECTED,  this);

    refreshStatusIcons();
    _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);  // 0xFF pct = blank
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void DisplayService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BATTERY_UPDATED:
            _last_batt_mv  = e.data.battery.mv;
            _last_batt_pct = e.data.battery.pct;
            _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
            break;

        case EV_TICK_1S:
            // Charge state (dumb-charger charging/charged) has no edge event,
            // so re-drive the prefix once a second to catch it. This also
            // re-reads the theme colors, so a live theme switch (owned by EEZ
            // Flow, no bus event) lands on the icons within a second.
            _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
            refreshStatusIcons();
            break;

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_SCAN) {
                refreshStatusIcons();
            }
            break;

        case EV_SCAN_STATE_CHANGED:
            if (e.data.scan_state.domain == SCAN_WIFI)
                _wifi_scanning = (e.data.scan_state.active != 0);
            else
                _ble_scanning  = (e.data.scan_state.active != 0);
            refreshStatusIcons();
            break;

        case EV_USB_CONNECTED:
        case EV_USB_DISCONNECTED:
        case EV_SERIAL_CONNECTED:
        case EV_SERIAL_DISCONNECTED:
            // Connection edge — prefix glyph/color changes immediately instead
            // of waiting up to a second for the next tick.
            _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
            break;

        default:
            break;
    }
}
