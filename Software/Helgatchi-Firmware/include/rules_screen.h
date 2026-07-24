#pragma once
#include "event_bus.h"

// RulesScreen
//
// Owns the UI side of the rules screen:
//   * One Rule card per loaded ruleset, rendered into objects.rules_container
//     via the EEZ Rule user widget (create_user_widget_rule)
//   * Card toggle drives RulesService::setEnabled and reflects external
//     changes (serial console, future tag bulk-toggles) via EV_RULES_CHANGED
//   * "N INDIVIDUAL RULES" header count + empty-state label visibility
//   * Keypad nav group population for the switches on screen load
//
// Layered like SettingsScreen/AlertsScreen: RulesService is the data store
// (LVGL-free); this is the presentation layer for one screen. Must be
// initialized AFTER g_ui (so EEZ objects.* exist) and AFTER g_rules (so the
// ruleset list is loaded for initial rendering).

class RulesScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

private:
    // Set when EV_RULES_CHANGED arrives while another screen is active;
    // the rebuild is deferred to the next rules-screen load.
    bool _dirty = false;
};

extern RulesScreen g_rules_screen;
