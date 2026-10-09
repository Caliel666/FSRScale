#include <Windows.h>
#include <d3d12.h>
#include <reshade.hpp>
#include <algorithm>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
using namespace reshade::api;

constexpr wchar_t kPreFsrResourceName[] = L"NRLive_PreFSR_Color";
constexpr GUID kDebugObjectNameW = { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };

std::mutex g_mutex;
HWND g_outputWindow = nullptr;
std::vector<effect_runtime *> g_runtimes;
std::unordered_map<command_list *, resource_view> g_boundRtvs;

effect_runtime *findRuntime(device *dev)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_runtimes.rbegin(); it != g_runtimes.rend(); ++it)
  {
    effect_runtime *runtime = *it;
    if (runtime != nullptr && runtime->get_device() == dev &&
        (g_outputWindow == nullptr || runtime->get_hwnd() == g_outputWindow))
      return runtime;
  }
  return nullptr;
}

bool isPreFsrTarget(command_list *cmd, resource_view view)
{
  if (cmd == nullptr || view == 0 || cmd->get_device()->get_api() != device_api::d3d12)
    return false;

  const resource resource = cmd->get_device()->get_resource_from_view(view);
  auto *nativeResource = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(resource.handle));
  if (nativeResource == nullptr)
    return false;

  wchar_t name[128]{};
  UINT bytes = sizeof(name);
  if (FAILED(nativeResource->GetPrivateData(kDebugObjectNameW, &bytes, name)))
    return false;
  const size_t chars = std::min<size_t>(bytes / sizeof(wchar_t), std::size(name) - 1);
  name[chars] = L'\0';
  return wcscmp(name, kPreFsrResourceName) == 0;
}

void onInitEffectRuntime(effect_runtime *runtime)
{
  if (runtime == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_runtimes.push_back(runtime);
}

void onDestroyEffectRuntime(effect_runtime *runtime)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  std::erase(g_runtimes, runtime);
}

void onBindRenderTargets(command_list *cmd, uint32_t count,
                         const resource_view *rtvs, resource_view)
{
  const resource_view current = (count != 0 && rtvs != nullptr) ? rtvs[0] : resource_view{ 0 };
  resource_view previous{};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_boundRtvs.find(cmd);
    if (it != g_boundRtvs.end())
      previous = it->second;

    // Update before rendering effects: render_effects itself binds render
    // targets and therefore re-enters this event callback.
    if (current == 0)
      g_boundRtvs.erase(cmd);
    else
      g_boundRtvs[cmd] = current;
  }

  // The app explicitly unbinds the pre-FSR target after its source-color blit.
  // Render effects at that point, while the target is still in RENDER_TARGET
  // state, before NRLive transitions it to shader-resource state for FSR.
  if (previous != 0 && isPreFsrTarget(cmd, previous))
  {
    if (effect_runtime *runtime = findRuntime(cmd->get_device()))
    {
      runtime->render_effects(cmd, previous, previous);
      reshade::log::message(reshade::log::level::info,
        "NRLive: rendered ReShade effects on the pre-FSR color target.");
    }
    else
    {
      reshade::log::message(reshade::log::level::warning,
        "NRLive: pre-FSR target found, but no matching ReShade runtime is available.");
    }

    // Ignore any transient render-target binds performed by the effects runtime.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (current == 0)
      g_boundRtvs.erase(cmd);
    else
      g_boundRtvs[cmd] = current;
  }
}

void onDestroyCommandList(command_list *cmd)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_boundRtvs.erase(cmd);
}
}

extern "C" __declspec(dllexport) BOOL WINAPI NRLiveSetOutputWindow(HWND hwnd)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_outputWindow = hwnd;
  for (auto it = g_runtimes.rbegin(); it != g_runtimes.rend(); ++it)
  {
    if (*it != nullptr && (*it)->get_hwnd() == hwnd &&
        (*it)->get_device()->get_api() == reshade::api::device_api::d3d12)
      return TRUE;
  }
  return FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    if (!reshade::register_addon(module))
      return FALSE;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargets);
    reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
  }
  else if (reason == DLL_PROCESS_DETACH)
  {
    reshade::unregister_addon(module);
  }
  return TRUE;
}
