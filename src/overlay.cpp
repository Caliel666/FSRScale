#include "overlay.h"
#include <windowsx.h>
#include <shlobj.h>
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

// MangoHud colors
static constexpr COLORREF C_BG       = RGB(2,2,2);       // near-black background
static constexpr COLORREF C_GPU      = RGB(46,151,98);   // #2E9762 green
static constexpr COLORREF C_CPU      = RGB(46,151,203);  // #2E97CB blue
static constexpr COLORREF C_ENGINE   = RGB(235,91,91);   // #EB5B5B red
static constexpr COLORREF C_TEXT     = RGB(255,255,255); // #FFFFFF white
static constexpr COLORREF C_FT       = RGB(0,255,0);     // #00FF00 green graph
static constexpr COLORREF C_OUTLINE  = RGB(0,0,0);       // #000000 black outline

// Steam dark theme + red highlights
static constexpr COLORREF C_BAR_BG   = RGB(24,25,31);
static constexpr COLORREF C_BTN      = RGB(38,40,48);
static constexpr COLORREF C_BTN_HOT  = RGB(48,50,60);
static constexpr COLORREF C_BTN_ACT  = RGB(55,28,31);   // red-tinted
static constexpr COLORREF C_ACCENT   = RGB(255,92,92);   // red highlight
static constexpr COLORREF C_BTN_TXT  = RGB(220,220,225);
static constexpr COLORREF C_SET_BG   = RGB(28,29,36);

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
  COLORREF c = active ? C_ACCENT : C_BTN_TXT;
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
    RoundRect(dc, cx - 14, cy - 9, cx + 14, cy + 9, 4, 4);
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
  HBRUSH bg = CreateSolidBrush(C_BAR_BG);
  FillRect(dc, &rc, bg); DeleteObject(bg);

  for (int i = 0; i < 4; ++i) {
    RECT b = btnRect(i);
    bool active = (i == 0 && g_fsr) || (i == 1 && g_fpsVisible) || (i == 3 && IsWindowVisible(g_settings));
    bool hot = (g_hoverBtn == i);
    HBRUSH br = CreateSolidBrush(active ? C_BTN_ACT : (hot ? C_BTN_HOT : C_BTN));
    roundRect(dc, b, 10, br); DeleteObject(br);
    drawBtnIcon(dc, b, i, active);
  }
}

// ── MangoHud-style FPS overlay paint ─────────────────────────────────────
static void paintFps(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);

  // Background
  if (g_cfg.background) {
    int alpha = (int)(g_cfg.background_alpha * 255);
    // Use a solid brush at near-black; true alpha blending requires
    // UpdateLayeredWindow, but for a simple overlay a dark brush works.
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &rc, bg); DeleteObject(bg);
  }

  int pad = 8;
  int y = pad;
  int fs = g_cfg.fontSize;
  int smFont = (int)(fs * 0.55);

  // FPS row: engine label (red) + FPS number (white) + "FPS" suffix (smFont)
  if (g_cfg.fps) {
    wchar_t buf[64];
    // Engine label
    drawText(dc, L"NRLive", pad, y, smFont, C_ENGINE, g_cfg.text_outline);
    // FPS number
    swprintf_s(buf, L"%.0f", g_lastFps);
    int labelW = 60;
    drawText(dc, buf, pad + labelW, y, fs, C_TEXT, g_cfg.text_outline);
    // "FPS" suffix
    int fpsW = buf[0] ? (int)wcslen(buf) * (fs * 0.6) : 0;
    drawText(dc, L"FPS", pad + labelW + fpsW + 4, y + (fs - smFont), smFont, C_TEXT, g_cfg.text_outline);
    // Frametime value
    if (g_cfg.frametime) {
      swprintf_s(buf, L"%.1f", g_lastMs);
      drawText(dc, buf, pad + labelW + fpsW + 40, y, fs, C_TEXT, g_cfg.text_outline);
      drawText(dc, L"ms", pad + labelW + fpsW + 40 + (int)(wcslen(buf) * fs * 0.6) + 2, y + (fs - smFont), smFont, C_TEXT, g_cfg.text_outline);
    }
    y += fs + 6;
  } else if (g_cfg.frametime) {
    // Just frametime without FPS
    wchar_t buf[64]; swprintf_s(buf, L"%.1f ms", g_lastMs);
    drawText(dc, buf, pad, y, smFont, C_TEXT, g_cfg.text_outline);
    y += smFont + 6;
  }

  // Frametime graph
  int graphH = g_cfg.frame_timing ? FT_HEIGHT : 0;
  int graphW = rc.right - pad * 2;
  if (graphH > 0 && graphW > 10) {
    // Header: "Frametime" (red) + min/max (white, smFont)
    {
      wchar_t buf[80];
      drawText(dc, L"Frametime", pad, y, smFont, C_ENGINE, g_cfg.text_outline);
      swprintf_s(buf, L"min: %.1fms  max: %.1fms", g_ftMin, g_ftMax);
      int hdrW = 100;
      drawText(dc, buf, pad + hdrW, y, smFont, C_TEXT, g_cfg.text_outline);
      y += smFont + 4;
    }

    // Graph area
    RECT gr{pad, y, pad + graphW, y + graphH};
    // Draw the polyline
    HPEN pen = CreatePen(PS_SOLID, 1, C_FT); // MangoHud uses 1.5 but GDI only does int
    auto op = (HPEN)SelectObject(dc, pen);
    auto ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    // No background fill for the graph (transparent, MangoHud style)
    bool first = true;
    int prevX = 0, prevY = 0;
    for (int i = 0; i < FT_SAMPLES; ++i) {
      int idx = (g_ftIdx + i) % FT_SAMPLES;
      float v = g_ftBuf[idx];
      if (v <= 0) v = 0;
      if (v > FT_RANGE) v = FT_RANGE;
      // Non-linear (sqrt) transform for more resolution at low frametimes
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
    drawText(dc, buf, pad, y, smFont, C_TEXT, g_cfg.text_outline);
  }
  // NOTE: auto-resize moved to overlayUpdate() — calling SetWindowPos
  // during WM_PAINT causes a paint loop and flickering.
}

// ── Settings panel paint ──────────────────────────────────────────────────
static void paintSettings(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  HBRUSH bg = CreateSolidBrush(C_SET_BG);
  FillRect(dc, &rc, bg); DeleteObject(bg);

  HFONT fTitle = makeFont(22, true);
  HFONT old = (HFONT)SelectObject(dc, fTitle);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(245,245,248));
  RECT r1{24, 16, rc.right - 24, 50};
  DrawTextW(dc, L"NRLive Settings", -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old);
  DeleteObject(fTitle);

  // Section labels
  HFONT fLabel = makeFont(14, true);
  old = (HFONT)SelectObject(dc, fLabel);
  SetTextColor(dc, C_ACCENT);
  RECT r2{24, 64, rc.right - 24, 84};
  DrawTextW(dc, L"FPS Overlay", -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  RECT r3{24, 180, rc.right - 24, 200};
  DrawTextW(dc, L"Screenshot Folder", -1, &r3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old);
  DeleteObject(fLabel);

  // Hint text
  HFONT fHint = makeFont(11, false);
  old = (HFONT)SelectObject(dc, fHint);
  SetTextColor(dc, RGB(135,135,145));
  RECT r4{24, rc.bottom - 40, rc.right - 24, rc.bottom - 16};
  DrawTextW(dc, L"Settings are saved to scaleconfig.ini next to NRLive.exe", -1, &r4, DT_LEFT | DT_TOP | DT_WORDBREAK);
  SelectObject(dc, old);
  DeleteObject(fHint);
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
        else if (i == 1) { g_fpsVisible = !g_fpsVisible; saveCfg(); }
        else if (i == 2) { g_screenshot = true; }
        else { // Settings toggle
          if (IsWindowVisible(g_settings)) ShowWindow(g_settings, SW_HIDE);
          else { SetWindowTextW(g_pathEdit, g_shotPath.c_str()); ShowWindow(g_settings, SW_SHOWNOACTIVATE); }
        }
        InvalidateRect(h, nullptr, FALSE);
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
  if (m == WM_COMMAND) {
    if (LOWORD(w) >= 101 && LOWORD(w) <= 105) {
      if (LOWORD(w) == 101) g_cfg.fps = (IsDlgButtonChecked(h, 101) == BST_CHECKED);
      if (LOWORD(w) == 102) g_cfg.frametime = (IsDlgButtonChecked(h, 102) == BST_CHECKED);
      if (LOWORD(w) == 103) g_cfg.frame_timing = (IsDlgButtonChecked(h, 103) == BST_CHECKED);
      if (LOWORD(w) == 104) g_cfg.resolution = (IsDlgButtonChecked(h, 104) == BST_CHECKED);
      if (LOWORD(w) == 105) g_cfg.background = (IsDlgButtonChecked(h, 105) == BST_CHECKED);
      saveCfg();
      if (g_fps) InvalidateRect(g_fps, nullptr, FALSE);
    }
    if (HIWORD(w) == EN_CHANGE && LOWORD(w) == 201) {
      wchar_t p[MAX_PATH * 4]{}; GetWindowTextW(g_pathEdit, p, MAX_PATH * 4);
      g_shotPath = p; saveCfg(); return 0;
    }
    if (LOWORD(w) == 202) {
      wchar_t p[MAX_PATH * 4]{}; BROWSEINFOW bi{};
      bi.hwndOwner = h; bi.lpszTitle = L"Choose screenshot folder";
      LPITEMIDLIST id = SHBrowseForFolderW(&bi);
      if (id) { SHGetPathFromIDListW(id, p); CoTaskMemFree(id); if (p[0]) { g_shotPath = p; saveCfg(); SetWindowTextW(g_pathEdit, g_shotPath.c_str()); } }
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

  // FPS overlay (top-level popup — NOT a child of g_output because DWM
  // doesn't render child windows of WS_EX_LAYERED parents.  Top-level
  // popup with WS_EX_TOPMOST stays above everything.)
  g_fps = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    FPS_CLS, L"", WS_POPUP, 0, 0, FPS_W, 100, nullptr, nullptr, inst, nullptr);

  // Settings panel (separate top-level popup, same reason as top bar)
  g_settings = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, SET_CLS, L"",
    WS_POPUP, 300, 80, 520, 400, nullptr, nullptr, inst, nullptr);
  if (g_settings) {
    // FPS overlay checkboxes
    CreateWindowW(L"BUTTON", L"FPS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 90, 100, 26, g_settings, (HMENU)101, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Frametime", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 130, 90, 120, 26, g_settings, (HMENU)102, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Graph", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 260, 90, 100, 26, g_settings, (HMENU)103, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Resolution", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 370, 90, 120, 26, g_settings, (HMENU)104, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Background", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 124, 120, 26, g_settings, (HMENU)105, inst, nullptr);

    // Screenshot path
    g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_shotPath.c_str(),
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 24, 210, 360, 28, g_settings, (HMENU)201, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 394, 210, 82, 28, g_settings, (HMENU)202, inst, nullptr);

    // Set checkbox states
    CheckDlgButton(g_settings, 101, g_cfg.fps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 102, g_cfg.frametime ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 103, g_cfg.frame_timing ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 104, g_cfg.resolution ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 105, g_cfg.background ? BST_CHECKED : BST_UNCHECKED);

    ShowWindow(g_settings, SW_HIDE);
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
      // Show the top bar first (ShowWindow is more reliable than
      // SetWindowPos SWP_SHOWWINDOW for a window that's never been shown)
      ShowWindow(g_ui, SW_SHOWNOACTIVATE);
      // Position at top-left of the output window
      RECT o{}; if (GetWindowRect(g_output, &o)) {
        int barW = BAR_PAD * 2 + 4 * BTN_SIZE + 3 * BTN_GAP;
        SetWindowPos(g_ui, HWND_TOPMOST, o.left + 16, o.top + 16, barW, BAR_PAD * 2 + BTN_SIZE,
          SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      InvalidateRect(g_ui, nullptr, TRUE);
      UpdateWindow(g_ui);
    } else {
      ShowWindow(g_ui, SW_HIDE);
    }
  }
  if (g_settings) {
    if (open) {
      // Position settings below the top bar
      RECT o{}; if (GetWindowRect(g_output, &o)) {
        SetWindowPos(g_settings, HWND_TOPMOST, o.left + 16, o.top + 16 + BAR_PAD * 2 + BTN_SIZE + 8,
          520, 400, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      InvalidateRect(g_settings, nullptr, TRUE);
    } else {
      ShowWindow(g_settings, SW_HIDE);
    }
  }
  // FPS overlay persists after overlay closes — only hide if g_fpsVisible is false
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
    // Auto-resize the FPS window to fit content (moved here from paintFps
    // to avoid calling SetWindowPos during WM_PAINT which caused flicker).
    int fs = g_cfg.fontSize;
    int smFont = (int)(fs * 0.55);
    int totalH = 8; // pad
    if (g_cfg.fps) totalH += fs + 6;
    else if (g_cfg.frametime) totalH += smFont + 6;
    if (g_cfg.frame_timing) totalH += smFont + 4 + FT_HEIGHT + 4;
    if (g_cfg.resolution) totalH += smFont + 4;
    totalH += 8; // bottom pad

    RECT cur{}; GetWindowRect(g_fps, &cur);
    if ((cur.bottom - cur.top) != totalH || (cur.right - cur.left) != FPS_W) {
      RECT o{}; GetWindowRect(g_output, &o);
      int x = o.left + 10, yp = o.top + 10;
      if (g_cfg.position == 1) x = o.right - FPS_W - 10;
      if (g_cfg.position == 2) yp = o.bottom - totalH - 10;
      if (g_cfg.position == 3) { x = o.right - FPS_W - 10; yp = o.bottom - totalH - 10; }
      SetWindowPos(g_fps, HWND_TOPMOST, x, yp, FPS_W, totalH,
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
