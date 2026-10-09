#pragma once
#include <string>

struct ReShadeDetection {
  bool enabled = false;
  std::wstring markerPath;
};

// Select the ReShade-specific pipeline when ReShade.ini or amd-nr.addon64
// sits beside NRLive.exe. Do not inspect the selected game's loaded modules:
// ReShade may be injected into NRLiveUI.exe, and module enumeration is both
// unreliable and unrelated to which pipeline NRLive should select.
ReShadeDetection detectReShadeMode();
