#include "vibe_menu_screen.h"
#include "vibe_service.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>

VibeMenuScreen g_vibe_menu_screen;

namespace {

void dropdownClicked(lv_event_t*) {
    g_vibe_menu_screen.onDropdownClicked();
}

void screenShown(lv_event_t*) {
    g_vibe_menu_screen.onScreenShown();
}

}  // namespace

void VibeMenuScreen::begin() {
    if (!objects.vibes_menu || !objects.vibe_pattern_dropdown) {
        Serial.println(
            "[vibe-menu] generated screen or dropdown is unavailable"
        );
        return;
    }

    String options;
    options.reserve(64);
    for (size_t i = 0; i < VIBE_MENU_OPTION_COUNT; ++i) {
        const VibePatternInfo* info = vibeMenuPatternAt(i);
        if (!info) {
            Serial.printf("[vibe-menu] missing catalog entry: %u\n",
                          static_cast<unsigned>(i));
            return;
        }
        if (i) options += '\n';
        options += info->display_name;
    }

    lv_dropdown_set_options(objects.vibe_pattern_dropdown,
                            options.c_str());
    lv_dropdown_set_selected(
        objects.vibe_pattern_dropdown,
        static_cast<uint32_t>(_model.selectedIndex())
    );

    lv_obj_add_event_cb(objects.vibe_pattern_dropdown,
                        dropdownClicked,
                        LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(objects.vibes_menu,
                        screenShown,
                        LV_EVENT_SCREEN_LOAD_START,
                        nullptr);
}

void VibeMenuScreen::onDropdownClicked() {
    lv_obj_t* dropdown = objects.vibe_pattern_dropdown;
    if (!dropdown || lv_dropdown_is_open(dropdown)) return;

    const size_t index = lv_dropdown_get_selected(dropdown);
    const VibePatternInfo* info = _model.commit(index);
    if (!info) {
        Serial.printf("[vibe-menu] invalid dropdown index: %u\n",
                      static_cast<unsigned>(index));
        return;
    }

    if (!g_vibe.playRepeating(info->pattern)) {
        Serial.printf("[vibe-menu] unable to repeat: %s\n",
                      info->command_name);
    }
}

void VibeMenuScreen::onScreenShown() {
    if (!objects.vibe_pattern_dropdown) return;
    lv_dropdown_set_selected(
        objects.vibe_pattern_dropdown,
        static_cast<uint32_t>(_model.selectedIndex())
    );
}

bool VibeMenuScreen::handleButton(EventId event_id) {
    const VibeMenuDecision decision =
        _model.handleButton(event_id, g_vibe.repeating());
    if (!decision.consumed) return false;

    switch (decision.action) {
        case VibeMenuAction::PLAY_SELECTED:
            if (objects.vibe_pattern_dropdown) {
                lv_dropdown_set_selected(
                    objects.vibe_pattern_dropdown,
                    static_cast<uint32_t>(_model.selectedIndex())
                );
            }
            if (!g_vibe.playRepeating(decision.pattern)) {
                Serial.printf("[vibe-menu] unable to repeat pattern: %u\n",
                              static_cast<unsigned>(decision.pattern));
            }
            break;

        case VibeMenuAction::STOP:
            g_vibe.stop();
            break;

        case VibeMenuAction::NONE:
            break;
    }

    return true;
}
