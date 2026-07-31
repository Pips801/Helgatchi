#include "party_session_state.h"

namespace {

bool isPhysicalButton(EventId event_id) {
    switch (event_id) {
        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
        case EV_BTN_CENTER_SHORT:
        case EV_BTN_CENTER_LONG:
        case EV_BTN_CENTER_HOLD:
            return true;
        default:
            return false;
    }
}

}  // namespace

void PartyCooldownState::arm(uint32_t now_ms) {
    _armed = true;
    _started_ms = now_ms;
}

void PartyCooldownState::clear() {
    _armed = false;
    _started_ms = 0;
}

bool PartyCooldownState::active(uint32_t now_ms, uint32_t duration_ms) const {
    return _armed && (uint32_t)(now_ms - _started_ms) < duration_ms;
}

void PartySessionState::startTimed(uint32_t now_ms, uint32_t duration_ms) {
    if (menu()) return;
    _mode = PartySessionMode::TIMED;
    _started_ms = now_ms;
    _duration_ms = duration_ms;
}

void PartySessionState::startMenu() {
    _mode = PartySessionMode::MENU;
    _started_ms = 0;
    _duration_ms = 0;
}

bool PartySessionState::stop() {
    if (!active()) return false;
    _clear();
    return true;
}

bool PartySessionState::consumeMenuExitButton(EventId event_id) {
    if (!menu() || !isPhysicalButton(event_id)) return false;
    if (event_id == EV_BTN_CENTER_LONG) _suppress_center_hold = true;
    _clear();
    return true;
}

bool PartySessionState::consumeMenuExitFollowup(EventId event_id) {
    if (!_suppress_center_hold) return false;
    if (event_id == EV_BTN_CENTER_HOLD) {
        _suppress_center_hold = false;
        return true;
    }
    if (event_id == EV_BTN_CENTER_SHORT || event_id == EV_BTN_CENTER_LONG) {
        _suppress_center_hold = false;
    }
    return false;
}

bool PartySessionState::expired(uint32_t now_ms) const {
    return timed() && (uint32_t)(now_ms - _started_ms) >= _duration_ms;
}

uint32_t PartySessionState::remainingMs(uint32_t now_ms) const {
    if (!timed()) return 0;
    const uint32_t elapsed = now_ms - _started_ms;
    return elapsed >= _duration_ms ? 0 : _duration_ms - elapsed;
}

void PartySessionState::_clear() {
    _mode = PartySessionMode::INACTIVE;
    _started_ms = 0;
    _duration_ms = 0;
}
