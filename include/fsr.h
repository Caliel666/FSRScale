#pragma once
#include <windows.h>
#include <d3d12.h>
#include <string>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
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
