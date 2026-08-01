#include <unity.h>
#include "vibe_pattern.h"
#include "vibe_repeat_state.h"

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

void test_repeat_state_controls_one_shot_ownership_and_loop_boundary() {
    VibeRepeatState state;

    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.pattern());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(VibeBoundaryAction::COMPLETE),
        static_cast<int>(state.boundaryAction())
    );

    TEST_ASSERT_TRUE(state.start(HAPTIC_BUMP, true));
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_FALSE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_BUMP, state.pattern());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(VibeBoundaryAction::RESTART),
        static_cast<int>(state.boundaryAction())
    );

    TEST_ASSERT_TRUE(state.start(HAPTIC_LONG_BUZZ, true));
    TEST_ASSERT_EQUAL_INT(HAPTIC_LONG_BUZZ, state.pattern());
    TEST_ASSERT_TRUE(state.start(HAPTIC_LONG_BUZZ, true));
    TEST_ASSERT_EQUAL_INT(HAPTIC_LONG_BUZZ, state.pattern());

    TEST_ASSERT_TRUE(state.stop());
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.pattern());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(VibeBoundaryAction::COMPLETE),
        static_cast<int>(state.boundaryAction())
    );
    TEST_ASSERT_FALSE(state.stop());
}

void test_repeat_state_rejects_unavailable_off_and_invalid_without_disturbing_playback() {
    VibeRepeatState state;

    TEST_ASSERT_FALSE(state.start(HAPTIC_BUMP, false));
    TEST_ASSERT_FALSE(state.start(HAPTIC_OFF, true));
    TEST_ASSERT_FALSE(state.start(HAPTIC_PATTERN_COUNT, true));
    TEST_ASSERT_FALSE(state.active());

    TEST_ASSERT_TRUE(state.start(HAPTIC_DOUBLE_TAP, true));
    TEST_ASSERT_FALSE(state.start(HAPTIC_LONG_BUZZ, false));
    TEST_ASSERT_FALSE(state.start(HAPTIC_OFF, true));
    TEST_ASSERT_FALSE(state.start(HAPTIC_PATTERN_COUNT, true));
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_EQUAL_INT(HAPTIC_DOUBLE_TAP, state.pattern());
}

void test_timer_expiry_state_consumes_dispatched_old_callbacks_before_new_sequence() {
    VibeTimerExpiryState state;
    uint8_t new_step_index = 0;

    // Two old one-shot expiries were already dispatched when their active
    // sequences were restarted. Both callbacks must be consumed as stale.
    state.recordTimerStop(false);
    state.recordTimerStop(false);

    if (state.acceptsNextExpiry()) ++new_step_index;
    TEST_ASSERT_EQUAL_UINT8(0, new_step_index);
    if (state.acceptsNextExpiry()) ++new_step_index;
    TEST_ASSERT_EQUAL_UINT8(0, new_step_index);

    // The next callback belongs to the sequence currently armed at step zero.
    if (state.acceptsNextExpiry()) ++new_step_index;
    TEST_ASSERT_EQUAL_UINT8(1, new_step_index);
}

void test_timer_expiry_state_keeps_current_expiry_after_successful_stop() {
    VibeTimerExpiryState state;

    state.recordTimerStop(true);

    TEST_ASSERT_TRUE(state.acceptsNextExpiry());
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_vibe_catalog_preserves_ids_names_and_display_labels);
    RUN_TEST(test_vibe_catalog_rejects_invalid_values_without_aliasing_off);
    RUN_TEST(test_repeat_state_controls_one_shot_ownership_and_loop_boundary);
    RUN_TEST(test_repeat_state_rejects_unavailable_off_and_invalid_without_disturbing_playback);
    RUN_TEST(test_timer_expiry_state_consumes_dispatched_old_callbacks_before_new_sequence);
    RUN_TEST(test_timer_expiry_state_keeps_current_expiry_after_successful_stop);
    return UNITY_END();
}
