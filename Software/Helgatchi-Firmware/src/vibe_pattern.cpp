#include "vibe_pattern.h"

namespace {

constexpr VibePatternInfo VIBE_PATTERNS[] = {
    { HAPTIC_OFF,        "off",        "Off" },
    { HAPTIC_TICK_LIGHT, "tick_light", "Tick Light" },
    { HAPTIC_TICK,       "tick",       "Tick" },
    { HAPTIC_BUMP,       "bump",       "Bump" },
    { HAPTIC_DOUBLE_TAP, "double_tap", "Double Tap" },
    { HAPTIC_LONG_BUZZ,  "long_buzz",  "Long Buzz" },
};

static_assert(sizeof(VIBE_PATTERNS) / sizeof(VIBE_PATTERNS[0]) ==
                  VIBE_PATTERN_CATALOG_COUNT,
              "Vibe catalog out of sync with HapticPatternId");

#define ASSERT_VIBE_PATTERN_SLOT(slot, id) \
    static_assert(VIBE_PATTERNS[slot].pattern == id, \
                  "Vibe catalog order out of sync with HapticPatternId")

ASSERT_VIBE_PATTERN_SLOT(0, HAPTIC_OFF);
ASSERT_VIBE_PATTERN_SLOT(1, HAPTIC_TICK_LIGHT);
ASSERT_VIBE_PATTERN_SLOT(2, HAPTIC_TICK);
ASSERT_VIBE_PATTERN_SLOT(3, HAPTIC_BUMP);
ASSERT_VIBE_PATTERN_SLOT(4, HAPTIC_DOUBLE_TAP);
ASSERT_VIBE_PATTERN_SLOT(5, HAPTIC_LONG_BUZZ);

#undef ASSERT_VIBE_PATTERN_SLOT

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

const VibePatternInfo* vibePatternAt(size_t index) {
    return index < VIBE_PATTERN_CATALOG_COUNT
        ? &VIBE_PATTERNS[index]
        : nullptr;
}

const VibePatternInfo* vibePatternInfo(HapticPatternId pattern) {
    const size_t index = static_cast<size_t>(pattern);
    return index < VIBE_PATTERN_CATALOG_COUNT
        ? &VIBE_PATTERNS[index]
        : nullptr;
}

const char* vibePatternName(HapticPatternId pattern) {
    const VibePatternInfo* info = vibePatternInfo(pattern);
    return info ? info->command_name : "?";
}

const char* vibePatternDisplayName(HapticPatternId pattern) {
    const VibePatternInfo* info = vibePatternInfo(pattern);
    return info ? info->display_name : "?";
}

HapticPatternId vibePatternByName(const char* name) {
    if (!name || !*name) return HAPTIC_PATTERN_COUNT;
    for (size_t i = 0; i < VIBE_PATTERN_CATALOG_COUNT; ++i) {
        if (equalsIgnoreCase(name, VIBE_PATTERNS[i].command_name)) {
            return VIBE_PATTERNS[i].pattern;
        }
    }
    return HAPTIC_PATTERN_COUNT;
}
