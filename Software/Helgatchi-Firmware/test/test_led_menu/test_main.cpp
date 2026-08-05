#include <unity.h>
#include <string.h>
#include "led_manual_state.h"
#include "led_menu_model.h"
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

void test_manual_state_starts_restarts_replaces_and_clears() {
    LedManualState state;
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_OFF, state.pattern());
    TEST_ASSERT_EQUAL_UINT32(0, state.phaseElapsed(123));

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_RAINBOW_SLOW, 100));
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_RAINBOW_SLOW, state.pattern());
    TEST_ASSERT_EQUAL_UINT32(60, state.phaseElapsed(160));

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_RAINBOW_SLOW, 200));
    TEST_ASSERT_EQUAL_UINT32(0, state.phaseElapsed(200));

    TEST_ASSERT_FALSE(state.set(LED_PATTERN_COUNT, 300));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_RAINBOW_SLOW, state.pattern());
    TEST_ASSERT_EQUAL_UINT32(100, state.phaseElapsed(300));

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_OFF, 400));
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_OFF, state.pattern());

    TEST_ASSERT_TRUE(state.clear());
    TEST_ASSERT_FALSE(state.active());
    TEST_ASSERT_FALSE(state.clear());
}

void test_manual_state_selects_layer_precedence() {
    LedManualState state;
    TEST_ASSERT_EQUAL_INT(LED_RENDER_AMBIENT,
                          state.renderSource(false, false, false));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_ALERT,
                          state.renderSource(false, false, true));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_HUNT,
                          state.renderSource(false, true, true));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_BROADCAST,
                          state.renderSource(true, true, true));

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_WHITE_CHASER, 10));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_MANUAL,
                          state.renderSource(false, false, false));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_ALERT,
                          state.renderSource(false, false, true));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_HUNT,
                          state.renderSource(false, true, true));
    TEST_ASSERT_EQUAL_INT(LED_RENDER_BROADCAST,
                          state.renderSource(true, true, true));
}

void test_manual_off_renders_without_requiring_awake_execution() {
    LedManualState state;
    TEST_ASSERT_FALSE(state.keepsAwake());

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_OFF, 100));
    TEST_ASSERT_TRUE(state.active());
    TEST_ASSERT_EQUAL_INT(LED_RENDER_MANUAL,
                          state.renderSource(false, false, false));
    TEST_ASSERT_FALSE(state.keepsAwake());

    TEST_ASSERT_TRUE(state.set(LED_PATTERN_WHITE_CHASER, 200));
    TEST_ASSERT_TRUE(state.keepsAwake());

    TEST_ASSERT_FALSE(state.set(LED_PATTERN_COUNT, 300));
    TEST_ASSERT_TRUE(state.keepsAwake());

    TEST_ASSERT_TRUE(state.clear());
    TEST_ASSERT_FALSE(state.keepsAwake());
}

void test_menu_maps_automatic_and_every_pattern() {
    LedMenuModel model;
    LedMenuChoice choice{};

    TEST_ASSERT_EQUAL_UINT32(LED_PATTERN_CATALOG_COUNT + 1,
                             LED_MENU_OPTION_COUNT);
    TEST_ASSERT_EQUAL_UINT32(0, model.selectedIndex());

    TEST_ASSERT_TRUE(model.commit(0, choice));
    TEST_ASSERT_TRUE(choice.automatic);
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_OFF, choice.pattern);

    for (size_t dropdown_index = 1;
         dropdown_index < LED_MENU_OPTION_COUNT;
         ++dropdown_index) {
        TEST_ASSERT_TRUE(model.commit(dropdown_index, choice));
        TEST_ASSERT_FALSE(choice.automatic);
        TEST_ASSERT_EQUAL_INT(
            ledPatternAt(dropdown_index - 1)->pattern,
            choice.pattern
        );
        TEST_ASSERT_EQUAL_UINT32(dropdown_index, model.selectedIndex());
    }
}

void test_menu_retains_selection_and_rejects_invalid_commit() {
    LedMenuModel model;
    LedMenuChoice choice{};

    TEST_ASSERT_TRUE(model.commit(8, choice));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_RAINBOW_FAST, choice.pattern);
    TEST_ASSERT_EQUAL_UINT32(8, model.selectedIndex());

    TEST_ASSERT_TRUE(model.commit(8, choice));
    TEST_ASSERT_EQUAL_INT(LED_PATTERN_RAINBOW_FAST, choice.pattern);
    TEST_ASSERT_EQUAL_UINT32(8, model.selectedIndex());

    TEST_ASSERT_FALSE(model.commit(LED_MENU_OPTION_COUNT, choice));
    TEST_ASSERT_EQUAL_UINT32(8, model.selectedIndex());
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_led_catalog_order_names_lookup_and_iteration);
    RUN_TEST(test_led_catalog_rejects_invalid_values);
    RUN_TEST(test_manual_state_starts_restarts_replaces_and_clears);
    RUN_TEST(test_manual_state_selects_layer_precedence);
    RUN_TEST(test_manual_off_renders_without_requiring_awake_execution);
    RUN_TEST(test_menu_maps_automatic_and_every_pattern);
    RUN_TEST(test_menu_retains_selection_and_rejects_invalid_commit);
    return UNITY_END();
}
