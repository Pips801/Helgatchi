#pragma once

#include "led_pattern.h"
#include <stdint.h>

enum LedRenderSource : uint8_t {
    LED_RENDER_AMBIENT,
    LED_RENDER_MANUAL,
    LED_RENDER_ALERT,
    LED_RENDER_HUNT,
    LED_RENDER_BROADCAST,
};

class LedManualState {
public:
    bool set(LedPatternId pattern, uint32_t phase_start_ms);
    bool clear();

    bool active() const { return _active; }
    bool keepsAwake() const {
        return _active && _pattern != LED_PATTERN_OFF;
    }
    LedPatternId pattern() const { return _pattern; }
    uint32_t phaseElapsed(uint32_t now_ms) const {
        return _active ? now_ms - _phase_start_ms : 0;
    }
    LedRenderSource renderSource(bool broadcast,
                                 bool hunt,
                                 bool alert) const;

private:
    LedPatternId _pattern = LED_PATTERN_OFF;
    uint32_t _phase_start_ms = 0;
    bool _active = false;
};
