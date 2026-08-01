#include "vibe_repeat_state.h"

void VibeTimerExpiryState::recordTimerStop(bool stopped_before_expiry) {
    if (!stopped_before_expiry && _stale_expiries < UINT32_MAX) {
        ++_stale_expiries;
    }
}

bool VibeTimerExpiryState::acceptsNextExpiry() {
    if (_stale_expiries == 0) return true;
    --_stale_expiries;
    return false;
}

bool VibeRepeatState::start(HapticPatternId pattern, bool playback_available) {
    if (!playback_available ||
        pattern == HAPTIC_OFF ||
        pattern >= HAPTIC_PATTERN_COUNT) {
        return false;
    }
    _active = true;
    _pattern = pattern;
    return true;
}

bool VibeRepeatState::stop() {
    if (!_active) return false;
    _active = false;
    _pattern = HAPTIC_OFF;
    return true;
}
