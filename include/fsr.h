#pragma once
#include <windows.h>
#include <d3d12.h>
#include <string>
#include <mutex>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#include "graphics.h"

class Fsr {
public:
  bool init(ID3D12Device* device, Size maxRender, Size maxDisplay);
  bool dispatch(ID3D12GraphicsCommandList* cmd,
                ID3D12Resource* color,
                ID3D12Resource* depth,
                ID3D12Resource* motionVectors,
                ID3D12Resource* reactive,
                ID3D12Resource* output,
                Size render, Size display,
                float dt, bool reset);
  void setQuality(int quality);
  void setSharpening(bool on, float amount);
  void shutdown();
  const std::wstring& lastError() const { return m_error; }

private:
  ffxContext m_ctx = nullptr;
  int m_quality = 1;
  bool m_sharpen = true;
  float m_sharpness = 0.5f;
  int m_jitterPhases = 0;
  std::wstring m_error;
};


// AMD FidelityFX Frame Generation (FSR 3.1.x API). This is a separate context
// from the upscaler. It consumes the post-upscale color plus the optical-flow
// and depth estimates produced by NRLive, then writes a real FFX-generated
// intermediate frame to a GPU texture for presentation.
class FsrFrameGeneration {
public:
  bool init(ID3D12Device* device, Size maxRender, Size display,
            IDXGISwapChain4** swapChain, ID3D12CommandQueue* queue);
  bool resize(Size maxRender, Size display);
  bool prepare(ID3D12GraphicsCommandList* cmd,
               ID3D12Resource* depth,
               ID3D12Resource* motionVectors,
               Size render, Size display,
               float dt, bool reset, bool enabled);
  void shutdown();
  const std::wstring& lastError() const { return m_error; }
  bool ready() const { return m_ctx != nullptr; }

private:
  static ffxReturnCode_t generationCallback(ffxDispatchDescFrameGeneration* params, void* userCtx);
  ffxContext m_ctx = nullptr;
  ffxContext m_swapChainCtx = nullptr;
  ID3D12Device* m_device = nullptr; // retained by Graphics for the lifetime of the context
  IDXGISwapChain4* m_swapChain = nullptr;
  Size m_maxRender{};
  Size m_display{};
  uint64_t m_frameId = 0;
  bool m_callbackEnabled = false;
  std::mutex m_mutex;
  std::wstring m_error;
};
