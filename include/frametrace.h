#pragma once
#include "graphics.h"
#include <fstream>
#include <string>

// Opt-in CPU-side frame trace. This intentionally measures timings around the
// capture/render/present calls, not GPU execution time; GPU timestamp queries
// will be a separate measurement so CPU and GPU latency are not conflated.
class FrameTrace {
public:
  bool open(const std::wstring& path);
  void record(uint64_t frame, double captureIntervalMs, double acquireMs, double renderCpuMs,
              Size render, Size display, const std::wstring& motionMode, bool fsrUsed);
private:
  std::ofstream m_file;
};
