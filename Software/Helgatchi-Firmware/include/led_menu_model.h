#pragma once

#include "led_pattern.h"

constexpr size_t LED_MENU_OPTION_COUNT = LED_PATTERN_CATALOG_COUNT + 1;

struct LedMenuChoice {
    bool automatic;
    LedPatternId pattern;
};

class LedMenuModel {
public:
    size_t selectedIndex() const { return _selected_index; }
    bool commit(size_t index, LedMenuChoice& choice_out);

private:
    size_t _selected_index = 0;
};
