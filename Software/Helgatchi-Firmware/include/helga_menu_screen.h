#pragma once

#include "helga_menu_model.h"

class HelgaMenuScreen {
public:
    void begin();
    void onDropdownClicked();
    void onScreenShown();

private:
    HelgaMenuModel _model;
};

extern HelgaMenuScreen g_helga_menu_screen;
