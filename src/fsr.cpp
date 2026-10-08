#include "fsr.h"
#include "graphics.h"
#include <ffx_api/ffx_api_loader.h>
#include <cfloat>
#include <mutex>
#include <string>

// CRITICAL for OptiScaler:
// Call ffxCreateContext / ffxDispatch ONLY through the module OptiScaler hooks
// (amd_fidelityfx_loader_dx12.dll). Do NOT import amd_fidelityfx_dx12.lib —
// that binds a second unhooked copy of the API.

static ffxFunctions g_ffx{};
static HMODULE g_mod = nullptr;
static std::once_flag g_once;
static std::wstring g_info;

static std::wstring hex(uint32_t v)
{
  wchar_t b[16]{};
  swprintf_s(b, L"%08X", v);
  return b;
}

static std::wstring exeDir()
{
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring d(path);
  auto s = d.find_last_of(L"\\/");
  if (s != std::wstring::npos) d.resize(s + 1);
  return d;
}

static bool resolve(HMODULE m)
{
  if (!m) return false;
  ffxLoadFunctions(&g_ffx, m);
  if (g_ffx.CreateContext && g_ffx.Dispatch)
    return true;
  // Loader may re-export from upscaler module only
  HMODULE up = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll");
  if (up) {
    ffxFunctions upFn{};
    ffxLoadFunctions(&upFn, up);
    if (!g_ffx.CreateContext) g_ffx.CreateContext = upFn.CreateContext;
    if (!g_ffx.DestroyContext) g_ffx.DestroyContext = upFn.DestroyContext;
    if (!g_ffx.Dispatch) g_ffx.Dispatch = upFn.Dispatch;
    if (!g_ffx.Query) g_ffx.Query = upFn.Query;
    if (!g_ffx.Configure) g_ffx.Configure = upFn.Configure;
  }
  return g_ffx.CreateContext && g_ffx.Dispatch;
}

static bool loadFfx()
{
  // Prefer modules OptiScaler already loaded into the process.
  const wchar_t* prefer[] = {
    L"amd_fidelityfx_loader_dx12.dll",
    L"amd_fidelityfx_dx12.dll",
    L"amd_fidelityfx_upscaler_dx12.dll",
  };
  for (auto* n : prefer) {
    HMODULE m = GetModuleHandleW(n);
    if (m && resolve(m)) {
      g_mod = m;
      g_info = std::wstring(L"hooked/") + n;
      return true;
    }
  }

  const std::wstring dir = exeDir();
  const wchar_t* subs[] = { L"OptiScaler\\", L"optiscaler\\", L"" };

  for (auto* n : prefer) {
    for (auto* sub : subs) {
      std::wstring full = dir + sub + n;
      if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
        continue;
      HMODULE m = LoadLibraryW(full.c_str());
      if (m && resolve(m)) {
        g_mod = m;
        g_info = full;
        return true;
      }
    }
  }

  for (auto* n : prefer) {
    HMODULE m = LoadLibraryW(n);
    if (m && resolve(m)) {
      g_mod = m;
      g_info = std::wstring(L"PATH/") + n;
      return true;
    }
  }

  g_info = L"no FFX DLL";
  return false;
}

static bool ensure()
{
  std::call_once(g_once, [] { loadFfx(); });
  return g_ffx.CreateContext && g_ffx.Dispatch;
}

bool Fsr::init(ID3D12Device* device, Size maxRender, Size maxDisplay)
{
  if (!device) { m_error = L"null device"; return false; }
  if (!ensure()) {
    m_error = L"FFX missing (" + g_info + L"). Place amd_fidelityfx_dx12.dll "
              L"or OptiScaler\\amd_fidelityfx_loader_dx12.dll next to the EXE.";
    return false;
  }

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  backend.device = device;

  ffxCreateContextDescUpscale up{};
  up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  up.header.pNext = &backend.header;
  up.maxRenderSize = { maxRender.w, maxRender.h };
  up.maxUpscaleSize = { maxDisplay.w, maxDisplay.h };
  up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
             FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
             FFX_UPSCALE_ENABLE_DEPTH_INFINITE;

  ffxReturnCode_t rc = g_ffx.CreateContext(&m_ctx, &up.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_ctx) {
    m_error = L"ffxCreateContext 0x" + hex((uint32_t)rc) + L" [" + g_info + L"]";
    m_ctx = nullptr;
    return false;
  }

  if (g_ffx.Query) {
    ffxQueryDescUpscaleGetJitterPhaseCount phases{};
    phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    int32_t phaseCount = 0;
    phases.renderWidth = maxRender.w;
    phases.displayWidth = maxDisplay.w;
    phases.pOutPhaseCount = &phaseCount;
    if (g_ffx.Query(&m_ctx, &phases.header) == FFX_API_RETURN_OK)
      m_jitterPhases = phaseCount;
  }

  m_error = L"OK " + g_info;
  return true;
}

void Fsr::setQuality(int quality)
{
  if (quality < 0) quality = 0;
  if (quality > 4) quality = 4;
  m_quality = quality;
}

void Fsr::setSharpening(bool on, float amount)
{
  m_sharpen = on;
  m_sharpness = amount;
}

static FfxApiResource apiRes(ID3D12Resource* r, uint32_t state)
{
  return ffxApiGetResourceDX12(r, state);
}

bool Fsr::dispatch(ID3D12GraphicsCommandList* cmd,
                   ID3D12Resource* color,
                   ID3D12Resource* depth,
                   ID3D12Resource* motionVectors,
                   ID3D12Resource* reactive,
                   ID3D12Resource* output,
                   Size render, Size display,
                   float dt, bool reset)
{
  if (!m_ctx || !g_ffx.Dispatch || !cmd || !color || !output)
    return false;

  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.header.pNext = nullptr;
  d.commandList = cmd;
  d.color = apiRes(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  d.output = apiRes(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  if (depth)
    d.depth = apiRes(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  if (motionVectors)
    d.motionVectors = apiRes(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  if (reactive)
    d.reactive = apiRes(reactive, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

  d.renderSize = { render.w, render.h };
  d.upscaleSize = { display.w, display.h };
  // MVs from our OF are in full-res pixel units → scale must be (1,1).
  // Using (w,h) made OptiScaler treat them as UV and reject/mis-scale them.
  d.motionVectorScale = { 1.f, 1.f };
  d.jitterOffset = { 0.f, 0.f };
  d.frameTimeDelta = (dt > 0.f) ? dt : (1000.f / 60.f);
  d.preExposure = 1.f;
  d.reset = reset ? 1 : 0;
  d.cameraNear = 0.01f;
  d.cameraFar = 1000.f;
  d.cameraFovAngleVertical = 1.0f;
  d.viewSpaceToMetersFactor = 1.f;
  d.sharpness = m_sharpen ? m_sharpness : 0.f;
  d.flags = 0;

  ffxReturnCode_t rc = g_ffx.Dispatch(&m_ctx, &d.header);
  if (rc != FFX_API_RETURN_OK) {
    m_error = L"ffxDispatch 0x" + hex((uint32_t)rc);
    return false;
  }
  return true;
}

void Fsr::shutdown()
{
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = nullptr;
}
