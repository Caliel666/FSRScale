#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"

struct OverlayHudConfig {
  bool fps = true;
  bool frametime = true;
  bool resolution = true;
  bool background = true;
  int fontSize = 24;
  int position = 0; // 0 TL, 1 TR, 2 BL, 3 BR
  float backgroundAlpha = 0.72f;
};

bool overlayInit(HINSTANCE inst, HWND output);
void overlayShutdown();
void overlaySetOpen(bool open);
void overlayUpdate(float fps, float frametimeMs, Size capture, Size output);
void overlaySetFsrEnabled(bool enabled);
bool overlayConsumeFsrToggle();
bool overlayConsumeScreenshot();
const OverlayHudConfig& overlayConfig();
