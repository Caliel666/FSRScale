#include "graphics.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <shlobj.h>
#include <filesystem>
#include <stdexcept>

static void hr(HRESULT x) { if (FAILED(x)) throw std::runtime_error("D3D12 failure"); }

// Fullscreen triangle, sample texture, write to RTV. Handles any src size.
static const char* kBlitHlsl = R"(
Texture2D    Tex : register(t0);
SamplerState Samp : register(s0);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv  = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  return Tex.SampleLevel(Samp, i.uv, 0);
}
)";

bool Graphics::createBlitPipeline()
{
  ComPtr<ID3DBlob> vs, ps, err, rsBlob;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr,
                        "VSMain", "vs_5_0", flags, 0, &vs, &err)))
    return false;
  if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr,
                        "PSMain", "ps_5_0", flags, 0, &ps, &err)))
    return false;

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER param{};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable.NumDescriptorRanges = 1;
  param.DescriptorTable.pDescriptorRanges = &range;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.ShaderRegister = 0;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 1;
  rsd.pParameters = &param;
  rsd.NumStaticSamplers = 1;
  rsd.pStaticSamplers = &samp;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  hr(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr));
  hr(m_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                IID_PPV_ARGS(&m_blitRs)));

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
  psd.pRootSignature = m_blitRs.Get();
  psd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
  psd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
  psd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
  psd.SampleMask = UINT_MAX;
  psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psd.DepthStencilState.DepthEnable = FALSE;
  psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psd.NumRenderTargets = 1;
  psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psd.SampleDesc.Count = 1;
  hr(m_dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_blitPso)));

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 8;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr(m_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));
  m_srvStride = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

void Graphics::blitToBackbuffer(ID3D12Resource* src)
{
  if (!src || !m_blitPso) return;

  // SRV for src — recreate only when the source resource pointer or format
  // changes (color and upscale are the only two callers, so this caches
  // stably most frames).
  D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
  if (src != m_lastBlitSrc || srcDesc.Format != m_lastBlitFmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = srcDesc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_dev->CreateShaderResourceView(src, &srv, cpu);
    m_lastBlitSrc = src;
    m_lastBlitFmt = srcDesc.Format;
  }

  ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
  m_cmd->SetDescriptorHeaps(1, heaps);
  m_cmd->SetGraphicsRootSignature(m_blitRs.Get());
  m_cmd->SetPipelineState(m_blitPso.Get());
  m_cmd->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

  D3D12_VIEWPORT vp{ 0, 0, (float)m_display.w, (float)m_display.h, 0, 1 };
  D3D12_RECT sc{ 0, 0, (LONG)m_display.w, (LONG)m_display.h };
  m_cmd->RSSetViewports(1, &vp);
  m_cmd->RSSetScissorRects(1, &sc);

  auto rtv = rtvHandle();
  m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_cmd->DrawInstanced(3, 1, 0, 0);
}

bool Graphics::createAuxTextures(Size render)
{
  if (render.w == 0 || render.h == 0) return false;
  m_render = render;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  hp.CreationNodeMask = 1;
  hp.VisibleNodeMask = 1;

  m_dummyDepth.Reset();
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_dummyDepth)));
  }
  m_motionVectors.Reset();
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_motionVectors)));
  }
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_reactiveMask)));
  }

  if (!m_clearGpuHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC gpu{};
    gpu.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpu.NumDescriptors = 1;
    gpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr(m_dev->CreateDescriptorHeap(&gpu, IID_PPV_ARGS(&m_clearGpuHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC cpu = gpu;
    cpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr(m_dev->CreateDescriptorHeap(&cpu, IID_PPV_ARGS(&m_clearCpuHeap)));
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
  uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uv.Format = DXGI_FORMAT_R32_FLOAT;
  m_dev->CreateUnorderedAccessView(m_dummyDepth.Get(), nullptr, &uv,
                                   m_clearGpuHeap->GetCPUDescriptorHandleForHeapStart());
  m_dev->CreateUnorderedAccessView(m_dummyDepth.Get(), nullptr, &uv,
                                   m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart());

  return true;
}

bool Graphics::ensureAuxTextures(Size render)
{
  if (m_dummyDepth && m_motionVectors && m_render.w == render.w && m_render.h == render.h)
    return true;
  return createAuxTextures(render);
}

bool Graphics::buildSwapChain(Size display)
{
  DXGI_SWAP_CHAIN_DESC1 s{};
  s.Width = display.w; s.Height = display.h;
  s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  s.BufferCount = 3;
  s.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  s.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  s.SampleDesc.Count = 1;
  s.Scaling = DXGI_SCALING_STRETCH;

  ComPtr<IDXGISwapChain1> sw;
  hr(m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_output, &s, nullptr, nullptr, &sw));
  m_swap1 = sw;
  hr(m_swap1.As(&m_swap));
  m_factory->MakeWindowAssociation(m_output, DXGI_MWA_NO_ALT_ENTER);

  m_display = display;
  m_index = m_swap->GetCurrentBackBufferIndex();

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.NumDescriptors = 3;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  if (!m_rtv) hr(m_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtv)));
  m_rtvStride = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_rtvBase = m_rtv->GetCPUDescriptorHandleForHeapStart();

  for (int i = 0; i < 3; i++) {
    m_back[i].Reset();
    hr(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i])));
    auto h = m_rtvBase; h.ptr += (SIZE_T)i * m_rtvStride;
    m_dev->CreateRenderTargetView(m_back[i].Get(), nullptr, h);
    if (!m_alloc[i])
      hr(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i])));
  }

  m_upscaleOutput.Reset();
  {
    D3D12_RESOURCE_DESC od{};
    od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    od.Width = display.w; od.Height = display.h;
    od.DepthOrArraySize = 1; od.MipLevels = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES ohp{};
    ohp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ohp.CreationNodeMask = 1; ohp.VisibleNodeMask = 1;
    hr(m_dev->CreateCommittedResource(&ohp, D3D12_HEAP_FLAG_NONE, &od,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_upscaleOutput)));
  }
  return true;
}

bool Graphics::adoptSwapChain(IDXGISwapChain4* wrapped)
{
  if (!wrapped || !m_rtv || !m_dev) return false;
  for (auto& b : m_back) b.Reset();
  m_swap1.Reset();
  m_swap.Reset();
  m_swap = wrapped;
  if (FAILED(m_swap.As(&m_swap1))) return false;
  m_index = m_swap->GetCurrentBackBufferIndex();
  for (UINT i = 0; i < 3; ++i) {
    if (FAILED(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i]))))
      return false;
    auto h = m_rtvBase;
    h.ptr += (SIZE_T)i * m_rtvStride;
    m_dev->CreateRenderTargetView(m_back[i].Get(), nullptr, h);
  }
  return true;
}

bool Graphics::resize(Size display)
{
  if (display.w == m_display.w && display.h == m_display.h) return true;
  for (auto& b : m_back) b.Reset();
  m_upscaleOutput.Reset();
  m_swap1.Reset(); m_swap.Reset();
  m_lastBlitSrc = nullptr;     // cached SRV is stale after swapchain rebuild
  m_lastBlitFmt = DXGI_FORMAT_UNKNOWN;
  return buildSwapChain(display);
}

bool Graphics::init(HWND output, Size render, Size display)
{
  m_output = output;
  ComPtr<IDXGIFactory6> f;
  hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
  m_factory = f;

  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0;
       f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
    if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
    a.Reset();
  }
  hr(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_dev)));

  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr(m_dev->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue)));

  buildSwapChain(display);
  createAuxTextures(render.w ? render : display);
  createBlitPipeline();

  hr(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(),
                              nullptr, IID_PPV_ARGS(&m_cmd)));
  hr(m_cmd->Close());
  hr(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return true;
}

bool Graphics::begin()
{
  m_alloc[m_index]->Reset();
  m_cmd->Reset(m_alloc[m_index].Get(), nullptr);

  // The capture pipeline has no real game depth. Always clear the synthetic
  // depth to deterministic far depth before FSR sees it; an uninitialized
  // UAV here was a major source of temporal instability/smearing.
  if (m_dummyDepth && m_clearGpuHeap && m_clearCpuHeap) {
    ID3D12DescriptorHeap* heaps[] = { m_clearGpuHeap.Get() };
    m_cmd->SetDescriptorHeaps(1, heaps);
    const FLOAT value[4] = { 0.f, 0.f, 0.f, 0.f };
    m_cmd->ClearUnorderedAccessViewFloat(
      m_clearGpuHeap->GetGPUDescriptorHandleForHeapStart(),
      m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart(),
      m_dummyDepth.Get(), value, 0, nullptr);
  }
  return true;
}

void Graphics::end() { m_cmd->Close(); }

bool Graphics::captureBackbufferScreenshot(const std::wstring& folder, uint64_t frameIndex)
{
  if (!m_cmd || !m_back[m_index] || m_screenshotPending)
    return false;

  ID3D12Resource* back = m_back[m_index].Get();
  const D3D12_RESOURCE_DESC desc = back->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width == 0 || desc.Height == 0)
    return false;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  UINT64 totalBytes = 0;
  m_dev->GetCopyableFootprints(&desc, 0, 1, 0,
                               &footprint, &numRows, &rowSize, &totalBytes);
  if (totalBytes == 0 || numRows == 0)
    return false;

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = totalBytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> readback;
  if (FAILED(m_dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readback))))
    return false;

  D3D12_RESOURCE_BARRIER toCopy{};
  toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toCopy.Transition.pResource = back;
  toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  m_cmd->ResourceBarrier(1, &toCopy);

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = back;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;

  m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER toRender{};
  toRender.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRender.Transition.pResource = back;
  toRender.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toRender.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  m_cmd->ResourceBarrier(1, &toRender);

  m_screenshotReadback = readback;
  m_screenshotFootprint = footprint;
  m_screenshotWidth = (UINT)desc.Width;
  m_screenshotHeight = (UINT)desc.Height;
  m_screenshotFolder = folder;
  m_screenshotFrame = frameIndex;
  m_screenshotPending = true;
  return true;
}

static bool writeScreenshotPng(const std::wstring& folder,
                               uint64_t frameIndex,
                               UINT width,
                               UINT height,
                               UINT rowPitch,
                               const BYTE* pixels)
{
  if (!pixels || width == 0 || height == 0)
    return false;

  std::wstring dir = folder;
  if (dir.empty()) {
    wchar_t p[MAX_PATH * 4]{};
    SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, p);
    dir = p;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"NRLive";
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return false;

  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[128]{};
  swprintf_s(name, L"NRLive_%04u%02u%02u_%02u%02u%02u_%llu.png",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             (unsigned long long)frameIndex);
  const std::wstring file = dir + L"\\" + name;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninit = SUCCEEDED(hr);

  ComPtr<IWICImagingFactory> fac;
  ComPtr<IWICBitmap> bmp;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;

  bool ok = false;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) break;
    if (FAILED(fac->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppRGBA,
          rowPitch, rowPitch * height,
          const_cast<BYTE*>(pixels), &bmp))) break;
    if (FAILED(fac->CreateStream(&stream))) break;
    if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) break;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) break;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) break;
    if (FAILED(frame->Initialize(nullptr))) break;
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) break;
    if (FAILED(frame->Commit())) break;
    if (FAILED(enc->Commit())) break;
    ok = true;
  } while (false);

  if (uninit)
    CoUninitialize();
  return ok;
}

bool Graphics::present()
{
  ID3D12CommandList* lists[] = { m_cmd.Get() };
  m_queue->ExecuteCommandLists(1, lists);
  // Sync interval 0 = no vsync wait on CPU.
  m_swap->Present(0, 0);

  // Frame buffering: only wait on a fence from maxFrames-1 ago so the CPU
  // never drains the GPU every frame (was the main stutter source).
  const uint64_t v = ++m_fenceValue;
  m_queue->Signal(m_fence.Get(), v);
  m_frameFence[m_index] = v;

  // A screenshot is synchronized only on the frame that requested it.
  // This guarantees the PNG contains the post-FSR backbuffer while normal
  // frames retain the existing 3-buffered, non-blocking path.
  if (m_screenshotPending) {
    if (m_fence->GetCompletedValue() < v) {
      m_fence->SetEventOnCompletion(v, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 1000);
    }

    if (m_fence->GetCompletedValue() >= v && m_screenshotReadback) {
      void* mapped = nullptr;
      D3D12_RANGE range{0, (SIZE_T)(m_screenshotFootprint.Footprint.RowPitch * m_screenshotHeight)};
      if (SUCCEEDED(m_screenshotReadback->Map(0, &range, &mapped)) && mapped) {
        const BYTE* base = static_cast<const BYTE*>(mapped) + m_screenshotFootprint.Offset;
        writeScreenshotPng(m_screenshotFolder, m_screenshotFrame,
                           m_screenshotWidth, m_screenshotHeight,
                           m_screenshotFootprint.Footprint.RowPitch, base);
        D3D12_RANGE written{0, 0};
        m_screenshotReadback->Unmap(0, &written);
      }
    }

    m_screenshotReadback.Reset();
    m_screenshotPending = false;
    m_screenshotFolder.clear();
    m_screenshotFrame = 0;
  }

  const UINT bufCount = 3; // match swap chain BufferCount
  if (m_fenceValue >= bufCount) {
    const uint64_t waitFor = m_fenceValue - (bufCount - 1);
    if (m_fence->GetCompletedValue() < waitFor) {
      m_fence->SetEventOnCompletion(waitFor, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 1000);
    }
  }

  m_index = m_swap->GetCurrentBackBufferIndex();
  // Wait only if this backbuffer is still in flight
  const uint64_t bbFence = m_frameFence[m_index];
  if (bbFence && m_fence->GetCompletedValue() < bbFence) {
    m_fence->SetEventOnCompletion(bbFence, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, 1000);
  }
  return true;
}

bool Graphics::waitForGpu()
{
  // Close any in-flight command list, execute it, then CPU-wait on a fresh
  // fence value so every previously submitted command has completed on the
  // GPU before the caller starts releasing D3D12 resources.
  if (!m_queue || !m_fence) return false;

  // If the command list is open, close + execute it so the GPU sees it.
  if (m_cmd) {
    HRESULT hrClose = m_cmd->Close();
    if (SUCCEEDED(hrClose)) {
      ID3D12CommandList* lists[] = { m_cmd.Get() };
      m_queue->ExecuteCommandLists(1, lists);
    }
    // Reset the allocator + list so the next begin() reuses cleanly.
    if (m_alloc[m_index]) {
      m_alloc[m_index]->Reset();
      m_cmd->Reset(m_alloc[m_index].Get(), nullptr);
      m_cmd->Close();
    }
  }

  const uint64_t v = ++m_fenceValue;
  m_queue->Signal(m_fence.Get(), v);
  if (m_fence->GetCompletedValue() < v) {
    if (m_fenceEvent) {
      m_fence->SetEventOnCompletion(v, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 5000);  // 5s safety timeout
    }
  }
  return true;
}
