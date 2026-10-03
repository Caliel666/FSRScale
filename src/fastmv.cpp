#include "fastmv.h"
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>

static const char* kLuma = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint pad0; uint pad1; uint pad2; uint pad3; };
[numthreads(8,8,1)]
void main(uint3 p : SV_DispatchThreadID) {
  if (p.x >= dw || p.y >= dh) return;
  // 4x4 box reduction. The quarter-resolution raster is the key speed win:
  // the expensive matcher never runs at full capture resolution.
  uint2 base = p.xy * 4;
  float y = 0;
  [unroll] for (uint yy=0; yy<4; ++yy)
    [unroll] for (uint xx=0; xx<4; ++xx) {
      uint2 q = min(base + uint2(xx,yy), uint2(sw,sh)-1);
      float3 c = src.Load(int3(q,0)).rgb;
      y += dot(c, float3(0.299,0.587,0.114));
    }
  dst[p.xy] = y * (1.0/16.0);
}
)";

static const char* kFlow = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prv : register(t1);
Texture2D<float2> guess : register(t2);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint useGuess; float gate; float ratio; uint step; };

float sad3(int2 p, int2 d, int2 lim) {
  float e = 0;
  [unroll] for (int y=-1; y<=1; ++y)
    [unroll] for (int x=-1; x<=1; ++x) {
      int2 a = clamp(p + int2(x,y), int2(0,0), lim);
      int2 b = clamp(p + d + int2(x,y), int2(0,0), lim);
      e += abs(cur.Load(int3(a,0)) - prv.Load(int3(b,0)));
    }
  return e;
}

[numthreads(8,8,1)]
void main(uint3 p : SV_DispatchThreadID) {
  if (p.x >= dw || p.y >= dh) return;
  int2 base = int2(p.xy);
  int2 lim = int2(dw-1,dh-1);

  float mn=1e9, mx=-1e9;
  [unroll] for (int y=-1;y<=1;++y)
    [unroll] for (int x=-1;x<=1;++x) {
      float v=cur.Load(int3(clamp(base+int2(x,y),int2(0,0),lim),0));
      mn=min(mn,v); mx=max(mx,v);
    }
  if (mx-mn < gate) { dst[p.xy]=0; return; }

  int2 g0 = useGuess != 0 ? int2(round(guess.Load(int3(p.xy,0)))) : int2(0,0);
  float best=1e9, still=1e9;
  int2 bestD=g0;
  int s=max(1,(int)step);

  // zmodelerlover's estimator uses a small 3x3 patch and radius-4 search.
  // Fast mode does the coarse search at quarter resolution with a stride of
  // two, then a 3x3 unit-pixel refinement around that winner.
  if (useGuess == 0) {
    [unroll] for (int dy=-4;dy<=4;dy+=2)
      [unroll] for (int dx=-4;dx<=4;dx+=2) {
        int2 d=int2(dx,dy)*s;
        float e=sad3(base,d,lim);
        if (d.x==0 && d.y==0) still=e;
        if (e<best) { best=e; bestD=d; }
      }
    if (best > still*ratio) bestD=0;
  } else {
    [unroll] for (int dy=-1;dy<=1;++dy)
      [unroll] for (int dx=-1;dx<=1;++dx) {
        int2 d=g0+int2(dx,dy);
        float e=sad3(base,d,lim);
        if(e<best){best=e;bestD=d;}
      }
  }
  dst[p.xy]=float2(bestD);
}
)";

static const char* kUp = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint pad0; uint pad1; uint pad2; uint pad3; };
[numthreads(8,8,1)]
void main(uint3 p : SV_DispatchThreadID) {
  if(p.x>=dw || p.y>=dh) return;
  float2 uv=(float2(p.xy)+0.5)/float2(dw,dh);
  // Flow is stored in quarter-resolution pixels. Convert to full render pixels.
  dst[p.xy]=src.SampleLevel(samp,uv,0)*4.0;
}
)";

static bool compile(ID3D12Device* dev,const char* src,const char* name,
                    ID3D12RootSignature* rs,ComPtr<ID3D12PipelineState>& out){
  ComPtr<ID3DBlob> b,e;
  if(FAILED(D3DCompile(src,strlen(src),name,nullptr,nullptr,"main","cs_5_1",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e))) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
  p.pRootSignature=rs; p.CS={b->GetBufferPointer(),b->GetBufferSize()};
  return SUCCEEDED(dev->CreateComputePipelineState(&p,IID_PPV_ARGS(&out)));
}

bool FastMv::createPipeline(){
  D3D12_DESCRIPTOR_RANGE r[2]{};
  r[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[0].NumDescriptors=3; r[0].BaseShaderRegister=0;
  r[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[1].NumDescriptors=1; r[1].BaseShaderRegister=0;
  D3D12_ROOT_PARAMETER p[2]{};
  p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[0].DescriptorTable.NumDescriptorRanges=1; p[0].DescriptorTable.pDescriptorRanges=&r[0];
  p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[1].Constants.ShaderRegister=0; p[1].Constants.Num32BitValues=8;
  D3D12_STATIC_SAMPLER_DESC s{};
  s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  s.MaxLOD=D3D12_FLOAT32_MAX; s.ShaderRegister=0;
  D3D12_ROOT_SIGNATURE_DESC d{};
  d.NumParameters=2; d.pParameters=p; d.NumStaticSamplers=1; d.pStaticSamplers=&s;
  ComPtr<ID3DBlob> b,e;
  if(FAILED(D3D12SerializeRootSignature(&d,D3D_ROOT_SIGNATURE_VERSION_1,&b,&e)) ||
     FAILED(m_device->CreateRootSignature(0,b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&m_rs))))
    return false;

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=12;
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(FAILED(m_device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&m_heap)))) return false;
  m_stride=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  return compile(m_device,kLuma,"fastmv_luma",m_rs.Get(),m_lumaPso) &&
         compile(m_device,kFlow,"fastmv_flow",m_rs.Get(),m_flowPso) &&
         compile(m_device,kFlow,"fastmv_refine",m_rs.Get(),m_refinePso) &&
         compile(m_device,kUp,"fastmv_up",m_rs.Get(),m_upPso);
}

bool FastMv::init(ID3D12Device* device, Size resolution){
  shutdown(); m_device=device; m_render=resolution;
  if(!createPipeline()){m_error=L"fast MV pipeline creation failed";return false;}
  m_ready=true; m_error=L"Fast MV (quarter-res coarse/refine)";
  return true;
}

static void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,
                       D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
  D3D12_RESOURCE_BARRIER x{};
  x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource=r;x.Transition.StateBefore=a;x.Transition.StateAfter=b;
  x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  c->ResourceBarrier(1,&x);
}

bool FastMv::ensureResources(Size rs){
  Size flow{std::max(1u,(rs.w+3)/4),std::max(1u,(rs.h+3)/4)};
  if(m_currLuma && flow.w==m_flow.w && flow.h==m_flow.h && rs.w==m_render.w && rs.h==m_render.h) return true;
  m_flow=flow; m_render=rs;
  m_currLuma.Reset();m_prevLuma.Reset();m_coarse.Reset();m_refined.Reset();m_hasHistory=false;
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  auto tex=[&](ComPtr<ID3D12Resource>& o,UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_STATES st){
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,st,nullptr,IID_PPV_ARGS(&o)));
  };
  return tex(m_currLuma,flow.w,flow.h,DXGI_FORMAT_R16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
         tex(m_prevLuma,flow.w,flow.h,DXGI_FORMAT_R16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
         tex(m_coarse,flow.w,flow.h,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
         tex(m_refined,flow.w,flow.h,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
         tex(m_dummyGuess,flow.w,flow.h,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void FastMv::createViews(ID3D12Resource* color,ID3D12Resource* full){
  auto cpu=m_heap->GetCPUDescriptorHandleForHeapStart();
  auto at=[&](UINT i){auto x=cpu;x.ptr+=SIZE_T(i)*m_stride;return x;};
  D3D12_SHADER_RESOURCE_VIEW_DESC s{};
  s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;

  s.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(color,&s,at(0));
  s.Format=DXGI_FORMAT_R16_FLOAT;m_device->CreateShaderResourceView(m_prevLuma.Get(),&s,at(1));
  s.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_dummyGuess.Get(),&s,at(2));
  D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
  u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;u.Format=DXGI_FORMAT_R16_FLOAT;
  m_device->CreateUnorderedAccessView(m_currLuma.Get(),nullptr,&u,at(3));
  u.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateUnorderedAccessView(m_coarse.Get(),nullptr,&u,at(4));
  m_device->CreateUnorderedAccessView(m_refined.Get(),nullptr,&u,at(5));
  m_device->CreateUnorderedAccessView(full,nullptr,&u,at(6));
  // Up-sampling pass uses a second contiguous SRV/UAV table starting at 7.
  s.Format=DXGI_FORMAT_R16G16_FLOAT;
  m_device->CreateShaderResourceView(m_refined.Get(),&s,at(7));
  m_device->CreateShaderResourceView(m_refined.Get(),&s,at(8));
  m_device->CreateShaderResourceView(m_refined.Get(),&s,at(9));
  u.Format=DXGI_FORMAT_R16G16_FLOAT;
  m_device->CreateUnorderedAccessView(full,nullptr,&u,at(10));
}

static void setTable(ID3D12GraphicsCommandList* c,D3D12_GPU_DESCRIPTOR_HANDLE h,UINT stride,UINT base){
  h.ptr+=UINT64(base)*stride;c->SetComputeRootDescriptorTable(0,h);
}

bool FastMv::dispatch(ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,
                       ID3D12Resource* fullResMv,Size renderSize,bool reset){
  if(!m_ready||!cmd||!color||!fullResMv) return false;
  if(!ensureResources(renderSize)) return false;
  createViews(color,fullResMv);
  auto heap=m_heap.Get();cmd->SetDescriptorHeaps(1,&heap);
  cmd->SetComputeRootSignature(m_rs.Get());

  if(reset||!m_hasHistory){
    clearOutput(cmd,fullResMv);
  }

  auto gpu=m_heap->GetGPUDescriptorHandleForHeapStart();
  UINT cb[8]={m_flow.w,m_flow.h,renderSize.w,renderSize.h,0,0,1,0};
  cmd->SetPipelineState(m_lumaPso.Get());
  setTable(cmd,gpu,m_stride,0); cmd->SetComputeRoot32BitConstants(1,8,cb,0);
  // Color is already in a combined SRV state from main.cpp; compute SRV reads are valid there.
  cmd->Dispatch((m_flow.w+7)/8,(m_flow.h+7)/8,1);

  if(!reset&&m_hasHistory){
    transition(cmd,m_currLuma.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd,m_coarse.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cb[4]=0; cb[5]=0.008f; cb[6]=0.85f; cb[7]=2;
    cmd->SetPipelineState(m_flowPso.Get()); setTable(cmd,gpu,m_stride,0);
    cmd->SetComputeRoot32BitConstants(1,8,cb,0);
    cmd->Dispatch((m_flow.w+7)/8,(m_flow.h+7)/8,1);

    transition(cmd,m_coarse.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd,m_refined.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Refinement reads the coarse field through t2. The first pass keeps t2
    // bound to a read-only dummy so it never aliases the UAV it is writing.
    auto cpu2=m_heap->GetCPUDescriptorHandleForHeapStart(); cpu2.ptr+=2ull*m_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC coarseSrv{};
    coarseSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coarseSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    coarseSrv.Format=DXGI_FORMAT_R16G16_FLOAT;
    coarseSrv.Texture2D.MipLevels=1;
    m_device->CreateShaderResourceView(m_coarse.Get(),&coarseSrv,cpu2);

    cb[4]=1; cb[5]=0.0f; cb[6]=0.0f; cb[7]=1;
    cmd->SetPipelineState(m_refinePso.Get()); setTable(cmd,gpu,m_stride,0);
    cmd->SetComputeRoot32BitConstants(1,8,cb,0);
    cmd->Dispatch((m_flow.w+7)/8,(m_flow.h+7)/8,1);

    transition(cmd,m_refined.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd,fullResMv,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cb[0]=renderSize.w;cb[1]=renderSize.h;
    cmd->SetPipelineState(m_upPso.Get());setTable(cmd,gpu,m_stride,7);
    cmd->SetComputeRoot32BitConstants(1,8,cb,0);
    cmd->Dispatch((renderSize.w+7)/8,(renderSize.h+7)/8,1);
    transition(cmd,fullResMv,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  } else {
    // First frame: prime previous luma only; output remains zero.
    transition(cmd,m_currLuma.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cmd,m_prevLuma.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_prevLuma.Get(),m_currLuma.Get());
    transition(cmd,m_currLuma.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,m_prevLuma.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Keep current luma as history for the next frame.
  if(!reset&&m_hasHistory){
    transition(cmd,m_currLuma.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cmd,m_prevLuma.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_prevLuma.Get(),m_currLuma.Get());
    transition(cmd,m_currLuma.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,m_prevLuma.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  m_hasHistory=true;
  return true;
}

void FastMv::clearOutput(ID3D12GraphicsCommandList* cmd,ID3D12Resource* full){
  auto cpu=m_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=10ull*m_stride;
  auto gpu=m_heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=10ull*m_stride;
  UINT z[4]={0,0,0,0};
  cmd->ClearUnorderedAccessViewUint(gpu,cpu,full,z,0,nullptr);
}

void FastMv::shutdown(){
  m_ready=false;m_hasHistory=false;m_currLuma.Reset();m_prevLuma.Reset();m_coarse.Reset();m_refined.Reset();m_dummyGuess.Reset();
  m_cb.Reset();m_lumaPso.Reset();m_flowPso.Reset();m_refinePso.Reset();m_upPso.Reset();m_rs.Reset();m_heap.Reset();
}
