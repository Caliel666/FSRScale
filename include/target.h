#pragma once
#include <windows.h>
#include <string>

enum class TargetMode { Picker, Pid, Window, Process };

struct TargetSpec {
  TargetMode mode = TargetMode::Picker;
  unsigned long pid = 0;
  std::wstring text;
  int delaySeconds = 0;
  bool delayExplicit = false;
  bool help = false;
  bool noOverlay = false;
  bool front = false;
  enum class MotionMode { AmdOf, Fast };
  MotionMode motionMode = MotionMode::AmdOf;
  std::wstring motionModeText = L"amdof";
  UINT stopHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
  UINT stopHotkeyVk = 'A';
  std::wstring stopHotkeyText = L"Ctrl+Shift+A";
};

bool parseTargetArgs(int argc, wchar_t** argv, TargetSpec& spec, std::wstring& error);
bool resolveTargetWindow(const TargetSpec& spec, HINSTANCE inst, HWND& out, std::wstring& label);
HWND resolveFrontWindow();
std::wstring targetUsage();
