#include "frametrace.h"
#include <filesystem>
#include <iomanip>

bool FrameTrace::open(const std::wstring& path)
{
  m_file.open(std::filesystem::path(path), std::ios::out | std::ios::trunc);
  if (!m_file.is_open()) return false;
  m_file << "frame,capture_interval_ms,acquire_cpu_ms,render_present_cpu_ms,render_width,render_height,display_width,display_height,motion_mode,fsr_used\n";
  m_file << std::fixed << std::setprecision(4);
  return true;
}

void FrameTrace::record(uint64_t frame, double loopMs, double acquireMs, double renderCpuMs,
                        Size render, Size display, const std::wstring& motionMode, bool fsrUsed)
{
  if (!m_file.is_open()) return;
  m_file << frame << ',' << loopMs << ',' << acquireMs << ',' << renderCpuMs << ','
         << render.w << ',' << render.h << ',' << display.w << ',' << display.h << ',';
  for (wchar_t c : motionMode) {
    const char ch = c >= 0x20 && c <= 0x7e ? static_cast<char>(c) : '?';
    m_file.put(ch);
  }
  m_file << ',' << (fsrUsed ? 1 : 0) << '\n';
  if ((frame % 120) == 0) m_file.flush();
}
