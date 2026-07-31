#include "led_menu_screen.h"
#include "led_service.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>

LedMenuScreen g_led_menu_screen;

namespace {

void dropdownClicked(lv_event_t*) {
    g_led_menu_screen.onDropdownClicked();
}

void screenShown(lv_event_t*) {
    g_led_menu_screen.onScreenShown();
}

}  // namespace

void LedMenuScreen::begin() {
    if (!objects.led_modes_menu || !objects.led_mode_dropdown) {
        Serial.println("[led-menu] generated screen or dropdown is unavailable");
        return;
    }

    String options("Automatic");
    options.reserve(192);
    for (size_t i = 0; i < LED_PATTERN_CATALOG_COUNT; ++i) {
        const LedPatternInfo* info = ledPatternAt(i);
        if (!info) {
            Serial.printf("[led-menu] missing catalog entry: %u\n",
                          static_cast<unsigned>(i));
            return;
        }
        options += '\n';
        options += info->display_name;
    }

    lv_dropdown_set_options(objects.led_mode_dropdown, options.c_str());
    lv_dropdown_set_selected(
        objects.led_mode_dropdown,
        static_cast<uint32_t>(_model.selectedIndex())
    );

    // CLICKED runs after LVGL's RELEASED handler. The first center click opens
    // the list; the second closes and commits, including the current option.
    lv_obj_add_event_cb(objects.led_mode_dropdown,
                        dropdownClicked,
                        LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(objects.led_modes_menu,
                        screenShown,
                        LV_EVENT_SCREEN_LOAD_START,
                        nullptr);
}

void LedMenuScreen::onDropdownClicked() {
    lv_obj_t* dropdown = objects.led_mode_dropdown;
    if (!dropdown || lv_dropdown_is_open(dropdown)) return;

    const size_t index = lv_dropdown_get_selected(dropdown);
    LedMenuChoice choice{};
    if (!_model.commit(index, choice)) {
        Serial.printf("[led-menu] invalid dropdown index: %u\n",
                      static_cast<unsigned>(index));
        return;
    }

    if (choice.automatic) {
        g_leds.clearManualPattern();
        return;
    }

    if (!g_leds.setManualPattern(choice.pattern)) {
        Serial.printf("[led-menu] invalid pattern: %u\n",
                      static_cast<unsigned>(choice.pattern));
    }
}

void LedMenuScreen::onScreenShown() {
    if (!objects.led_mode_dropdown) return;
    lv_dropdown_set_selected(
        objects.led_mode_dropdown,
        static_cast<uint32_t>(_model.selectedIndex())
    );
}
