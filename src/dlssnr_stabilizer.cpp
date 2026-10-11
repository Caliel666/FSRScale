#include "dlssnr_stabilizer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <climits>

namespace {
constexpr UINT kSets=8, kViews=6;
const char* kShader=R"(
Texture2D<float4> source:register(t0); Texture2D<float4> result:register(t1);
Texture2D<float2> motion:register(t2); Texture2D<float4> history:register(t3);
RWTexture2D<float4> output:register(u0); RWTexture2D<float4> next:register(u1);
cbuffer Params:register(b0){uint w,h,mvW,mvH;float alpha,delta;uint validHistory,pad;}
float3 E(float3 c){c=max(c,0);return c/(1+c);}
float3 Einv(float3 y){y=clamp(y,0,.999);return min(y/max(1-y,1e-5),1);}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 float4 o=result.Load(int3(id.xy,0));
 float3 base=source.Load(int3(id.xy,0)).rgb;
 float3 r=E(o.rgb)-E(base), stable=r;
 if(validHistory!=0){
   float2 v=motion.Load(int3(min(id.xy,uint2(mvW-1,mvH-1)),0));
   int2 p=int2(round(float2(id.xy)+v));
   if(p.x>=0&&p.y>=0&&p.x<int(w)&&p.y<int(h)){
     float4 old=history.Load(int3(p,0));
     if(old.a>.5) stable=r+alpha*(clamp(old.rgb,r-delta,r+delta)-r);
   }
 }
 output[id.xy]=float4(Einv(E(base)+stable),o.a);
 next[id.xy]=float4(stable,1);
}";

D3D12_RESOURCE_STATES kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
D3D12_RESOURCE_STATES kWrite=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void DlssNrStabilizer::barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r,
                               D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
  if(!cmd||!r||before==after)return;
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource=r; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore=before; b.Transition.StateAfter=after; cmd->ResourceBarrier(1,&b);
}

bool DlssNrStabilizer::build(ID3D12Device* device) {
  if(m_pipeline)return true;
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0};
  ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,4};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable={2,ranges};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants={0,0,8};
  D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=params;
  ComPtr<ID3DBlob> sig,errors,cs;
  if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&errors)))return false;
  if(FAILED(device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_root))))return false;
  if(FAILED(D3DCompile(kShader,strlen(kShader),"DLSSNR residual stabilizer",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&cs,&errors)))return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=m_root.Get();
  pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
  if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&m_pipeline))))return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=kSets*kViews; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&m_heap))))return false;
  m_device=device; return true;
}

bool DlssNrStabilizer::ensureResources(ID3D12Resource* result) {
  const auto d=result->GetDesc();
  if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
     (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) ||
     d.Width>UINT_MAX || d.Height>UINT_MAX) return false;
  const UINT w=(UINT)d.Width,h=d.Height;
  if(m_width==w&&m_height==h&&m_format==d.Format&&m_output&&m_history[0]&&m_history[1])return true;
  for(auto& x:m_history)if(x)m_retired.push_back(std::move(x));
  if(m_output)m_retired.push_back(std::move(m_output));
  D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width=w; rd.Height=h; rd.DepthOrArraySize=1; rd.MipLevels=1; rd.SampleDesc.Count=1;
  rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
  if(FAILED(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,kRead,nullptr,IID_PPV_ARGS(&m_history[0]))))return false;
  if(FAILED(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,kRead,nullptr,IID_PPV_ARGS(&m_history[1]))))return false;
  // FP16 output avoids requiring typed-UAV support on the captured BGRA8 surface.
  if(FAILED(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,kRead,nullptr,IID_PPV_ARGS(&m_output))))return false;
  m_width=w;m_height=h;m_format=d.Format;m_current=0;m_hasHistory=false;
  return true;
}

ID3D12Resource* DlssNrStabilizer::record(ID3D12Device* device,ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* original,ID3D12Resource* result,ID3D12Resource* motion,
    D3D12_RESOURCE_STATES motionState,bool resetHistory) {
  if(!device||!cmd||!original||!result||!motion||!build(device)||!ensureResources(result))return nullptr;
  auto desc=motion->GetDesc(); if(desc.Width==0||desc.Height==0)return false;
  UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu=m_heap->GetCPUDescriptorHandleForHeapStart(),gpu=m_heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr+=SIZE_T(m_set)*kViews*stride; gpu.ptr+=UINT64(m_set)*kViews*stride;
  auto srv=[&](ID3D12Resource* r,DXGI_FORMAT fmt){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=fmt;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&v,cpu);cpu.ptr+=stride;};
  auto uav=[&](ID3D12Resource* r,DXGI_FORMAT fmt){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=fmt;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&v,cpu);cpu.ptr+=stride;};
  const DXGI_FORMAT colourFormat=result->GetDesc().Format;
  srv(original,colourFormat);srv(result,colourFormat);
  srv(motion,DXGI_FORMAT_R16G16_FLOAT);srv(m_history[m_current].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
  uav(m_output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);uav(m_history[1-m_current].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
  barrier(cmd,result,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,kRead);
  barrier(cmd,motion,motionState,kRead);barrier(cmd,m_output.Get(),kRead,kWrite);barrier(cmd,m_history[1-m_current].Get(),kRead,kWrite);
  ID3D12DescriptorHeap* heaps[]={m_heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
  cmd->SetComputeRootSignature(m_root.Get());cmd->SetPipelineState(m_pipeline.Get());cmd->SetComputeRootDescriptorTable(0,gpu);
  struct Params{UINT w,h,mvW,mvH;float alpha,delta;UINT validHistory,pad;} p{m_width,m_height,(UINT)desc.Width,(UINT)desc.Height,0.45f,6.0f/255.0f,(m_hasHistory&&!resetHistory)?1u:0u,0};
  cmd->SetComputeRoot32BitConstants(1,8,&p,0);cmd->Dispatch((m_width+7)/8,(m_height+7)/8,1);
  barrier(cmd,result,kRead,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  barrier(cmd,m_output.Get(),kWrite,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  barrier(cmd,m_history[1-m_current].Get(),kWrite,kRead);barrier(cmd,motion,kRead,motionState);
  m_current=1-m_current;m_set=(m_set+1)%kSets;m_hasHistory=true;return m_output.Get();
}
