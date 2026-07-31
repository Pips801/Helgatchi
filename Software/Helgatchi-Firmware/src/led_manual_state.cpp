#include "led_manual_state.h"

bool LedManualState::set(LedPatternId pattern, uint32_t phase_start_ms) {
    if (!ledPatternInfo(pattern)) return false;
    _pattern = pattern;
    _phase_start_ms = phase_start_ms;
    _active = true;
    return true;
}

bool LedManualState::clear() {
    if (!_active) return false;
    _active = false;
    return true;
}

LedRenderSource LedManualState::renderSource(bool broadcast,
                                             bool hunt,
                                             bool alert) const {
    if (broadcast) return LED_RENDER_BROADCAST;
    if (hunt) return LED_RENDER_HUNT;
    if (alert) return LED_RENDER_ALERT;
    if (_active) return LED_RENDER_MANUAL;
    return LED_RENDER_AMBIENT;
}
