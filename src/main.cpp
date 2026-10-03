#include "graphics.h"
#include "capture.h"
#include "fsr.h"
#include "amdof.h"
#include "fastmv.h"
#include "ui.h"
#include "overlay.h"
#include "target.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>

static bool hasConsole() { return GetConsoleWindow() != nullptr; }

static void printCli(const std::wstring& text)
{
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &n, nullptr);
  WriteConsoleW(h, L"\r\n", 2, &n, nullptr);
}


static bool keyComboDown(UINT modifiers, UINT vk)
{
  auto down = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
  // Required modifiers must be down.
  if ((modifiers & MOD_CONTROL) && !down(VK_CONTROL)) return false;
  if ((modifiers & MOD_SHIFT) && !down(VK_SHIFT)) return false;
  if ((modifiers & MOD_ALT) && !down(VK_MENU)) return false;
  if ((modifiers & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
  // Extra modifiers must NOT be down (so Ctrl+Home is not also "Home").
  if (!(modifiers & MOD_CONTROL) && down(VK_CONTROL)) return false;
  if (!(modifiers & MOD_SHIFT) && down(VK_SHIFT)) return false;
  if (!(modifiers & MOD_ALT) && down(VK_MENU)) return false;
  if (!(modifiers & MOD_WIN) && (down(VK_LWIN) || down(VK_RWIN))) return false;
  return down((int)vk);
}

// Rising-edge inject of overlay bypass keys into our output window.

static void pollOverlayToggle(const TargetSpec& spec)
{
  static bool prev = false;
  const bool now = keyComboDown(spec.overlayHotkeyModifiers, spec.overlayHotkeyVk);
  if (now && !prev)
    setOverlayOpen(!isOverlayOpen());
  prev = now;
}

static void pollBindBypass(HWND out, const TargetSpec& spec)
{
  // Overlay mode already focuses our window: physical keys reach OptiScaler /
  // ReShade once. Injecting again would double-toggle menus.
  if (isOverlayOpen()) {
    static bool prevOpen[64]{};
    // Keep edge state in sync so we don't fire a stale edge on close.
    const size_t n = spec.bindBypass.size() < 64 ? spec.bindBypass.size() : 64;
    for (size_t i = 0; i < n; ++i)
      prevOpen[i] = keyComboDown(spec.bindBypass[i].modifiers, spec.bindBypass[i].vk);
    return;
  }

  static bool prev[64]{};
  const size_t n = spec.bindBypass.size() < 64 ? spec.bindBypass.size() : 64;
  for (size_t i = 0; i < n; ++i) {
    const auto& b = spec.bindBypass[i];
    const bool now = keyComboDown(b.modifiers, b.vk);
    if (now && !prev[i]) {
      HWND dest = out;
      if (dest) {
        UINT scan = MapVirtualKeyW(b.vk, MAPVK_VK_TO_VSC);
        LPARAM lpDown = 1 | (LPARAM)(scan << 16);
        LPARAM lpUp = 1 | (LPARAM)(scan << 16) | (1 << 30) | (1 << 31);
        PostMessageW(dest, WM_KEYDOWN, b.vk, lpDown);
        PostMessageW(dest, WM_KEYUP, b.vk, lpUp);
      }
    }
    prev[i] = now;
  }
}

static bool stopHotkeyDown(UINT modifiers, UINT vk)
{
  auto down = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
  if ((modifiers & MOD_CONTROL) && !down(VK_CONTROL)) return false;
  if ((modifiers & MOD_SHIFT) && !down(VK_SHIFT)) return false;
  if ((modifiers & MOD_ALT) && !down(VK_MENU)) return false;
  if ((modifiers & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
  return down((int)vk);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  TargetSpec spec;
  std::wstring err;
  bool parsed = parseTargetArgs(argc, argv, spec, err);
  LocalFree(argv);

  const bool cliMode = (argc > 1);
  if (!parsed) {
    if (cliMode && hasConsole()) printCli(err);
    else MessageBoxW(nullptr, err.c_str(), L"NRLive", MB_ICONERROR);
    return 1;
  }
  if (spec.help) {
    auto u = targetUsage();
    if (cliMode && hasConsole()) printCli(u);
    else MessageBoxW(nullptr, u.c_str(), L"NRLive", MB_OK);
    return 0;
  }

  HWND target = nullptr;
  std::wstring label;
  if (spec.delaySeconds > 0 && (spec.front || spec.mode != TargetMode::Picker))
    std::this_thread::sleep_for(std::chrono::seconds(spec.delaySeconds));

  if (!resolveTargetWindow(spec, inst, target, label)) {
    if (cliMode && hasConsole()) printCli(label);
    else MessageBoxW(nullptr, label.c_str(), L"NRLive", MB_ICONERROR);
    return 2;
  }

  HWND out = createOutput(inst, 1280, 720);
  if (!out) return 1;

  constexpr int kStopId = 0x4653;
  bool hk = RegisterHotKey(nullptr, kStopId,
      spec.stopHotkeyModifiers | MOD_NOREPEAT, spec.stopHotkeyVk) != 0;

  // Overlay keybind bypass: these VKs are not forwarded to the game and are
  // re-injected into our output HWND so OptiScaler / ReShade menus receive them.
  {
    std::vector<UINT> vks;
    vks.reserve(spec.bindBypass.size());
    for (const auto& b : spec.bindBypass) vks.push_back(b.vk);
    setBindBypassVks(vks.data(), vks.size());
  }


  HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{ sizeof(mi) };
  GetMonitorInfoW(mon, &mi);
  Size display{
    (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left),
    (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top)
  };
  setOutputFullscreen(out, mon);

  RECT cr{};
  GetClientRect(target, &cr);
  Size render{
    (uint32_t)std::max<LONG>(1, cr.right - cr.left),
    (uint32_t)std::max<LONG>(1, cr.bottom - cr.top)
  };
  setCaptureTarget(target);
  setScaleSizes(render, display);

  try {
    Graphics gfx;
    if (!gfx.init(out, render, display)) throw 1;

    Capture cap;
    if (!cap.init(gfx.device(), gfx.queue()) || !cap.start(target)) {
      std::wstring msg = L"Capture failed: " + cap.lastError();
      if (cliMode && hasConsole()) printCli(msg);
      else MessageBoxW(nullptr, msg.c_str(), L"NRLive", MB_ICONERROR);
      if (hk) UnregisterHotKey(nullptr, kStopId);
      DestroyWindow(out);
      return 3;
    }

    // Let OptiScaler finish DXGI/D3D12/AmdExtFfxApi hooks before we touch FFX.
    // It loads as dxgi.dll and attaches on CreateDevice/CreateSwapChain (already done).
    Sleep(50);

    Fsr fsr;
    bool fsrOk = fsr.init(gfx.device(), display, display);
    bool fsrEnabled = fsrOk;
    if (cliMode && hasConsole()) {
      if (fsrOk) printCli(L"FSR OK: " + fsr.lastError());
      else       printCli(L"FSR FAIL (raw capture only): " + fsr.lastError());
    }
    // Always show FSR path/status on the HUD so we can verify the hooked module.
    {
      std::wstring st = (fsrOk ? L"FSR " : L"FSR off: ") + fsr.lastError();
      setStatus(out, st.c_str());
    }

    AmdOf amdof;
    FastMv fastmv;
    if (fsrOk) {
      amdof.init(gfx.device(), gfx.queue(), render, false); // existing implementation
      if (spec.motionMode == TargetSpec::MotionMode::Fast)
        fastmv.init(gfx.device(), render);
    }

    overlayInit(inst, out);
    overlaySetFsrEnabled(fsrEnabled);
    setOverlayHud(nullptr);
    setOverlayOpen(false);
    setStatus(out, label.c_str());

    bool reset = true, running = true;
    bool stopLatched = false;
    float fps = 0;
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    ShowWindow(out, SW_SHOWNOACTIVATE);
    /* HUD starts hidden; Ctrl+Home (or --overlaykey) toggles it */

    while (running) {
      MSG msg{};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) running = false;
        if (msg.message == WM_HOTKEY && (int)msg.wParam == kStopId) running = false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!running) break;

      pollOverlayToggle(spec);
      pollBindBypass(out, spec);
      if (overlayConsumeFsrToggle() && fsrOk) { fsrEnabled = !fsrEnabled; overlaySetFsrEnabled(fsrEnabled); }
      drawCursor();

      // If the target (game) window is gone, quit.  This happens when the
      // game exits — NRLive should not keep running with a dead target.
      if (!target || !IsWindow(target)) {
        running = false;
        break;
      }

      if (stopHotkeyDown(spec.stopHotkeyModifiers, spec.stopHotkeyVk)) {
        if (!stopLatched) { running = false; break; }
        stopLatched = true;
      } else stopLatched = false;

      ComPtr<ID3D12Resource> color;
      Size cs{};
      uint64_t fenceVal = 0;
      if (!cap.acquire(color, cs, fenceVal)) {
        // No new WGC frame yet — do not re-submit with stale resource states.
        // Sleep(1) on Windows can stall for 1-15ms and was a stutter source.
        // Yield the thread cheaply instead: try another thread on the same
        // core first, then a 0-time sleep that the scheduler can ignore.
        if (!SwitchToThread()) SleepEx(0, TRUE);
        continue;
      }
      if (cap.fence() && fenceVal)
        gfx.queue()->Wait(cap.fence(), fenceVal);
      if (overlayConsumeScreenshot()) cap.saveScreenshot(L"", cap.totalFrames());
      setScaleSizes(cs, display);
      gfx.ensureAuxTextures(cs);

      QueryPerformanceCounter(&now);
      float ms = (float)((now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart));
      last = now;
      float dt = std::clamp(ms, 1.0f, 100.0f);
      fps = (ms > 0.001f) ? (1000.0f / ms) : 0.0f;

      gfx.begin();
      auto* cmd = gfx.cmd();
      ID3D12Resource* back = gfx.backbuffer();
      ID3D12Resource* upscale = gfx.upscaleOutput();
      ID3D12Resource* depth = gfx.dummyDepth();
      ID3D12Resource* mv = gfx.motionVectors();

      ID3D12Resource* presentSrc = color.Get();
      bool usedFsr = false;

      if (fsrEnabled && depth && mv && upscale) {
        // ---- Batch A: pre-OF+FSR prep -------------------------------------
        //   color:   COMMON  -> PS|NPS   (FSR & AMDOF read as SRV)
        //   depth:   UAV     -> PS|NPS   (FSR reads as SRV)
        //   upscale: NPS     -> UAV      (FSR writes as UAV)
        // (mv stays in UAV — AMDOF writes it; AMDOF will leave it in PS|NPS
        //  so FSR can read it directly without an extra barrier.)
        {
          D3D12_RESOURCE_BARRIER b[3]{};
          for (int i = 0; i < 3; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          }
          b[0].Transition.pResource = color.Get();
          b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
          b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[1].Transition.pResource = depth;
          b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[2].Transition.pResource = upscale;
          b[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          cmd->ResourceBarrier(3, b);
        }

        // Motion dispatch. --mv amdof is the existing implementation; --mv fast is the
        // new quarter-resolution screen-space estimator.
        if (spec.motionMode == TargetSpec::MotionMode::Fast)
          fastmv.dispatch(cmd, color.Get(), mv, cs, reset);
        else
          amdof.dispatch(cmd, color.Get(), mv, cs, reset);

        // FSR dispatch (reads color/depth/mv as SRV, writes upscale as UAV).
        // No barrier needed for mv — AMDOF left it in PS|NPS.
        usedFsr = fsr.dispatch(cmd, color.Get(), depth, mv, upscale, cs, display, dt, reset);
        reset = false;
        presentSrc = usedFsr ? upscale : color.Get();

        // ---- Batch C: post-FSR cleanup + presentation prep ----------------
        //   upscale: UAV -> PS|NPS (if usedFsr, blit reads as SRV)
        //            UAV -> NPS     (if !usedFsr, idle for next frame)
        //   depth:   PS|NPS -> UAV  (next frame's FSR/AMDOF start state)
        //   mv:      PS|NPS -> UAV  (next frame's AMDOF writes it as UAV)
        //   back:    PRESENT -> RTV (clear + blit write as RTV)
        // (color stays in PS|NPS for the blit in case !usedFsr; it is
        //  returned to COMMON in batch D after the blit.)
        {
          D3D12_RESOURCE_BARRIER b[4]{};
          int n = 0;
          auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
            b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[n].Transition.pResource = r;
            b[n].Transition.StateBefore = a;
            b[n].Transition.StateAfter = s;
            b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++n;
          };
          tr(upscale, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             usedFsr ? (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          tr(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          tr(mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          tr(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
          cmd->ResourceBarrier(n, b);
        }
      } else {
        // FSR disabled — still need to flip color COMMON→PS|NPS→COMMON and
        // backbuffer PRESENT→RTV for the blit.  Batch them.
        D3D12_RESOURCE_BARRIER b[2]{};
        int n = 0;
        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
          b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b[n].Transition.pResource = r;
          b[n].Transition.StateBefore = a;
          b[n].Transition.StateAfter = s;
          b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++n;
        };
        tr(color.Get(), D3D12_RESOURCE_STATE_COMMON,
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tr(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(n, b);
      }

      const float clear[4] = { 0, 0, 0, 1 };
      cmd->ClearRenderTargetView(gfx.rtvHandle(), clear, 0, nullptr);
      gfx.blitToBackbuffer(presentSrc);

      // ---- Batch D: post-blit backbuffer + colour restore -----------------
      //   back:  RTV -> PRESENT  (always)
      //   color: PS|NPS -> COMMON  (return for next D3D11 copy; always,
      //                              whether or not FSR was used)
      {
        D3D12_RESOURCE_BARRIER b[2]{};
        int n = 0;
        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
          b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b[n].Transition.pResource = r;
          b[n].Transition.StateBefore = a;
          b[n].Transition.StateAfter = s;
          b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++n;
        };
        tr(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        tr(color.Get(),
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
           D3D12_RESOURCE_STATE_COMMON);
        cmd->ResourceBarrier(n, b);
      }

      gfx.end();
      gfx.present();

      HudInfo hi;
      hi.fps = fps;
      hi.capture = cs;
      hi.output = display;
      hi.visible = !spec.noOverlay && isOverlayOpen();
      hi.status = L"frames=" + std::to_wstring(cap.totalFrames()) +
                  (usedFsr ? L" FSR" : L" blit") +
                  L" | mv=" + spec.motionModeText + L" | " + spec.stopHotkeyText + L"=quit | " + spec.overlayHotkeyText + L"=overlay | bypass=" + (spec.bindBypassText.empty() ? std::wstring(L"default") : spec.bindBypassText);
      overlayUpdate(fps, ms, cs, display);
      updateHud(nullptr, hi);
    }

    // IMPORTANT: drain the GPU before tearing down any D3D12 resources.
    // The FSR context, AMDOF pipelines, capture shared textures, and
    // swapchain all hold GPU-side references.  Releasing them while the
    // GPU still has in-flight command lists is what was crashing the
    // AMD driver on quit.
    gfx.waitForGpu();

    fastmv.shutdown();
    amdof.shutdown();
    fsr.shutdown();
    overlayShutdown();
    cap.stop();
    if (hk) UnregisterHotKey(nullptr, kStopId);
  } catch (const std::exception& e) {
    if (hk) UnregisterHotKey(nullptr, kStopId);
    MessageBoxA(nullptr, e.what(), "NRLive", MB_ICONERROR);
  } catch (...) {
    if (hk) UnregisterHotKey(nullptr, kStopId);
    MessageBoxW(nullptr, L"Fatal graphics error.", L"NRLive", MB_ICONERROR);
  }
  DestroyWindow(out);
  return 0;
}
