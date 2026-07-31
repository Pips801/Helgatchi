#pragma once

#include <stddef.h>
#include <stdint.h>

enum LedPatternId : uint8_t {
    LED_PATTERN_OFF = 0,
    LED_PATTERN_CHARGING,
    LED_PATTERN_FULLY_CHARGED,
    LED_PATTERN_SERIAL,
    LED_PATTERN_LOW_BATTERY,
    LED_PATTERN_ALERT_DEFAULT,
    LED_PATTERN_RED_BLUE_CHASER,
    LED_PATTERN_RAINBOW_FAST,
    LED_PATTERN_RAINBOW_SLOW,
    LED_PATTERN_WHITE_CHASER,
    LED_PATTERN_ADMIN_BROADCAST,
    LED_PATTERN_COUNT,
};

struct LedPatternInfo {
    LedPatternId pattern;
    const char* command_name;
    const char* display_name;
};

constexpr size_t LED_PATTERN_CATALOG_COUNT =
    static_cast<size_t>(LED_PATTERN_COUNT);

const LedPatternInfo* ledPatternAt(size_t index);
const LedPatternInfo* ledPatternInfo(LedPatternId pattern);
const char* ledPatternName(LedPatternId pattern);
const char* ledPatternDisplayName(LedPatternId pattern);
LedPatternId ledPatternByName(const char* name);

typedef void (*LedPatternVisitor)(LedPatternId pattern,
                                  const char* command_name,
                                  void* user);
void ledPatternForEach(LedPatternVisitor fn, void* user);
