// Built as a small C-ABI DLL alongside NRLive from the upstream DLSSNR-AMD
// Windows sources. Keeping the runtime on the other side of a C ABI lets the
// MSVC launcher and the upstream MinGW-built Vulkan bridge interoperate safely.
#include "nr_pe_session.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_config.hpp"
#include "dlssnr_bridge_api.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

namespace {
struct HostSession {
    std::unique_ptr<nr::pe::Session> session;
    std::string error;
    std::mutex mutex;
};

std::string utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), bytes, nullptr, nullptr);
    result.pop_back();
    return result;
}

nr::Controls makeControls(const NRLiveDlssNrSettings& s) {
    nr::Controls c{};
    c.enabled = s.enabled != 0;
    c.apply_model = true;
    c.style = std::clamp(s.style, 0, 2);
    c.intensity = std::clamp(s.intensity, 0.0f, 2.0f);
    c.local_structure = std::clamp(s.structure, 0.0f, 2.0f);
    c.skin_structure = std::clamp(s.skin_structure, -1.0f, 2.0f);
    c.automatic_mask = s.automatic_skin_mask != 0;
    c.passes = 1;
    return c;
}
}

extern "C" __declspec(dllexport) void* NRLiveDlssNrCreate(const wchar_t* root) {
    try {
        auto* host = new HostSession();
        host->session = std::make_unique<nr::pe::Session>(utf8(root));
        return host;
    } catch (...) {
        return nullptr;
    }
}

extern "C" __declspec(dllexport) void NRLiveDlssNrDestroy(void* opaque) {
    delete static_cast<HostSession*>(opaque);
}

extern "C" __declspec(dllexport) int NRLiveDlssNrProcess(
    void* opaque, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* colour, uint32_t colour_state, ID3D12Resource* motion,
    uint32_t motion_state, int reset_history, const NRLiveDlssNrSettings* settings,
    NRLiveDlssNrFlushCallback flush, void* flush_user_data) {
    auto* host = static_cast<HostSession*>(opaque);
    if (!host || !host->session || !device || !queue || !colour || !settings ||
        !settings->enabled || !flush)
        return 0;

    std::lock_guard lock(host->mutex);
    try {
        host->session->set_model_scale(std::clamp(settings->model_scale, 0.25f, 1.0f));
        host->session->set_history_strength(std::clamp(settings->history_strength, 0.0f, 1.0f));
        host->session->set_max_passes(1);

        nr::pe::Session::D3D12QueueFrame frame{};
        frame.device = device;
        frame.queue = queue;
        frame.flush = [flush, flush_user_data] { flush(flush_user_data); };
        frame.target = colour;
        frame.target_state = static_cast<D3D12_RESOURCE_STATES>(colour_state);
        frame.motion = motion;
        frame.motion_state = static_cast<D3D12_RESOURCE_STATES>(motion_state);
        frame.depth = nullptr; // NRLive does not currently capture real game depth.
        frame.reset = reset_history != 0;
        const bool ok = host->session->run_d3d12_queue(frame, makeControls(*settings));
        if (!ok) host->error = "DLSSNR did not run this frame; inspect dlssnr-amd.log for the runtime reason.";
        else host->error.clear();
        return ok ? 1 : 0;
    } catch (const std::exception& e) {
        host->error = e.what();
        return 0;
    } catch (...) {
        host->error = "unknown DLSSNR runtime error";
        return 0;
    }
}

extern "C" __declspec(dllexport) const char* NRLiveDlssNrLastError(void* opaque) {
    auto* host = static_cast<HostSession*>(opaque);
    if (!host) return "DLSSNR session is not initialized";
    std::lock_guard lock(host->mutex);
    return host->error.c_str();
}
