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

// Warn (via a toast) that a rule just enabled keys on a radio that's switched off,
// so it won't detect what it's for — e.g. enabling the Flock detector with WiFi
// scanning disabled. Returns true if a warning was shown, so a caller with its own
// confirmation toast can skip it; false (showing nothing) when every radio the rule
// needs is already running.
//
// Free function rather than a RulesScreen method because the serial console's
// `rule enable` needs it too and there's no screen involved there. It lives with the
// rules UI so the generic ToastService doesn't have to know about the rules engine.
bool toastRuleRadioWarning(const char* rule_name);

// Same, for a tag switch: warns once against the UNION of the radio requirements
// of every rule carrying the tag, rather than per rule — enabling a tag turns a
// dozen rules on at once and one toast per rule would strobe the screen.
bool toastTagRadioWarning(const char* tag);
