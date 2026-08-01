#pragma once
#include "event_bus.h"
#include "vibe_pattern.h"
#include <stdint.h>
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// Haptic pattern catalog
//
// Patterns are short sequences of (intensity, duration) steps that VibeService
// plays out via PWM on the vibration motor. Patterns are fire-and-forget — a
// subsequent play() preempts whatever was running.
//
// Step timing runs on a one-shot esp_timer, NOT the main loop: each step arms
// the timer for its duration and the callback advances to the next step (or
// drives the motor off at the terminating step). This decouples haptics from
// UI/render latency — a stalled loop can no longer stretch a buzz — and makes
// motor-off structurally guaranteed, since it's always the last scheduled
// step. play() / stop() / the timer callback are serialized by a mutex because
// the callback fires on the esp_timer task, so play() is safe from any task.
//
// Two trigger paths:
//   • Direct: g_vibe.play(HAPTIC_TICK_LIGHT) for instant UI haptics.
//   • Bus:    EV_ALERT_RAISED → VibeService picks the alert pattern.
//
// SKEY_ALERT_VIBRATION gates only the bus path. Button-press haptics and any
// direct play() call always fire.
// ---------------------------------------------------------------------------


class VibeService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Fire-and-forget pattern playback. Preempts any in-flight pattern and
    // always fires (the SKEY_ALERT_VIBRATION gate is applied on the bus path
    // before calling play). Safe to call from any task.
    void play(HapticPatternId pattern);

    // Cancel any in-flight pattern and drive the motor off now.
    void stop();

private:
    static void _timerCb(void* arg);   // esp_timer callback trampoline → _onTimer()
    void _onTimer();                   // advance to the next step
    void _armCurrentLocked();          // drive current step + arm its timer, or
                                       // finish; caller must hold the vibe lock

    EventBus*          _bus         = nullptr;
    esp_timer_handle_t _timer       = nullptr;
    HapticPatternId    _current     = HAPTIC_OFF;
    const void*        _steps       = nullptr;  // const Step* erased to keep header light
    uint8_t            _step_index  = 0;
};

extern VibeService g_vibe;
