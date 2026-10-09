#include "reshade_detect.h"
#include <tlhelp32.h>
#include <vector>

namespace {
bool hasReShadeExports(const std::wstring& path)
{
  // Load the image without running DllMain. We only inspect the public export
  // table; the module is immediately unloaded after the query.
  HMODULE image = LoadLibraryExW(path.c_str(), nullptr,
      DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!image) {
    // LOAD_LIBRARY_AS_IMAGE_RESOURCE can prevent GetProcAddress on some
    // Windows versions, so retry with the standard non-executing mapping.
    image = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  }
  if (!image) return false;
  const bool hasRegister = GetProcAddress(image, "ReShadeRegisterAddon") != nullptr;
  const bool hasUnregister = GetProcAddress(image, "ReShadeUnregisterAddon") != nullptr;
  FreeLibrary(image);
  return hasRegister && hasUnregister;
}
}

ReShadeDetection detectReShadeInProcess(DWORD processId)
{
  ReShadeDetection result;
  if (!processId) return result;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                             processId);
  if (snapshot == INVALID_HANDLE_VALUE) return result;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      // Skip NRLive itself if callers ever query their own process.
      if (entry.hModule == GetModuleHandleW(nullptr)) continue;
      if (hasReShadeExports(entry.szExePath)) {
        result.hooked = true;
        result.modulePath = entry.szExePath;
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}
