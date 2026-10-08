#include "frame_timing.h"
#include <cmath>
#include <iostream>
int main() {
  const auto near = [](float a, float b) { return std::abs(a - b) < 0.05f; };
  if (!near(frame_timing::clampFrameDeltaMs(1000.0 / 60.0), 16.6667f)) { std::cerr << "60 Hz delta must be about 16.67 ms\n"; return 1; }
  if (!near(frame_timing::fpsFromFrameDeltaMs(1000.0 / 120.0), 120.0f)) { std::cerr << "120 Hz cadence must report 120 FPS\n"; return 2; }
  if (!near(frame_timing::fpsFromFrameDeltaMs(1000.0 / 30.0), 30.0f)) { std::cerr << "30 Hz cadence must report 30 FPS\n"; return 3; }
  if (!near(frame_timing::clampFrameDeltaMs(0.1), 1.0f) || !near(frame_timing::clampFrameDeltaMs(1000.0), 100.0f)) { std::cerr << "frame delta bounds are not enforced\n"; return 4; }
  return 0;
}
