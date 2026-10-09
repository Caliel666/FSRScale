#pragma once
#include <cstdint>

namespace frame_limit {
constexpr int kMinFps = 15;
constexpr int kMaxFps = 500;

constexpr int clampFps(int fps) {
  return fps < kMinFps ? kMinFps : (fps > kMaxFps ? kMaxFps : fps);
}

constexpr uint64_t intervalMicroseconds(int fps) {
  return 1000000ULL / static_cast<uint64_t>(clampFps(fps));
}

constexpr int normalizeMethod(int method) {
  return method == 1 ? 1 : 0; // 0=early/smooth, 1=late/snappy
}
}
