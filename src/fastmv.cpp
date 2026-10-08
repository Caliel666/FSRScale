#include "fastmv.h"
#include "fastmv_shaders.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace {

struct Constants {
    uint32_t sizeW = 0, sizeH = 0;
    uint32_t gridW = 0, gridH = 0;
    uint32_t coarseW = 0, coarseH = 0;
    uint32_t radius = 0, flags = 0;
    float strayWeight = 0.0025f;
    float candidateBias = 0.0010f;
    float fastMotion = 10.0f;
    float distrustThreshold = 0.035f;
    uint32_t pad0 = 0, pad1 = 0;
};
static_assert(sizeof(Constants) == 56, "FastMv constants must stay 14 DWORDs");

static void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r,
                       D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    if (!r || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

static bool compileCs(ID3D12Device* dev, const char* source,
                      const char* name, ID3D12RootSignature* rs,
                      ComPtr<ID3D12PipelineState>& out)
{
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(source, strlen(source), name, nullptr, nullptr,
                            "main", "cs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs;
    p.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    return SUCCEEDED(dev->CreateComputePipelineState(&p, IID_PPV_ARGS(&out)));
}

} // namespace

bool FastMv::createPipeline()
{
    D3D12_DESCRIPTOR_RANGE srv{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 3;
    srv.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE uav{};
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 2;
    uav.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srv;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uav;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = sizeof(Constants) / sizeof(uint32_t);

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &blob, &err)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rs))))
        return false;

    auto compile = [&](const char* src, const char* name,
                       ComPtr<ID3D12PipelineState>& pso)
    {
        const std::string text = std::string(FastMvShaders::Common) + src;
        return compileCs(m_device, text.c_str(), name, m_rs.Get(), pso);
    };

    if (!compile(FastMvShaders::Luma, "fastmv_luma", m_lumaPso) ||
        !compile(FastMvShaders::Down, "fastmv_down", m_downPso) ||
        !compile(FastMvShaders::Search, "fastmv_search", m_searchPso) ||
        !compile(FastMvShaders::Median, "fastmv_median", m_medianPso) ||
        !compile(FastMvShaders::Pixel, "fastmv_pixel", m_pixelPso))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 128;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap))))
        return false;

    m_stride = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool FastMv::init(ID3D12Device* device, Size resolution)
{
    shutdown();
    m_device = device;
    m_render = resolution;

    if (!m_device || !createPipeline())
    {
        m_error = L"fast MV pipeline creation failed";
        return false;
    }

    m_ready = true;
    m_error = L"Fast MV: pyramidal coarse-to-fine + confidence/reactive mask";
    return true;
}

bool FastMv::ensureResources(Size rs)
{
    if (m_luma[0][0] && m_render.w == rs.w && m_render.h == rs.h)
        return true;

    for (auto& level : m_luma)
        for (auto& r : level) r.Reset();
    for (auto& r : m_grid) r.Reset();
    m_filtered.Reset();

    m_render = rs;
    m_hasHistory = false;
    m_current = 0;

    m_levels = 1;
    m_lw[0] = rs.w;
    m_lh[0] = rs.h;

    while (m_levels < kMaxLevels)
    {
        const uint32_t nw = (m_lw[m_levels - 1] + 1) / 2;
        const uint32_t nh = (m_lh[m_levels - 1] + 1) / 2;
        if (nw < 64 || nh < 32)
            break;
        m_lw[m_levels] = nw;
        m_lh[m_levels] = nh;
        ++m_levels;
    }
    if (m_levels < 2)
    {
        m_error = L"fast MV requires at least a 2-level pyramid";
        return false;
    }

    for (int k = 0; k < m_levels; ++k)
    {
        m_gw[k] = (m_lw[k] + 3) / 4;
        m_gh[k] = (m_lh[k] + 3) / 4;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto make = [&](ComPtr<ID3D12Resource>& out, uint32_t w,
                    uint32_t h, DXGI_FORMAT format)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&out)));
    };

    for (int b = 0; b < 2; ++b)
        for (int k = 0; k < m_levels; ++k)
            if (!make(m_luma[b][k], m_lw[k], m_lh[k], DXGI_FORMAT_R16_FLOAT))
                return false;

    for (int k = 1; k < m_levels; ++k)
        if (!make(m_grid[k], m_gw[k], m_gh[k], DXGI_FORMAT_R16G16_FLOAT))
            return false;

    if (!make(m_filtered, m_gw[1], m_gh[1], DXGI_FORMAT_R16G16_FLOAT))
        return false;

    return true;
}

void FastMv::bindDescriptors(ID3D12GraphicsCommandList* cmd,
                             ID3D12Resource* const srv[3],
                             const DXGI_FORMAT srvFormat[3],
                             ID3D12Resource* const uav[2],
                             const DXGI_FORMAT uavFormat[2],
                             UINT& cursor)
{
    if (cursor + 5 > 128)
        return;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(cursor) * m_stride;
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(cursor) * m_stride;

    for (int i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Format = srvFormat[i];
        s.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(srv[i], &s, cpu);
        cpu.ptr += m_stride;
    }

    for (int i = 0; i < 2; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Format = uavFormat[i];
        m_device->CreateUnorderedAccessView(uav[i], nullptr, &u, cpu);
        cpu.ptr += m_stride;
    }

    cmd->SetComputeRootDescriptorTable(0, gpu);

    gpu.ptr += UINT64(3) * m_stride;
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cursor += 5;
}

void FastMv::setConstants(ID3D12GraphicsCommandList* cmd,
                          const uint32_t* values, UINT count)
{
    cmd->SetComputeRoot32BitConstants(2, count, values, 0);
}

void FastMv::dispatchPass(ID3D12GraphicsCommandList* cmd,
                          ID3D12PipelineState* pso,
                          ID3D12Resource* const srv[3],
                          const DXGI_FORMAT srvFormat[3],
                          ID3D12Resource* const uav[2],
                          const DXGI_FORMAT uavFormat[2],
                          const uint32_t* constants, UINT constantCount,
                          uint32_t width, uint32_t height,
                          UINT& cursor)
{
    bindDescriptors(cmd, srv, srvFormat, uav, uavFormat, cursor);
    setConstants(cmd, constants, constantCount);
    cmd->SetPipelineState(pso);
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

bool FastMv::dispatch(ID3D12GraphicsCommandList* cmd,
                      ID3D12Resource* color,
                      ID3D12Resource* fullResMv,
                      ID3D12Resource* reactive,
                      Size renderSize,
                      bool reset)
{
    if (!m_ready || !cmd || !color || !fullResMv || !reactive)
        return false;
    if (!ensureResources(renderSize))
        return false;

    if (reset)
        m_hasHistory = false;

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rs.Get());

    const int cur = m_current;
    const int prev = 1 - cur;
    UINT cursor = 0;

    auto makeConstants = [&](uint32_t w, uint32_t h,
                             uint32_t gw, uint32_t gh,
                             uint32_t cw, uint32_t ch,
                             uint32_t radius, uint32_t flags)
    {
        Constants c{};
        c.sizeW = w; c.sizeH = h;
        c.gridW = gw; c.gridH = gh;
        c.coarseW = cw; c.coarseH = ch;
        c.radius = radius;
        c.flags = flags;
        return c;
    };

    // 1. Full-resolution luminance.
    {
        transition(cmd, m_luma[cur][0].Get(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto c = makeConstants(renderSize.w, renderSize.h,
                               renderSize.w, renderSize.h,
                               renderSize.w, renderSize.h, 0, 0);
        ID3D12Resource* s[3] = { color, nullptr, nullptr };
        DXGI_FORMAT sf[3] = { DXGI_FORMAT_B8G8R8A8_UNORM,
                              DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
        ID3D12Resource* u[2] = { m_luma[cur][0].Get(), nullptr };
        DXGI_FORMAT uf[2] = { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT };
        dispatchPass(cmd, m_lumaPso.Get(), s, sf, u, uf,
                     reinterpret_cast<const uint32_t*>(&c),
                     sizeof(c) / sizeof(uint32_t),
                     renderSize.w, renderSize.h, cursor);

        transition(cmd, m_luma[cur][0].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // 2. Native -> half -> quarter ... brightness pyramid.
    for (int k = 1; k < m_levels; ++k)
    {
        transition(cmd, m_luma[cur][k].Get(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto c = makeConstants(m_lw[k], m_lh[k],
                               m_lw[k], m_lh[k],
                               m_lw[k-1], m_lh[k-1], 0, 0);
        ID3D12Resource* s[3] = { m_luma[cur][k-1].Get(), nullptr, nullptr };
        DXGI_FORMAT sf[3] = { DXGI_FORMAT_R16_FLOAT,
                              DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
        ID3D12Resource* u[2] = { m_luma[cur][k].Get(), nullptr };
        DXGI_FORMAT uf[2] = { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT };

        dispatchPass(cmd, m_downPso.Get(), s, sf, u, uf,
                     reinterpret_cast<const uint32_t*>(&c),
                     sizeof(c) / sizeof(uint32_t),
                     m_lw[k], m_lh[k], cursor);

        transition(cmd, m_luma[cur][k].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (m_hasHistory)
    {
        // 3. Coarse-to-fine block search. Only one vector is estimated per
        // 4x4 block; the expensive search therefore scales with blocks, not
        // full-resolution pixels.
        for (int k = m_levels - 1; k >= 1; --k)
        {
            transition(cmd, m_grid[k].Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const bool hasCoarse = (k + 1 < m_levels);
            auto c = makeConstants(
                m_lw[k], m_lh[k], m_gw[k], m_gh[k],
                hasCoarse ? m_gw[k+1] : m_gw[k],
                hasCoarse ? m_gh[k+1] : m_gh[k],
                hasCoarse ? 2u : 4u,
                hasCoarse ? 1u : 0u);

            ID3D12Resource* s[3] = {
                m_luma[cur][k].Get(),
                m_luma[prev][k].Get(),
                hasCoarse ? m_grid[k+1].Get() : nullptr
            };
            DXGI_FORMAT sf[3] = {
                DXGI_FORMAT_R16_FLOAT,
                DXGI_FORMAT_R16_FLOAT,
                DXGI_FORMAT_R16G16_FLOAT
            };
            ID3D12Resource* u[2] = { m_grid[k].Get(), nullptr };
            DXGI_FORMAT uf[2] = {
                DXGI_FORMAT_R16G16_FLOAT,
                DXGI_FORMAT_R32_FLOAT
            };

            dispatchPass(cmd, m_searchPso.Get(), s, sf, u, uf,
                         reinterpret_cast<const uint32_t*>(&c),
                         sizeof(c) / sizeof(uint32_t),
                         m_gw[k], m_gh[k], cursor);

            transition(cmd, m_grid[k].Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // 4. Median the half-resolution vectors. This removes isolated bad
        // matches before they can influence the full-resolution resolve.
        transition(cmd, m_filtered.Get(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto c = makeConstants(m_gw[1], m_gh[1],
                               m_gw[1], m_gh[1],
                               m_gw[1], m_gh[1], 0, 0);
        ID3D12Resource* s[3] = { m_grid[1].Get(), nullptr, nullptr };
        DXGI_FORMAT sf[3] = {
            DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16_FLOAT,
            DXGI_FORMAT_R16G16_FLOAT
        };
        ID3D12Resource* u[2] = { m_filtered.Get(), nullptr };
        DXGI_FORMAT uf[2] = {
            DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16_FLOAT
        };

        dispatchPass(cmd, m_medianPso.Get(), s, sf, u, uf,
                     reinterpret_cast<const uint32_t*>(&c),
                     sizeof(c) / sizeof(uint32_t),
                     m_gw[1], m_gh[1], cursor);

        transition(cmd, m_filtered.Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // 5. Per-pixel resolve. Static pixels explicitly prefer zero motion.
    // The same confidence value becomes an FSR reactive mask, so mismatched
    // effects/HUD regions don't drag stale history through the frame.
    {
        auto c = makeConstants(renderSize.w, renderSize.h,
                               m_gw[1], m_gh[1],
                               m_gw[1], m_gh[1], 0,
                               m_hasHistory ? 1u : 0u);

        ID3D12Resource* s[3] = {
            m_luma[cur][0].Get(),
            m_luma[prev][0].Get(),
            m_hasHistory ? m_filtered.Get() : m_filtered.Get()
        };
        DXGI_FORMAT sf[3] = {
            DXGI_FORMAT_R16_FLOAT,
            DXGI_FORMAT_R16_FLOAT,
            DXGI_FORMAT_R16G16_FLOAT
        };
        ID3D12Resource* u[2] = { fullResMv, reactive };
        DXGI_FORMAT uf[2] = {
            DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R32_FLOAT
        };

        dispatchPass(cmd, m_pixelPso.Get(), s, sf, u, uf,
                     reinterpret_cast<const uint32_t*>(&c),
                     sizeof(c) / sizeof(uint32_t),
                     renderSize.w, renderSize.h, cursor);
    }

    m_hasHistory = true;
    m_current = prev;
    return true;
}

void FastMv::shutdown()
{
    m_ready = false;
    m_hasHistory = false;
    m_current = 0;

    for (auto& level : m_luma)
        for (auto& r : level) r.Reset();
    for (auto& r : m_grid) r.Reset();
    m_filtered.Reset();

    m_lumaPso.Reset();
    m_downPso.Reset();
    m_searchPso.Reset();
    m_medianPso.Reset();
    m_pixelPso.Reset();
    m_rs.Reset();
    m_heap.Reset();
}
