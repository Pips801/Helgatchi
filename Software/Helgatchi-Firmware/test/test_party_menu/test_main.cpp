#include <unity.h>
#include "party_session_state.h"

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {

void test_timed_session_refreshes_and_expires() {
    PartySessionState state;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(PartySessionMode::INACTIVE),
        static_cast<int>(state.mode())
    );
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_FALSE(state.stop());
    TEST_ASSERT_EQUAL_UINT32(0, state.remainingMs(1000));

    state.startTimed(1000, 20000);
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_TRUE(state.timed());
    TEST_ASSERT_FALSE(state.menu());
    TEST_ASSERT_EQUAL_UINT32(20000, state.remainingMs(1000));
    TEST_ASSERT_EQUAL_UINT32(15000, state.remainingMs(6000));
    TEST_ASSERT_FALSE(state.expired(6000));

    state.startTimed(6000, 3000);
    TEST_ASSERT_EQUAL_UINT32(3000, state.remainingMs(6000));
    TEST_ASSERT_EQUAL_UINT32(1, state.remainingMs(8999));
    TEST_ASSERT_TRUE(state.expired(9000));
    TEST_ASSERT_EQUAL_UINT32(0, state.remainingMs(9000));
}

void test_timed_expiry_is_millis_wrap_safe() {
    PartySessionState state;
    state.startTimed(0xFFFFFFF0u, 32u);

    TEST_ASSERT_FALSE(state.expired(0x00000008u));
    TEST_ASSERT_EQUAL_UINT32(8, state.remainingMs(0x00000008u));
    TEST_ASSERT_FALSE(state.expired(0x0000000Fu));
    TEST_ASSERT_EQUAL_UINT32(1, state.remainingMs(0x0000000Fu));
    TEST_ASSERT_TRUE(state.expired(0x00000010u));
    TEST_ASSERT_EQUAL_UINT32(0, state.remainingMs(0x00000010u));
}

void test_menu_session_has_no_deadline_and_ignores_timed_retriggers() {
    PartySessionState state;
    state.startTimed(100, 25);
    state.startMenu();

    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_TRUE(state.menu());
    TEST_ASSERT_FALSE(state.timed());
    TEST_ASSERT_FALSE(state.expired(0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0, state.remainingMs(0xFFFFFFFFu));

    state.startTimed(200, 1);
    TEST_ASSERT_TRUE(state.menu());
    TEST_ASSERT_FALSE(state.expired(1000000));

    TEST_ASSERT_TRUE(state.stop());
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_FALSE(state.stop());
}

void test_each_physical_button_consumes_menu_session_once() {
    const EventId exit_events[] = {
        EV_BTN_LEFT,
        EV_BTN_RIGHT,
        EV_BTN_CENTER_SHORT,
        EV_BTN_CENTER_LONG,
        EV_BTN_CENTER_HOLD,
    };

    PartySessionState state;
    state.startMenu();
    TEST_ASSERT_FALSE(state.consumeMenuExitButton(EV_TICK_1S));
    TEST_ASSERT_TRUE(state.menu());

    for (size_t i = 0; i < sizeof(exit_events) / sizeof(exit_events[0]); ++i) {
        TEST_ASSERT_TRUE(state.consumeMenuExitButton(exit_events[i]));
        TEST_ASSERT_FALSE(state.active());
        TEST_ASSERT_FALSE(state.consumeMenuExitButton(exit_events[i]));
        state.startMenu();
    }
}

void test_menu_center_long_suppresses_its_followup_hold_once() {
    PartySessionState state;
    state.startMenu();

    TEST_ASSERT_TRUE(state.consumeMenuExitButton(EV_BTN_CENTER_LONG));
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_TRUE(state.consumeMenuExitFollowup(EV_BTN_CENTER_HOLD));
    TEST_ASSERT_FALSE(state.consumeMenuExitFollowup(EV_BTN_CENTER_HOLD));
}

void test_distinct_center_action_clears_stale_hold_suppression() {
    const EventId distinct_center_events[] = {
        EV_BTN_CENTER_SHORT,
        EV_BTN_CENTER_LONG,
    };

    for (size_t i = 0;
         i < sizeof(distinct_center_events) / sizeof(distinct_center_events[0]);
         ++i) {
        PartySessionState state;
        state.startMenu();
        TEST_ASSERT_TRUE(state.consumeMenuExitButton(EV_BTN_CENTER_LONG));

        TEST_ASSERT_FALSE(state.consumeMenuExitFollowup(distinct_center_events[i]));
        TEST_ASSERT_FALSE(state.consumeMenuExitFollowup(EV_BTN_CENTER_HOLD));
    }
}

void test_party_cooldown_is_millis_wrap_safe() {
    PartyCooldownState cooldown;
    cooldown.arm(0xFFFFFFF0u);

    TEST_ASSERT_TRUE(cooldown.active(0xFFFFFFF0u, 300000u));
    TEST_ASSERT_TRUE(cooldown.active(0x000493CFu, 300000u));
    TEST_ASSERT_FALSE(cooldown.active(0x000493D0u, 300000u));
}

void test_party_cooldown_clear_disarms_zero_deadline_collision() {
    PartyCooldownState cooldown;
    TEST_ASSERT_FALSE(cooldown.active(0xFFFB6C20u, 300000u));

    cooldown.arm(0xFFFB6C20u);
    TEST_ASSERT_TRUE(cooldown.active(0xFFFB6C20u, 300000u));

    cooldown.clear();
    TEST_ASSERT_FALSE(cooldown.active(0xFFFB6C20u, 300000u));
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_timed_session_refreshes_and_expires);
    RUN_TEST(test_timed_expiry_is_millis_wrap_safe);
    RUN_TEST(test_menu_session_has_no_deadline_and_ignores_timed_retriggers);
    RUN_TEST(test_each_physical_button_consumes_menu_session_once);
    RUN_TEST(test_menu_center_long_suppresses_its_followup_hold_once);
    RUN_TEST(test_distinct_center_action_clears_stale_hold_suppression);
    RUN_TEST(test_party_cooldown_is_millis_wrap_safe);
    RUN_TEST(test_party_cooldown_clear_disarms_zero_deadline_collision);
    return UNITY_END();
}
