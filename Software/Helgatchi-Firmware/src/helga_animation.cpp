#include "helga_animation.h"

namespace {

const HelgaAnimationInfo ANIMATIONS[] = {
    { HELGA_IDLE,  "idle",      "Idle" },
    { HELGA_IDLE2, "fidget",    "Idle Fidget" },
    { HELGA_IDLE3, "sneeze",    "Idle Sneeze" },
    { HELGA_IDLE4, "wag",       "Idle Wag" },
    { HELGA_IDLE5, "head_tilt", "Idle Head Tilt" },
    { HELGA_SIT,   "sit",       "Sit" },
    { HELGA_WALK,  "walk",      "Walk" },
    { HELGA_PARTY, "party",     "Party" },
    { HELGA_DANCE, "dance",     "Dance" },
    { HELGA_SNIFF, "sniff",     "Sniff" },
    { HELGA_ALERT, "alert",     "Alert" },
    { HELGA_BRUSH, "brush",     "Brush" },
    { HELGA_SLEEP, "sleep",     "Sleep" },
};

static_assert(sizeof(ANIMATIONS) / sizeof(ANIMATIONS[0]) ==
                  HELGA_ANIMATION_COUNT,
              "animation registry out of sync with HelgaAnim");

char foldAscii(char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

bool equalsIgnoreCase(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) return false;
    while (*lhs && *rhs) {
        if (foldAscii(*lhs) != foldAscii(*rhs)) return false;
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

}  // namespace

const HelgaAnimationInfo* helgaAnimationAt(size_t index) {
    return index < HELGA_ANIMATION_COUNT ? &ANIMATIONS[index] : nullptr;
}

const HelgaAnimationInfo* helgaAnimationInfo(HelgaAnim animation) {
    const int index = static_cast<int>(animation);
    return index >= 0 && static_cast<size_t>(index) < HELGA_ANIMATION_COUNT
        ? &ANIMATIONS[index]
        : nullptr;
}

const char* helgaAnimName(HelgaAnim animation) {
    const HelgaAnimationInfo* info = helgaAnimationInfo(animation);
    return info ? info->command_name : "?";
}

HelgaAnim helgaAnimByName(const char* name) {
    if (!name || !*name) return HELGA__COUNT;
    for (size_t i = 0; i < HELGA_ANIMATION_COUNT; ++i) {
        if (equalsIgnoreCase(name, ANIMATIONS[i].command_name)) {
            return ANIMATIONS[i].animation;
        }
    }
    return HELGA__COUNT;
}
