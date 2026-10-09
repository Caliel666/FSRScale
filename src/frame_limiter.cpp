#include "frame_limiter.h"
#include <algorithm>

FrameLimiter::FrameLimiter() {
  QueryPerformanceFrequency(&m_frequency);
  // High-resolution waitable timers are available on current Windows 10/11.
  // Fall back to a normal waitable timer if the flag is unsupported.
  m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
  if (!m_timer)
    m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

FrameLimiter::~FrameLimiter() {
  if (m_timer) CloseHandle(m_timer);
}

void FrameLimiter::configure(bool enabled, int fps, int method) {
  fps = frame_limit::clampFps(fps);
  method = frame_limit::normalizeMethod(method);
  if (enabled != m_enabled || fps != m_fps || method != m_method) {
    m_enabled = enabled;
    m_fps = fps;
    m_method = method;
    m_hasDeadline = false;
  }
}

void FrameLimiter::wait() {
  if (!m_enabled || m_frequency.QuadPart <= 0) return;

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const LONGLONG interval = std::max<LONGLONG>(
      1, (m_frequency.QuadPart * 1000000LL) /
             static_cast<LONGLONG>(m_fps));

  if (!m_hasDeadline) {
    m_deadline.QuadPart = now.QuadPart + interval;
    m_hasDeadline = true;
  } else {
    m_deadline.QuadPart += interval;
    // Do not attempt catch-up bursts after a slow frame or debugger pause.
    if (m_deadline.QuadPart <= now.QuadPart)
      m_deadline.QuadPart = now.QuadPart + interval;
  }

  // Sleep most of the remaining interval, then spin only for the final
  // ~0.5 ms. This trades a tiny bounded CPU tail for more repeatable pacing.
  for (;;) {
    QueryPerformanceCounter(&now);
    const LONGLONG remaining = m_deadline.QuadPart - now.QuadPart;
    if (remaining <= 0) break;

    const LONGLONG slack = std::max<LONGLONG>(1, m_frequency.QuadPart / 2000);
    if (m_timer && remaining > slack) {
      const LONGLONG sleepTicks = remaining - slack;
      LARGE_INTEGER due{};
      due.QuadPart = -std::max<LONGLONG>(
          1, (sleepTicks * 10000000LL) / m_frequency.QuadPart);
      if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(m_timer, INFINITE);
      else
        SwitchToThread();
    } else {
      YieldProcessor();
    }
  }
}
