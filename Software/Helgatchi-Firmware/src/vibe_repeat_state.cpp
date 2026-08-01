#include "vibe_repeat_state.h"

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
