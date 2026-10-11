// NRLive uses the native D3D12 external-memory bridge, not the in-process
// Vulkan device hooks used by a Vulkan game. Session::run_d3d12_queue() takes
// the bridge path and never consults this capability probe.
#include "nr_pe_vkdevice.hpp"
namespace nr::pe::vkdevice {
bool network_features_added(VkDevice) { return false; }
}
