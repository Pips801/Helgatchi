#include "ui_boot.h"
#include "log_service.h"
#include "display_service.h"
#include "settings_screen.h"
#include "alerts_screen.h"
#include "devices_screen.h"
#include "foxhunting_screen.h"
#include "debug_screen.h"
#include "rules_screen.h"
#include "overview_screen.h"
#include "power_menu_screen.h"
#include "party_service.h"
#include "admin_service.h"
#include "ui_controller.h"

static bool _up      = false;
static bool _pending = false;
// setup() calls uiBringUpNow() exactly once, unconditionally, so every call
// after the first came from loop() (or a console/admin path that ticks from it)
// — i.e. something woke the screen mid-window. Services that replay missed
// state need to tell those apart.
static bool _setup_pass_done = false;

bool uiIsUp()          { return _up; }
bool uiBringUpPending() { return _pending && !_up; }

void uiRequestBringUp() {
    if (_up) return;
    _pending = true;
}

void uiBringUpNow(EventBus& bus) {
    const bool late = _setup_pass_done;
    _setup_pass_done = true;

    if (_up || !_pending) return;
    _pending = false;

    // Same sequence, same order, same dependency comments as the block this
    // replaced in setup(). Everything here needs g_hal, g_settings, g_alerts,
    // g_scan_service, g_scan_engine and g_rules to be up already — all of which
    // begin() unconditionally, headless or not.
    g_ui.begin(bus);        // creates the LVGL display — auto-shows perf overlay
    g_logger.attachLvglLog(); // route LVGL logs to serial (Render debug level) — must follow lv_init
    g_display.begin(bus);   // top-bar indicators — must follow g_ui (objects.* must exist)
    g_settings_screen.begin(bus); // settings widget wiring — must follow g_ui
    g_alerts_screen.begin(bus);   // alert cards UI — must follow g_ui + g_display + g_alerts
    g_devices_screen.begin(bus);  // device cards UI — must follow g_ui + g_scan_service
    g_foxhunting_screen.begin(bus); // foxhunt lock-on UI — must follow g_ui + g_scan_service + g_scan_engine
    g_debug_screen.begin(bus);    // diagnostics view — must follow g_ui
    g_rules_screen.begin(bus);    // rule cards UI — must follow g_ui + g_rules
    g_overview_screen.begin(bus); // Helga character animation — must follow g_ui
    g_admin.beginUi();            // admin dropdowns — must follow g_ui (objects.*); rest of admin begins headless
    g_power_menu_screen.begin(bus); // power menu buttons + sleep countdown — must follow g_ui
    g_logger.applyPerfMonitor();   // re-hide unless level >= RENDERING_PERF

    // Marked up only after every begin() has run: a half-built UI must still
    // read as down so the uiIsUp() guards keep no-opping.
    _up = true;

    // Replay what the screen services missed while they weren't subscribed.
    // AlertsScreen::begin already rebuilt cards from the RTC alert store and
    // DisplayService::begin repainted the status bar; these cover the actions
    // that only fire on an event edge.
    g_party.onUiUp();
    g_alerts_screen.onUiUp(late);
}
