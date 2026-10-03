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

void setCaptureTarget(HWND target);
void setScaleSizes(Size capture, Size output);
void drawCursor();
void setBindBypassVks(const UINT* vks, size_t count);

// Steam-style overlay: HUD + mouse not forwarded to game + cursor clipped
// to the presentation (game view) rectangle.
void setOverlayOpen(bool open);
bool isOverlayOpen();
void setOverlayHud(HWND hud);
HWND outputHwnd(); // main presentation window
