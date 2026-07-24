#include "led_service.h"
#include "hal.h"
#include "settings_service.h"
#include "power_manager.h"
#include "alerts_service.h"
#include "event_payload.h"
#include <Arduino.h>
#include <FastLED.h>
#include <strings.h>   // strcasecmp

LedService g_leds;

// ---------------------------------------------------------------------------
// Name registry — string identifiers for each LedPatternId. Used by the
// serial console for `led <name>` and by RulesService at rule load time to
// resolve `led=red_blue_chaser` style criteria.
//
// Order must match the LedPatternId enum exactly. The static_assert below
// fails the build if entries drift out of sync.
// ---------------------------------------------------------------------------

static const char* const s_led_name[] = {
    "off",                  // LED_PATTERN_OFF
    "charging",             // LED_PATTERN_CHARGING
    "fully_charged",        // LED_PATTERN_FULLY_CHARGED
    "serial",               // LED_PATTERN_SERIAL
    "low_battery",          // LED_PATTERN_LOW_BATTERY
    "alert",                // LED_PATTERN_ALERT_DEFAULT
    "red_blue",             // LED_PATTERN_RED_BLUE_CHASER
    "rainbow_fast",         // LED_PATTERN_RAINBOW_FAST
    "rainbow_slow",         // LED_PATTERN_RAINBOW_SLOW
    "white_chaser",         // LED_PATTERN_WHITE_CHASER
    "admin_broadcast",      // LED_PATTERN_ADMIN_BROADCAST
};
static_assert(sizeof(s_led_name) / sizeof(s_led_name[0]) == LED_PATTERN_COUNT,
              "s_led_name out of sync with LedPatternId");

const char* ledPatternName(LedPatternId id) {
    if (id >= LED_PATTERN_COUNT) return "?";
    return s_led_name[id];
}

LedPatternId ledPatternByName(const char* name) {
    if (!name || !*name) return LED_PATTERN_COUNT;
    for (uint8_t i = 0; i < LED_PATTERN_COUNT; i++) {
        if (strcasecmp(name, s_led_name[i]) == 0) return (LedPatternId)i;
    }
    return LED_PATTERN_COUNT;
}

void ledPatternForEach(LedPatternVisitor fn, void* user) {
    if (!fn) return;
    for (uint8_t i = 0; i < LED_PATTERN_COUNT; i++)
        fn((LedPatternId)i, s_led_name[i], user);
}

static constexpr uint32_t FRAME_PERIOD_MS = 33;     // ~30 FPS render cap
static constexpr uint32_t ALERT_DEFAULT_MS = 3000;  // default alert duration when raised via the bus
static constexpr uint32_t ALERT_FADE_MS    = 500;   // fade-out length on alert end
static constexpr uint8_t  LOW_BATTERY_PCT  = 15;    // pulse below this threshold (and not charging)

// ---------------------------------------------------------------------------
// Pattern renderers — each fills a 6-element CRGB array based on now_ms.
//
// Conventions:
//   • Final brightness scaling is applied by FastLED's global setBrightness
//     (which HAL syncs from SKEY_LED_BRIGHTNESS), so each pattern caps its
//     internal brightness at modest values and the user setting scales down.
//   • No FastLED.show() in the pattern functions — the service writes once.
//   • Use sin8/scale8/CHSV for cheap integer math (FastLED helpers).
// ---------------------------------------------------------------------------

static void _patOff(CRGB out[6], uint32_t /*now_ms*/) {
    for (int i = 0; i < 6; i++) out[i] = CRGB::Black;
}

static void _patCharging(CRGB out[6], uint32_t now_ms) {
    // Slow green pulse, ~2 s period, brightness ~30..160.
    uint8_t phase = (uint8_t)((now_ms * 256u / 2000u) & 0xFF);
    uint8_t b     = (uint8_t)(scale8(sin8(phase), 130) + 30);
    CRGB c(0, b, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patFullyCharged(CRGB out[6], uint32_t /*now_ms*/) {
    // Restful steady dim green.
    CRGB c(0, 60, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patLowBattery(CRGB out[6], uint32_t now_ms) {
    // Slow red pulse, ~3 s period, brightness ~30..200.
    uint8_t phase = (uint8_t)((now_ms * 256u / 3000u) & 0xFF);
    uint8_t b     = (uint8_t)(scale8(sin8(phase), 170) + 30);
    CRGB c(b, 0, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patSerial(CRGB out[6], uint32_t now_ms) {
    // Smooth blue half-ring gradient that rotates around the loop. Brightness
    // peaks at the "head" and falls along a cosine curve over half the ring
    // behind it; the leading half stays dark. Half off / half lit, no discrete
    // bright pixels — the whole ring fades through together.
    const uint32_t period_ms = 3000;
    uint8_t head = (uint8_t)((now_ms * 256u / period_ms) & 0xFF);

    for (int i = 0; i < 6; i++) {
        uint8_t led_pos = (uint8_t)((i * 256) / 6);
        // uint8 modular subtraction = "how far is this LED behind the head"
        // (0 at head, 128 at exactly opposite, wrapping at 256).
        uint8_t diff = (uint8_t)(head - led_pos);

        // Cosine falloff over the trailing half; ahead-of-head half is dark.
        // cos8 returns 255 at 0, drops smoothly to ~0 at 128.
        uint8_t b = (diff < 128) ? cos8(diff) : 0;

        // Deep blue with a small cyan tint at the brightest part.
        out[i] = CRGB(0, scale8(b, 40), b);
    }
}

static void _patAlertDefault(CRGB out[6], uint32_t now_ms) {
    // Bright red strobe, 150 ms on / 150 ms off.
    bool on = ((now_ms / 150u) & 1u) == 0u;
    CRGB c = on ? CRGB(255, 0, 0) : CRGB::Black;
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patRedBlueChaser(CRGB out[6], uint32_t now_ms) {
    // 3+3 split alternating ~120 ms — uses the physical left/right grouping.
    bool red_phase = ((now_ms / 120u) & 1u) == 0u;
    CRGB red(220, 0, 0);
    CRGB blue(0, 0, 220);
    if (red_phase) {
        out[0] = out[1] = out[2] = red;
        out[3] = out[4] = out[5] = CRGB::Black;
    } else {
        out[0] = out[1] = out[2] = CRGB::Black;
        out[3] = out[4] = out[5] = blue;
    }
}

static void _renderRainbow(CRGB out[6], uint32_t now_ms, uint32_t period_ms) {
    // Smooth rainbow, all six lit, hue offset 60° apart, rotating once per period.
    uint8_t base_hue = (uint8_t)((now_ms * 256u / period_ms) & 0xFF);
    for (int i = 0; i < 6; i++) {
        uint8_t hue = base_hue + (uint8_t)((i * 256) / 6);
        out[i] = CHSV(hue, 255, 200);
    }
}

static void _patRainbowFast(CRGB out[6], uint32_t now_ms) { _renderRainbow(out, now_ms, 1500); }
static void _patRainbowSlow(CRGB out[6], uint32_t now_ms) { _renderRainbow(out, now_ms, 5000); }

static void _patWhiteChaser(CRGB out[6], uint32_t now_ms) {
    // Single bright "head" racing the ring at ~600 ms/lap, with a sharp fade
    // either side that briefly overlaps adjacent LEDs for a smooth glide.
    const uint32_t lap = 600;
    uint32_t phase = now_ms % lap;

    // Position in fixed-point fractional-LED units: 0..(6 * 256) - 1.
    int pos = (int)((phase * (6u * 256u)) / lap);

    for (int i = 0; i < 6; i++) {
        int center = i * 256;
        int diff = pos - center;
        // Wrap to nearest direction around the ring.
        if (diff >  3 * 256) diff -= 6 * 256;
        if (diff < -3 * 256) diff += 6 * 256;
        int dist = diff < 0 ? -diff : diff;

        // Sharp falloff: peak (220) at dist 0, zero by dist 200.
        uint8_t b = 0;
        if (dist < 200) {
            b = (uint8_t)(220 * (200 - dist) / 200);
        }
        out[i] = CRGB(b, b, b);
    }
}

static void _patAdminBroadcast(CRGB out[6], uint32_t now_ms) {
    // Yellow pulse that rises through the three pairs — {0,5} → {1,4} → {2,3}.
    // The leading edge fades in quickly; the trailing edge fades out slowly, so
    // each pair leaves a glowing trail behind the head as it climbs. It clears
    // the top into darkness, then rises again. The chain is 3 + 3, so a "pair"
    // is the matching position on each side. now_ms is phase-relative (LedService
    // resets it at each broadcast) so the wave always starts from the bottom.
    static const uint8_t PAIR[3][2] = { {0, 5}, {1, 4}, {2, 3} };

    constexpr int32_t  PAIR_FP  = 256;                     // one pair-unit (8.8 fixed point)
    constexpr int32_t  LEAD_FP  = 1 * PAIR_FP;             // fade-in width (sharp leading edge)
    constexpr int32_t  TRAIL_FP = 2 * PAIR_FP;             // fade-out width = trail length (tune here)
    constexpr int32_t  HEAD_MIN = 0 * PAIR_FP - LEAD_FP;   // enters just below pair 0
    constexpr int32_t  HEAD_MAX = 2 * PAIR_FP + TRAIL_FP;  // trail fully clears above pair 2
    constexpr int32_t  SPAN_FP  = HEAD_MAX - HEAD_MIN;
    constexpr uint32_t STEP_MS  = 330;                     // ms per pair-unit of travel (speed knob)
    constexpr uint32_t CYCLE_MS = (uint32_t)SPAN_FP * STEP_MS / PAIR_FP;

    const uint32_t t    = now_ms % CYCLE_MS;
    const int32_t  head = HEAD_MIN + (int32_t)((uint32_t)t * (uint32_t)SPAN_FP / CYCLE_MS);

    for (int i = 0; i < 6; i++) out[i] = CRGB::Black;

    for (uint8_t p = 0; p < 3; p++) {
        const int32_t delta = head - (int32_t)p * PAIR_FP;   // >0 once the head has passed (trailing)
        const int32_t width = (delta >= 0) ? TRAIL_FP : LEAD_FP;
        const int32_t adist = delta < 0 ? -delta : delta;
        if (adist >= width) continue;                        // beyond this edge → dark

        // Asymmetric crossfade tent, eased for a rounder glow: full at the head,
        // linearly down to 0 over LEAD ahead of it / TRAIL behind it.
        uint8_t b = ease8InOutQuad((uint8_t)(255 * (width - adist) / width));

        CRGB c(255, 190, 0);                                 // warm yellow (tune here)
        c.nscale8(b);
        out[PAIR[p][0]] = c;
        out[PAIR[p][1]] = c;
    }
}

// Foxhunt proximity meter tunables. The whole ring throbs as a smooth pulse
// whose RATE and COLOUR track live signal quality — slow red when far, fast
// green when close — and both the LED and the motor ramp to fully SOLID once you
// are right on top of the device (q >= HUNT_SOLID_Q). `qs` below is quality
// rescaled so that the solid point maps to 100 and the pulse ramp spans 0..solid.
static constexpr uint32_t HUNT_PERIOD_FAR_MS   = 1500;  // qs=0   → slow pulse
static constexpr uint32_t HUNT_PERIOD_NEAR_MS  = 300;   // qs=100 → fast pulse (then locked solid)
static constexpr uint8_t  HUNT_SOLID_Q         = 90;    // q at/above this → LED + motor stay fully on
static constexpr uint8_t  HUNT_PEAK_B          = 210;   // pulse peak brightness (user brightness scales further)
static constexpr uint8_t  HUNT_MOTOR_INTENSITY = 175;   // felt-but-gentle motor duty per pulse (ERM whines below ~150)
static constexpr uint8_t  HUNT_MOTOR_GATE_FAR  = 230;   // wave gate at qs=0 → a brief far tick; eases to 0 (solid) near qs=100

// Pulse period (ms) from proximity-scaled quality. Continuous phase means the
// rate can change frame-to-frame without ever jumping the pulse.
static uint32_t _huntPeriodMs(uint8_t qs) {
    return HUNT_PERIOD_FAR_MS - ((HUNT_PERIOD_FAR_MS - HUNT_PERIOD_NEAR_MS) * qs) / 100;
}

// Render one hunt frame at pulse phase `phase8` for proximity-scaled quality
// `qs`, and RETURN the motor intensity for the same phase. LED and motor share
// the phase so their pulses peak together; as qs→100 the LED brightness floor
// rises to the peak (solid) and the motor gate opens to the full period (solid).
static uint8_t _renderHunt(CRGB out[6], uint8_t phase8, uint8_t qs) {
    const uint8_t wave = sin8(phase8);                              // smooth 0..255 hump

    // LED: floor rises with proximity so the throb gets shallower and becomes a
    // steady wash at qs=100; hue sweeps red(0)→green(96).
    const uint8_t floorB = (uint8_t)((uint32_t)HUNT_PEAK_B * qs / 100);
    const uint8_t b      = (uint8_t)(floorB + scale8(wave, (uint8_t)(HUNT_PEAK_B - floorB)));
    const uint8_t hue    = (uint8_t)((96u * qs) / 100u);
    const CRGB c = CHSV(hue, 255, b);
    for (int i = 0; i < 6; i++) out[i] = c;

    // Motor: gated square at a felt intensity (smooth low-duty PWM would just
    // whine). The gate opens on a QUADRATIC ease so far/mid stay a brief tick and
    // the buzz only stretches toward continuous as you get right on top of it —
    // 0 gate (solid) at qs=100. (Linear opened the window too early → buzzed
    // almost constantly at mid range.)
    const uint8_t qe   = (uint8_t)((uint32_t)qs * qs / 100);   // eased proximity: stays low until qs is high
    const uint8_t gate = (uint8_t)(HUNT_MOTOR_GATE_FAR - (uint32_t)HUNT_MOTOR_GATE_FAR * qe / 100);
    return (wave >= gate) ? HUNT_MOTOR_INTENSITY : 0;
}

// ---------------------------------------------------------------------------

static void _renderPattern(LedPatternId p, uint32_t now_ms, CRGB out[6]) {
    switch (p) {
        case LED_PATTERN_OFF:             _patOff(out, now_ms);           break;
        case LED_PATTERN_CHARGING:        _patCharging(out, now_ms);      break;
        case LED_PATTERN_FULLY_CHARGED:   _patFullyCharged(out, now_ms);  break;
        case LED_PATTERN_SERIAL:          _patSerial(out, now_ms);        break;
        case LED_PATTERN_LOW_BATTERY:     _patLowBattery(out, now_ms);    break;
        case LED_PATTERN_ALERT_DEFAULT:   _patAlertDefault(out, now_ms);  break;
        case LED_PATTERN_RED_BLUE_CHASER: _patRedBlueChaser(out, now_ms); break;
        case LED_PATTERN_RAINBOW_FAST:    _patRainbowFast(out, now_ms);   break;
        case LED_PATTERN_RAINBOW_SLOW:    _patRainbowSlow(out, now_ms);   break;
        case LED_PATTERN_WHITE_CHASER:    _patWhiteChaser(out, now_ms);   break;
        case LED_PATTERN_ADMIN_BROADCAST: _patAdminBroadcast(out, now_ms);break;
        default:                          _patOff(out, now_ms);           break;
    }
}

static void _scaleFrame(CRGB frame[6], uint8_t alpha) {
    if (alpha == 255) return;
    for (int i = 0; i < 6; i++) frame[i].nscale8(alpha);
}

// ---------------------------------------------------------------------------
// LedService
// ---------------------------------------------------------------------------

void LedService::begin(EventBus& bus) {
    _bus = &bus;

    bus.subscribe(EV_BATTERY_UPDATED, this);
    bus.subscribe(EV_TICK_1S,         this);
    bus.subscribe(EV_ALERT_RAISED,    this);
    bus.subscribe(EV_ALERT_CLEARED,   this);
}

void LedService::tick() {
    uint32_t now = millis();
    if (now - _last_render_ms < FRAME_PERIOD_MS) return;
    _last_render_ms = now;

    // ---- Layer composition ----
    // Alert preempts ambient. When an alert's duration expires (or someone
    // clears it via EV_ALERT_CLEARED), we fade-out over ALERT_FADE_MS by
    // scaling the alert frame's brightness, then drop the alert layer entirely
    // so the ambient pattern resumes on the next frame.
    LedPatternId effective = _ambient;
    uint8_t      alpha     = 255;

    if (_alert != LED_PATTERN_OFF) {
        // Trigger fade if the alert's duration has elapsed.
        if (_alert_until_ms != 0 && now >= _alert_until_ms && _alert_fade_start_ms == 0) {
            _alert_fade_start_ms = now;
        }

        if (_alert_fade_start_ms != 0) {
            uint32_t elapsed = now - _alert_fade_start_ms;
            if (elapsed >= ALERT_FADE_MS) {
                // Fade complete — drop the layer.
                _alert               = LED_PATTERN_OFF;
                _alert_until_ms      = 0;
                _alert_fade_start_ms = 0;
            } else {
                effective = _alert;
                alpha     = (uint8_t)(255u - (elapsed * 255u / ALERT_FADE_MS));
            }
        } else {
            effective = _alert;
        }
    }

    // Top-priority overrides, highest first: the admin broadcast indicator, then
    // the foxhunt proximity meter. Both preempt alert + ambient and ignore the
    // alert fade alpha. Broadcast renders phase-relative to its start so its wave
    // always begins at the bottom.
    // ---- Render and push ----
    CRGB frame[6];
    if (_broadcast) {
        _renderPattern(LED_PATTERN_ADMIN_BROADCAST, now - _broadcast_start_ms, frame);
    } else if (_hunt) {
        // Rescale quality so the "on top of it" point (HUNT_SOLID_Q) maps to a
        // fully solid LED + motor, and the pulse ramp spans everything below it.
        const uint8_t qs = (_hunt_q >= HUNT_SOLID_Q)
                           ? 100 : (uint8_t)((uint32_t)_hunt_q * 100 / HUNT_SOLID_Q);

        // Advance the pulse phase by real elapsed time so the rate can change
        // smoothly (faster as you close in) without ever jumping the pulse.
        const uint32_t dt = _hunt_last_ms ? (now - _hunt_last_ms) : 0;
        _hunt_last_ms = now;
        _hunt_phase  += (uint16_t)((dt * 65536u) / _huntPeriodMs(qs));

        // LED frame + the motor drive for the same phase — kept in lockstep so
        // the buzz tracks the blink and both go solid together when close. The
        // haptic is opt-in (SKEY_HUNT_VIBRATION, default off); the LED meter runs
        // regardless.
        const uint8_t motor = _renderHunt(frame, (uint8_t)(_hunt_phase >> 8), qs);
        g_hal.setVibrate(g_settings.getBool(SKEY_HUNT_VIBRATION) ? motor : 0);
    } else {
        _renderPattern(effective, now, frame);
        _scaleFrame(frame, alpha);
    }
    g_hal.writeLEDFrame(frame);
}

void LedService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BATTERY_UPDATED: {
            uint8_t pct = e.data.battery.pct;
            _is_charging = (pct == BATT_PCT_CHARGING);
            _is_charged  = (pct == BATT_PCT_CHARGED);
            // Low only when on battery, with a real percentage reading (sentinels are >100).
            _is_low_batt = (pct <= 100) && (pct < LOW_BATTERY_PCT) && !_is_charging;
            _recomputeAmbient();
            break;
        }

        case EV_TICK_1S: {
            // Re-evaluate serial state once per second so the comet kicks in/out
            // when a terminal opens/closes.
            bool serial_now = (bool)Serial;
            if (serial_now != _last_serial) {
                _last_serial = serial_now;
                _recomputeAmbient();
            }
            break;
        }

        case EV_ALERT_RAISED:
            // Look up the alert's per-record LED pattern. AlertsService owns
            // the pattern; AlertPayload just carries the alert_id. Fall back
            // to the default pattern if the record can't be found.
            if (g_settings.getBool(SKEY_ALERT_LED)) {
                const AlertRecord* rec = g_alerts.find(e.data.alert.alert_id);
                playAlertPattern(rec ? rec->led : LED_PATTERN_ALERT_DEFAULT,
                                 ALERT_DEFAULT_MS);
            }
            break;

        case EV_ALERT_CLEARED:
            // Start fade-out immediately if an alert is currently playing.
            if (_alert != LED_PATTERN_OFF && _alert_fade_start_ms == 0) {
                _alert_fade_start_ms = millis();
            }
            break;

        default:
            break;
    }
}

void LedService::setBroadcast(bool on) {
    // Reset the phase only on the off→on edge so the wave restarts from the
    // bottom each broadcast (re-arming while already on doesn't stutter it).
    if (on && !_broadcast) _broadcast_start_ms = millis();
    _broadcast = on;
}

void LedService::setHunt(bool on) {
    if (on && !_hunt) { _hunt_phase = 0; _hunt_last_ms = 0; }   // fresh pulse from the bottom
    _hunt = on;
    if (!on) { _hunt_q = 0; g_hal.stopVibrate(); }              // release the motor; ambient LEDs resume
}

void LedService::setHuntQuality(uint8_t quality) {
    _hunt_q = (quality > 100) ? 100 : quality;
}

void LedService::playAlertPattern(LedPatternId pattern, uint32_t duration_ms) {
    _alert               = pattern;
    _alert_until_ms      = (duration_ms > 0) ? (millis() + duration_ms) : 0;
    _alert_fade_start_ms = 0;
}

void LedService::_recomputeAmbient() {
    // Priority order: serial > charging > charged > low-battery > off.
    //
    // (bool)Serial is only true when a host has actively opened the CDC port
    // (developer present), so it's a stronger signal of intent than passive
    // charging. Showing the comet while a terminal is open also confirms the
    // device is responsive to that connection.
    LedPatternId next = LED_PATTERN_OFF;
    if      (_last_serial) next = LED_PATTERN_SERIAL;
    else if (_is_charging) next = LED_PATTERN_CHARGING;
    else if (_is_charged)  next = LED_PATTERN_FULLY_CHARGED;
    else if (_is_low_batt) next = LED_PATTERN_LOW_BATTERY;
    _ambient = next;
}
