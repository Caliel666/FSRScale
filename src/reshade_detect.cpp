#include "reshade_detect.h"
#include <windows.h>
#include <filesystem>

namespace {
std::filesystem::path executableDirectory()
{
  wchar_t path[MAX_PATH * 4]{};
  const DWORD capacity = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
  const DWORD length = GetModuleFileNameW(nullptr, path, capacity);
  if (length == 0 || length >= capacity) return {};
  return std::filesystem::path(path).parent_path();
}
}

ReShadeDetection detectReShadeMode()
{
  ReShadeDetection result;
  const auto directory = executableDirectory();
  if (directory.empty()) return result;

  // amd-nr.addon64 is a specific opt-in marker for the NRLive/ReShade route.
  // ReShade.ini is accepted as the general local ReShade configuration marker.
  const std::filesystem::path markers[] = {
    directory / L"amd-nr.addon64",
    directory / L"ReShade.ini"
  };
  for (const auto& marker : markers) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(marker, ec) && !ec) {
      result.enabled = true;
      result.markerPath = marker.wstring();
      return result;
    }
  }
  return result;
}
