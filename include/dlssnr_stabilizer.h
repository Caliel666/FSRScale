#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
using Microsoft::WRL::ComPtr;

// GPU-only temporal residual clamp adapted from the DLSSNR-AMD OptiScaler
// residual-stabilizer approach. NRLive has no real game depth, so this version
// uses motion reprojection and residual clamping without surface-key rejection.
class DlssNrStabilizer {
public:
  ID3D12Resource* record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
              ID3D12Resource* original, ID3D12Resource* result,
              ID3D12Resource* motion, D3D12_RESOURCE_STATES motionState,
              bool resetHistory);
private:
  bool build(ID3D12Device* device);
  bool ensureResources(ID3D12Resource* result);
  static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                      D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12RootSignature> m_root;
  ComPtr<ID3D12PipelineState> m_pipeline;
  ComPtr<ID3D12DescriptorHeap> m_heap;
  ComPtr<ID3D12Resource> m_history[2], m_output;
  std::vector<ComPtr<ID3D12Resource>> m_retired;
  UINT m_width=0, m_height=0, m_format=DXGI_FORMAT_UNKNOWN, m_set=0, m_current=0;
  bool m_hasHistory=false;
  D3D12_RESOURCE_STATES m_outputState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};
