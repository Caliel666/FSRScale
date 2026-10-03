#include "ui.h"
#include <windowsx.h>
#include <algorithm>

static const wchar_t* OUT_CLS = L"FSRScaleOutput";
static const wchar_t* HUD_CLS = L"FSRScaleHud";
static const wchar_t* CURSOR_CLS = L"FSRScaleCursor";
static HWND g_hudOwner = nullptr;

static UINT g_bypassVks[32]{};
static size_t g_bypassCount = 0;

void setBindBypassVks(const UINT* vks, size_t count)
{
  g_bypassCount = 0;
  if (!vks || !count) return;
  if (count > 32) count = 32;
  for (size_t i = 0; i < count; ++i)
    g_bypassVks[g_bypassCount++] = vks[i];
}

static bool isBypassVk(UINT vk)
{
  for (size_t i = 0; i < g_bypassCount; ++i)
    if (g_bypassVks[i] == vk) return true;
  return false;
}

static HWND g_target = nullptr;
static HWND g_output = nullptr;
static HWND g_overlayHud = nullptr;
static HudInfo g_hud;
static Size g_captureSize{};
static Size g_outputSize{};
static bool g_overlayOpen = false;
static LONG_PTR g_savedExStyle = 0;

include "ui.h"
#include <windowsx.h>
#include <algorithm>

static const wchar_t* OUT_CLS = L"FSRScaleOutput";
static const wchar_t* HUD_CLS = L"FSRScaleHud";
static const wchar_t* CURSOR_CLS = L"FSRScaleCursor";
static HWND g_hudOwner = nullptr;

static UINT g_bypassVks[32]{};
static size_t g_bypassCount = 0;

void setBindBypassVks(const UINT* vks, size_t count)
{
  g_bypassCount = 0;
  if (!vks || !count) return;
  if (count > 32) count = 32;
  for (size_t i = 0; i < count; ++i)
    g_bypassVks[g_bypassCount++] = vks[i];
}

static bool isBypassVk(UINT vk)
{
  for (size_t i = 0; i < g_bypassCount; ++i)
    if (g_bypassVks[i] == vk) return true;
  return false;
}

static HWND g_target = nullptr;
static HWND g_output = nullptr;
static HWND g_overlayHud = nullptr;
static HudInfo g_hud;
static Size g_captureSize{};
static Size g_outputSize{};
static bool g_overlayOpen = false;
static LONG_PTR g_savedExStyle = 0;

// Game mode uses the physical mouse stream rather than synthesizing legacy
// WM_MOUSE* messages. The target remains foreground and receives the same raw
// input itself; FSRScale only keeps the presentation cursor clipped to output.
static bool g_rawMouseRegistered = false;

static void registerRawMouse(HWND hwnd)
{
  if (!hwnd || g_rawMouseRegistered) return;
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;
  rid.usUsage = 0x02;
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = hwnd;
  g_rawMouseRegistered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
}

static void unregisterRawMouse()
{
  if (!g_rawMouseRegistered) return;
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;
  rid.usUsage = 0x02;
  rid.dwFlags = RIDEV_REMOVE;
  rid.hwndTarget = nullptr;
  RegisterRawInputDevices(&rid, 1, sizeof(rid));
  g_rawMouseRegistered = false;
}

static void handleRawMouse(HRAWINPUT handle)
{
  // Observe raw input only. Never move or clip the system cursor here.
  // The foreground game receives its own raw input independently.
  if (g_overlayOpen || !handle) return;

  UINT size = 0;
  if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                      sizeof(RAWINPUTHEADER)) == (UINT)-1 || size == 0)
    return;

  std::vector<BYTE> data(size);
  if (GetRawInputData(handle, RID_INPUT, data.data(), &size,
                      sizeof(RAWINPUTHEADER)) == (UINT)-1)
    return;

  const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data.data());
  if (raw->header.dwType != RIM_TYPEMOUSE) return;
}


void setCaptureTarget(HWND target) { g_target = target; }

void setOverlayHud(HWND hud) { g_overlayHud = hud; }

HWND outputHwnd() { return g_output; }

bool isOverlayOpen() { return g_overlayOpen; }

// Clip cursor to the presentation window client area (game view on screen).
static void clipCursorToOutput(bool enable)
{
  if (!enable) {
    ClipCursor(nullptr);
    return;
  }
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;
  RECT rc{};
  if (!GetClientRect(h, &rc)) return;
  POINT tl{ rc.left, rc.top };
  POINT br{ rc.right, rc.bottom };
  ClientToScreen(h, &tl);
  ClientToScreen(h, &br);
  RECT screen{ tl.x, tl.y, br.x, br.y };
  ClipCursor(&screen);
}

static HWND g_cursorWindow = nullptr;
static HCURSOR g_drawnCursor = nullptr;
static int g_cursorW = 32;
static int g_cursorH = 32;
static POINT g_cursorHotspot{};

static LRESULT CALLBACK cursorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
}

static void createCursorWindow(HINSTANCE inst)
{
  if (g_cursorWindow) return;
  WNDCLASSEXW c{ sizeof(c) };
  c.hInstance = inst;
  c.lpfnWndProc = cursorProc;
  c.lpszClassName = CURSOR_CLS;
  c.hCursor = nullptr;
  c.hbrBackground = nullptr;
  RegisterClassExW(&c);

  g_cursorWindow = CreateWindowExW(
    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    CURSOR_CLS, L"", WS_POPUP,
    0, 0, g_cursorW, g_cursorH,
    nullptr, nullptr, inst, nullptr);

  if (g_cursorWindow)
    ShowWindow(g_cursorWindow, SW_HIDE);
}

static void hideDrawnCursor()
{
  if (g_cursorWindow)
    ShowWindow(g_cursorWindow, SW_HIDE);
}

static bool rebuildDrawnCursor(HCURSOR cursor)
{
  if (!g_cursorWindow || !cursor) return false;

  ICONINFO ii{};
  if (!GetIconInfo(cursor, &ii))
    return false;

  BITMAP bm{};
  HBITMAP shape = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
  if (!shape || !GetObjectW(shape, sizeof(bm), &bm)) {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return false;
  }

  int w = bm.bmWidth;
  int h = bm.bmHeight;
  if (!ii.hbmColor) h /= 2;
  w = std::max(1, std::min(w, 256));
  h = std::max(1, std::min(h, 256));

  g_cursorW = w;
  g_cursorH = h;
  g_cursorHotspot.x = (LONG)ii.xHotspot;
  g_cursorHotspot.y = (LONG)ii.yHotspot;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!mem || !dib || !bits) {
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return false;
  }

  SelectObject(mem, dib);
  memset(bits, 0, (size_t)w * (size_t)h * 4);

  DrawIconEx(mem, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);

  POINT dst{};
  SIZE size{ w, h };
  POINT src{};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;

  BOOL ok = UpdateLayeredWindow(
    g_cursorWindow, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);

  if (!ok) return false;
  g_drawnCursor = cursor;
  return true;
}

static void drawCursor()
{
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;

  // The real cursor must always be confined to the presentation surface in
  // both game mode and overlay mode. Games can call ClipCursor themselves,
  // so repeat this every frame just like Magpie's 3D-game path.
  clipCursorToOutput(true);

  // Overlay mode owns the real cursor. Do not put a second cursor on top of it.
  if (g_overlayOpen) {
    hideDrawnCursor();
    return;
  }

  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
    hideDrawnCursor();
    return;
  }

  if (ci.hCursor != g_drawnCursor && !rebuildDrawnCursor(ci.hCursor)) {
    hideDrawnCursor();
    return;
  }

  RECT out{};
  if (!GetWindowRect(h, &out)) {
    hideDrawnCursor();
    return;
  }

  // The output is a fullscreen presentation surface. Keep the cursor in
  // physical screen coordinates so it stays pixel-accurate instead of being
  // scaled with the game image.
  const int x = ci.ptScreenPos.x - (int)g_cursorHotspot.x;
  const int y = ci.ptScreenPos.y - (int)g_cursorHotspot.y;

  SetWindowPos(g_cursorWindow, HWND_TOPMOST,
               x, y, g_cursorW, g_cursorH,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void applyOverlayActivation(bool open)
{
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;

  if (open) {
    LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
    SetWindowLongPtrW(h, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
    // OptiScaler / ReShade need a real activatable, focused window.
    g_savedExStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    SetWindowLongPtrW(h, GWL_EXSTYLE,
      (g_savedExStyle & ~WS_EX_NOACTIVATE) | WS_EX_TOPMOST);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    AllowSetForegroundWindow(ASFW_ANY);
    // Attach input so SetForegroundWindow succeeds from our thread.
    const DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD ourTid = GetCurrentThreadId();
    if (fgTid && fgTid != ourTid)
      AttachThreadInput(fgTid, ourTid, TRUE);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (fgTid && fgTid != ourTid)
      AttachThreadInput(fgTid, ourTid, FALSE);

    clipCursorToOutput(true);
  } else {
    ClipCursor(nullptr);
    if (GetCapture()) ReleaseCapture();
    // Restore non-activating presentation surface so the game keeps input.
    LONG_PTR ex = g_savedExStyle ? g_savedExStyle
      : (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // Return focus toward the game if possible.
    if (g_target && IsWindow(g_target)) {
      AllowSetForegroundWindow(ASFW_ANY);
      SetForegroundWindow(g_target);
      // Do not SetCursorPos/ClipCursor here. The target owns cursor state.
    }
  }
}

void setOverlayOpen(bool open)
{
  if (g_overlayOpen == open) {
    if (!open && GetCapture() == g_output) ReleaseCapture();
    return;
  }
  g_overlayOpen = open;

  if (g_overlayHud && IsWindow(g_overlayHud)) {
    ShowWindow(g_overlayHud, open ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (open) InvalidateRect(g_overlayHud, nullptr, FALSE);
  }

  applyOverlayActivation(open);
}
void setScaleSizes(Size capture, Size output)
{
  g_captureSize = capture;
  g_outputSize = output;
}


static void registerClass(const wchar_t* name, WNDPROC proc)
{
  WNDCLASSEXW c{ sizeof(c) };
  c.hInstance = GetModuleHandleW(nullptr);
  c.lpfnWndProc = proc;
  c.lpszClassName = name;
  c.hCursor = LoadCursor(nullptr, IDC_ARROW);
  c.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassExW(&c);
}

static LRESULT CALLBACK outProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m) {
  case WM_NCHITTEST:
    if (!g_overlayOpen) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
  case WM_MOUSEACTIVATE:
    // Game mode: never steal activation. Overlay mode: accept activation so
    // OptiScaler / ReShade ImGui can take mouse + keyboard.
    if (g_overlayOpen) return MA_ACTIVATE;
    return MA_NOACTIVATE;
  case WM_INPUT:
    return DefWindowProcW(h, m, w, l);
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN: case WM_LBUTTONUP:
  case WM_RBUTTONDOWN: case WM_RBUTTONUP:
  case WM_MBUTTONDOWN: case WM_MBUTTONUP:
  case WM_MOUSEWHEEL:
    if (g_overlayOpen) {
      if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)
        SetCapture(h);
      if (m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP)
        ReleaseCapture();
      clipCursorToOutput(true);
      return DefWindowProcW(h, m, w, l);
    }
    // Game mode: this window must NEVER own mouse capture.
    if (GetCapture() == h) ReleaseCapture();
    return 0;
  case WM_CAPTURECHANGED:
    if (!g_overlayOpen && GetCapture() == h) ReleaseCapture();
    return 0;
  case WM_SETCURSOR:
    // Never overwrite the game's cursor from the presentation window.
    if (!g_overlayOpen)
      return FALSE;
    return DefWindowProcW(h, m, w, l);

  case WM_KEYDOWN: case WM_KEYUP:
  case WM_SYSKEYDOWN: case WM_SYSKEYUP:
  case WM_CHAR: case WM_SYSCHAR:
    // Overlay mode or bypass keys: keep on this window for OptiScaler/ReShade.
    if (g_overlayOpen || isBypassVk((UINT)w))
      return DefWindowProcW(h, m, w, l);
    if (g_target && IsWindow(g_target))
      PostMessageW(g_target, m, w, l);
    return DefWindowProcW(h, m, w, l);
  case WM_CLOSE:
    PostQuitMessage(0);
    return 0;
  case WM_DESTROY:
    if (GetCapture() == h) ReleaseCapture();
    ClipCursor(nullptr);
    hideDrawnCursor();
    if (g_cursorWindow) { DestroyWindow(g_cursorWindow); g_cursorWindow = nullptr; }
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(h, m, w, l);
  }
}

static LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    HBRUSH br = CreateSolidBrush(RGB(16, 18, 24));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    if (g_hud.visible) {
      SetBkMode(dc, TRANSPARENT);
      wchar_t head[256]{};
      swprintf_s(head, L"%.1f fps   %ux%u -> %ux%u",
                 (double)g_hud.fps,
                 (unsigned)g_hud.capture.w, (unsigned)g_hud.capture.h,
                 (unsigned)g_hud.output.w, (unsigned)g_hud.output.h);
      SetTextColor(dc, RGB(0, 255, 170));
      RECT r1{ 10, 6, 800, 28 };
      DrawTextW(dc, head, -1, &r1, DT_LEFT | DT_TOP | DT_SINGLELINE);
      if (!g_hud.status.empty()) {
        SetTextColor(dc, RGB(255, 215, 120));
        RECT r2{ 10, 28, 800, 70 };
        DrawTextW(dc, g_hud.status.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
      }
    }
    EndPaint(h, &ps);
    return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(h, m, w, l);
}

HWND createOutput(HINSTANCE i, int w, int h)
{
  registerClass(OUT_CLS, outProc);
  HWND hwnd = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
    OUT_CLS, L"FSRScale",
    WS_POPUP, 0, 0, w, h, nullptr, nullptr, i, nullptr);
  if (hwnd) {
    g_output = hwnd;
    createCursorWindow(i);
    // WDA_EXCLUDEFROMCAPTURE = 0x00000011 (Win10 2004+)
    SetWindowDisplayAffinity(hwnd, 0x00000011);
  }
  return hwnd;
}

void setOutputFullscreen(HWND h, HMONITOR mon)
{
  MONITORINFO mi{ sizeof(mi) };
  GetMonitorInfoW(mon, &mi);
  SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP);
  SetWindowLongPtrW(h, GWL_EXSTYLE,
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
    (g_overlayOpen ? 0 : WS_EX_TRANSPARENT));
  SetWindowPos(h, HWND_TOPMOST,
               mi.rcMonitor.left, mi.rcMonitor.top,
               mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top,
               SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE);
  // Do not initialize, reposition, or clip the real cursor here.
  // The target game owns cursor/input state.
}

void setOutputWindowed(HWND h, int w, int t)
{
  SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_BORDER);
  SetWindowLongPtrW(h, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
  SetWindowPos(h, HWND_TOPMOST, 80, 80, w, t, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void setStatus(HWND h, const wchar_t* t)
{
  if (!t) return;
  if (h) SetWindowTextW(h, t);
}

HWND createHud(HINSTANCE inst, HWND owner)
{
  registerClass(HUD_CLS, hudProc);
  g_hudOwner = owner;
  RECT r{}; GetWindowRect(owner, &r);
  return CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
    HUD_CLS, L"", WS_POPUP,
    r.left + 12, r.top + 12, 820, 76,
    owner, nullptr, inst, nullptr);
}

void updateHud(HWND hud, const HudInfo& info)
{
  // Always update the cached info (used by WM_PAINT when it does fire).
  g_hud = info;
  if (!hud) return;
  // Steam-style: HUD only paints while the FSRScale overlay is open.
  if (!g_overlayOpen) return;

  // Throttle HUD repaints to ~5 Hz (200 ms).  The overlay text is informational
  // only — frame-by-frame redraws force a GDI InvalidateRect + BeginPaint /
  // EndPaint round-trip every frame and were a measurable stutter source on
  // integrated GPUs where the desktop compositor interacts with our swapchain.
  static LONGLONG s_lastHudTicks = 0;
  static LONGLONG s_tickFreq = 0;
  if (s_tickFreq == 0) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    s_tickFreq = f.QuadPart;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const LONGLONG elapsed = now.QuadPart - s_lastHudTicks;
  const bool due = (s_tickFreq != 0) && (elapsed >= (s_tickFreq / 5));  // 5 Hz
  if (!due && info.visible) return;
  s_lastHudTicks = now.QuadPart;

  // Cache the owner window rect — it only changes when the user moves the
  // borderless output window, so skip SetWindowPos on the common steady-state
  // path.  The previous code called GetWindowRect + SetWindowPos every frame.
  static RECT s_lastOwnerRect{};
  RECT o{};
  if (!GetWindowRect(g_hudOwner, &o)) return;
  if (o.left != s_lastOwnerRect.left || o.top != s_lastOwnerRect.top) {
    s_lastOwnerRect = o;
    SetWindowPos(hud, HWND_TOPMOST, o.left + 12, o.top + 12, 820, 76,
                 SWP_NOACTIVATE | SWP_NOSIZE | (g_overlayOpen ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  }
  InvalidateRect(hud, nullptr, FALSE);
}
