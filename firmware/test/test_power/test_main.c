// The low-battery cutoff for the storage card (power.c).
#include <unity.h>

#include "power.h"

void setUp(void) {}
void tearDown(void) {}

static const bps_power_cfg CFG = {3400, 3550};

static void test_a_full_cell_is_not_low(void)
{
    bps_power p = {0};
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 4100));
    TEST_ASSERT_FALSE(p.low);
}

static void test_it_goes_low_under_the_cutoff(void)
{
    bps_power p = {0};
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 3400));
    TEST_ASSERT_EQUAL_INT(BPS_POWER_WENT_LOW, bps_power_update(&p, &CFG, true, 3399));
    TEST_ASSERT_TRUE(p.low);
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 3300));
}

static void test_it_recovers_only_at_the_resume_voltage(void)
{
    bps_power p = {0};
    bps_power_update(&p, &CFG, true, 3350);
    // Back over the cutoff but under the resume voltage: still low.
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 3450));
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 3549));
    TEST_ASSERT_EQUAL_INT(BPS_POWER_RECOVERED, bps_power_update(&p, &CFG, true, 3550));
    TEST_ASSERT_FALSE(p.low);
    // And goes low again only under the cutoff.
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, true, 3450));
}

static void test_no_battery_is_never_low(void)
{
    bps_power p = {0};
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &CFG, false, 0));
    bps_power_update(&p, &CFG, true, 3300);
    // The cell taken out, the board on the charger: the card may be used.
    TEST_ASSERT_EQUAL_INT(BPS_POWER_RECOVERED, bps_power_update(&p, &CFG, false, 0));
}

static void test_a_cutoff_of_zero_turns_it_off(void)
{
    const bps_power_cfg off = {0, 0};
    bps_power p = {0};
    TEST_ASSERT_EQUAL_INT(BPS_POWER_SAME, bps_power_update(&p, &off, true, 3000));
    TEST_ASSERT_FALSE(p.low);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_full_cell_is_not_low);
    RUN_TEST(test_it_goes_low_under_the_cutoff);
    RUN_TEST(test_it_recovers_only_at_the_resume_voltage);
    RUN_TEST(test_no_battery_is_never_low);
    RUN_TEST(test_a_cutoff_of_zero_turns_it_off);
    return UNITY_END();
}
