#pragma once

#include "vibe_pattern.h"
#include <stdint.h>

enum class VibeBoundaryAction : uint8_t {
    COMPLETE,
    RESTART,
};

struct VibeStep {
    uint8_t intensity;
    uint16_t duration_ms;
};

struct VibePlaybackOperations {
    void* context;
    bool (*start_timer_once)(void* context, uint64_t timeout_us);
    bool (*stop_timer)(void* context);
    void (*write_motor)(void* context, uint8_t intensity);
};

class VibeTimerExpiryState {
public:
    void recordTimerStop(bool stopped_before_expiry);
    bool acceptsNextExpiry();

private:
    uint32_t _stale_expiries = 0;
};

class VibeRepeatState {
public:
    bool start(HapticPatternId pattern, bool playback_available);
    bool stop();

    bool active() const { return _active; }
    HapticPatternId pattern() const { return _pattern; }
    bool acceptsOneShot() const { return !_active; }
    VibeBoundaryAction boundaryAction() const {
        return _active
            ? VibeBoundaryAction::RESTART
            : VibeBoundaryAction::COMPLETE;
    }

private:
    bool _active = false;
    HapticPatternId _pattern = HAPTIC_OFF;
};

// Hardware-independent playback state. VibeService serializes calls with its
// existing mutex and supplies the esp_timer/HAL operations; native tests can
// inject deterministic failures without replacing the timer task architecture.
class VibePlaybackState {
public:
    bool play(HapticPatternId pattern, const VibeStep* steps,
              const VibePlaybackOperations& operations);
    bool playRepeating(HapticPatternId pattern, const VibeStep* steps,
                       bool playback_available,
                       const VibePlaybackOperations& operations);
    bool onTimerExpired(const VibePlaybackOperations& operations);
    void stop(const VibePlaybackOperations& operations);

    bool repeating() const { return _repeat.active(); }
    HapticPatternId repeatingPattern() const { return _repeat.pattern(); }
    bool acceptsOneShot() const { return _repeat.acceptsOneShot(); }

private:
    void _cancelTimer(const VibePlaybackOperations& operations);
    void _failClosed(const VibePlaybackOperations& operations);
    bool _armCurrent(const VibePlaybackOperations& operations);

    HapticPatternId _current = HAPTIC_OFF;
    const VibeStep* _steps = nullptr;
    uint8_t _step_index = 0;
    VibeTimerExpiryState _timer_expiries;
    VibeRepeatState _repeat;
};
