#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"

// ── MangoHud-style overlay config ─────────────────────────────────────────
struct OverlayHudConfig {
  bool fps = true;              // show FPS number
  bool frametime = true;        // show frametime ms
  bool frame_timing = true;    // show frametime graph
  bool resolution = true;      // show resolution
  bool background = true;      // show semi-transparent background
  bool text_outline = true;   // draw black outline around text
  int  fontSize = 24;          // main font size (px)
  int  position = 0;           // 0=TL 1=TR 2=BL 3=BR
  float background_alpha = 0.5f; // 0..1
  // MangoHud default colors (RRGGBB → RGB)
  COLORREF engine_color = RGB(235,91,91);    // #EB5B5B red
  COLORREF text_color = RGB(255,255,255);   // #FFFFFF white
  COLORREF frametime_color = RGB(0,255,0);  // #00FF00 green
};

// Frame pacing settings persisted in scaleconfig.ini.
struct OverlayFrameLimitConfig {
  bool enabled = false;
  int fps = 60;
  int method = 0; // 0=early/smooth, 1=late/snappy
};

// ── Overlay API ───────────────────────────────────────────────────────────
// The overlay has two parts:
//   1. Top bar (buttons: FSR / FPS / Camera / Settings) — only visible when
//      overlay mode is open.
//   2. MangoHud-style FPS overlay — persists even after overlay mode closes,
//      toggled by the FPS button.
// Both are GDI child windows of the main NRLive output window.

bool overlayInit(HINSTANCE inst, HWND output);
void overlayShutdown();
void overlaySetOpen(bool open);             // show/hide top bar + settings
void overlayUpdate(float fps, float frametimeMs, Size capture, Size output);
void overlaySetFsrEnabled(bool enabled);
bool overlayConsumeFsrToggle();             // returns true once when FSR button clicked
bool overlayConsumeScreenshot();            // returns true once when Camera button clicked
bool overlayConsumeFrameLimitToggle();       // returns true once when the CAP button is clicked
OverlayFrameLimitConfig overlayFrameLimitConfig();
bool overlayConsumeFgToggle();                // placeholder toggle only; no generation is performed
bool overlayFgEnabled();
const OverlayHudConfig& overlayConfig();
std::wstring overlayScreenshotPath();
