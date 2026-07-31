#include <unity.h>
#include "helga_animation.h"
#include "helga_playback_state.h"

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {

struct ExpectedAnimation {
    HelgaAnim animation;
    const char* command_name;
    const char* display_name;
};

const ExpectedAnimation EXPECTED[] = {
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

void test_animation_catalog_order_names_and_lookup() {
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(EXPECTED) / sizeof(EXPECTED[0]),
        HELGA_ANIMATION_COUNT
    );

    for (size_t i = 0; i < HELGA_ANIMATION_COUNT; ++i) {
        const HelgaAnimationInfo* info = helgaAnimationAt(i);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(EXPECTED[i].animation, info->animation);
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].command_name, info->command_name);
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].display_name, info->display_name);
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].command_name,
                                 helgaAnimName(EXPECTED[i].animation));
        TEST_ASSERT_EQUAL_INT(EXPECTED[i].animation,
                              helgaAnimByName(EXPECTED[i].command_name));
        TEST_ASSERT_EQUAL_PTR(info, helgaAnimationInfo(EXPECTED[i].animation));
    }

    TEST_ASSERT_EQUAL_INT(HELGA_IDLE5, helgaAnimByName("HeAd_TiLt"));
}

void test_animation_catalog_rejects_invalid_values() {
    TEST_ASSERT_NULL(helgaAnimationAt(HELGA_ANIMATION_COUNT));
    TEST_ASSERT_NULL(helgaAnimationInfo(static_cast<HelgaAnim>(-1)));
    TEST_ASSERT_NULL(helgaAnimationInfo(HELGA__COUNT));
    TEST_ASSERT_EQUAL_STRING("?", helgaAnimName(static_cast<HelgaAnim>(-1)));
    TEST_ASSERT_EQUAL_STRING("?", helgaAnimName(HELGA__COUNT));
    TEST_ASSERT_EQUAL_INT(HELGA__COUNT, helgaAnimByName(nullptr));
    TEST_ASSERT_EQUAL_INT(HELGA__COUNT, helgaAnimByName(""));
    TEST_ASSERT_EQUAL_INT(HELGA__COUNT, helgaAnimByName("missing"));
}

void test_manual_playback_overrides_visible_animation_only() {
    HelgaPlaybackState state;
    TEST_ASSERT_FALSE(state.manualActive());
    TEST_ASSERT_EQUAL_INT(HELGA_IDLE, state.automaticAnimation());
    TEST_ASSERT_EQUAL_INT(HELGA_IDLE, state.visibleAnimation());

    TEST_ASSERT_TRUE(state.startManual(HELGA_WALK));
    TEST_ASSERT_TRUE(state.manualActive());
    TEST_ASSERT_EQUAL_INT(HELGA_IDLE, state.automaticAnimation());
    TEST_ASSERT_EQUAL_INT(HELGA_WALK, state.visibleAnimation());

    TEST_ASSERT_TRUE(state.setAutomatic(HELGA_ALERT));
    TEST_ASSERT_EQUAL_INT(HELGA_ALERT, state.automaticAnimation());
    TEST_ASSERT_EQUAL_INT(HELGA_WALK, state.visibleAnimation());

    TEST_ASSERT_TRUE(state.startManual(HELGA_DANCE));
    TEST_ASSERT_EQUAL_INT(HELGA_DANCE, state.visibleAnimation());

    TEST_ASSERT_TRUE(state.stopManual());
    TEST_ASSERT_FALSE(state.manualActive());
    TEST_ASSERT_EQUAL_INT(HELGA_ALERT, state.visibleAnimation());
    TEST_ASSERT_FALSE(state.stopManual());
}

void test_manual_playback_rejects_invalid_animations() {
    HelgaPlaybackState state;
    TEST_ASSERT_FALSE(state.setAutomatic(HELGA__COUNT));
    TEST_ASSERT_EQUAL_INT(HELGA_IDLE, state.automaticAnimation());

    TEST_ASSERT_TRUE(state.startManual(HELGA_SIT));
    TEST_ASSERT_FALSE(state.startManual(static_cast<HelgaAnim>(-1)));
    TEST_ASSERT_TRUE(state.manualActive());
    TEST_ASSERT_EQUAL_INT(HELGA_SIT, state.visibleAnimation());
}

void test_each_physical_button_consumes_manual_playback_once() {
    const EventId exit_events[] = {
        EV_BTN_LEFT,
        EV_BTN_RIGHT,
        EV_BTN_CENTER_SHORT,
        EV_BTN_CENTER_LONG,
        EV_BTN_CENTER_HOLD,
    };

    HelgaPlaybackState state(HELGA_SLEEP);
    TEST_ASSERT_TRUE(state.startManual(HELGA_PARTY));
    TEST_ASSERT_FALSE(state.consumeExitButton(EV_TICK_1S));
    TEST_ASSERT_TRUE(state.manualActive());

    for (size_t i = 0; i < sizeof(exit_events) / sizeof(exit_events[0]); ++i) {
        TEST_ASSERT_TRUE(state.consumeExitButton(exit_events[i]));
        TEST_ASSERT_FALSE(state.manualActive());
        TEST_ASSERT_EQUAL_INT(HELGA_SLEEP, state.visibleAnimation());
        TEST_ASSERT_FALSE(state.consumeExitButton(exit_events[i]));
        TEST_ASSERT_TRUE(state.startManual(HELGA_PARTY));
    }
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_animation_catalog_order_names_and_lookup);
    RUN_TEST(test_animation_catalog_rejects_invalid_values);
    RUN_TEST(test_manual_playback_overrides_visible_animation_only);
    RUN_TEST(test_manual_playback_rejects_invalid_animations);
    RUN_TEST(test_each_physical_button_consumes_manual_playback_once);
    return UNITY_END();
}
