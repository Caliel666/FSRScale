#include "ui.h"
#include <windowsx.h>

static const wchar_t* OUT_CLS = L"FSRScaleOutput";
static const wchar_t* HUD_CLS = L"FSRScaleHud";
static HWND g_hudOwner = nullptr;
static HWND g_target = nullptr;
static HudInfo g_hud;
static Size g_captureSize{};
static Size g_outputSize{};

void setCaptureTarget(HWND target) { g_target = target; }
void setScaleSizes(Size capture, Size output)
{
  g_captureSize = capture;
  g_outputSize = output;
}

static bool mapOutputToTargetClient(int ox, int oy, int& tx, int& ty)
{
  if (!g_target || !IsWindow(g_target)) return false;
  if (g_outputSize.w == 0 || g_outputSize.h == 0) return false;
  RECT tr{};
  if (!GetClientRect(g_target, &tr)) return false;
  const int tw = tr.right - tr.left;
  const int th = tr.bottom - tr.top;
  if (tw <= 0 || th <= 0) return false;
  tx = (int)((double)ox * tw / (double)g_outputSize.w);
  ty = (int)((double)oy * th / (double)g_outputSize.h);
  if (tx < 0) tx = 0;
  if (ty < 0) ty = 0;
  if (tx >= tw) tx = tw - 1;
  if (ty >= th) ty = th - 1;
  return true;
}

static void forwardMouse(UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (!g_target || !IsWindow(g_target)) return;

  int ox = GET_X_LPARAM(lParam);
  int oy = GET_Y_LPARAM(lParam);
  int tx = 0, ty = 0;
  if (!mapOutputToTargetClient(ox, oy, tx, ty)) return;

  // Magpie-style: keep the visible cursor on the overlay. Do NOT SetCursorPos
  // to the game window (that caused continuous upward drift). Only forward
  // client-space messages so the game sees the mapped position.
  LPARAM lp = MAKELPARAM(tx, ty);
  switch (msg) {
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN: case WM_LBUTTONUP:
  case WM_RBUTTONDOWN: case WM_RBUTTONUP:
  case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    PostMessageW(g_target, msg, wParam, lp);
    break;
  case WM_MOUSEWHEEL:
    PostMessageW(g_target, WM_MOUSEWHEEL, wParam, lp);
    break;
  default: break;
  }
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
  case WM_MOUSEACTIVATE:
    // The scaler is a presentation surface, never the active application.
    // Keep the game foreground while still forwarding the mouse messages below.
    return MA_NOACTIVATE;
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN: case WM_LBUTTONUP:
  case WM_RBUTTONDOWN: case WM_RBUTTONUP:
  case WM_MBUTTONDOWN: case WM_MBUTTONUP:
  case WM_MOUSEWHEEL:
    // Keep mouse captured while a button is held so we don't lose tracking.
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)
      SetCapture(h);
    if (m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP)
      ReleaseCapture();
    forwardMouse(m, w, l);
    return 0;
  case WM_SETCURSOR:
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
  case WM_KEYDOWN: case WM_KEYUP:
  case WM_SYSKEYDOWN: case WM_SYSKEYUP:
  case WM_CHAR: case WM_SYSCHAR:
    if (g_target && IsWindow(g_target))
      PostMessageW(g_target, m, w, l);
    return 0;
  case WM_CLOSE:
    PostQuitMessage(0);
    return 0;
  case WM_DESTROY:
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
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
    OUT_CLS, L"FSRScale",
    WS_POPUP, 0, 0, w, h, nullptr, nullptr, i, nullptr);
  // Prevent our overlay from being fed back into capture paths.
  if (hwnd) {
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
  SetWindowLongPtrW(h, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
  SetWindowPos(h, HWND_TOPMOST,
               mi.rcMonitor.left, mi.rcMonitor.top,
               mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top,
               SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE);
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
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  InvalidateRect(hud, nullptr, FALSE);
}
