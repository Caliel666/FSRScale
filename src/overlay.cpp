#include "overlay.h"
#include <windowsx.h>
#include <shlobj.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

// ============================================================================
// NRLive overlay — Steam-style top bar + MangoHud-style FPS overlay
// ============================================================================

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr int BTN_SIZE  = 56;
static constexpr int BTN_GAP   = 10;
static constexpr int BAR_PAD   = 12;
static constexpr int FPS_W     = 200;      // FPS overlay base width
static constexpr int FT_SAMPLES = 200;     // frametime graph ring buffer size
static constexpr int FT_HEIGHT  = 50;      // frametime graph height (px)
static constexpr int FT_RANGE   = 50;      // frametime graph Y range (ms)

// Design language: dark charcoal (#1A1A1D) with red brand accent (#DC2626)
// Inspired by the AMD-NR ReShade Installer design.
// Discord/Electron aesthetic — flat with subtle depth, no heavy borders.

// ── Colors ───────────────────────────────────────────────────────────────
static constexpr COLORREF C_WIN_BG    = RGB(26,26,29);     // #1A1A1D main bg
static constexpr COLORREF C_SIDEBAR   = RGB(20,20,22);     // #141416 darker
static constexpr COLORREF C_CARD      = RGB(36,36,40);      // #242428 elevated
static constexpr COLORREF C_CARD_ALT  = RGB(30,30,33);     // #1E1E21 content area
static constexpr COLORREF C_HOVER     = RGB(63,63,70);      // #3F3F46 hover bg
static constexpr COLORREF C_BORDER    = RGB(63,63,70);      // #3F3F46 borders
static constexpr COLORREF C_INPUT_BG  = RGB(38,38,44);      // #26262C input fields
static constexpr COLORREF C_BTN       = RGB(42,42,46);      // #2A2A2E ghost btn
static constexpr COLORREF C_BTN_HOT   = RGB(63,63,70);      // #3F3F46 ghost hover
static constexpr COLORREF C_RED       = RGB(220,38,38);     // #DC2626 brand red
static constexpr COLORREF C_RED_HOT    = RGB(239,68,68);     // #EF4444 hover
static constexpr COLORREF C_RED_TINT  = RGB(58,44,47);      // #3A2C2F active/red tint
static constexpr COLORREF C_GREEN     = RGB(34,197,94);     // #22C55E success
static constexpr COLORREF C_GREEN_DOT  = RGB(74,222,128);   // #4ADE80 bright green
static constexpr COLORREF C_AMBER     = RGB(245,158,11);    // #F59E0B info/warning
static constexpr COLORREF C_TEXT      = RGB(232,232,232);   // #E8E8E8 primary text
static constexpr COLORREF C_TEXT_DIM  = RGB(139,139,139);   // #8B8B8B secondary text
static constexpr COLORREF C_TEXT_MUTED= RGB(107,107,107);   // #6B6B6B tertiary

// MangoHud FPS overlay colors (unchanged — these are MangoHud defaults)
static constexpr COLORREF C_BG       = RGB(2,2,2);       // FPS near-black bg
static constexpr COLORREF C_OUTLINE  = RGB(0,0,0);       // text outline

// ── State ─────────────────────────────────────────────────────────────────
static const wchar_t* UI_CLS   = L"NRLiveOverlayUI";
static const wchar_t* FPS_CLS  = L"NRLiveMangoHud";
static const wchar_t* SET_CLS  = L"NRLiveSettings";

static HWND g_output = nullptr, g_ui = nullptr, g_fps = nullptr;
static HWND g_settings = nullptr, g_pathEdit = nullptr;
static HINSTANCE g_inst = nullptr;
static bool g_open = false, g_fsr = true, g_fpsVisible = true;
static bool g_initialized = false;
static bool g_toggleFsr = false, g_screenshot = false;
static OverlayHudConfig g_cfg{};
static std::wstring g_shotPath;

// FPS metrics state
static float g_lastFps = 0, g_lastMs = 0;
static Size g_cap{}, g_out{};
static float g_ftBuf[FT_SAMPLES] = {};
static int g_ftIdx = 0;
static float g_ftMin = 999, g_ftMax = 0;

// Mouse hover state for buttons
static int g_hoverBtn = -1;

// ── Config persistence (scaleconfig.ini) ──────────────────────────────────
static std::wstring iniPath() {
  wchar_t p[MAX_PATH]{}; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p; auto n = s.find_last_of(L"\\/");
  return s.substr(0, n + 1) + L"scaleconfig.ini";
}

static void loadCfg() {
  const std::wstring ini = iniPath();
  g_fpsVisible = GetPrivateProfileIntW(L"FPS", L"enabled", 1, ini.c_str()) != 0;
  g_cfg.fps = GetPrivateProfileIntW(L"FPS", L"fps", 1, ini.c_str()) != 0;
  g_cfg.frametime = GetPrivateProfileIntW(L"FPS", L"frametime", 1, ini.c_str()) != 0;
  g_cfg.frame_timing = GetPrivateProfileIntW(L"FPS", L"frame_timing", 1, ini.c_str()) != 0;
  g_cfg.resolution = GetPrivateProfileIntW(L"FPS", L"resolution", 1, ini.c_str()) != 0;
  g_cfg.background = GetPrivateProfileIntW(L"FPS", L"background", 1, ini.c_str()) != 0;
  g_cfg.text_outline = GetPrivateProfileIntW(L"FPS", L"text_outline", 1, ini.c_str()) != 0;
  g_cfg.fontSize = (int)std::clamp<long>(GetPrivateProfileIntW(L"FPS", L"font_size", 24, ini.c_str()), 12, 48);
  g_cfg.position = (int)std::clamp<long>(GetPrivateProfileIntW(L"FPS", L"position", 0, ini.c_str()), 0, 3);
  g_cfg.background_alpha = (float)std::clamp<long>(GetPrivateProfileIntW(L"FPS", L"background_alpha", 5, ini.c_str()), 0, 10) / 10.0f;
  wchar_t path[MAX_PATH * 4]{};
  GetPrivateProfileStringW(L"General", L"screenshot_path", L"", path, MAX_PATH * 4, ini.c_str());
  if (path[0]) g_shotPath = path;
  else { SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path); g_shotPath = path; if (!g_shotPath.empty() && g_shotPath.back() != L'\\') g_shotPath += L'\\'; g_shotPath += L"NRLive"; }
}

static void saveCfg() {
  const std::wstring ini = iniPath();
  WritePrivateProfileStringW(L"FPS", L"enabled", g_fpsVisible ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"fps", g_cfg.fps ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"frametime", g_cfg.frametime ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"frame_timing", g_cfg.frame_timing ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"resolution", g_cfg.resolution ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"background", g_cfg.background ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"text_outline", g_cfg.text_outline ? L"1" : L"0", ini.c_str());
  wchar_t b[32];
  swprintf_s(b, L"%d", g_cfg.fontSize);  WritePrivateProfileStringW(L"FPS", L"font_size", b, ini.c_str());
  swprintf_s(b, L"%d", g_cfg.position);  WritePrivateProfileStringW(L"FPS", L"position", b, ini.c_str());
  swprintf_s(b, L"%d", (int)(g_cfg.background_alpha * 10)); WritePrivateProfileStringW(L"FPS", L"background_alpha", b, ini.c_str());
  WritePrivateProfileStringW(L"General", L"screenshot_path", g_shotPath.c_str(), ini.c_str());
}

// ── GDI helpers ───────────────────────────────────────────────────────────
static HFONT makeFont(int size, bool bold = false) {
  return CreateFontW(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    bold ? L"Segoe UI" : L"Consolas");
}

// Draw text with optional black outline (MangoHud text_outline style)
static void drawText(HDC dc, const wchar_t* s, int x, int y, int size,
                     COLORREF color, bool outline = true, UINT flags = DT_LEFT | DT_NOPREFIX) {
  HFONT f = makeFont(size, false);
  auto old = (HFONT)SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  RECT r{x, y, x + 400, y + size + 4};
  if (outline && g_cfg.text_outline) {
    SetTextColor(dc, C_OUTLINE);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0) continue;
        RECT ro = r; ro.left += dx; ro.top += dy;
        DrawTextW(dc, s, -1, &ro, flags);
      }
  }
  SetTextColor(dc, color);
  DrawTextW(dc, s, -1, &r, flags);
  SelectObject(dc, old);
  DeleteObject(f);
}

static void roundRect(HDC dc, RECT r, int radius, HBRUSH br) {
  HPEN oldp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
  HBRUSH oldb = (HBRUSH)SelectObject(dc, br);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  SelectObject(dc, oldb);
  SelectObject(dc, oldp);
}

// ── Button icon drawing ───────────────────────────────────────────────────
static void drawBtnIcon(HDC dc, RECT r, int kind, bool active) {
  int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  COLORREF c = active ? C_RED : C_TEXT_DIM;
  HFONT f = makeFont(11, true);
  auto old = (HFONT)SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, c);
  switch (kind) {
  case 0: { // FSR
    RECT tr{cx - 22, cy - 8, cx + 22, cy + 8};
    DrawTextW(dc, L"FSR", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    break;
  }
  case 1: { // FPS
    RECT tr{cx - 22, cy - 8, cx + 22, cy + 8};
    DrawTextW(dc, L"FPS", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    break;
  }
  case 2: { // Camera
    HPEN p = CreatePen(PS_SOLID, 2, c);
    auto op = (HPEN)SelectObject(dc, p);
    auto ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH ob2 = (HBRUSH)GetStockObject(NULL_BRUSH);
    roundRect(dc, {cx - 14, cy - 9, cx + 14, cy + 9}, 6, ob2);
    Ellipse(dc, cx - 6, cy - 5, cx + 6, cy + 6);
    MoveToEx(dc, cx - 10, cy - 9, nullptr); LineTo(dc, cx - 6, cy - 13);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
    break;
  }
  case 3: { // Settings gear
    HPEN p = CreatePen(PS_SOLID, 2, c);
    auto op = (HPEN)SelectObject(dc, p);
    auto ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 5, cy - 5, cx + 5, cy + 5);
    for (int i = 0; i < 8; ++i) {
      double a = i * 3.14159265 / 4;
      MoveToEx(dc, cx + (int)(cos(a) * 7), cy + (int)(sin(a) * 7), nullptr);
      LineTo(dc, cx + (int)(cos(a) * 12), cy + (int)(sin(a) * 12));
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
    break;
  }
  }
  SelectObject(dc, old);
  DeleteObject(f);
}

static RECT btnRect(int i) {
  return {BAR_PAD + i * (BTN_SIZE + BTN_GAP), BAR_PAD,
          BAR_PAD + i * (BTN_SIZE + BTN_GAP) + BTN_SIZE, BAR_PAD + BTN_SIZE};
}

// ── Top bar paint ─────────────────────────────────────────────────────────
static void paintTopBar(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  // Main background — dark charcoal
  HBRUSH bg = CreateSolidBrush(C_WIN_BG);
  FillRect(dc, &rc, bg); DeleteObject(bg);

  for (int i = 0; i < 4; ++i) {
    RECT b = btnRect(i);
    bool active = (i == 0 && g_fsr) || (i == 1 && g_fpsVisible) || (i == 3 && IsWindowVisible(g_settings));
    bool hot = (g_hoverBtn == i);
    // Active = red tint, hot = lighter, default = ghost button
    COLORREF btnCol = active ? C_RED_TINT : (hot ? C_BTN_HOT : C_BTN);
    HBRUSH br = CreateSolidBrush(btnCol);
    roundRect(dc, b, 6, br); DeleteObject(br);
    drawBtnIcon(dc, b, i, active);
  }
}

// ── MangoHud-style FPS overlay paint ─────────────────────────────────────
static void paintFps(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);

  // Only draw the background fill when background is enabled.
  // The window alpha (SetLayeredWindowAttributes) controls overall
  // transparency — when background is OFF we set alpha=255 and don't
  // draw any fill, so only the text pixels are visible.
  if (g_cfg.background) {
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &rc, bg); DeleteObject(bg);
  }

  int pad = 8;
  int y = pad;
  int fs = g_cfg.fontSize;
  int smFont = (int)(fs * 0.55);

  // FPS row: engine label (config color) + FPS number (config color) + suffixes
  if (g_cfg.fps) {
    wchar_t buf[64];
    // Engine label
    drawText(dc, L"NRLive", pad, y, smFont, g_cfg.engine_color, g_cfg.text_outline);
    // FPS number
    swprintf_s(buf, L"%.0f", g_lastFps);
    int labelW = (int)(smFont * 4.5);  // scale label width with font
    drawText(dc, buf, pad + labelW, y, fs, g_cfg.text_color, g_cfg.text_outline);
    // "FPS" suffix
    int numW = buf[0] ? (int)(wcslen(buf) * (fs * 0.6)) : 0;
    drawText(dc, L"FPS", pad + labelW + numW + 4, y + (fs - smFont), smFont, g_cfg.text_color, g_cfg.text_outline);
    // Frametime value
    if (g_cfg.frametime) {
      int ftX = pad + labelW + numW + (int)(smFont * 4.5);  // scale spacing
      swprintf_s(buf, L"%.1f", g_lastMs);
      drawText(dc, buf, ftX, y, fs, g_cfg.text_color, g_cfg.text_outline);
      drawText(dc, L"ms", ftX + (int)(wcslen(buf) * fs * 0.6) + 2, y + (fs - smFont), smFont, g_cfg.text_color, g_cfg.text_outline);
    }
    y += fs + 8;
  } else if (g_cfg.frametime) {
    wchar_t buf[64]; swprintf_s(buf, L"%.1f ms", g_lastMs);
    drawText(dc, buf, pad, y, smFont, g_cfg.text_color, g_cfg.text_outline);
    y += smFont + 8;
  }

  // Frametime graph
  int graphH = g_cfg.frame_timing ? FT_HEIGHT : 0;
  int graphW = rc.right - pad * 2;
  if (graphH > 0 && graphW > 10) {
    // Header: "Frametime" (config color) + min/max (config color)
    {
      wchar_t buf[80];
      drawText(dc, L"Frametime", pad, y, smFont, g_cfg.engine_color, g_cfg.text_outline);
      swprintf_s(buf, L"min: %.1fms  max: %.1fms", g_ftMin, g_ftMax);
      int hdrW = (int)(smFont * 5);
      drawText(dc, buf, pad + hdrW, y, smFont, g_cfg.text_color, g_cfg.text_outline);
      y += smFont + 4;
    }

    // Graph area — polyline in config frametime_color
    HPEN pen = CreatePen(PS_SOLID, 1, g_cfg.frametime_color);
    auto op = (HPEN)SelectObject(dc, pen);
    auto ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    bool first = true;
    for (int i = 0; i < FT_SAMPLES; ++i) {
      int idx = (g_ftIdx + i) % FT_SAMPLES;
      float v = g_ftBuf[idx];
      if (v <= 0) v = 0;
      if (v > FT_RANGE) v = FT_RANGE;
      float nv = sqrtf(v / FT_RANGE);
      int px = pad + (int)((float)i / (FT_SAMPLES - 1) * graphW);
      int py = y + graphH - (int)(nv * graphH);
      if (first) { MoveToEx(dc, px, py, nullptr); first = false; }
      else LineTo(dc, px, py);
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    y += graphH + 4;
  }

  // Resolution
  if (g_cfg.resolution) {
    wchar_t buf[64]; swprintf_s(buf, L"%ux%u → %ux%u", g_cap.w, g_cap.h, g_out.w, g_out.h);
    drawText(dc, buf, pad, y, smFont, g_cfg.text_color, g_cfg.text_outline);
  }
}

// ── Settings panel paint ──────────────────────────────────────────────────
static void paintSettings(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  HBRUSH bg = CreateSolidBrush(C_WIN_BG);
  FillRect(dc, &rc, bg); DeleteObject(bg);

  // Title bar (draggable area)
  HBRUSH titleBar = CreateSolidBrush(C_SIDEBAR);
  RECT tr{0, 0, rc.right, 48};
  FillRect(dc, &tr, titleBar); DeleteObject(titleBar);

  HFONT fTitle = makeFont(18, true);
  HFONT old = (HFONT)SelectObject(dc, fTitle);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, C_TEXT);
  RECT r1{24, 12, rc.right - 24, 42};
  DrawTextW(dc, L"NRLive Settings", -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  // Drag hint
  SetTextColor(dc, C_TEXT_MUTED);
  HFONT fHint = makeFont(10, false);
  SelectObject(dc, fHint);
  RECT rDrag{rc.right - 120, 12, rc.right - 24, 42};
  DrawTextW(dc, L"drag to move", -1, &rDrag, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old);
  DeleteObject(fTitle);
  DeleteObject(fHint);

  // Section labels with accent underline
  auto drawSection = [&](const wchar_t* title, int y) {
    HFONT f = makeFont(13, true);
    auto o = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, C_RED);
    RECT sr{24, y, rc.right - 24, y + 20};
    DrawTextW(dc, title, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // Underline
    HPEN pen = CreatePen(PS_SOLID, 1, C_CARD_ALT);
    auto op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, 24, y + 22, nullptr);
    LineTo(dc, rc.right - 24, y + 22);
    SelectObject(dc, op);
    DeleteObject(pen);
    SelectObject(dc, o);
    DeleteObject(f);
  };

  drawSection(L"FPS OVERLAY", 58);
  drawSection(L"APPEARANCE", 190);
  drawSection(L"SCREENSHOT", 310);

  // Bottom hint
  HFONT fBottom = makeFont(10, false);
  old = (HFONT)SelectObject(dc, fBottom);
  SetTextColor(dc, C_TEXT_MUTED);
  RECT r4{24, rc.bottom - 28, rc.right - 24, rc.bottom - 8};
  DrawTextW(dc, L"Saved to scaleconfig.ini", -1, &r4, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old);
  DeleteObject(fBottom);
}

// ── Window procedures ─────────────────────────────────────────────────────
static LRESULT CALLBACK uiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_PAINT: {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    paintTopBar(h, dc);
    EndPaint(h, &ps); return 0;
  }
  case WM_LBUTTONUP: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    for (int i = 0; i < 4; ++i) {
      RECT b = btnRect(i);
      if (PtInRect(&b, p)) {
        if (i == 0) { g_fsr = !g_fsr; g_toggleFsr = true; }
        else if (i == 1) {
          g_fpsVisible = !g_fpsVisible; saveCfg();
          // Immediately show/hide the FPS window — don't wait for next frame
          if (g_fps) ShowWindow(g_fps, g_fpsVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        else if (i == 2) { g_screenshot = true; }
        else { // Settings toggle
          if (IsWindowVisible(g_settings)) {
            ShowWindow(g_settings, SW_HIDE);
          } else {
            // Center settings panel on screen
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            int setW = 480, setH = 460;
            SetWindowTextW(g_pathEdit, g_shotPath.c_str());
            SetWindowPos(g_settings, HWND_TOPMOST, (sw - setW) / 2, (sh - setH) / 2,
              setW, setH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(g_settings, nullptr, TRUE);
          }
        }
        InvalidateRect(h, nullptr, FALSE);
        UpdateWindow(h);  // force immediate repaint so button state updates NOW
        return 0;
      }
    }
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    int old = g_hoverBtn; g_hoverBtn = -1;
    for (int i = 0; i < 4; ++i) { RECT b = btnRect(i); if (PtInRect(&b, p)) { g_hoverBtn = i; break; } }
    if (g_hoverBtn != old) {
      InvalidateRect(h, nullptr, FALSE);
      TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
      TrackMouseEvent(&tme);
    }
    return 0;
  }
  case WM_MOUSELEAVE: g_hoverBtn = -1; InvalidateRect(h, nullptr, FALSE); return 0;
  default: return DefWindowProcW(h, m, w, l);
  }
}

static LRESULT CALLBACK fpsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    // Double-buffer: draw to a memory DC, then BitBlt to screen.
    // This eliminates flicker caused by GDI clearing + redrawing.
    RECT rc{}; GetClientRect(h, &rc);
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, ht);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    paintFps(h, mem);
    BitBlt(dc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps); return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_ERASEBKGND) return 1; // prevent background erase (reduces flicker)
  return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    paintSettings(h, dc);
    EndPaint(h, &ps); return 0;
  }
  if (m == WM_NCHITTEST) {
    // Allow dragging by the title bar area (top 48px)
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    if (p.y < 48) return HTCAPTION;
    return HTCLIENT;
  }
  if (m == WM_COMMAND) {
    if (LOWORD(w) >= 101 && LOWORD(w) <= 105) {
      if (LOWORD(w) == 101) g_cfg.fps = (IsDlgButtonChecked(h, 101) == BST_CHECKED);
      if (LOWORD(w) == 102) g_cfg.frametime = (IsDlgButtonChecked(h, 102) == BST_CHECKED);
      if (LOWORD(w) == 103) g_cfg.frame_timing = (IsDlgButtonChecked(h, 103) == BST_CHECKED);
      if (LOWORD(w) == 104) g_cfg.resolution = (IsDlgButtonChecked(h, 104) == BST_CHECKED);
      if (LOWORD(w) == 105) g_cfg.background = (IsDlgButtonChecked(h, 105) == BST_CHECKED);
      saveCfg();
      // Update FPS window alpha + repaint
      if (g_fps) {
        int alpha = g_cfg.background ? (int)(g_cfg.background_alpha * 255) : 255;
        SetLayeredWindowAttributes(g_fps, 0, alpha, LWA_ALPHA);
        InvalidateRect(g_fps, nullptr, FALSE);
      }
    }
    // Color picker buttons
    if (LOWORD(w) >= 401 && LOWORD(w) <= 403) {
      static COLORREF cust[16] = {};
      CHOOSECOLORW cc{}; cc.lStructSize = sizeof(cc);
      cc.hwndOwner = h; cc.lpCustColors = cust;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;
      COLORREF* target = nullptr;
      if (LOWORD(w) == 401) { cc.rgbResult = g_cfg.engine_color; target = &g_cfg.engine_color; }
      if (LOWORD(w) == 402) { cc.rgbResult = g_cfg.text_color; target = &g_cfg.text_color; }
      if (LOWORD(w) == 403) { cc.rgbResult = g_cfg.frametime_color; target = &g_cfg.frametime_color; }
      if (target && ChooseColorW(&cc)) {
        *target = cc.rgbResult;
        saveCfg();
        if (g_fps) InvalidateRect(g_fps, nullptr, FALSE);
      }
    }
    if (HIWORD(w) == EN_CHANGE && LOWORD(w) == 203) {
      wchar_t p[MAX_PATH * 4]{}; GetWindowTextW(g_pathEdit, p, MAX_PATH * 4);
      g_shotPath = p; saveCfg(); return 0;
    }
    if (LOWORD(w) == 204) {
      wchar_t p[MAX_PATH * 4]{}; BROWSEINFOW bi{};
      bi.hwndOwner = h; bi.lpszTitle = L"Choose screenshot folder";
      LPITEMIDLIST id = SHBrowseForFolderW(&bi);
      if (id) { SHGetPathFromIDListW(id, p); CoTaskMemFree(id); if (p[0]) { g_shotPath = p; saveCfg(); SetWindowTextW(g_pathEdit, g_shotPath.c_str()); } }
    }
  }
  if (m == WM_HSCROLL) {
    HWND ctrl = (HWND)l;
    if (ctrl == GetDlgItem(h, 201)) {
      g_cfg.fontSize = (int)SendMessageW(ctrl, TBM_GETPOS, 0, 0);
      wchar_t b[8]; swprintf_s(b, L"%d", g_cfg.fontSize);
      SetWindowTextW(GetDlgItem(h, 301), b);
      saveCfg();
      if (g_fps) InvalidateRect(g_fps, nullptr, FALSE);
    } else if (ctrl == GetDlgItem(h, 202)) {
      int val = (int)SendMessageW(ctrl, TBM_GETPOS, 0, 0);
      g_cfg.background_alpha = (float)val / 100.0f;
      wchar_t b[8]; swprintf_s(b, L"%d%%", val);
      SetWindowTextW(GetDlgItem(h, 302), b);
      saveCfg();
      // Apply alpha to the FPS window immediately
      if (g_fps) {
        int alpha = g_cfg.background ? (int)(g_cfg.background_alpha * 255) : 255;
        SetLayeredWindowAttributes(g_fps, 0, alpha, LWA_ALPHA);
        InvalidateRect(g_fps, nullptr, FALSE);
      }
    }
  }
  return DefWindowProcW(h, m, w, l);
}

// ── FPS overlay position update ───────────────────────────────────────────
static void updateFpsPos() {
  if (!g_fps || !IsWindow(g_fps) || !g_output) return;
  // The paintFps function auto-resizes, but we need to set the initial position
  RECT o{}; GetWindowRect(g_output, &o);
  int h = 100; // initial height, paintFps will resize
  int x = o.left + 10, y = o.top + 10;
  if (g_cfg.position == 1) x = o.right - FPS_W - 10;
  if (g_cfg.position == 2) y = o.bottom - h - 10;
  if (g_cfg.position == 3) { x = o.right - FPS_W - 10; y = o.bottom - h - 10; }
  SetWindowPos(g_fps, HWND_TOPMOST, x, y, FPS_W, h,
    SWP_NOACTIVATE | (g_fpsVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

// ── Public API ────────────────────────────────────────────────────────────
bool overlayInit(HINSTANCE inst, HWND output) {
  if (g_initialized) return true;
  g_inst = inst; g_output = output; loadCfg();

  // Register classes
  WNDCLASSEXW c{sizeof(c)}; c.hInstance = inst; c.hCursor = LoadCursor(nullptr, IDC_ARROW); c.hbrBackground = nullptr;
  c.lpfnWndProc = uiProc;      c.lpszClassName = UI_CLS;  RegisterClassExW(&c);
  c.lpfnWndProc = fpsProc;     c.lpszClassName = FPS_CLS; RegisterClassExW(&c);
  c.lpfnWndProc = settingsProc; c.lpszClassName = SET_CLS; RegisterClassExW(&c);

  // Top bar (separate top-level popup, NOT a child of g_output — child
  // windows don't render correctly when the parent toggles WS_EX_LAYERED)
  int barW = BAR_PAD * 2 + 4 * BTN_SIZE + 3 * BTN_GAP;
  g_ui = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, UI_CLS, L"",
    WS_POPUP, 0, 0, barW, BAR_PAD * 2 + BTN_SIZE, nullptr, nullptr, inst, nullptr);

  // FPS overlay (layered top-level popup with per-window alpha for background)
  g_fps = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    FPS_CLS, L"", WS_POPUP, 0, 0, FPS_W, 100, nullptr, nullptr, inst, nullptr);
  if (g_fps) SetLayeredWindowAttributes(g_fps, 0, 255, LWA_ALPHA);

  // Settings panel — centered on screen, draggable
  {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int setW = 480, setH = 460;
    g_settings = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, SET_CLS, L"",
      WS_POPUP, (sw - setW) / 2, (sh - setH) / 2, setW, setH, nullptr, nullptr, inst, nullptr);
  }
  if (g_settings) {
    // FPS OVERLAY section: checkboxes
    CreateWindowW(L"BUTTON", L"FPS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 84, 80, 22, g_settings, (HMENU)101, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Frametime", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 110, 84, 100, 22, g_settings, (HMENU)102, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Graph", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 216, 84, 80, 22, g_settings, (HMENU)103, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Resolution", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 302, 84, 100, 22, g_settings, (HMENU)104, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Background", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 112, 100, 22, g_settings, (HMENU)105, inst, nullptr);

    // APPEARANCE section
    CreateWindowW(L"STATIC", L"Font size:", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 196, 70, 18, g_settings, nullptr, inst, nullptr);
    CreateWindowW(L"STATIC", L"24", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTER, 410, 196, 30, 18, g_settings, (HMENU)301, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_AUTOTICKS, 100, 192, 300, 26, g_settings, (HMENU)201, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 201), TBM_SETRANGE, TRUE, MAKELONG(12, 48));
    SendMessageW(GetDlgItem(g_settings, 201), TBM_SETPOS, TRUE, g_cfg.fontSize);

    CreateWindowW(L"STATIC", L"Bg alpha:", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 228, 70, 18, g_settings, nullptr, inst, nullptr);
    CreateWindowW(L"STATIC", L"50%", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTER, 410, 228, 30, 18, g_settings, (HMENU)302, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_AUTOTICKS, 100, 224, 300, 26, g_settings, (HMENU)202, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 202), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(GetDlgItem(g_settings, 202), TBM_SETPOS, TRUE, (int)(g_cfg.background_alpha * 100));

    // Color buttons
    CreateWindowW(L"STATIC", L"Engine:", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 260, 50, 18, g_settings, nullptr, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 256, 40, 22, g_settings, (HMENU)401, inst, nullptr);
    CreateWindowW(L"STATIC", L"Text:", WS_CHILD | WS_VISIBLE | SS_LEFT, 130, 260, 40, 18, g_settings, nullptr, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 174, 256, 40, 22, g_settings, (HMENU)402, inst, nullptr);
    CreateWindowW(L"STATIC", L"Graph:", WS_CHILD | WS_VISIBLE | SS_LEFT, 224, 260, 40, 18, g_settings, nullptr, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 268, 256, 40, 22, g_settings, (HMENU)403, inst, nullptr);

    // SCREENSHOT section
    CreateWindowW(L"STATIC", L"Folder:", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 326, 60, 18, g_settings, nullptr, inst, nullptr);
    g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_shotPath.c_str(),
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 24, 348, 340, 24, g_settings, (HMENU)203, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 374, 348, 80, 24, g_settings, (HMENU)204, inst, nullptr);

    // Set checkbox states
    CheckDlgButton(g_settings, 101, g_cfg.fps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 102, g_cfg.frametime ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 103, g_cfg.frame_timing ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 104, g_cfg.resolution ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 105, g_cfg.background ? BST_CHECKED : BST_UNCHECKED);

    ShowWindow(g_settings, SW_HIDE);
  }

  if (g_fps) {
    // Apply initial background alpha
    int alpha = g_cfg.background ? (int)(g_cfg.background_alpha * 255) : 255;
    SetLayeredWindowAttributes(g_fps, 0, alpha, LWA_ALPHA);
  }

  g_initialized = true;
  updateFpsPos();
  return true;
}

void overlayShutdown() {
  saveCfg();
  if (g_fps) DestroyWindow(g_fps);
  if (g_ui) DestroyWindow(g_ui);
  if (g_settings) DestroyWindow(g_settings);
  g_fps = g_ui = g_settings = nullptr;
  g_initialized = false;
}

void overlaySetOpen(bool open) {
  g_open = open;
  if (g_ui) {
    if (open) {
      ShowWindow(g_ui, SW_SHOWNOACTIVATE);
      RECT o{}; if (GetWindowRect(g_output, &o)) {
        int barW = BAR_PAD * 2 + 4 * BTN_SIZE + 3 * BTN_GAP;
        int screenW = o.right - o.left;
        int cx = o.left + (screenW - barW) / 2;
        SetWindowPos(g_ui, HWND_TOPMOST, cx, o.top + 16, barW, BAR_PAD * 2 + BTN_SIZE,
          SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      InvalidateRect(g_ui, nullptr, TRUE);
      UpdateWindow(g_ui);
    } else {
      ShowWindow(g_ui, SW_HIDE);
    }
  }
  // Settings panel: do NOT auto-show on overlay open.  Only show/hide
  // via the Settings button click.  But always hide on overlay close.
  if (!open && g_settings) ShowWindow(g_settings, SW_HIDE);
  updateFpsPos();
}

void overlayUpdate(float fps, float ms, Size cap, Size out) {
  g_lastFps = fps; g_lastMs = ms; g_cap = cap; g_out = out;

  // Update frametime ring buffer
  g_ftBuf[g_ftIdx] = ms;
  g_ftIdx = (g_ftIdx + 1) % FT_SAMPLES;

  // Recompute min/max (rolling window of the last FT_SAMPLES frames)
  g_ftMin = 9999; g_ftMax = 0;
  for (int i = 0; i < FT_SAMPLES; ++i) {
    if (g_ftBuf[i] > 0) {
      if (g_ftBuf[i] < g_ftMin) g_ftMin = g_ftBuf[i];
      if (g_ftBuf[i] > g_ftMax) g_ftMax = g_ftBuf[i];
    }
  }

  if (g_fps && g_fpsVisible) {
    // Dynamic width based on font size to prevent clipping
    int dynW = std::max(FPS_W, g_cfg.fontSize * 10);

    int fs = g_cfg.fontSize;
    int smFont = (int)(fs * 0.55);
    int totalH = 8;
    if (g_cfg.fps) totalH += fs + 6;
    else if (g_cfg.frametime) totalH += smFont + 6;
    if (g_cfg.frame_timing) totalH += smFont + 4 + FT_HEIGHT + 4;
    if (g_cfg.resolution) totalH += smFont + 4;
    totalH += 8;

    RECT cur{}; GetWindowRect(g_fps, &cur);
    if ((cur.bottom - cur.top) != totalH || (cur.right - cur.left) != dynW) {
      RECT o{}; GetWindowRect(g_output, &o);
      int x = o.left + 10, yp = o.top + 10;
      if (g_cfg.position == 1) x = o.right - dynW - 10;
      if (g_cfg.position == 2) yp = o.bottom - totalH - 10;
      if (g_cfg.position == 3) { x = o.right - dynW - 10; yp = o.bottom - totalH - 10; }
      SetWindowPos(g_fps, HWND_TOPMOST, x, yp, dynW, totalH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    InvalidateRect(g_fps, nullptr, FALSE);
  }
}

void overlaySetFsrEnabled(bool e) {
  g_fsr = e;
  if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
}

bool overlayConsumeFsrToggle() {
  bool v = g_toggleFsr; g_toggleFsr = false; return v;
}

bool overlayConsumeScreenshot() {
  bool v = g_screenshot; g_screenshot = false; return v;
}

const OverlayHudConfig& overlayConfig() { return g_cfg; }
std::wstring overlayScreenshotPath() { return g_shotPath; }
