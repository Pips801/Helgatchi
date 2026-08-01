#include "vibe_service.h"
#include "hal.h"
#include "settings_service.h"
#include "alerts_service.h"
#include "event_payload.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

VibeService g_vibe;

// Serializes play() / stop() (called from the loop task) against the esp_timer
// callback (which runs on the esp_timer task). One global VibeService, so a
// file-static mutex is all we need. Held only across a few register writes and
// esp_timer arm/stop calls — never across anything that blocks.
static SemaphoreHandle_t s_vibe_lock = nullptr;

// ---------------------------------------------------------------------------
// Pattern definitions
//
// Each pattern is an array of {intensity, duration_ms} steps terminated by
// {0, 0}. Playback is driven by a one-shot esp_timer, NOT the main loop:
// _armCurrentLocked() writes the current step's intensity to the motor and
// arms the timer for that step's duration; the callback (_onTimer) advances to
// the next step. At the {0, 0} terminator a one-shot drives the motor to 0 and
// returns to OFF; repeat mode restarts step zero immediately without injecting
// an off write or delay.
// ---------------------------------------------------------------------------

struct Step {
    uint8_t  intensity;
    uint16_t duration_ms;
};

// Intensity numbers are uint8 PWM duty (0..255). For ERM motors, anything
// below ~150 just whines without spinning — the eccentric mass needs enough
// average voltage to overcome static friction. The "minor" feel of TICK_LIGHT
// comes from a *short* duration, not a low duty cycle.
static const Step PAT_TICK_LIGHT[] = { {255,  35}, {0, 0} };
static const Step PAT_TICK[]       = { {220,  45}, {0, 0} };
static const Step PAT_BUMP[]       = { {255,  70}, {0, 0} };
static const Step PAT_DOUBLE_TAP[] = { {220,  40}, {0, 60}, {220, 40}, {0, 0} };
static const Step PAT_LONG_BUZZ[]  = { {255, 500}, {0, 0} };

// Indexed by HapticPatternId. HAPTIC_OFF maps to nullptr — "no steps".
static const Step* const PATTERNS[HAPTIC_PATTERN_COUNT] = {
    nullptr,            // HAPTIC_OFF
    PAT_TICK_LIGHT,     // HAPTIC_TICK_LIGHT
    PAT_TICK,           // HAPTIC_TICK
    PAT_BUMP,           // HAPTIC_BUMP
    PAT_DOUBLE_TAP,     // HAPTIC_DOUBLE_TAP
    PAT_LONG_BUZZ,      // HAPTIC_LONG_BUZZ
};

// ---------------------------------------------------------------------------

void VibeService::begin(EventBus& bus) {
    _bus = &bus;

    if (!s_vibe_lock) s_vibe_lock = xSemaphoreCreateMutex();

    const esp_timer_create_args_t args = {
        .callback        = &VibeService::_timerCb,
        .arg             = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "vibe",
    };
    const esp_err_t err = esp_timer_create(&args, &_timer);
    if (err != ESP_OK) {
        Serial.printf("[vibe] esp_timer_create failed (%d) — haptics disabled\n", (int)err);
        _timer = nullptr;
    }

    // Button haptics are fired by UIController at the decision site, not here.
    // Only it knows the focused widget and its position in the nav group, so it
    // can stay silent at a scroll boundary or on a non-clickable object.
    // CENTER_LONG already worked this way; now all button haptics do. This
    // service only owns the alert path on the bus.
    //
    // Alerts: rules engine fires EV_ALERT_RAISED, we pick a pattern.
    bus.subscribe(EV_ALERT_RAISED, this);
}

void VibeService::onEvent(const Event& e) {
    switch (e.id) {
        // Button haptics are intentionally not handled here. UIController fires
        // them at the action site so a press that changes nothing — a scroll
        // boundary, a non-clickable object, a dead-end long-press — stays
        // silent and the bump reflects real UI state, not the raw press.

        case EV_ALERT_RAISED: {
            // SKEY_ALERT_VIBRATION gates alerts only — button-press haptics
            // and direct play() calls bypass this.
            if (!g_settings.getBool(SKEY_ALERT_VIBRATION)) break;
            // Look up the alert's per-record vibe pattern. AlertsService is
            // the source of truth — AlertPayload only carries the alert_id,
            // not the pattern itself. Fall back to a sensible default if
            // the record is gone (race-cleared or unknown id).
            const AlertRecord* rec = g_alerts.find(e.data.alert.alert_id);
            play(rec ? rec->vibe : HAPTIC_DOUBLE_TAP);
            break;
        }

        default:
            break;
    }
}

void VibeService::play(HapticPatternId pattern) {
    if (pattern >= HAPTIC_PATTERN_COUNT) return;
    if (pattern == HAPTIC_OFF) {
        stop();
        return;
    }

    const Step* steps = PATTERNS[pattern];
    if (!steps || !_timer || !s_vibe_lock) return;

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    if (!_repeat.acceptsOneShot()) {
        xSemaphoreGive(s_vibe_lock);
        return;
    }

    esp_timer_stop(_timer);
    _current = pattern;
    _steps = steps;
    _step_index = 0;
    _armCurrentLocked();
    xSemaphoreGive(s_vibe_lock);
}

bool VibeService::playRepeating(HapticPatternId pattern) {
    if (pattern == HAPTIC_OFF || pattern >= HAPTIC_PATTERN_COUNT) {
        return false;
    }

    const Step* steps = PATTERNS[pattern];
    if (!steps || !s_vibe_lock) return false;

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    if (!_repeat.start(pattern, _timer != nullptr)) {
        xSemaphoreGive(s_vibe_lock);
        return false;
    }

    esp_timer_stop(_timer);
    _current = pattern;
    _steps = steps;
    _step_index = 0;
    _armCurrentLocked();
    xSemaphoreGive(s_vibe_lock);
    return true;
}

bool VibeService::repeating() const {
    if (!s_vibe_lock) return false;
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    const bool value = _repeat.active();
    xSemaphoreGive(s_vibe_lock);
    return value;
}

HapticPatternId VibeService::repeatingPattern() const {
    if (!s_vibe_lock) return HAPTIC_OFF;
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    const HapticPatternId value = _repeat.pattern();
    xSemaphoreGive(s_vibe_lock);
    return value;
}

void VibeService::stop() {
    if (!s_vibe_lock) {
        _repeat.stop();
        _current = HAPTIC_OFF;
        _steps = nullptr;
        _step_index = 0;
        g_hal.stopVibrate();
        return;
    }

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    if (_timer) esp_timer_stop(_timer);
    _repeat.stop();
    _current = HAPTIC_OFF;
    _steps = nullptr;
    _step_index = 0;
    g_hal.stopVibrate();
    xSemaphoreGive(s_vibe_lock);
}

// Caller must hold s_vibe_lock. Drives the current step's intensity and arms
// the timer for its duration. At the {0,0} terminator, completes a one-shot or
// immediately restarts repeat playback.
void VibeService::_armCurrentLocked() {
    const Step* steps = static_cast<const Step*>(_steps);
    if (!steps) return;
    const Step* step = &steps[_step_index];

    if (step->duration_ms == 0) {
        if (_repeat.boundaryAction() == VibeBoundaryAction::RESTART) {
            _step_index = 0;
            step = &steps[0];
            if (step->duration_ms == 0) {
                _repeat.stop();
                g_hal.stopVibrate();
                _current = HAPTIC_OFF;
                _steps = nullptr;
                return;
            }
        } else {
            g_hal.stopVibrate();
            _current = HAPTIC_OFF;
            _steps = nullptr;
            _step_index = 0;
            return;
        }
    }

    g_hal.setVibrate(step->intensity);
    esp_timer_start_once(_timer,
                         static_cast<uint64_t>(step->duration_ms) * 1000);
}

void VibeService::_onTimer() {
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    // A concurrent play()/stop() can retire the pattern between this timer
    // firing and us taking the lock; the guard drops those stale wakeups. The
    // motor-off is always driven under the lock (here at the terminator, or by
    // stop()), so it can never be left energized.
    if (_current != HAPTIC_OFF && _steps != nullptr) {
        _step_index++;
        _armCurrentLocked();
    }
    xSemaphoreGive(s_vibe_lock);
}

void VibeService::_timerCb(void* arg) {
    static_cast<VibeService*>(arg)->_onTimer();
}
