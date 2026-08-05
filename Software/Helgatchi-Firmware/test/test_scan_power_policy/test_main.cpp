#include <unity.h>
#include "scan_power_policy.h"

void setUp() {}
void tearDown() {}

// windows_x86 Unity lacks 64-bit numeric assertion support; preserve the
// uint64_t policy contract with boolean equality checks instead.

void test_disabled_scan_has_no_window_or_timer_wake() {
    TEST_ASSERT_EQUAL_UINT8(0, scan_power_policy::enabledRadioCount(0));
    TEST_ASSERT_EQUAL_UINT32(0, scan_power_policy::windowMs(0, 4));
    TEST_ASSERT_TRUE(scan_power_policy::wakeIntervalUs(0, 30) == 0ULL);
}

void test_single_radio_preserves_one_duration_unit() {
    TEST_ASSERT_EQUAL_UINT8(1, scan_power_policy::enabledRadioCount(1));
    TEST_ASSERT_EQUAL_UINT8(1, scan_power_policy::enabledRadioCount(2));
    TEST_ASSERT_EQUAL_UINT32(4000, scan_power_policy::windowMs(1, 4));
    TEST_ASSERT_EQUAL_UINT32(4000, scan_power_policy::windowMs(2, 4));
    TEST_ASSERT_TRUE(scan_power_policy::wakeIntervalUs(1, 30) == 30000000ULL);
}

void test_two_radios_preserve_two_duration_units() {
    TEST_ASSERT_EQUAL_UINT8(2, scan_power_policy::enabledRadioCount(3));
    TEST_ASSERT_EQUAL_UINT32(8000, scan_power_policy::windowMs(3, 4));
    TEST_ASSERT_TRUE(scan_power_policy::wakeIntervalUs(3, 30) == 30000000ULL);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_scan_has_no_window_or_timer_wake);
    RUN_TEST(test_single_radio_preserves_one_duration_unit);
    RUN_TEST(test_two_radios_preserve_two_duration_units);
    return UNITY_END();
}
