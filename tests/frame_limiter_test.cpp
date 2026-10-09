#include "frame_limiter_math.h"
#include <cstdint>
#include <iostream>

int main() {
  using namespace frame_limit;
  if (clampFps(1) != 15 || clampFps(60) != 60 || clampFps(999) != 500) {
    std::cerr << "FPS clamp invariant failed\n"; return 1;
  }
  if (intervalMicroseconds(60) != 16666 ||
      intervalMicroseconds(120) != 8333 ||
      intervalMicroseconds(30) != 33333) {
    std::cerr << "Frame interval conversion failed\n"; return 2;
  }
  if (normalizeMethod(0) != 0 || normalizeMethod(1) != 1 ||
      normalizeMethod(-1) != 0 || normalizeMethod(99) != 0) {
    std::cerr << "Limiter method normalization failed\n"; return 3;
  }
  return 0;
}
