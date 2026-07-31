#pragma once
#include "event_bus.h"
#include "party_navigation_state.h"
#include "party_session_state.h"
#include <stdint.h>

// PartyService — the Party-mode device state.
//
// Party mode is a sustained, look-at-me state: a looping rainbow LED pattern,
// rhythmic haptic pulses, Helga's dance animation on the overview screen, and a
// colour-cycling Party banner. Serial/rule/admin sessions are timed; the main-
// menu entry runs until its next physical button action is consumed.
//
// Entry points:
//   - start(duration, from_rule) for timed serial, rule, and admin callers;
//   - startMenu() for the direct-launch main-menu card; and
//   - stop() for explicit shutdown from serial, admin, or UI paths.
//
// Both start paths bring up Overview. Timed timeout or explicit stop tears down
// effects and retains the existing Overview navigation behavior. UIController
// consumes the first menu-session button, tears down Party, and returns to Main
// Menu. tick() also dismisses Party after an unexpected settled screen change;
// Helga Menu retains its existing background exception.
//
// The banner is a runtime-owned label parented to the Overview top bar because
// EEZ reasserts its bound title text every flow tick. The new EEZ change adds
// only the Party card; banner rendering remains self-contained in this service.
//
// Initialize AFTER g_ui + g_overview_screen (it references objects.*).

class PartyService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Timed entry point used by serial, rules, and admin. A zero duration keeps the
    // existing 20-second default. Timed retriggers refresh timed sessions but never
    // shorten an active menu session.
    void start(uint32_t duration_ms, bool from_rule = false);

    // Explicit foreground entry point used only by the generated Party card.
    void startMenu();

    // Ends and consumes the first physical action only while menu mode is active.
    bool handleMenuButton(EventId event_id);

    // Silently consumes the HOLD continuation of a menu-exiting CENTER_LONG.
    bool consumeMenuExitFollowup(EventId event_id);

    void stop(bool arm_cooldown = true);

    bool active() const { return _session.active(); }
    bool menuActive() const { return _session.menu(); }
    uint32_t remainingMs() const;

    static constexpr uint32_t DEFAULT_DURATION_MS = 20000;
    static constexpr uint32_t COOLDOWN_MS = 5 * 60000;

private:
    void _ensureBanner();
    void _refreshColors();
    void _activate(uint32_t now_ms);
    void _teardownEffects(bool set_cooldown);
    void _end(bool set_cooldown);

    EventBus* _bus = nullptr;
    PartyNavigationState _navigation;
    PartySessionState _session;
    PartyCooldownState _cooldown;
    uint32_t _last_vibe_ms = 0;
    uint32_t _last_text_ms = 0;
    uint32_t _last_awake_ms = 0;
    uint16_t _hue = 0;
    void* _banner = nullptr;
};

extern PartyService g_party;
