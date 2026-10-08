#pragma once
#include <algorithm>
namespace frame_timing {
inline float clampFrameDeltaMs(double elapsedMs) noexcept { return std::clamp(static_cast<float>(elapsedMs), 1.0f, 100.0f); }
inline float fpsFromFrameDeltaMs(double elapsedMs) noexcept { return 1000.0f / clampFrameDeltaMs(elapsedMs); }
}
