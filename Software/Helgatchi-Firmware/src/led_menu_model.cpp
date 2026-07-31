#include "led_menu_model.h"

bool LedMenuModel::commit(size_t index, LedMenuChoice& choice_out) {
    if (index >= LED_MENU_OPTION_COUNT) return false;

    if (index == 0) {
        choice_out.automatic = true;
        choice_out.pattern = LED_PATTERN_OFF;
    } else {
        const LedPatternInfo* info = ledPatternAt(index - 1);
        if (!info) return false;
        choice_out.automatic = false;
        choice_out.pattern = info->pattern;
    }

    _selected_index = index;
    return true;
}
