#pragma once
#include <windows.h>
#include <string>

struct ReShadeDetection {
  bool hooked = false;
  std::wstring modulePath;
};

// Detects ReShade by its public addon exports, not by DLL filename.
// This deliberately targets the selected game's process; DLL names alone
// are unreliable because proxy loaders and renamed modules are common.
ReShadeDetection detectReShadeInProcess(DWORD processId);
