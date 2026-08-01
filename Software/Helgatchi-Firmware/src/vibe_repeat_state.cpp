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

bool VibePlaybackState::play(
    HapticPatternId pattern,
    const VibeStep* steps,
    const VibePlaybackOperations& operations
) {
    if (pattern >= HAPTIC_PATTERN_COUNT || !_repeat.acceptsOneShot()) {
        return false;
    }
    if (pattern == HAPTIC_OFF) {
        stop(operations);
        return true;
    }
    if (!steps) return false;

    _cancelTimer(operations);
    _current = pattern;
    _steps = steps;
    _step_index = 0;
    return _armCurrent(operations);
}

bool VibePlaybackState::playRepeating(
    HapticPatternId pattern,
    const VibeStep* steps,
    bool playback_available,
    const VibePlaybackOperations& operations
) {
    if (!steps || !_repeat.start(pattern, playback_available)) {
        return false;
    }

    _cancelTimer(operations);
    _current = pattern;
    _steps = steps;
    _step_index = 0;
    return _armCurrent(operations);
}

bool VibePlaybackState::onTimerExpired(
    const VibePlaybackOperations& operations
) {
    if (!_timer_expiries.acceptsNextExpiry()) return true;
    if (_current == HAPTIC_OFF || !_steps) return true;

    ++_step_index;
    return _armCurrent(operations);
}

void VibePlaybackState::stop(
    const VibePlaybackOperations& operations
) {
    _cancelTimer(operations);
    _failClosed(operations);
}

void VibePlaybackState::_cancelTimer(
    const VibePlaybackOperations& operations
) {
    const bool had_playback = _current != HAPTIC_OFF && _steps != nullptr;
    const bool stopped_before_expiry =
        operations.stop_timer && operations.stop_timer(operations.context);
    if (had_playback) {
        _timer_expiries.recordTimerStop(stopped_before_expiry);
    }
}

void VibePlaybackState::_failClosed(
    const VibePlaybackOperations& operations
) {
    _repeat.stop();
    _current = HAPTIC_OFF;
    _steps = nullptr;
    _step_index = 0;
    if (operations.write_motor) {
        operations.write_motor(operations.context, 0);
    }
}

bool VibePlaybackState::_armCurrent(
    const VibePlaybackOperations& operations
) {
    if (!_steps) return false;
    const VibeStep* step = &_steps[_step_index];

    if (step->duration_ms == 0) {
        if (_repeat.boundaryAction() == VibeBoundaryAction::RESTART) {
            _step_index = 0;
            step = &_steps[0];
            if (step->duration_ms == 0) {
                _failClosed(operations);
                return false;
            }
        } else {
            _failClosed(operations);
            return true;
        }
    }

    if (operations.write_motor) {
        operations.write_motor(operations.context, step->intensity);
    }
    if (!operations.start_timer_once ||
        !operations.start_timer_once(
            operations.context,
            static_cast<uint64_t>(step->duration_ms) * 1000
        )) {
        _failClosed(operations);
        return false;
    }
    return true;
}
