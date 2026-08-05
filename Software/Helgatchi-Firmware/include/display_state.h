#pragma once

#include <stdint.h>

enum class DisplayState : uint8_t {
    UNINITIALIZED,
    OFF,
    ON,
    DIM,
};

class DisplayStateTracker {
public:
    bool transitionTo(DisplayState next) {
        if (_current == next) return false;
        _current = next;
        return true;
    }

    DisplayState current() const { return _current; }

private:
    DisplayState _current = DisplayState::UNINITIALIZED;
};
