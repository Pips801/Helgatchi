#pragma once
#include "led_pattern.h"
#include "led_manual_state.h"
#include "event_bus.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// LED pattern catalog
//
// Patterns are referenced by enum value (1 byte). Adding a new look:
//   1. Add an enum entry in led_pattern.h
//   2. Add a renderer function in led_service.cpp
//   3. Add a case in the dispatch switch
//
// Layering: an "alert" pattern preempts whatever ambient pattern is currently
// active. When an alert ends (timeout or EV_ALERT_CLEARED), we fade-out over
// 500 ms and the underlying ambient resumes.
// ---------------------------------------------------------------------------

class LedService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Trigger an alert pattern manually (rules engine, serial console testing).
    // duration_ms = 0 means "play indefinitely until EV_ALERT_CLEARED arrives".
    // SKEY_ALERT_LED gates EV_ALERT_RAISED-driven calls but NOT this method —
    // explicit programmatic triggers always fire.
    void playAlertPattern(LedPatternId pattern, uint32_t duration_ms);

    // Runtime-only ambient override used by the LED Modes screen. The selected
    // pattern is never persisted; functional broadcast/hunt/alert layers retain
    // priority and reveal this pattern again when they end.
    bool setManualPattern(LedPatternId pattern);
    void clearManualPattern();
    bool manualPatternActive() const { return _manual.active(); }
    bool manualPatternKeepsAwake() const { return _manual.keepsAwake(); }
    LedPatternId manualPattern() const { return _manual.pattern(); }

    // Top-priority broadcast indicator. AdminService toggles this while a
    // controller is transmitting a command; it preempts both the alert and
    // ambient layers and restores them untouched when cleared. It is
    // service-driven and also available for manual selection. Enabling it
    // resets the animation phase so the wave always starts from the bottom.
    void setBroadcast(bool on);

    // Foxhunt proximity meter. FoxhuntingScreen turns this on for the duration
    // of a hunt and pushes the live signal quality (0..100, the same value that
    // drives the on-screen bar) every refresh. The renderer maps it to a Geiger
    // pulse: slow red blips when far/weak → fast green blips when close/strong.
    // Preempts alert + ambient, sits just below the broadcast indicator.
    void setHunt(bool on);
    void setHuntQuality(uint8_t quality);   // 0..100; clamped

private:
    EventBus* _bus = nullptr;

    // Layer state
    LedPatternId _ambient   = LED_PATTERN_OFF;
    LedPatternId _alert     = LED_PATTERN_OFF;
    LedManualState _manual;
    bool         _broadcast = false;   // controller transmitting → top-priority indicator
    uint32_t     _broadcast_start_ms = 0;  // millis() at broadcast start → phase-relative render
    uint32_t     _alert_until_ms      = 0;  // millis() when alert expires (0 = no expiry)
    uint32_t     _alert_fade_start_ms = 0;  // millis() when fade-out began (0 = not fading)

    // Foxhunt proximity meter layer. A continuous phase accumulator (advanced by
    // real dt each frame, rate set by quality) drives a smooth pulse whose rate,
    // colour, and motor duty all ramp with proximity and lock solid at the top —
    // continuous phase means a changing rate never jumps the pulse.
    bool         _hunt         = false;
    uint8_t      _hunt_q       = 0;    // live signal quality 0..100 (pushed by FoxhuntingScreen)
    uint16_t     _hunt_phase   = 0;    // pulse phase (one cycle = full uint16 range); continuous across rate changes
    uint32_t     _hunt_last_ms = 0;    // millis() of last hunt frame → dt for phase advance

    // Cached ambient inputs
    bool _is_charging = false;
    bool _is_charged  = false;
    bool _is_low_batt = false;
    bool _last_serial = false;

    uint32_t _last_render_ms = 0;

    void _recomputeAmbient();
};

extern LedService g_leds;
