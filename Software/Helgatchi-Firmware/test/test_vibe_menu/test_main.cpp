#include <unity.h>
#include "vibe_pattern.h"
#include "vibe_repeat_state.h"
#include "vibe_menu_model.h"

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

struct FakeVibeOperations {
    FakeVibeOperations()
        : start_count(0), stop_count(0), motor_write_count(0),
          stop_result(true) {
        for (size_t i = 0; i < 4; ++i) start_results[i] = true;
        for (size_t i = 0; i < 8; ++i) motor_writes[i] = 0;
    }

    static bool startTimerOnce(void* context, uint64_t) {
        FakeVibeOperations* fake =
            static_cast<FakeVibeOperations*>(context);
        const size_t index = fake->start_count++;
        return index < 4 ? fake->start_results[index] : true;
    }

    static bool stopTimer(void* context) {
        FakeVibeOperations* fake =
            static_cast<FakeVibeOperations*>(context);
        ++fake->stop_count;
        return fake->stop_result;
    }

    static void writeMotor(void* context, uint8_t intensity) {
        FakeVibeOperations* fake =
            static_cast<FakeVibeOperations*>(context);
        if (fake->motor_write_count < 8) {
            fake->motor_writes[fake->motor_write_count] = intensity;
        }
        ++fake->motor_write_count;
    }

    VibePlaybackOperations operations() {
        const VibePlaybackOperations value = {
            this,
            &FakeVibeOperations::startTimerOnce,
            &FakeVibeOperations::stopTimer,
            &FakeVibeOperations::writeMotor,
        };
        return value;
    }

    bool start_results[4];
    size_t start_count;
    size_t stop_count;
    uint8_t motor_writes[8];
    size_t motor_write_count;
    bool stop_result;
};

const VibeStep TEST_SINGLE_STEP[] = {
    { 255, 35 },
    { 0, 0 },
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

void test_repeating_initial_timer_arm_failure_fails_closed() {
    FakeVibeOperations fake;
    fake.start_results[0] = false;
    VibePlaybackState state;

    TEST_ASSERT_FALSE(state.playRepeating(
        HAPTIC_BUMP, TEST_SINGLE_STEP, true, fake.operations()
    ));

    TEST_ASSERT_EQUAL_UINT32(1, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(2, fake.motor_write_count);
    TEST_ASSERT_EQUAL_UINT8(255, fake.motor_writes[0]);
    TEST_ASSERT_EQUAL_UINT8(0, fake.motor_writes[1]);
    TEST_ASSERT_FALSE(state.repeating());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.repeatingPattern());
    const size_t start_count = fake.start_count;
    const size_t motor_write_count = fake.motor_write_count;
    TEST_ASSERT_TRUE(state.onTimerExpired(fake.operations()));
    TEST_ASSERT_EQUAL_UINT32(start_count, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(motor_write_count, fake.motor_write_count);
}

void test_repeating_boundary_timer_rearm_failure_fails_closed() {
    FakeVibeOperations fake;
    fake.start_results[0] = true;
    fake.start_results[1] = false;
    VibePlaybackState state;

    TEST_ASSERT_TRUE(state.playRepeating(
        HAPTIC_BUMP, TEST_SINGLE_STEP, true, fake.operations()
    ));
    TEST_ASSERT_TRUE(state.repeating());
    TEST_ASSERT_FALSE(state.onTimerExpired(fake.operations()));

    TEST_ASSERT_EQUAL_UINT32(2, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(3, fake.motor_write_count);
    TEST_ASSERT_EQUAL_UINT8(255, fake.motor_writes[0]);
    TEST_ASSERT_EQUAL_UINT8(255, fake.motor_writes[1]);
    TEST_ASSERT_EQUAL_UINT8(0, fake.motor_writes[2]);
    TEST_ASSERT_FALSE(state.repeating());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.repeatingPattern());
    const size_t start_count = fake.start_count;
    const size_t motor_write_count = fake.motor_write_count;
    TEST_ASSERT_TRUE(state.onTimerExpired(fake.operations()));
    TEST_ASSERT_EQUAL_UINT32(start_count, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(motor_write_count, fake.motor_write_count);
}

void test_incidental_off_cannot_bypass_repeat_ownership() {
    FakeVibeOperations fake;
    VibePlaybackState state;

    TEST_ASSERT_TRUE(state.playRepeating(
        HAPTIC_BUMP, TEST_SINGLE_STEP, true, fake.operations()
    ));
    const size_t start_count = fake.start_count;
    const size_t stop_count = fake.stop_count;
    const size_t motor_write_count = fake.motor_write_count;

    state.play(HAPTIC_OFF, nullptr, fake.operations());

    TEST_ASSERT_TRUE(state.repeating());
    TEST_ASSERT_FALSE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_BUMP, state.repeatingPattern());
    TEST_ASSERT_EQUAL_UINT32(start_count, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(stop_count, fake.stop_count);
    TEST_ASSERT_EQUAL_UINT32(motor_write_count, fake.motor_write_count);
}

void test_incidental_off_preserves_inactive_one_shot_stop_behavior() {
    FakeVibeOperations fake;
    VibePlaybackState state;

    TEST_ASSERT_TRUE(state.play(
        HAPTIC_BUMP, TEST_SINGLE_STEP, fake.operations()
    ));

    state.play(HAPTIC_OFF, nullptr, fake.operations());

    TEST_ASSERT_FALSE(state.repeating());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.repeatingPattern());
    TEST_ASSERT_EQUAL_UINT8(0, fake.motor_writes[fake.motor_write_count - 1]);
    const size_t start_count = fake.start_count;
    const size_t motor_write_count = fake.motor_write_count;
    TEST_ASSERT_TRUE(state.onTimerExpired(fake.operations()));
    TEST_ASSERT_EQUAL_UINT32(start_count, fake.start_count);
    TEST_ASSERT_EQUAL_UINT32(motor_write_count, fake.motor_write_count);
}

void test_explicit_stop_still_ends_active_repeat() {
    FakeVibeOperations fake;
    VibePlaybackState state;

    TEST_ASSERT_TRUE(state.playRepeating(
        HAPTIC_BUMP, TEST_SINGLE_STEP, true, fake.operations()
    ));
    state.stop(fake.operations());

    TEST_ASSERT_FALSE(state.repeating());
    TEST_ASSERT_TRUE(state.acceptsOneShot());
    TEST_ASSERT_EQUAL_INT(HAPTIC_OFF, state.repeatingPattern());
    TEST_ASSERT_EQUAL_UINT8(0, fake.motor_writes[fake.motor_write_count - 1]);
}

void test_vibe_menu_maps_only_active_patterns_and_retains_valid_commit() {
    const HapticPatternId expected[] = {
        HAPTIC_TICK_LIGHT, HAPTIC_TICK, HAPTIC_BUMP,
        HAPTIC_DOUBLE_TAP, HAPTIC_LONG_BUZZ,
    };
    VibeMenuModel model;
    TEST_ASSERT_EQUAL_UINT32(5, VIBE_MENU_OPTION_COUNT);
    TEST_ASSERT_EQUAL_UINT32(0, model.selectedIndex());
    for (size_t i = 0; i < VIBE_MENU_OPTION_COUNT; ++i) {
        const VibePatternInfo* mapped = vibeMenuPatternAt(i);
        TEST_ASSERT_NOT_NULL(mapped);
        TEST_ASSERT_EQUAL_INT(expected[i], mapped->pattern);
        const VibePatternInfo* committed = model.commit(i);
        TEST_ASSERT_EQUAL_PTR(mapped, committed);
        TEST_ASSERT_EQUAL_UINT32(i, model.selectedIndex());
    }
    const VibePatternInfo* same = model.commit(4);
    TEST_ASSERT_EQUAL_INT(HAPTIC_LONG_BUZZ, same->pattern);
    TEST_ASSERT_EQUAL_UINT32(4, model.selectedIndex());
    TEST_ASSERT_NULL(vibeMenuPatternAt(VIBE_MENU_OPTION_COUNT));
    TEST_ASSERT_NULL(model.commit(VIBE_MENU_OPTION_COUNT));
    TEST_ASSERT_EQUAL_UINT32(4, model.selectedIndex());
}

void test_vibe_menu_left_and_right_switch_and_wrap() {
    VibeMenuModel model;
    VibeMenuDecision decision = model.handleButton(EV_BTN_LEFT, true);
    TEST_ASSERT_TRUE(decision.consumed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VibeMenuAction::PLAY_SELECTED), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_INT(HAPTIC_LONG_BUZZ, decision.pattern);
    TEST_ASSERT_EQUAL_UINT32(4, model.selectedIndex());
    decision = model.handleButton(EV_BTN_RIGHT, true);
    TEST_ASSERT_TRUE(decision.consumed);
    TEST_ASSERT_EQUAL_INT(HAPTIC_TICK_LIGHT, decision.pattern);
    TEST_ASSERT_EQUAL_UINT32(0, model.selectedIndex());
    TEST_ASSERT_NOT_NULL(model.commit(2));
    decision = model.handleButton(EV_BTN_RIGHT, true);
    TEST_ASSERT_EQUAL_INT(HAPTIC_DOUBLE_TAP, decision.pattern);
    TEST_ASSERT_EQUAL_UINT32(3, model.selectedIndex());
    decision = model.handleButton(EV_BTN_LEFT, true);
    TEST_ASSERT_EQUAL_INT(HAPTIC_BUMP, decision.pattern);
    TEST_ASSERT_EQUAL_UINT32(2, model.selectedIndex());
}

void test_vibe_menu_center_short_is_noop_and_long_stops_with_paired_hold() {
    VibeMenuModel model;
    VibeMenuDecision decision = model.handleButton(EV_BTN_CENTER_SHORT, true);
    TEST_ASSERT_TRUE(decision.consumed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VibeMenuAction::NONE), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_UINT32(0, model.selectedIndex());
    decision = model.handleButton(EV_BTN_CENTER_LONG, true);
    TEST_ASSERT_TRUE(decision.consumed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VibeMenuAction::STOP), static_cast<int>(decision.action));
    decision = model.handleButton(EV_TICK_1S, false);
    TEST_ASSERT_FALSE(decision.consumed);
    decision = model.handleButton(EV_BTN_CENTER_HOLD, false);
    TEST_ASSERT_TRUE(decision.consumed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VibeMenuAction::NONE), static_cast<int>(decision.action));
    decision = model.handleButton(EV_BTN_CENTER_HOLD, false);
    TEST_ASSERT_FALSE(decision.consumed);
}

void test_vibe_menu_independent_button_clears_stale_hold_guard() {
    const EventId independent[] = { EV_BTN_LEFT, EV_BTN_RIGHT, EV_BTN_CENTER_SHORT, EV_BTN_CENTER_LONG };
    for (size_t i = 0; i < sizeof(independent) / sizeof(independent[0]); ++i) {
        VibeMenuModel model;
        TEST_ASSERT_TRUE(model.handleButton(EV_BTN_CENTER_LONG, true).consumed);
        VibeMenuDecision decision = model.handleButton(independent[i], false);
        TEST_ASSERT_FALSE(decision.consumed);
        TEST_ASSERT_FALSE(model.handleButton(EV_BTN_CENTER_HOLD, false).consumed);
    }
}

void test_vibe_menu_direct_hold_stops_but_inactive_buttons_pass_through() {
    VibeMenuModel model;
    const VibeMenuDecision hold = model.handleButton(EV_BTN_CENTER_HOLD, true);
    TEST_ASSERT_TRUE(hold.consumed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VibeMenuAction::STOP), static_cast<int>(hold.action));
    const EventId inactive[] = { EV_BTN_LEFT, EV_BTN_RIGHT, EV_BTN_CENTER_SHORT, EV_BTN_CENTER_LONG, EV_BTN_CENTER_HOLD };
    for (size_t i = 0; i < sizeof(inactive) / sizeof(inactive[0]); ++i) {
        TEST_ASSERT_FALSE(model.handleButton(inactive[i], false).consumed);
    }
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
    RUN_TEST(test_repeating_initial_timer_arm_failure_fails_closed);
    RUN_TEST(test_repeating_boundary_timer_rearm_failure_fails_closed);
    RUN_TEST(test_incidental_off_cannot_bypass_repeat_ownership);
    RUN_TEST(test_incidental_off_preserves_inactive_one_shot_stop_behavior);
    RUN_TEST(test_explicit_stop_still_ends_active_repeat);
    RUN_TEST(test_vibe_menu_maps_only_active_patterns_and_retains_valid_commit);
    RUN_TEST(test_vibe_menu_left_and_right_switch_and_wrap);
    RUN_TEST(test_vibe_menu_center_short_is_noop_and_long_stops_with_paired_hold);
    RUN_TEST(test_vibe_menu_independent_button_clears_stale_hold_guard);
    RUN_TEST(test_vibe_menu_direct_hold_stops_but_inactive_buttons_pass_through);
    return UNITY_END();
}
