#pragma once

#include <stddef.h>

enum HelgaAnim {
    HELGA_IDLE,
    HELGA_IDLE2,
    HELGA_IDLE3,
    HELGA_IDLE4,
    HELGA_IDLE5,
    HELGA_SIT,
    HELGA_WALK,
    HELGA_PARTY,
    HELGA_DANCE,
    HELGA_SNIFF,
    HELGA_ALERT,
    HELGA_BRUSH,
    HELGA_SLEEP,
    HELGA__COUNT
};

struct HelgaAnimationInfo {
    HelgaAnim animation;
    const char* command_name;
    const char* display_name;
};

constexpr size_t HELGA_ANIMATION_COUNT = static_cast<size_t>(HELGA__COUNT);

const HelgaAnimationInfo* helgaAnimationAt(size_t index);
const HelgaAnimationInfo* helgaAnimationInfo(HelgaAnim animation);

// Stable serial-console names retained for `helga list` and `helga play`.
const char* helgaAnimName(HelgaAnim animation);
HelgaAnim helgaAnimByName(const char* name);
