#include "helga_menu_screen.h"
#include "overview_screen.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>

HelgaMenuScreen g_helga_menu_screen;

namespace {

void dropdownClicked(lv_event_t*) {
    g_helga_menu_screen.onDropdownClicked();
}

void screenShown(lv_event_t*) {
    g_helga_menu_screen.onScreenShown();
}

}  // namespace

void HelgaMenuScreen::begin() {
    if (!objects.helga_menu || !objects.helga_animation_dropdown) {
        Serial.println("[helga-menu] generated screen or dropdown is unavailable");
        return;
    }

    String options;
    options.reserve(160);
    for (size_t i = 0; i < HELGA_ANIMATION_COUNT; ++i) {
        const HelgaAnimationInfo* info = helgaAnimationAt(i);
        if (i) options += '\n';
        options += info->display_name;
    }
    lv_dropdown_set_options(objects.helga_animation_dropdown, options.c_str());
    lv_dropdown_set_selected(objects.helga_animation_dropdown,
                             static_cast<uint32_t>(_model.selectedIndex()));

    // CLICKED runs after LVGL's RELEASED handler. The first center click opens
    // the list, so is_open remains true. The second closes it and commits even
    // when the selected index did not change.
    lv_obj_add_event_cb(objects.helga_animation_dropdown, dropdownClicked,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.helga_menu, screenShown,
                        LV_EVENT_SCREEN_LOAD_START, nullptr);
}

void HelgaMenuScreen::onDropdownClicked() {
    lv_obj_t* dropdown = objects.helga_animation_dropdown;
    if (!dropdown || lv_dropdown_is_open(dropdown)) return;

    const size_t index = lv_dropdown_get_selected(dropdown);
    const HelgaAnimationInfo* info = _model.commit(index);
    if (!info) {
        Serial.printf("[helga-menu] invalid dropdown index: %u\n",
                      static_cast<unsigned>(index));
        return;
    }

    if (!g_overview_screen.startManualPlayback(info->animation)) {
        Serial.printf("[helga-menu] unable to play: %s\n", info->command_name);
    }
}

void HelgaMenuScreen::onScreenShown() {
    if (!objects.helga_animation_dropdown) return;
    lv_dropdown_set_selected(objects.helga_animation_dropdown,
                             static_cast<uint32_t>(_model.selectedIndex()));
}
