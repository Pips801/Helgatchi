#pragma once

#include <stddef.h>
#include <stdint.h>

enum HapticPatternId : uint8_t {
    HAPTIC_OFF = 0,
    HAPTIC_TICK_LIGHT,
    HAPTIC_TICK,
    HAPTIC_BUMP,
    HAPTIC_DOUBLE_TAP,
    HAPTIC_LONG_BUZZ,
    HAPTIC_PATTERN_COUNT,
};

struct VibePatternInfo {
    HapticPatternId pattern;
    const char* command_name;
    const char* display_name;
};

constexpr size_t VIBE_PATTERN_CATALOG_COUNT =
    static_cast<size_t>(HAPTIC_PATTERN_COUNT);

const VibePatternInfo* vibePatternAt(size_t index);
const VibePatternInfo* vibePatternInfo(HapticPatternId pattern);
const char* vibePatternName(HapticPatternId pattern);
const char* vibePatternDisplayName(HapticPatternId pattern);
HapticPatternId vibePatternByName(const char* name);
