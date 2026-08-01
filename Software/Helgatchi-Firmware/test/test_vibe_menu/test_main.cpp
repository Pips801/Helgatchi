#include <unity.h>
#include "vibe_pattern.h"

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {

struct ExpectedPattern {
    HapticPatternId pattern;
    const char* command_name;
    const char* display_name;
};

const ExpectedPattern EXPECTED_PATTERNS[] = {
    { HAPTIC_OFF,        "off",        "Off" },
    { HAPTIC_TICK_LIGHT, "tick_light", "Tick Light" },
    { HAPTIC_TICK,       "tick",       "Tick" },
    { HAPTIC_BUMP,       "bump",       "Bump" },
    { HAPTIC_DOUBLE_TAP, "double_tap", "Double Tap" },
    { HAPTIC_LONG_BUZZ,  "long_buzz",  "Long Buzz" },
};

void test_vibe_catalog_preserves_ids_names_and_display_labels() {
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(EXPECTED_PATTERNS) / sizeof(EXPECTED_PATTERNS[0]),
        VIBE_PATTERN_CATALOG_COUNT
    );

    for (size_t i = 0; i < VIBE_PATTERN_CATALOG_COUNT; ++i) {
        const VibePatternInfo* info = vibePatternAt(i);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(EXPECTED_PATTERNS[i].pattern, info->pattern);
        TEST_ASSERT_EQUAL_STRING(EXPECTED_PATTERNS[i].command_name,
                                 info->command_name);
        TEST_ASSERT_EQUAL_STRING(EXPECTED_PATTERNS[i].display_name,
                                 info->display_name);
        TEST_ASSERT_EQUAL_PTR(info,
                              vibePatternInfo(EXPECTED_PATTERNS[i].pattern));
        TEST_ASSERT_EQUAL_STRING(EXPECTED_PATTERNS[i].command_name,
                                 vibePatternName(EXPECTED_PATTERNS[i].pattern));
        TEST_ASSERT_EQUAL_STRING(
            EXPECTED_PATTERNS[i].display_name,
            vibePatternDisplayName(EXPECTED_PATTERNS[i].pattern)
        );
        TEST_ASSERT_EQUAL_INT(
            EXPECTED_PATTERNS[i].pattern,
            vibePatternByName(EXPECTED_PATTERNS[i].command_name)
        );
    }

    TEST_ASSERT_EQUAL_INT(HAPTIC_DOUBLE_TAP,
                          vibePatternByName("DoUbLe_TaP"));
}

void test_vibe_catalog_rejects_invalid_values_without_aliasing_off() {
    TEST_ASSERT_NULL(vibePatternAt(VIBE_PATTERN_CATALOG_COUNT));
    TEST_ASSERT_NULL(vibePatternInfo(HAPTIC_PATTERN_COUNT));
    TEST_ASSERT_NULL(vibePatternInfo(static_cast<HapticPatternId>(255)));
    TEST_ASSERT_EQUAL_STRING("?", vibePatternName(HAPTIC_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_STRING("?",
                             vibePatternDisplayName(HAPTIC_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_INT(HAPTIC_PATTERN_COUNT, vibePatternByName(nullptr));
    TEST_ASSERT_EQUAL_INT(HAPTIC_PATTERN_COUNT, vibePatternByName(""));
    TEST_ASSERT_EQUAL_INT(HAPTIC_PATTERN_COUNT,
                          vibePatternByName("missing"));
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_vibe_catalog_preserves_ids_names_and_display_labels);
    RUN_TEST(test_vibe_catalog_rejects_invalid_values_without_aliasing_off);
    return UNITY_END();
}
