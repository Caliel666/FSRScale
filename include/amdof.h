#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <cstdint>
#include "graphics.h"

using Microsoft::WRL::ComPtr;

// Performance-mode optical flow for FSR / ReShade motion vectors.
// half-res search + densify + temporal blend. Not the full FFX OF context
// (that requires the optical-flow provider DLL); this is a stable proxy.
class AmdOf {
public:
  bool init(ID3D12Device* device, ID3D12CommandQueue* queue, Size resolution, bool qualityMode);
  void shutdown();

  bool dispatch(ID3D12GraphicsCommandList* cmd,
                ID3D12Resource* color,
                ID3D12Resource* fullResMv,
                Size renderSize,
                bool reset);

  bool available() const { return m_ready; }
  const std::wstring& lastError() const { return m_error; }

private:
  bool createBlockResources(Size ofSize);
  bool createDensifyPipeline();
  void clearMv(ID3D12GraphicsCommandList* cmd, ID3D12Resource* fullResMv, Size renderSize);
  void clearPrevMv(ID3D12GraphicsCommandList* cmd);
  bool ensureTemporal(Size renderSize);
  void writeDescriptors(ID3D12Resource* color, ID3D12Resource* fullResMv);

  ID3D12Device* m_device = nullptr;
  ID3D12CommandQueue* m_queue = nullptr;
  Size m_ofSize{};
  Size m_blockSize{};
  Size m_renderSize{};
  Size m_prevFullSize{};
  bool m_quality = false;
  bool m_ready = false;
  bool m_hasHistory = false;
  std::wstring m_error;

  ComPtr<ID3D12Resource> m_blockMv;
  ComPtr<ID3D12Resource> m_scd;
  ComPtr<ID3D12Resource> m_prevColor;
  ComPtr<ID3D12Resource> m_currOf;
  ComPtr<ID3D12Resource> m_prevColorFull;
  ComPtr<ID3D12Resource> m_prevMv;
  ComPtr<ID3D12Resource> m_cb;   // upload buffer for the Params CBV

  ComPtr<ID3D12RootSignature> m_rs;
  ComPtr<ID3D12PipelineState> m_pso;
  ComPtr<ID3D12DescriptorHeap> m_heap;
  UINT m_descStride = 0;

  // Cached descriptor identity — recreate only when the pointer changes.
  ID3D12Resource* m_lastColor = nullptr;
  ID3D12Resource* m_lastMv = nullptr;
  ID3D12Resource* m_lastPrevColor = nullptr;
  ID3D12Resource* m_lastPrevMv = nullptr;

  void* m_ofContext = nullptr;
  void* m_backendScratch = nullptr;
  size_t m_backendScratchBytes = 0;
};
