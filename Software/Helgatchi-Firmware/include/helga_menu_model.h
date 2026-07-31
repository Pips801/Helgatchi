#pragma once

#include "helga_animation.h"

class HelgaMenuModel {
public:
    size_t selectedIndex() const { return _selected_index; }
    const HelgaAnimationInfo* commit(size_t index);

private:
    size_t _selected_index = 0;
};
