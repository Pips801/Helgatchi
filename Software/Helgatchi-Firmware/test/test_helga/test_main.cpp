#include <unity.h>
#include "helga_animation.h"

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

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_animation_catalog_order_names_and_lookup);
    RUN_TEST(test_animation_catalog_rejects_invalid_values);
    return UNITY_END();
}
