#include "helga_playback_state.h"

namespace {

bool isValidAnimation(HelgaAnim animation) {
    return helgaAnimationInfo(animation) != nullptr;
}

bool isExitButton(EventId event_id) {
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

HelgaPlaybackState::HelgaPlaybackState(HelgaAnim initial)
    : _automatic(isValidAnimation(initial) ? initial : HELGA_IDLE) {}

bool HelgaPlaybackState::setAutomatic(HelgaAnim animation) {
    if (!isValidAnimation(animation)) return false;
    _automatic = animation;
    return true;
}

bool HelgaPlaybackState::startManual(HelgaAnim animation) {
    if (!isValidAnimation(animation)) return false;
    _manual = animation;
    _manual_active = true;
    return true;
}

bool HelgaPlaybackState::stopManual() {
    if (!_manual_active) return false;
    _manual_active = false;
    return true;
}

bool HelgaPlaybackState::consumeExitButton(EventId event_id) {
    if (!_manual_active || !isExitButton(event_id)) return false;
    _manual_active = false;
    return true;
}
