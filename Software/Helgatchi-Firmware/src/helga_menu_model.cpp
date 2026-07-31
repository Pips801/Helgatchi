#include "helga_menu_model.h"

const HelgaAnimationInfo* HelgaMenuModel::commit(size_t index) {
    const HelgaAnimationInfo* info = helgaAnimationAt(index);
    if (!info) return nullptr;
    _selected_index = index;
    return info;
}
