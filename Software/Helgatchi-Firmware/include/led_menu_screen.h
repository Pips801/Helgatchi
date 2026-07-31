#pragma once

#include "led_menu_model.h"

class LedMenuScreen {
public:
    void begin();
    void onDropdownClicked();
    void onScreenShown();

private:
    LedMenuModel _model;
};

extern LedMenuScreen g_led_menu_screen;
