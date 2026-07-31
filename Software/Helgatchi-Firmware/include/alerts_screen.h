#pragma once
#include "event_bus.h"

// AlertsScreen
//
// Owns the UI side of alerts:
//   * Dynamic alert card list rendered into objects.alert_container
//   * "No active alerts" placeholder label visibility
//   * Dismiss-button fade-out animation mirroring the EEZ "Fade and Hide
//     Alert" user action
//   * Keypad nav group population for dismiss buttons on screen load
//   * Triggers the status-bar bell refresh in DisplayService
//
// Layered like SettingsScreen: AlertsService is the data store (LVGL-free);
// this is the presentation layer for one screen. Must be initialized AFTER
// g_ui (so EEZ objects.* exist) and AFTER g_alerts (so the restored alerts
// list from RTC memory is available for initial rendering).

class AlertsScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Called by uiBringUpNow(). `late` means the UI was built after setup() —
    // something woke the screen mid-window, so alerts already in the store were
    // raised while this screen wasn't subscribed and never got their
    // SKEY_ALERT_FOCUS navigation. Applies it once here. See ui_boot.h.
    void onUiUp(bool late);

private:
    // Latched after SKEY_ALERT_FOCUS pulls (or would have pulled) to the
    // alerts screen for the current batch. Cleared when alerts count drops
    // back to zero so the next fresh batch can pull again.
    bool _focus_consumed = false;
};

extern AlertsScreen g_alerts_screen;
