#include <unity.h>

#include "ShutterMath.h"

using shutter::math::clampFloat;
using shutter::math::clampLong;
using shutter::math::directionSign;
using shutter::math::logicalToRaw;
using shutter::math::percentToSteps;
using shutter::math::rawToLogical;
using shutter::math::stepsToPercent;

void test_clamp_long_bounds() {
  TEST_ASSERT_EQUAL(10L, clampLong(10, 0, 100));
  TEST_ASSERT_EQUAL(0L, clampLong(-10, 0, 100));
  TEST_ASSERT_EQUAL(100L, clampLong(500, 0, 100));
}

void test_clamp_float_bounds() {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, clampFloat(1.5f, 0.0f, 2.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, clampFloat(-3.0f, 0.0f, 2.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, clampFloat(7.0f, 0.0f, 2.0f));
}

void test_direction_conversion() {
  TEST_ASSERT_EQUAL_INT(1, directionSign(false));
  TEST_ASSERT_EQUAL_INT(-1, directionSign(true));

  TEST_ASSERT_EQUAL(250L, logicalToRaw(250, false));
  TEST_ASSERT_EQUAL(-250L, logicalToRaw(250, true));

  TEST_ASSERT_EQUAL(250L, rawToLogical(250, false));
  TEST_ASSERT_EQUAL(250L, rawToLogical(-250, true));
}

void test_percent_step_conversion() {
  TEST_ASSERT_EQUAL(0L, percentToSteps(0.0f, 12000));
  TEST_ASSERT_EQUAL(6000L, percentToSteps(50.0f, 12000));
  TEST_ASSERT_EQUAL(12000L, percentToSteps(100.0f, 12000));
  TEST_ASSERT_EQUAL(12000L, percentToSteps(170.0f, 12000));

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, stepsToPercent(6000, 12000));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, stepsToPercent(0, 0));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_clamp_long_bounds);
  RUN_TEST(test_clamp_float_bounds);
  RUN_TEST(test_direction_conversion);
  RUN_TEST(test_percent_step_conversion);
  return UNITY_END();
}
