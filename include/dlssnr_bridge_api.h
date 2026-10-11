#pragma once
// C ABI shared between NRLive.exe (MSVC) and the MinGW-built upstream runtime bridge.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

#ifdef NR_LIVE_DLSSNR_BRIDGE_EXPORTS
#define NRLIVE_DLSSNR_API __declspec(dllexport)
#else
#define NRLIVE_DLSSNR_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NRLiveDlssNrSettings {
  int enabled;
  int style;                 // 0 Neutral, 1 Natural, 2 Cinematic
  float model_scale;         // 0.25..1.0
  float intensity;           // 0..2
  float local_tone;           // 0..2
  float max_ratio;            // 1..8
  float structure;           // 0..2
  float skin_structure;      // -1..2
  float history_strength;    // 0..1
  int automatic_skin_mask;
} NRLiveDlssNrSettings;

typedef void (*NRLiveDlssNrFlushCallback)(void* user_data);

// Root is the NRLive package directory containing dlssnr-amd/shaders and
// dlssnr-amd/dlssnr.bin. The model is not bundled; see docs/dlssnr-integration.md.
NRLIVE_DLSSNR_API void* NRLiveDlssNrCreate(const wchar_t* root);
NRLIVE_DLSSNR_API void NRLiveDlssNrDestroy(void* session);
NRLIVE_DLSSNR_API int NRLiveDlssNrProcess(
    void* session,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Resource* input_colour,
    uint32_t input_colour_state,
    ID3D12Resource* output_colour,
    uint32_t output_colour_state,
    ID3D12Resource* motion,
    uint32_t motion_state,
    int reset_history,
    const NRLiveDlssNrSettings* settings,
    NRLiveDlssNrFlushCallback flush,
    void* flush_user_data);
NRLIVE_DLSSNR_API const char* NRLiveDlssNrLastError(void* session);

#ifdef __cplusplus
}
#endif
