#pragma once
#include <windows.h>
#include "frame_limiter_math.h"

class FrameLimiter {
public:
  FrameLimiter();
  ~FrameLimiter();
  FrameLimiter(const FrameLimiter&) = delete;
  FrameLimiter& operator=(const FrameLimiter&) = delete;

  // Method controls where the caller invokes wait(): early before capture,
  // late immediately before Present. Both share the same phase-stable clock.
  void configure(bool enabled, int fps, int method);
  void wait();
  bool enabled() const { return m_enabled; }
  int targetFps() const { return m_fps; }
  int method() const { return m_method; }

private:
  LARGE_INTEGER m_frequency{};
  LARGE_INTEGER m_deadline{};
  HANDLE m_timer = nullptr;
  int m_fps = 60;
  int m_method = 0;
  bool m_enabled = false;
  bool m_hasDeadline = false;
};
