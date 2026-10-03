#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include "graphics.h"

using Microsoft::WRL::ComPtr;

// Fast screen-space motion estimator inspired by zmodelerlover/dlss5-neural-amd:
// luminance matching on a reduced raster, coarse search + local refinement,
// then linear reconstruction to the render resolution.
//
// This deliberately remains separate from AmdOf. --mv amdof keeps the existing
// implementation untouched; --mv fast selects this path.
class FastMv {
public:
  bool init(ID3D12Device* device, Size resolution);
  void shutdown();
  bool dispatch(ID3D12GraphicsCommandList* cmd,
                ID3D12Resource* color,
                ID3D12Resource* fullResMv,
                Size renderSize,
                bool reset);
  bool available() const { return m_ready; }
  const std::wstring& lastError() const { return m_error; }

private:
  bool ensureResources(Size renderSize);
  bool createPipeline();
  void createViews(ID3D12Resource* color, ID3D12Resource* fullResMv);
  void clearOutput(ID3D12GraphicsCommandList* cmd, ID3D12Resource* fullResMv);

  ID3D12Device* m_device = nullptr;
  Size m_render{};
  Size m_flow{};
  bool m_ready = false;
  bool m_hasHistory = false;
  std::wstring m_error;

  ComPtr<ID3D12Resource> m_currLuma;
  ComPtr<ID3D12Resource> m_prevLuma;
  ComPtr<ID3D12Resource> m_coarse;
  ComPtr<ID3D12Resource> m_refined;
  ComPtr<ID3D12Resource> m_dummyGuess;
  ComPtr<ID3D12Resource> m_cb;

  ComPtr<ID3D12RootSignature> m_rs;
  ComPtr<ID3D12PipelineState> m_lumaPso;
  ComPtr<ID3D12PipelineState> m_flowPso;
  ComPtr<ID3D12PipelineState> m_refinePso;
  ComPtr<ID3D12PipelineState> m_upPso;
  ComPtr<ID3D12DescriptorHeap> m_heap;
  UINT m_stride = 0;
};
