#include <unity.h>
#include <string.h>
#include "led_pattern.h"

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {

struct ExpectedPattern {
    LedPatternId pattern;
    const char* command_name;
    const char* display_name;
};

const ExpectedPattern EXPECTED[] = {
    { LED_PATTERN_OFF,             "off",             "Off" },
    { LED_PATTERN_CHARGING,        "charging",        "Charging" },
    { LED_PATTERN_FULLY_CHARGED,   "fully_charged",   "Fully Charged" },
    { LED_PATTERN_SERIAL,          "serial",          "Serial" },
    { LED_PATTERN_LOW_BATTERY,     "low_battery",     "Low Battery" },
    { LED_PATTERN_ALERT_DEFAULT,   "alert",           "Alert" },
    { LED_PATTERN_RED_BLUE_CHASER, "red_blue",        "Red/Blue" },
    { LED_PATTERN_RAINBOW_FAST,    "rainbow_fast",    "Rainbow Fast" },
    { LED_PATTERN_RAINBOW_SLOW,    "rainbow_slow",    "Rainbow Slow" },
    { LED_PATTERN_WHITE_CHASER,    "white_chaser",    "White Chaser" },
    { LED_PATTERN_ADMIN_BROADCAST, "admin_broadcast", "Admin Broadcast" },
};

struct VisitState {
    size_t count;
    bool order_ok;
};

void visitPattern(LedPatternId pattern, const char* name, void* user) {
    VisitState* state = static_cast<VisitState*>(user);
    if (state->count >= sizeof(EXPECTED) / sizeof(EXPECTED[0])) {
        state->order_ok = false;
        return;
    }
    const ExpectedPattern& expected = EXPECTED[state->count];
    if (pattern != expected.pattern ||
        strcmp(name, expected.command_name) != 0) {
        state->order_ok = false;
    }
    ++state->count;
}

void test_led_catalog_order_names_lookup_and_iteration() {
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(EXPECTED) / sizeof(EXPECTED[0]),
        LED_PATTERN_CATALOG_COUNT
    );

    for (size_t i = 0; i < LED_PATTERN_CATALOG_COUNT; ++i) {
        const LedPatternInfo* info = ledPatternAt(i);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(EXPECTED[i].pattern, info->pattern);
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].command_name, info->command_name);
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].display_name, info->display_name);
        TEST_ASSERT_EQUAL_PTR(info, ledPatternInfo(EXPECTED[i].pattern));
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].command_name,
                                 ledPatternName(EXPECTED[i].pattern));
        TEST_ASSERT_EQUAL_STRING(EXPECTED[i].display_name,
                                 ledPatternDisplayName(EXPECTED[i].pattern));
        TEST_ASSERT_EQUAL_INT(EXPECTED[i].pattern,
                              ledPatternByName(EXPECTED[i].command_name));
    }

    TEST_ASSERT_EQUAL_INT(LED_PATTERN_RAINBOW_FAST,
                          ledPatternByName("RaInBoW_FaSt"));

    VisitState state{0, true};
    ledPatternForEach(visitPattern, &state);
    TEST_ASSERT_TRUE(state.order_ok);
    TEST_ASSERT_EQUAL_UINT32(LED_PATTERN_CATALOG_COUNT, state.count);
}

void test_led_catalog_rejects_invalid_values() {
    TEST_ASSERT_NULL(ledPatternAt(LED_PATTERN_CATALOG_COUNT));
    TEST_ASSERT_NULL(ledPatternInfo(LED_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_STRING("?", ledPatternName(LED_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_STRING("?", ledPatternDisplayName(LED_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_COUNT, ledPatternByName(nullptr));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_COUNT, ledPatternByName(""));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_COUNT, ledPatternByName("missing"));
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_led_catalog_order_names_lookup_and_iteration);
    RUN_TEST(test_led_catalog_rejects_invalid_values);
    return UNITY_END();
}
