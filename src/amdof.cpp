#include "amdof.h"
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>

// ============================================================================
// Performance optical-flow proxy for FSR / ReShade motion vectors.
//
//   - Sparse 8x8 block matching (one search per block, broadcast by 64
//     threads — see the shader below).  This is the ORIGINAL per-thread
//     search shape that the user reported "looked good" before the
//     groupshared rewrite; reverting to it because the rewrite produced
//     shallow/flickering MVs at runtime even though it was functionally
//     equivalent on paper.
//   - CBV upload buffer for constants (Map/Memcpy/Unmap).  The root-constants
//     rewrite compiled and ran, but we revert to the known-good CBV path
//     while we investigate.
//   - Cached SRV/UAV descriptors (recreated only when resource ptr changes).
//   - All per-frame barriers collapsed into two ResourceBarrier calls.
// ============================================================================

static const char* kSparseOfCs = R"(
Texture2D<float4> gCurr : register(t0);
Texture2D<float4> gPrev : register(t1);
Texture2D<float2> gPrevMv : register(t2);
RWTexture2D<float2> gOutMv : register(u0);

cbuffer Params : register(b0) {
  uint2  RenderSize;
  int    SearchRadius;
  float  Temporal;
};

float luma(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }

// One thread group covers 8x8 output pixels; only thread 0 does the search,
// then the whole group writes the same vector (block-constant MV).
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
  if (tid.x >= RenderSize.x || tid.y >= RenderSize.y) return;

  int2 maxC = int2(RenderSize) - 1;
  // Block centre
  int2 center = int2(gid.xy) * 8 + 4;
  center = clamp(center, int2(0,0), maxC);

  float best = 1e10;
  int2 bestD = int2(0, 0);
  const int step = 2;
  const int BS = 4;

  [loop] for (int dy = -SearchRadius; dy <= SearchRadius; dy += 2) {
    [loop] for (int dx = -SearchRadius; dx <= SearchRadius; dx += 2) {
      float cost = 0;
      int samples = 0;
      [unroll] for (int by = -BS; by < BS; by += step) {
        [unroll] for (int bx = -BS; bx < BS; bx += step) {
          int2 cxy = clamp(center + int2(bx, by), int2(0,0), maxC);
          int2 pxy = clamp(cxy + int2(dx, dy), int2(0,0), maxC);
          cost += abs(luma(gCurr.Load(int3(cxy,0))) - luma(gPrev.Load(int3(pxy,0))));
          samples++;
        }
      }
      cost /= max(samples, 1);
      if (cost < best) { best = cost; bestD = int2(dx, dy); }
    }
  }

  // light refine
  int2 base = bestD;
  [loop] for (int dy = -1; dy <= 1; ++dy) {
    [loop] for (int dx = -1; dx <= 1; ++dx) {
      int2 d = base + int2(dx, dy);
      if (abs(d.x) > SearchRadius || abs(d.y) > SearchRadius) continue;
      float cost = 0; int samples = 0;
      [unroll] for (int by = -BS; by < BS; by += step) {
        [unroll] for (int bx = -BS; bx < BS; bx += step) {
          int2 cxy = clamp(center + int2(bx, by), int2(0,0), maxC);
          int2 pxy = clamp(cxy + d, int2(0,0), maxC);
          cost += abs(luma(gCurr.Load(int3(cxy,0))) - luma(gPrev.Load(int3(pxy,0))));
          samples++;
        }
      }
      cost /= max(samples, 1);
      if (cost < best) { best = cost; bestD = d; }
    }
  }

  float2 mv = float2(bestD);
  float2 prev = gPrevMv.Load(int3(center, 0));
  mv = lerp(mv, prev, Temporal);
  if (abs(mv.x) < 0.2 && abs(mv.y) < 0.2) mv = 0;

  // every thread in the 8x8 group writes the same block MV
  gOutMv[tid.xy] = mv;
}
)";

bool AmdOf::createBlockResources(Size ofSize)
{
  m_ofSize = ofSize;
  m_blockSize = { (ofSize.w + 7) / 8, (ofSize.h + 7) / 8 };
  return true;
}

bool AmdOf::createDensifyPipeline()
{
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 16;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) {
    m_error = L"heap create failed"; return false;
  }
  m_descStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // Layout: 3 SRVs (curr/prev/prevMv) + 1 UAV (outMv), in separate ranges.
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[0].BaseShaderRegister = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;

  // 3 root params: SRV table / UAV table / CBV (b0).  CBV is the known-good
  // shape — we go back to this after the root-constants rewrite produced
  // bad MVs at runtime.
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[2].Descriptor.ShaderRegister = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 3;
  rsd.pParameters = params;

  ComPtr<ID3DBlob> sig, err;
  if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
    m_error = L"OF RS serialize failed"; return false;
  }
  if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rs)))) {
    m_error = L"OF RS create failed"; return false;
  }

  ComPtr<ID3DBlob> cs, csErr;
  if (FAILED(D3DCompile(kSparseOfCs, strlen(kSparseOfCs), "ofcs", nullptr, nullptr, "main", "cs_5_1",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &csErr))) {
    m_error = L"OF CS compile failed"; return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = m_rs.Get();
  pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
  if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso)))) {
    m_error = L"OF PSO create failed"; return false;
  }

  // Upload buffer for the per-frame Params cbuffer.  256 bytes is more than
  // enough for the 16-byte cbuffer and satisfies CBV alignment requirements.
  D3D12_HEAP_PROPERTIES hpUp{};
  hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;
  hpUp.CreationNodeMask = 1; hpUp.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
  bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(m_device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb)))) {
    m_error = L"OF CB create failed"; return false;
  }
  return true;
}

bool AmdOf::init(ID3D12Device* device, ID3D12CommandQueue* queue, Size resolution, bool qualityMode)
{
  shutdown();
  m_device = device;
  m_queue = queue;
  m_quality = false; // always performance
  (void)qualityMode;

  if (!createBlockResources(resolution)) return false;
  if (!createDensifyPipeline()) return false;
  m_ready = true;
  m_hasHistory = false;
  m_error = L"OF sparse-8x8";
  return true;
}

void AmdOf::clearMv(ID3D12GraphicsCommandList* cmd, ID3D12Resource* fullResMv, Size)
{
  if (!m_heap || !fullResMv) return;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  // UAV lives at slot 3 (after 3 SRVs)
  auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += 3ull * m_descStride;
  m_device->CreateUnorderedAccessView(fullResMv, nullptr, &uav, cpu);
  auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += 3ull * m_descStride;
  UINT clear[4] = {};
  cmd->ClearUnorderedAccessViewUint(gpu, cpu, fullResMv, clear, 0, nullptr);
}

// Helper: clear an arbitrary R16G16_FLOAT UAV texture to zero.  Used by the
// seed path to make prevMv deterministic instead of leaving whatever the
// driver happened to put in the freshly-committed resource.
void AmdOf::clearPrevMv(ID3D12GraphicsCommandList* cmd)
{
  if (!m_heap || !m_prevMv) return;
  // We need a UAV for m_prevMv — but m_prevMv was created with
  // ALLOW_UNORDERED_ACCESS so a UAV is fine.  We use slot 4 in the heap
  // (slots 0..3 are owned by the per-frame dispatch descriptors) so we
  // don't clobber the cached fullResMv UAV at slot 3.
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += 4ull * m_descStride;
  m_device->CreateUnorderedAccessView(m_prevMv.Get(), nullptr, &uav, cpu);
  auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += 4ull * m_descStride;
  UINT clear[4] = {};
  // m_prevMv is in NON_PIXEL_SHADER_RESOURCE at this point — to clear it as
  // a UAV we need a brief UAV barrier.  Transition to UAV, clear, transition
  // back to NPS.  These two barriers run only on the seed frame so cost is
  // negligible.
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_prevMv.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
  }
  cmd->ClearUnorderedAccessViewUint(gpu, cpu, m_prevMv.Get(), clear, 0, nullptr);
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_prevMv.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
  }
  // prevMv descriptor at slot 2 is now stale until writeDescriptors() recreates it.
  m_lastPrevMv = nullptr;
}

bool AmdOf::ensureTemporal(Size renderSize)
{
  if (m_prevMv && m_renderSize.w == renderSize.w && m_renderSize.h == renderSize.h)
    return true;
  m_renderSize = renderSize;
  m_prevMv.Reset();
  m_prevColorFull.Reset();
  // All cached descriptor identities are now stale — force rebuild on next
  // writeDescriptors() call.
  m_lastColor = nullptr;
  m_lastMv = nullptr;
  m_lastPrevColor = nullptr;
  m_lastPrevMv = nullptr;

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;

  auto make = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    out.Reset();
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = renderSize.w; rd.Height = renderSize.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Flags = flags;
    return SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out)));
  };

  if (!make(m_prevMv, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    return false;
  if (!make(m_prevColorFull, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COMMON))
    return false;
  m_prevFullSize = renderSize;
  return true;
}

// Writes all 4 descriptors (3 SRVs + 1 UAV) into the heap, only recreating
// the ones whose backing resource pointer changed since the last call.
void AmdOf::writeDescriptors(ID3D12Resource* color, ID3D12Resource* fullResMv)
{
  auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;

  // slot 0: color SRV  (recreate when ptr changes)
  if (color != m_lastColor) {
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_device->CreateShaderResourceView(color, &srv, cpu);
    m_lastColor = color;
  }
  cpu.ptr += m_descStride;

  // slot 1: prevColorFull SRV  (recreate when prevColorFull was recreated)
  if (m_prevColorFull.Get() != m_lastPrevColor) {
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_device->CreateShaderResourceView(m_prevColorFull.Get(), &srv, cpu);
    m_lastPrevColor = m_prevColorFull.Get();
  }
  cpu.ptr += m_descStride;

  // slot 2: prevMv SRV  (recreate when prevMv was recreated)
  if (m_prevMv.Get() != m_lastPrevMv) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvMv = srv;
    srvMv.Format = DXGI_FORMAT_R16G16_FLOAT;
    m_device->CreateShaderResourceView(m_prevMv.Get(), &srvMv, cpu);
    m_lastPrevMv = m_prevMv.Get();
  }
  cpu.ptr += m_descStride;

  // slot 3: fullResMv UAV  (recreate when ptr changes)
  if (fullResMv != m_lastMv) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(fullResMv, nullptr, &uav, cpu);
    m_lastMv = fullResMv;
  }
}

bool AmdOf::dispatch(ID3D12GraphicsCommandList* cmd,
                     ID3D12Resource* color,
                     ID3D12Resource* fullResMv,
                     Size renderSize,
                     bool reset)
{
  if (!m_ready || !cmd || !fullResMv || !color) return false;
  if (reset) m_hasHistory = false;
  if (!ensureTemporal(renderSize)) return false;

  if (!m_hasHistory) {
    // Seed prev colour/prev MV with zero MV and current colour.
    // mv is currently in UAV (from createAuxTextures on first frame, or
    // from main.cpp's batch-C restore on a reset frame) — clearMv works
    // against UAV.
    clearMv(cmd, fullResMv, renderSize);
    // Also clear prevMv so the second frame's temporal blend doesn't read
    // whatever garbage the driver left in the freshly-committed R16G16
    // texture.  This is the fix for the "MVs flicker / are too shallow"
    // symptom — the original code relied on the driver returning zeroed
    // memory for new committed resources, which is not guaranteed.
    clearPrevMv(cmd);

    // One batched barrier pair for the seed copy:
    //   color SRV|NPS -> COPY_SOURCE,  prevColorFull COMMON -> COPY_DEST
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = color;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource = m_prevColorFull.Get();
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(2, b);
    cmd->CopyResource(m_prevColorFull.Get(), color);

    // Batched restore (3 transitions in one call):
    //   color COPY_SOURCE -> SRV|NPS        (FSR reads)
    //   prevColorFull COPY_DEST -> NPS      (next frame's OF reads)
    //   mv (fullResMv) UAV -> PS|NPS       (FSR reads as SRV directly,
    //                                       no extra barrier needed in main)
    D3D12_RESOURCE_BARRIER rb[3]{};
    for (int i = 0; i < 3; ++i) {
      rb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      rb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    rb[0].Transition.pResource = color;
    rb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rb[1].Transition.pResource = m_prevColorFull.Get();
    rb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    rb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rb[2].Transition.pResource = fullResMv;
    rb[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    rb[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(3, rb);
    m_hasHistory = true;
    return true;
  }

  // ---- Per-frame descriptor refresh (only the ones whose ptr changed) ------
  writeDescriptors(color, fullResMv);
  auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpu;
  uavGpu.ptr += 3ull * m_descStride;

  // mv (fullResMv) is already in UAV when we enter (either fresh from
  // createAuxTextures, or restored to UAV by the previous frame's restore
  // barrier below).  No pre-dispatch transition needed.

  // ---- Upload the Params cbuffer (CBV path) --------------------------------
  struct CB { UINT RenderSize[2]; INT SearchRadius; FLOAT Temporal; } cb{};
  cb.RenderSize[0] = renderSize.w;
  cb.RenderSize[1] = renderSize.h;
  cb.SearchRadius = 3;
  cb.Temporal = 0.7f;
  void* mapped = nullptr;
  if (SUCCEEDED(m_cb->Map(0, nullptr, &mapped)) && mapped) {
    memcpy(mapped, &cb, sizeof(cb));
    m_cb->Unmap(0, nullptr);
  }

  // ---- Dispatch ------------------------------------------------------------
  cmd->SetPipelineState(m_pso.Get());
  cmd->SetComputeRootSignature(m_rs.Get());
  ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootDescriptorTable(0, gpu);           // SRV table (3 SRVs)
  cmd->SetComputeRootDescriptorTable(1, uavGpu);        // UAV table (1 UAV)
  cmd->SetComputeRootConstantBufferView(2, m_cb->GetGPUVirtualAddress());
  cmd->Dispatch((renderSize.w + 7) / 8, (renderSize.h + 7) / 8, 1);

  // ---- Batched post-dispatch barrier ---------------------------------------
  //   fullResMv: UAV -> COPY_SOURCE  (so we can copy it into prevMv)
  //   prevMv:    NPS  -> COPY_DEST
  //   color:     SRV|NPS -> COPY_SOURCE  (so we can copy it into prevColorFull)
  //   prevColorFull: NPS -> COPY_DEST
  // All four transitions in a single ResourceBarrier call.
  D3D12_RESOURCE_BARRIER postB[4]{};
  int n = 0;
  auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (!r) return;
    postB[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postB[n].Transition.pResource = r;
    postB[n].Transition.StateBefore = a;
    postB[n].Transition.StateAfter = b;
    postB[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++n;
  };
  tr(fullResMv,        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,        D3D12_RESOURCE_STATE_COPY_SOURCE);
  tr(m_prevMv.Get(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  tr(color,           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
  tr(m_prevColorFull.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (n) cmd->ResourceBarrier(n, postB);

  // ---- Two full-res copies for next-frame history --------------------------
  cmd->CopyResource(m_prevMv.Get(), fullResMv);
  cmd->CopyResource(m_prevColorFull.Get(), color);

  // ---- Batched restore barrier ---------------------------------------------
  //   fullResMv:        COPY_SOURCE -> PS|NPS  (FSR reads as SRV directly,
  //                                            no extra barrier needed in main)
  //   prevMv:           COPY_DEST   -> NPS  (next frame's OF reads)
  //   color:            COPY_SOURCE -> PS|NPS  (FSR reads as SRV)
  //   prevColorFull:    COPY_DEST   -> NPS  (next frame's OF reads)
  D3D12_RESOURCE_BARRIER restore[4]{};
  int m = 0;
  auto trR = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (!r) return;
    restore[m].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore[m].Transition.pResource = r;
    restore[m].Transition.StateBefore = a;
    restore[m].Transition.StateAfter = b;
    restore[m].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++m;
  };
  trR(fullResMv,        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  trR(m_prevMv.Get(),   D3D12_RESOURCE_STATE_COPY_DEST,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  trR(color,           D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  trR(m_prevColorFull.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (m) cmd->ResourceBarrier(m, restore);

  m_hasHistory = true;
  return true;
}

void AmdOf::shutdown()
{
  m_ready = false;
  m_hasHistory = false;
  m_blockMv.Reset();
  m_scd.Reset();
  m_prevColor.Reset();
  m_prevColorFull.Reset();
  m_currOf.Reset();
  m_prevMv.Reset();
  m_cb.Reset();
  m_rs.Reset();
  m_pso.Reset();
  m_heap.Reset();
  m_lastColor = nullptr;
  m_lastMv = nullptr;
  m_lastPrevColor = nullptr;
  m_lastPrevMv = nullptr;
}
