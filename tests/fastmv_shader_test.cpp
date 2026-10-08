#include "fastmv.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <iostream>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

static void fail(const char* s) { std::cerr << s << "\n"; ExitProcess(1); }

static ComPtr<ID3D12Resource> makeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) fail("texture creation failed");
    return r;
}

static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    cmd->ResourceBarrier(1, &x);
}

int main()
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) fail("DXGI factory failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }
    if (!adapter) fail("no hardware adapter");

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&dev)))) fail("D3D12 device failed");

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) fail("queue failed");

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&cmd)))) fail("command list failed");

    constexpr UINT W = 640, H = 360;
    auto color = makeTex(dev.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM);
    auto mv = makeTex(dev.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT);
    auto reactive = makeTex(dev.Get(), W, H, DXGI_FORMAT_R8_UNORM);

    FastMv mvEstimator;
    if (!mvEstimator.init(dev.Get(), {W,H})) fail("FastMv init failed");

    // Capture input is SRV; FastMv outputs start as UAV.
    barrier(cmd.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cmd.Get(), mv.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cmd.Get(), reactive.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!mvEstimator.dispatch(cmd.Get(), color.Get(), mv.Get(), reactive.Get(), {W,H}, true))
        fail("FastMv first dispatch failed");

    if (!mvEstimator.dispatch(cmd.Get(), color.Get(), mv.Get(), reactive.Get(), {W,H}, false))
        fail("FastMv second dispatch failed");

    if (FAILED(cmd->Close())) fail("command list close failed");
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        fail("fence failed");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) fail("event failed");
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() != 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    CloseHandle(ev);

    const HRESULT removed = dev->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        std::cerr << "FastMv GPU execution removed the device: 0x"
                  << std::hex << static_cast<unsigned long>(removed) << "\n";
        return 2;
    }

    std::cout << "FastMv real D3D12 GPU dispatch: PASS\n";
    return 0;
}
