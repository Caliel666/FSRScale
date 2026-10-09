#pragma once
#include <windows.h>
#include <d3d12.h>
#include <string>
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
  bool init(ID3D12Device* device, Size maxRender, Size display);
  bool dispatch(ID3D12GraphicsCommandList* cmd,
                ID3D12Resource* presentColor,
                ID3D12Resource* depth,
                ID3D12Resource* motionVectors,
                ID3D12Resource* output,
                void* swapChain,
                Size render, Size display,
                float dt, bool reset);
  void shutdown();
  const std::wstring& lastError() const { return m_error; }
  bool ready() const { return m_ctx != nullptr; }

private:
  ffxContext m_ctx = nullptr;
  Size m_maxRender{};
  Size m_display{};
  uint64_t m_frameId = 0;
  std::wstring m_error;
};
