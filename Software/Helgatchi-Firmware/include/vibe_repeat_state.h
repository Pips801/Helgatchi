#pragma once

#include "vibe_pattern.h"
#include <stdint.h>

enum class VibeBoundaryAction : uint8_t {
    COMPLETE,
    RESTART,
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
