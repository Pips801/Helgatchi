#pragma once

#include "vibe_menu_model.h"

class VibeMenuScreen {
public:
    void begin();
    void onDropdownClicked();
    void onScreenShown();
    bool handleButton(EventId event_id);

private:
    VibeMenuModel _model;
};

extern VibeMenuScreen g_vibe_menu_screen;
