#include <unity.h>
#include "display_state.h"

void setUp() {}
void tearDown() {}

void test_uninitialized_state_applies_first_off_transition() {
    DisplayStateTracker state;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DisplayState::UNINITIALIZED),
                          static_cast<int>(state.current()));
    TEST_ASSERT_TRUE(state.transitionTo(DisplayState::OFF));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DisplayState::OFF),
                          static_cast<int>(state.current()));
}

void test_uninitialized_state_applies_first_on_transition() {
    DisplayStateTracker state;
    TEST_ASSERT_TRUE(state.transitionTo(DisplayState::ON));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DisplayState::ON),
                          static_cast<int>(state.current()));
}

void test_same_initialized_state_is_a_noop() {
    DisplayStateTracker state;
    TEST_ASSERT_TRUE(state.transitionTo(DisplayState::OFF));
    TEST_ASSERT_FALSE(state.transitionTo(DisplayState::OFF));
    TEST_ASSERT_TRUE(state.transitionTo(DisplayState::DIM));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_uninitialized_state_applies_first_off_transition);
    RUN_TEST(test_uninitialized_state_applies_first_on_transition);
    RUN_TEST(test_same_initialized_state_is_a_noop);
    return UNITY_END();
}
