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


// ---------------------------------------------------------------------------
// AMD FSR Frame Generation with the SDK's frame-interpolation swapchain.
// The swapchain owns pacing and invokes generationCallback during Present.
bool FsrFrameGeneration::init(ID3D12Device* device, Size maxRender, Size display,
                              IDXGISwapChain4** swapChain, ID3D12CommandQueue* queue)
{
  shutdown();
  if (!device || !swapChain || !*swapChain || !queue || !ensure() ||
      !g_ffx.CreateContext || !g_ffx.Dispatch || !g_ffx.DestroyContext ||
      !g_ffx.Configure || !g_ffx.Query) {
    m_error = L"FSR FG API unavailable (missing FFX context, query, dispatch or configure entry points)";
    return false;
  }
  ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
  wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
  wrap.header.pNext = nullptr;
  wrap.swapchain = swapChain;
  wrap.gameQueue = queue;
  ffxReturnCode_t rc = g_ffx.CreateContext(&m_swapChainCtx, &wrap.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_swapChainCtx || !*swapChain) {
    m_error = L"FSR FG swapchain wrapping failed 0x" + hex((uint32_t)rc);
    if (m_swapChainCtx) g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
    m_swapChainCtx = nullptr;
    return false;
  }
  m_swapChain = *swapChain;
  m_device = device;

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  backend.device = device;
  ffxCreateContextDescFrameGeneration fg{};
  fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  fg.header.pNext = &backend.header;
  fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
             FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
  fg.displaySize = { display.w, display.h };
  fg.maxRenderSize = { maxRender.w, maxRender.h };
  fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  rc = g_ffx.CreateContext(&m_ctx, &fg.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_ctx) {
    m_error = L"FSR FG context creation failed 0x" + hex((uint32_t)rc);
    if (m_swapChainCtx) g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
    m_swapChainCtx = nullptr;
    m_swapChain = nullptr;
    m_ctx = nullptr;
    return false;
  }
  m_maxRender = maxRender;
  m_display = display;
  m_frameId = 0;
  m_callbackEnabled = false;
  m_failed = false;
  m_error = L"FSR FG ready; AMD frame-interpolation swapchain active [" + g_info + L"]";
  return true;
}

ffxReturnCode_t FsrFrameGeneration::generationCallback(
    ffxDispatchDescFrameGeneration* params, void* userCtx)
{
  auto* self = static_cast<FsrFrameGeneration*>(userCtx);
  if (!self || !params || !g_ffx.Dispatch)
    return FFX_API_RETURN_ERROR_PARAMETER;
  std::lock_guard<std::mutex> lock(self->m_mutex);
  if (!self->m_ctx)
    return FFX_API_RETURN_ERROR_PARAMETER;
  // A provider can race a failure/toggle with Present. If generation was
  // disabled after configuration, leave the ordinary present path intact.
  if (!self->m_callbackEnabled)
    return FFX_API_RETURN_OK;
  const ffxReturnCode_t rc = g_ffx.Dispatch(&self->m_ctx, &params->header);
  if (rc != FFX_API_RETURN_OK) {
    self->m_failed = true;
    self->m_callbackEnabled = false;
    self->m_error = L"FSR FG generation callback failed 0x" + hex((uint32_t)rc);
  }
  return rc;
}

bool FsrFrameGeneration::resize(Size maxRender, Size display)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_swapChainCtx || !g_ffx.CreateContext || !g_ffx.DestroyContext) {
    m_error = L"FSR FG resize failed: swapchain context unavailable";
    return false;
  }
  m_callbackEnabled = false;
  m_failed = false;
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = nullptr;

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  // Recreate only the effect context; do not wrap an already wrapped swapchain.
  backend.device = m_device;
  ffxCreateContextDescFrameGeneration fg{};
  fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  fg.header.pNext = &backend.header;
  fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
             FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
  fg.displaySize = { display.w, display.h };
  fg.maxRenderSize = { maxRender.w, maxRender.h };
  fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  const ffxReturnCode_t rc = g_ffx.CreateContext(&m_ctx, &fg.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_ctx) {
    m_ctx = nullptr;
    m_error = L"FSR FG resize context creation failed 0x" + hex((uint32_t)rc);
    return false;
  }
  m_maxRender = maxRender;
  m_display = display;
  m_error = L"FSR FG context resized";
  return true;
}

bool FsrFrameGeneration::prepare(ID3D12GraphicsCommandList* cmd,
                                 ID3D12Resource* depth,
                                 ID3D12Resource* motionVectors,
                                 Size render, Size display,
                                 float dt, bool reset, bool enabled)
{
  if (failed()) {
    m_error = L"FSR FG disabled after generation callback failure";
    return false;
  }
  if (!m_ctx || !m_swapChainCtx || !g_ffx.Configure || !g_ffx.Dispatch || !cmd) {
    m_error = L"FSR FG prepare skipped: runtime or swapchain unavailable";
    return false;
  }
  if (render.w > m_maxRender.w || render.h > m_maxRender.h ||
      display.w != m_display.w || display.h != m_display.h) {
    m_error = L"FSR FG prepare skipped: resolution changed; recreate context";
    return false;
  }
  if (enabled && (!depth || !motionVectors)) {
    m_error = L"FSR FG prepare skipped: depth or motion vectors unavailable";
    enabled = false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const uint64_t frameId = m_frameId;
  ffxConfigureDescFrameGeneration config{};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.header.pNext = nullptr;
  config.swapChain = m_swapChain;
  config.presentCallback = nullptr;
  config.presentCallbackUserContext = nullptr;
  config.frameGenerationCallback = enabled ? &FsrFrameGeneration::generationCallback : nullptr;
  config.frameGenerationCallbackUserContext = enabled ? this : nullptr;
  config.frameGenerationEnabled = enabled;
  config.allowAsyncWorkloads = false;
  config.HUDLessColor = {};
  config.flags = 0;
  config.onlyPresentGenerated = false;
  config.generationRect = { 0, 0, (int32_t)display.w, (int32_t)display.h };
  config.frameID = frameId;
  ffxReturnCode_t rc = g_ffx.Configure(&m_ctx, &config.header);
  if (rc != FFX_API_RETURN_OK) {
    m_error = L"FSR FG configure failed 0x" + hex((uint32_t)rc);
    m_callbackEnabled = false;
    return false;
  }

  m_callbackEnabled = false;
  if (enabled) {
    ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
    camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    camera.header.pNext = nullptr;
    camera.cameraPosition[0] = camera.cameraPosition[1] = camera.cameraPosition[2] = 0.0f;
    camera.cameraUp[0] = 0.0f; camera.cameraUp[1] = 1.0f; camera.cameraUp[2] = 0.0f;
    camera.cameraRight[0] = 1.0f; camera.cameraRight[1] = 0.0f; camera.cameraRight[2] = 0.0f;
    camera.cameraForward[0] = 0.0f; camera.cameraForward[1] = 0.0f; camera.cameraForward[2] = 1.0f;
    ffxDispatchDescFrameGenerationPrepare prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prep.header.pNext = &camera.header;
    prep.frameID = frameId;
    prep.flags = 0;
    prep.commandList = cmd;
    prep.renderSize = { render.w, render.h };
    prep.jitterOffset = { 0.0f, 0.0f };
    prep.motionVectorScale = { 1.0f, 1.0f };
    prep.frameTimeDelta = dt > 0.0f ? dt : (1000.0f / 60.0f);
    prep.unused_reset = reset;
    prep.cameraNear = 0.01f;
    prep.cameraFar = 1000.0f;
    prep.cameraFovAngleVertical = 1.0f;
    prep.viewSpaceToMetersFactor = 1.0f;
    prep.depth = apiRes(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prep.motionVectors = apiRes(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    rc = g_ffx.Dispatch(&m_ctx, &prep.header);
    if (rc != FFX_API_RETURN_OK) {
      config.frameGenerationEnabled = false;
      config.frameGenerationCallback = nullptr;
      config.frameGenerationCallbackUserContext = nullptr;
      g_ffx.Configure(&m_ctx, &config.header);
      ++m_frameId;
      m_error = L"FSR FG prepare dispatch failed 0x" + hex((uint32_t)rc);
      return false;
    }
    m_callbackEnabled = true;
  }
  ++m_frameId;
  m_error = enabled ? L"FSR FG prepared for paced Present" : L"FSR FG disabled for this frame";
  return true;
}

bool FsrFrameGeneration::failed() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_failed;
}

void FsrFrameGeneration::shutdown()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_callbackEnabled = false;
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = nullptr;
  if (m_swapChainCtx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
  m_swapChainCtx = nullptr;
  m_swapChain = nullptr;
  m_device = nullptr;
  m_maxRender = {};
  m_display = {};
  m_frameId = 0;
}
