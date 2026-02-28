#pragma once

#include <math.h>

namespace shutter {
namespace math {

inline long clampLong(long value, long minValue, long maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

inline float clampFloat(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

inline int directionSign(bool reverseDirection) {
  return reverseDirection ? -1 : 1;
}

inline long logicalToRaw(long logicalPos, bool reverseDirection) {
  return logicalPos * directionSign(reverseDirection);
}

inline long rawToLogical(long rawPos, bool reverseDirection) {
  return rawPos * directionSign(reverseDirection);
}

inline float stepsToPercent(long steps, long travelSteps) {
  if (travelSteps <= 0) return 0.0f;
  return 100.0f * static_cast<float>(steps) / static_cast<float>(travelSteps);
}

inline long percentToSteps(float percent, long travelSteps) {
  if (travelSteps <= 0) return 0;
  const float clamped = clampFloat(percent, 0.0f, 100.0f);
  return lroundf((clamped / 100.0f) * static_cast<float>(travelSteps));
}

}  // namespace math
}  // namespace shutter
