#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include "graphics.h"

using Microsoft::WRL::ComPtr;

class FastMv {
public:
    bool init(ID3D12Device* device, Size resolution);
    void shutdown();

    // Generates full-render-resolution screen-space motion vectors and an
    // R32_FLOAT reactive mask. Both outputs are left in UAV state so main.cpp
    // can hand them directly to FSR 3.1.
    bool dispatch(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* color,
                  ID3D12Resource* fullResMv,
                  ID3D12Resource* reactive,
                  Size renderSize,
                  bool reset);

    bool available() const { return m_ready; }
    const std::wstring& lastError() const { return m_error; }

private:
    static constexpr int kMaxLevels = 7;

    bool createPipeline();
    bool ensureResources(Size renderSize);

    void bindDescriptors(ID3D12GraphicsCommandList* cmd,
                         ID3D12Resource* const srv[3],
                         const DXGI_FORMAT srvFormat[3],
                         ID3D12Resource* const uav[2],
                         const DXGI_FORMAT uavFormat[2],
                         UINT& cursor);

    void setConstants(ID3D12GraphicsCommandList* cmd,
                      const uint32_t* values, UINT count);

    void dispatchPass(ID3D12GraphicsCommandList* cmd,
                      ID3D12PipelineState* pso,
                      ID3D12Resource* const srv[3],
                      const DXGI_FORMAT srvFormat[3],
                      ID3D12Resource* const uav[2],
                      const DXGI_FORMAT uavFormat[2],
                      const uint32_t* constants, UINT constantCount,
                      uint32_t width, uint32_t height,
                      UINT& cursor);

    ID3D12Device* m_device = nullptr;
    Size m_render{};
    bool m_ready = false;
    bool m_hasHistory = false;
    int m_current = 0;
    int m_levels = 0;
    uint32_t m_lw[kMaxLevels]{};
    uint32_t m_lh[kMaxLevels]{};
    uint32_t m_gw[kMaxLevels]{};
    uint32_t m_gh[kMaxLevels]{};
    std::wstring m_error;

    ComPtr<ID3D12Resource> m_currLuma[kMaxLevels];
    ComPtr<ID3D12Resource> m_prevLuma[kMaxLevels];
    ComPtr<ID3D12Resource> m_grid[kMaxLevels];
    ComPtr<ID3D12Resource> m_filtered;

    ComPtr<ID3D12RootSignature> m_rs;
    ComPtr<ID3D12PipelineState> m_lumaPso;
    ComPtr<ID3D12PipelineState> m_downPso;
    ComPtr<ID3D12PipelineState> m_searchPso;
    ComPtr<ID3D12PipelineState> m_medianPso;
    ComPtr<ID3D12PipelineState> m_pixelPso;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_stride = 0;
};
