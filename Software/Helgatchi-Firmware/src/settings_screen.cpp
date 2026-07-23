#include "settings_screen.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_ids.h"
#include "event_payload.h"
#include "UI/screens.h"
#include <lvgl.h>
#include <Arduino.h>

SettingsScreen g_settings_screen;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static EventBus* _bus     = nullptr;
static bool      _inhibit = false;  // blocks VALUE_CHANGED callbacks while populating

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void _postSetting(SettingsKey key, uint32_t value) {
    if (_inhibit || !_bus) return;
    EventPayload p{};
    p.settings_set.key   = key;
    p.settings_set.value = value;
    _bus->post(CMD_SETTINGS_SET, p);   // SET marks dirty; the deferred flush persists it (no explicit SAVE)
}

static void _setSwitch(lv_obj_t* sw, bool on) {
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void _setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Show/hide the debug rows per SKEY_DEBUG_ENABLED. Hiding the row containers
// is enough for keypad nav too: lv_group focus skips objects with a hidden
// ancestor, so the widgets can stay in groups.UINavigation untouched.
static void _applyDebugVisibility() {
    const bool hidden = !g_settings.getBool(SKEY_DEBUG_ENABLED);
    _setHidden(objects.debug_over_serial_container, hidden);
    _setHidden(objects.debug_level_container,       hidden);
    _setHidden(objects.device_info_container,       hidden);
    _setHidden(objects.reset_device_container,      hidden);
    _setHidden(objects.restart_tutorial_container,  hidden);
}

// Perf mode ↔ scan_mode_dropdown index:
//   dropdown 0 = "Power Saver" = PERF_BATTERY_SAVER(2)
//   dropdown 1 = "Balanced"    = PERF_BALANCED(1)
//   dropdown 2 = "Performance" = PERF_PERFORMANCE(0)
//   dropdown 3 = "Always-on"   = PERF_ALWAYS_ON(4)
static constexpr uint8_t kPerfToIdx[PERF_MODE_COUNT] = {2, 1, 0, 1, 3};  // DYNAMIC→Balanced
static constexpr uint8_t kIdxToPerf[]                = {PERF_BATTERY_SAVER, PERF_BALANCED, PERF_PERFORMANCE, PERF_ALWAYS_ON};

// ---------------------------------------------------------------------------
// Populate all widgets from current settings (inhibits feedback callbacks)
// ---------------------------------------------------------------------------

static void _populate() {
    _inhibit = true;

    lv_dropdown_set_selected(objects.screen_brightness_dropdown,
                             g_settings.get(SKEY_SCREEN_BRIGHTNESS));
    lv_dropdown_set_selected(objects.led_brightness_dropdown,
                             g_settings.get(SKEY_LED_BRIGHTNESS));

    uint8_t perf = (uint8_t)g_settings.get(SKEY_PERF_MODE);
    lv_dropdown_set_selected(objects.scan_mode_dropdown,
                             kPerfToIdx[perf < PERF_MODE_COUNT ? perf : PERF_BALANCED]);

    lv_dropdown_set_selected(objects.debug_level_dropdown,
                             g_settings.get(SKEY_DEBUG_LEVEL));

    _setSwitch(objects.vibrate_on_alert_switch,       g_settings.getBool(SKEY_ALERT_VIBRATION));
    _setSwitch(objects.le_ds_on_alert_switch,         g_settings.getBool(SKEY_ALERT_LED));
    _setSwitch(objects.wake_screen_on_alert_switch,   g_settings.getBool(SKEY_ALERT_WAKE_SCREEN));
    _setSwitch(objects.focus_on_alert_page_switch,    g_settings.getBool(SKEY_ALERT_FOCUS));

    uint32_t scan = g_settings.get(SKEY_SCAN_MODE);
    _setSwitch(objects.ble_scanning_switch,   scan & 1u);
    _setSwitch(objects.wi_fi_scanning_switch, scan & 2u);

    _setSwitch(objects.active_ble_scanning,            g_settings.getBool(SKEY_SCAN_ACTIVE));
    _setSwitch(objects.ignore_nameless_random_ma_cs,  g_settings.getBool(SKEY_IGNORE_RANDOMIZED_MACS));

    _setSwitch(objects.debug_over_serial_switch,  g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED));
    _setSwitch(objects.sleep_with_serial_switch,  g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL));
    _setSwitch(objects.sleep_with_usb_switch,     g_settings.getBool(SKEY_SLEEP_WHILE_USB));
    _setSwitch(objects.sleep_while_charging,      g_settings.getBool(SKEY_SLEEP_WHILE_CHARGING));

    _setSwitch(objects.show_debug_options_switch, g_settings.getBool(SKEY_DEBUG_ENABLED));
    _applyDebugVisibility();

    _inhibit = false;
}

// ---------------------------------------------------------------------------
// Widget VALUE_CHANGED callbacks
// ---------------------------------------------------------------------------

static void _on_screen_brightness(lv_event_t* /*e*/) {
    _postSetting(SKEY_SCREEN_BRIGHTNESS,
                 lv_dropdown_get_selected(objects.screen_brightness_dropdown));
}

static void _on_led_brightness(lv_event_t* /*e*/) {
    _postSetting(SKEY_LED_BRIGHTNESS,
                 lv_dropdown_get_selected(objects.led_brightness_dropdown));
}

static void _on_perf_mode(lv_event_t* /*e*/) {
    uint16_t idx = lv_dropdown_get_selected(objects.scan_mode_dropdown);
    _postSetting(SKEY_PERF_MODE, idx < 4 ? kIdxToPerf[idx] : PERF_BALANCED);
}

static void _on_debug_level(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_LEVEL,
                 lv_dropdown_get_selected(objects.debug_level_dropdown));
}

static void _on_vibrate_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_VIBRATION,
                 lv_obj_has_state(objects.vibrate_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_leds_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_LED,
                 lv_obj_has_state(objects.le_ds_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_wake_screen_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_WAKE_SCREEN,
                 lv_obj_has_state(objects.wake_screen_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_focus_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_FOCUS,
                 lv_obj_has_state(objects.focus_on_alert_page_switch, LV_STATE_CHECKED));
}

static void _on_scan_switches(lv_event_t* /*e*/) {
    bool ble  = lv_obj_has_state(objects.ble_scanning_switch,   LV_STATE_CHECKED);
    bool wifi = lv_obj_has_state(objects.wi_fi_scanning_switch, LV_STATE_CHECKED);
    _postSetting(SKEY_SCAN_MODE, (wifi ? 2u : 0u) | (ble ? 1u : 0u));
}

static void _on_active_ble_scanning(lv_event_t* /*e*/) {
    _postSetting(SKEY_SCAN_ACTIVE,
                 lv_obj_has_state(objects.active_ble_scanning, LV_STATE_CHECKED));
}

static void _on_ignore_randomized_macs(lv_event_t* /*e*/) {
    _postSetting(SKEY_IGNORE_RANDOMIZED_MACS,
                 lv_obj_has_state(objects.ignore_nameless_random_ma_cs, LV_STATE_CHECKED));
}

static void _on_debug_over_serial(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_SERIAL_ENABLED,
                 lv_obj_has_state(objects.debug_over_serial_switch, LV_STATE_CHECKED));
}

static void _on_sleep_with_serial(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_SLEEP_WITH_SERIAL,
                 lv_obj_has_state(objects.sleep_with_serial_switch, LV_STATE_CHECKED));
}

static void _on_sleep_with_usb(lv_event_t* /*e*/) {
    _postSetting(SKEY_SLEEP_WHILE_USB,
                 lv_obj_has_state(objects.sleep_with_usb_switch, LV_STATE_CHECKED));
}

static void _on_sleep_while_charging(lv_event_t* /*e*/) {
    _postSetting(SKEY_SLEEP_WHILE_CHARGING,
                 lv_obj_has_state(objects.sleep_while_charging, LV_STATE_CHECKED));
}

static void _on_show_debug_options(lv_event_t* /*e*/) {
    // Visibility re-applies via the EV_SETTINGS_CHANGED echo → _populate().
    // Switching OFF also resets the [DEBUG] keys to defaults — that cascade
    // lives in SettingsService so serial `setting set` behaves identically.
    _postSetting(SKEY_DEBUG_ENABLED,
                 lv_obj_has_state(objects.show_debug_options_switch, LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Button CLICKED callbacks
// ---------------------------------------------------------------------------

static void _on_reset_device_button(lv_event_t* /*e*/) {
    // Full factory wipe (settings/rules/alerts/admin), then reboot — the
    // device comes back up like a first boot. The wipe+shipping-sleep variant
    // (CMD_POWER_SHIPPING_RESET) is deliberately serial-only for assembly.
    if (_bus) _bus->post(CMD_POWER_FACTORY_RESET);
}

// ---------------------------------------------------------------------------
// Screen load — fires after EEZ's handler (registered after ui_init)
// ---------------------------------------------------------------------------

static void _on_settings_load(lv_event_t* /*e*/) {
    _populate();
}

// ---------------------------------------------------------------------------
// Lifecycle — must be called after g_ui.begin() so objects.* are valid
// ---------------------------------------------------------------------------

void SettingsScreen::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_SETTINGS_CHANGED, this);

    lv_obj_add_event_cb(objects.settings, _on_settings_load, LV_EVENT_SCREEN_LOAD_START, nullptr);

    lv_obj_add_event_cb(objects.screen_brightness_dropdown,  _on_screen_brightness,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.led_brightness_dropdown,     _on_led_brightness,      LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.scan_mode_dropdown,          _on_perf_mode,           LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.debug_level_dropdown,        _on_debug_level,         LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_add_event_cb(objects.vibrate_on_alert_switch,     _on_vibrate_on_alert,    LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.le_ds_on_alert_switch,       _on_leds_on_alert,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.wake_screen_on_alert_switch, _on_wake_screen_on_alert,LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.focus_on_alert_page_switch,  _on_focus_on_alert,      LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.ble_scanning_switch,         _on_scan_switches,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.wi_fi_scanning_switch,       _on_scan_switches,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.active_ble_scanning,           _on_active_ble_scanning,     LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.ignore_nameless_random_ma_cs,  _on_ignore_randomized_macs,  LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.debug_over_serial_switch,    _on_debug_over_serial,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.sleep_with_serial_switch,    _on_sleep_with_serial,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.sleep_with_usb_switch,       _on_sleep_with_usb,      LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.sleep_while_charging,        _on_sleep_while_charging,LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.show_debug_options_switch,   _on_show_debug_options,  LV_EVENT_VALUE_CHANGED, nullptr);

    // Sleep / reboot moved to the power menu; of the settings action buttons
    // only reset-device is C-wired (debug screen + restart tutorial navigate
    // via EEZ flow).
    lv_obj_add_event_cb(objects.reset_device_button, _on_reset_device_button, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void SettingsScreen::onEvent(const Event& e) {
    if (e.id == EV_SETTINGS_CHANGED && lv_scr_act() == objects.settings) {
        _populate();
    }
}
