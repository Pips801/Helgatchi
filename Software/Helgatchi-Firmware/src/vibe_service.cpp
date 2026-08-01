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
// VibePlaybackState writes the current step's intensity through the HAL adapter
// and arms the timer for that step's duration; the callback (_onTimer) advances
// to the next step. At the {0, 0} terminator a one-shot drives the motor to 0
// and returns to OFF; repeat mode restarts step zero immediately without
// injecting an off write or delay.
// ---------------------------------------------------------------------------

// Intensity numbers are uint8 PWM duty (0..255). For ERM motors, anything
// below ~150 just whines without spinning — the eccentric mass needs enough
// average voltage to overcome static friction. The "minor" feel of TICK_LIGHT
// comes from a *short* duration, not a low duty cycle.
static const VibeStep PAT_TICK_LIGHT[] = { {255,  35}, {0, 0} };
static const VibeStep PAT_TICK[]       = { {220,  45}, {0, 0} };
static const VibeStep PAT_BUMP[]       = { {255,  70}, {0, 0} };
static const VibeStep PAT_DOUBLE_TAP[] = { {220,  40}, {0, 60}, {220, 40}, {0, 0} };
static const VibeStep PAT_LONG_BUZZ[]  = { {255, 500}, {0, 0} };

// Indexed by HapticPatternId. HAPTIC_OFF maps to nullptr — "no steps".
static const VibeStep* const PATTERNS[HAPTIC_PATTERN_COUNT] = {
    nullptr,            // HAPTIC_OFF
    PAT_TICK_LIGHT,     // HAPTIC_TICK_LIGHT
    PAT_TICK,           // HAPTIC_TICK
    PAT_BUMP,           // HAPTIC_BUMP
    PAT_DOUBLE_TAP,     // HAPTIC_DOUBLE_TAP
    PAT_LONG_BUZZ,      // HAPTIC_LONG_BUZZ
};

static bool startTimerOnce(void* context, uint64_t timeout_us) {
    esp_timer_handle_t* timer = static_cast<esp_timer_handle_t*>(context);
    return timer && *timer &&
           esp_timer_start_once(*timer, timeout_us) == ESP_OK;
}

static bool stopTimer(void* context) {
    esp_timer_handle_t* timer = static_cast<esp_timer_handle_t*>(context);
    return timer && *timer && esp_timer_stop(*timer) == ESP_OK;
}

static void writeMotor(void*, uint8_t intensity) {
    g_hal.setVibrate(intensity);
}

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

    const VibeStep* steps = PATTERNS[pattern];
    if (!s_vibe_lock) {
        if (pattern == HAPTIC_OFF) stop();
        return;
    }
    if (pattern != HAPTIC_OFF && (!steps || !_timer)) return;

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    _playback.play(pattern, steps, _operations());
    xSemaphoreGive(s_vibe_lock);
}

bool VibeService::playRepeating(HapticPatternId pattern) {
    if (pattern == HAPTIC_OFF || pattern >= HAPTIC_PATTERN_COUNT) {
        return false;
    }

    const VibeStep* steps = PATTERNS[pattern];
    if (!steps || !s_vibe_lock) return false;

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    const bool started = _playback.playRepeating(
        pattern, steps, _timer != nullptr, _operations()
    );
    xSemaphoreGive(s_vibe_lock);
    return started;
}

bool VibeService::repeating() const {
    if (!s_vibe_lock) return false;
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    const bool value = _playback.repeating();
    xSemaphoreGive(s_vibe_lock);
    return value;
}

HapticPatternId VibeService::repeatingPattern() const {
    if (!s_vibe_lock) return HAPTIC_OFF;
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    const HapticPatternId value = _playback.repeatingPattern();
    xSemaphoreGive(s_vibe_lock);
    return value;
}

void VibeService::stop() {
    if (!s_vibe_lock) {
        _playback.stop(_operations());
        return;
    }

    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    _playback.stop(_operations());
    xSemaphoreGive(s_vibe_lock);
}

VibePlaybackOperations VibeService::_operations() {
    const VibePlaybackOperations operations = {
        &_timer,
        &startTimerOnce,
        &stopTimer,
        &writeMotor,
    };
    return operations;
}

void VibeService::_onTimer() {
    xSemaphoreTake(s_vibe_lock, portMAX_DELAY);
    _playback.onTimerExpired(_operations());
    xSemaphoreGive(s_vibe_lock);
}

void VibeService::_timerCb(void* arg) {
    static_cast<VibeService*>(arg)->_onTimer();
}
