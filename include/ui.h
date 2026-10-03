#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"

struct HudInfo {
  float fps = 0;
  Size capture{};
  Size output{};
  bool visible = true;
  std::wstring status;
};

HWND createOutput(HINSTANCE inst, int w, int h);
void setOutputFullscreen(HWND hwnd, HMONITOR mon);
void setOutputWindowed(HWND hwnd, int w, int h);
void setStatus(HWND hwnd, const wchar_t* text);
HWND createHud(HINSTANCE inst, HWND owner);
void updateHud(HWND hud, const HudInfo& info);

// Input redirection: which window receives mapped mouse/keyboard.
void setCaptureTarget(HWND target);
void setScaleSizes(Size capture, Size output);
