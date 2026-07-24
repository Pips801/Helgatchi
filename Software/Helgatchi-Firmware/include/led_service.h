#pragma once
#include "event_bus.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// LED pattern catalog
//
// Patterns are referenced by enum value (1 byte). Adding a new look:
//   1. Add an enum entry below
//   2. Add a renderer function in led_service.cpp
//   3. Add a case in the dispatch switch
//
// Layering: an "alert" pattern preempts whatever ambient pattern is currently
// active. When an alert ends (timeout or EV_ALERT_CLEARED), we fade-out over
// 500 ms and the underlying ambient resumes.
// ---------------------------------------------------------------------------

enum LedPatternId : uint8_t {
    LED_PATTERN_OFF = 0,

    // --- Ambient (device-state driven) ---
    LED_PATTERN_CHARGING,         // slow green pulse
    LED_PATTERN_FULLY_CHARGED,    // steady dim green
    LED_PATTERN_SERIAL,           // cyan comet rotation
    LED_PATTERN_LOW_BATTERY,      // slow red pulse

    // --- Alert effects (rule-driven, neutral names) ---
    LED_PATTERN_ALERT_DEFAULT,    // red strobe (fallback when rule has none)
    LED_PATTERN_RED_BLUE_CHASER,  // 3+3 split, alternating red/blue strobe
    LED_PATTERN_RAINBOW_FAST,     // rainbow rotation, ~1.5s/lap
    LED_PATTERN_RAINBOW_SLOW,     // rainbow rotation, ~5s/lap
    LED_PATTERN_WHITE_CHASER,     // single white LED chasing the ring with fast fade

    // --- Status indicators (service-driven, not user-selectable ambient) ---
    LED_PATTERN_ADMIN_BROADCAST,  // yellow power-up: outer pair inward while a controller transmits

    LED_PATTERN_COUNT,
};

// ---------------------------------------------------------------------------
// Name registry — string identifiers for use in rule files, serial console,
// and anything that needs a stable text form. Keep in sync with the enum;
// led_service.cpp has a static_assert that catches drift at compile time.
// ---------------------------------------------------------------------------

// Returns the registered name for `id` or "?" if out of range.
const char* ledPatternName(LedPatternId id);

// Case-insensitive name -> id. Returns LED_PATTERN_COUNT (sentinel) if no
// pattern carries that name. Callers (RulesService) substitute their own
// default when the lookup misses; the registry stays neutral about what
// "default" means.
LedPatternId ledPatternByName(const char* name);

// Visit every registered pattern in enum order (id == visit index). The single
// source for anything that enumerates the catalog — the serial `led list` /
// `dump`, the admin send-menu dropdown — so none re-roll the LED_PATTERN_COUNT
// sweep. `user` is passed through untouched (use a captureless lambda + ctx).
typedef void (*LedPatternVisitor)(LedPatternId id, const char* name, void* user);
void ledPatternForEach(LedPatternVisitor fn, void* user);

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

    // Top-priority broadcast indicator. AdminService toggles this while a
    // controller is transmitting a command; it preempts both the alert and
    // ambient layers and restores them untouched when cleared. Enabling it
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
