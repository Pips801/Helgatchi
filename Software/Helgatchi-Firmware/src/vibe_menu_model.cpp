#include "vibe_menu_model.h"

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

VibeMenuDecision passThrough() {
    return { false, VibeMenuAction::NONE, HAPTIC_OFF };
}

VibeMenuDecision consume(VibeMenuAction action,
                         HapticPatternId pattern = HAPTIC_OFF) {
    return { true, action, pattern };
}

}  // namespace

const VibePatternInfo* vibeMenuPatternAt(size_t index) {
    return index < VIBE_MENU_OPTION_COUNT
        ? vibePatternAt(index + 1)
        : nullptr;
}

const VibePatternInfo* VibeMenuModel::commit(size_t index) {
    const VibePatternInfo* info = vibeMenuPatternAt(index);
    if (!info) return nullptr;
    _selected_index = index;
    return info;
}

const VibePatternInfo* VibeMenuModel::_cycle(bool next) {
    if (next) {
        _selected_index =
            (_selected_index + 1) % VIBE_MENU_OPTION_COUNT;
    } else {
        _selected_index =
            (_selected_index + VIBE_MENU_OPTION_COUNT - 1) %
            VIBE_MENU_OPTION_COUNT;
    }
    return vibeMenuPatternAt(_selected_index);
}

VibeMenuDecision VibeMenuModel::handleButton(EventId event_id,
                                              bool repeating) {
    if (_suppress_center_hold) {
        if (event_id == EV_BTN_CENTER_HOLD) {
            _suppress_center_hold = false;
            return consume(VibeMenuAction::NONE);
        }
        if (isPhysicalButton(event_id)) {
            _suppress_center_hold = false;
        }
    }

    if (!repeating) return passThrough();

    switch (event_id) {
        case EV_BTN_LEFT: {
            const VibePatternInfo* info = _cycle(false);
            return consume(VibeMenuAction::PLAY_SELECTED, info->pattern);
        }
        case EV_BTN_RIGHT: {
            const VibePatternInfo* info = _cycle(true);
            return consume(VibeMenuAction::PLAY_SELECTED, info->pattern);
        }
        case EV_BTN_CENTER_SHORT:
            return consume(VibeMenuAction::NONE);
        case EV_BTN_CENTER_LONG:
            _suppress_center_hold = true;
            return consume(VibeMenuAction::STOP);
        case EV_BTN_CENTER_HOLD:
            return consume(VibeMenuAction::STOP);
        default:
            return passThrough();
    }
}
