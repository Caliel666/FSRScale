#include "fastmv_shaders.h"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

static bool compileShader(const char* body, const char* name)
{
    const std::string src = std::string(FastMvShaders::Common) + body;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(
        src.data(), src.size(), name, nullptr, nullptr,
        "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
        if (errors) errors->Release();
        if (code) code->Release();
        return false;
    }
    code->Release();
    if (errors) errors->Release();
    return true;
}

static bool testRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srv{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 3;

    D3D12_DESCRIPTOR_RANGE uav{};
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 2;

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = { 1, &srv };
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable = { 1, &uav };
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[2].Constants.ShaderRegister = 0;
    p[2].Constants.Num32BitValues = 14;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = p;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3D12SerializeRootSignature(
        &rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr) && errors)
        std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
    if (errors) errors->Release();
    if (blob) blob->Release();
    return SUCCEEDED(hr);
}

// CPU reference checks for the two invariants that matter most for this
// estimator: a still image must stay exactly still, and a clean translation
// must be recovered rather than replaced with arbitrary noise.
static float sad(const std::vector<float>& a, const std::vector<float>& b,
                 int w, int h, int dx, int dy)
{
    const int x0 = std::max(0, -dx);
    const int x1 = std::min(w, w - dx);
    const int y0 = std::max(0, -dy);
    const int y1 = std::min(h, h - dy);
    if (x0 >= x1 || y0 >= y1) return 1e9f;

    float s = 0.0f;
    int n = 0;
    for (int y=y0; y<y1; ++y)
        for (int x=x0; x<x1; ++x) {
            s += std::fabs(a[y*w+x] - b[(y+dy)*w + (x+dx)]);
            ++n;
        }
    return s / float(n);
}

static bool testMotionMath()
{
    constexpr int W = 64, H = 48;
    std::vector<float> a(W*H), b(W*H);
    for (int y=0; y<H; ++y)
        for (int x=0; x<W; ++x)
            a[y*W+x] = float(((x*13 + y*7) ^ (x*y*3)) & 255) / 255.0f;

    b = a;
    if (sad(a,b,W,H,0,0) != 0.0f)
        return false;

    float best = 1e9f;
    int bestDx = 0, bestDy = 0;
    // b is a translated copy of a; current -> previous is tested in the same
    // direction used by the FastMv shader.
    const int tx = 3, ty = -2;
    for (int y=0; y<H; ++y)
        for (int x=0; x<W; ++x) {
            int sx = std::clamp(x-tx,0,W-1);
            int sy = std::clamp(y-ty,0,H-1);
            b[y*W+x] = a[sy*W+sx];
        }

    for (int dy=-6; dy<=6; ++dy)
        for (int dx=-6; dx<=6; ++dx) {
            float c = sad(b,a,W,H,dx,dy);
            if (c < best) { best=c; bestDx=dx; bestDy=dy; }
        }

    return best < 0.02f && bestDx == -tx && bestDy == -ty;
}

int main()
{
    struct Shader { const char* name; const char* src; };
    const Shader shaders[] = {
        {"luma", FastMvShaders::Luma},
        {"down", FastMvShaders::Down},
        {"search", FastMvShaders::Search},
        {"median", FastMvShaders::Median},
        {"pixel", FastMvShaders::Pixel},
    };

    for (const auto& s : shaders) {
        if (!compileShader(s.src, s.name)) {
            std::cerr << "shader compile failed: " << s.name << "\n";
            return 1;
        }
    }

    if (!testRootSignature()) {
        std::cerr << "root signature serialization failed\n";
        return 2;
    }

    if (!testMotionMath()) {
        std::cerr << "motion reference test failed\n";
        return 3;
    }

    std::cout << "FastMv shader + root-signature + motion invariants: PASS\n";
    return 0;
}
