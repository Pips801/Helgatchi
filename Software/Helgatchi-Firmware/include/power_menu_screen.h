#pragma once
#include "event_bus.h"

// Power Menu screen wiring — mirrors SettingsScreen. Routes its three buttons
// to PowerManager commands and reflects the live sleep countdown in the
// on-screen text (same EV_SLEEP_COUNTDOWN_UPDATED source the Settings screen
// uses).
class PowerMenuScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Show the Power Action screen ("Sleeping now..." / "Wiping device..." /
    // ...) and post `cmd` after the short hold. The single entry point for
    // every power transition with a UI beat: the menu's own buttons, the
    // serial `power` subcommands, and the settings Reset-device button.
    // Wakes the display first so a serially-triggered action renders (and its
    // countdown timer runs) even when the screen was off. No-op if an action
    // is already counting down; long-press back cancels.
    void beginAction(EventId cmd);
};

extern PowerMenuScreen g_power_menu_screen;
