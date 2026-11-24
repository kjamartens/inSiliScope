///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldGpuD3D11.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldGpuD3D11.h: the Direct3D 11 backend of
//                WidefieldGpuHost.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "WidefieldGpuD3D11.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace sim {

#ifdef _WIN32

namespace {

#include "WidefieldGpuHlsl.inc"

using Microsoft::WRL::ComPtr;

std::string HrText(HRESULT hr)
{
   std::ostringstream os;
   os << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
   return os.str();
}

std::string Narrow(const wchar_t* w)
{
   std::string s;
   for (; *w; ++w)
      s += (*w < 128) ? static_cast<char>(*w) : '?';
   return s;
}

// A raw (ByteAddress) buffer with both views.
struct D3DBuf : GpuBackend::Buffer
{
   ComPtr<ID3D11Buffer> b;
   ComPtr<ID3D11ShaderResourceView> srv;
   ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Kernel
{
   ComPtr<ID3D11ComputeShader> cs;
   bool uav[8] = {false, false, false, false, false, false, false, false}; // binding i is a UAV (else SRV)
};

class D3D11Backend : public GpuBackend
{
public:
   ComPtr<ID3D11Device> dev;
   ComPtr<ID3D11DeviceContext> ctx;
   ComPtr<ID3D11Buffer> cb; // 64 bytes of parameters
   std::map<std::string, Kernel> kernels;
   ComPtr<ID3D11Buffer> staging;
   size_t stagingBytes = 0;

   BufPtr Make(size_t bytes, const void* data, std::string& err) override
   {
      const size_t want = bytes;
      bytes = std::max<size_t>(16, (bytes + 3) / 4 * 4);
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = static_cast<UINT>(bytes);
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      std::vector<uint8_t> zeros;
      D3D11_SUBRESOURCE_DATA init = {};
      if (data)
      {
         zeros.assign(bytes, 0);
         std::memcpy(zeros.data(), data, want);
         init.pSysMem = zeros.data();
      }
      auto out = std::make_shared<D3DBuf>();
      HRESULT hr = dev->CreateBuffer(&d, data ? &init : nullptr, &out->b);
      if (FAILED(hr))
      {
         err = "CreateBuffer(" + std::to_string(bytes) + " bytes) failed: " + HrText(hr);
         return nullptr;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sd.BufferEx.NumElements = static_cast<UINT>(bytes / 4);
      sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      hr = dev->CreateShaderResourceView(out->b.Get(), &sd, &out->srv);
      if (FAILED(hr))
      {
         err = "raw SRV: " + HrText(hr);
         return nullptr;
      }
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = static_cast<UINT>(bytes / 4);
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      hr = dev->CreateUnorderedAccessView(out->b.Get(), &ud, &out->uav);
      if (FAILED(hr))
      {
         err = "raw UAV: " + HrText(hr);
         return nullptr;
      }
      out->bytes = bytes;
      return out;
   }

   bool Write(Buffer& b, size_t offset, const void* data, size_t bytes, std::string&) override
   {
      if (bytes == 0)
         return true;
      D3D11_BOX box = {static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + bytes), 1, 1};
      ctx->UpdateSubresource(static_cast<D3DBuf&>(b).b.Get(), 0, &box, data, 0, 0);
      return true;
   }

   bool Copy(Buffer& dst, Buffer& src, size_t bytes, std::string&) override
   {
      D3D11_BOX box = {0, 0, 0, static_cast<UINT>(bytes), 1, 1};
      ctx->CopySubresourceRegion(static_cast<D3DBuf&>(dst).b.Get(), 0, 0, 0, 0, static_cast<D3DBuf&>(src).b.Get(), 0,
                                 &box);
      return true;
   }

   bool Dispatch(const char* name, const uint32_t* params, size_t nParams, Buffer* const* bufs, size_t nBufs,
                 unsigned x, unsigned y, unsigned z, std::string& err) override
   {
      auto it = kernels.find(name);
      if (it == kernels.end())
      {
         err = std::string("no kernel ") + name;
         return false;
      }
      uint32_t p[16] = {};
      std::memcpy(p, params, std::min<size_t>(nParams, 16) * 4);
      D3D11_MAPPED_SUBRESOURCE ms;
      HRESULT hr = ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
      if (FAILED(hr))
      {
         err = "map constants: " + HrText(hr);
         return false;
      }
      std::memcpy(ms.pData, p, sizeof p);
      ctx->Unmap(cb.Get(), 0);
      const Kernel& k = it->second;
      ctx->CSSetShader(k.cs.Get(), nullptr, 0);
      ID3D11Buffer* cbs[1] = {cb.Get()};
      ctx->CSSetConstantBuffers(0, 1, cbs);
      for (size_t i = 0; i < nBufs; ++i)
      {
         const UINT slot = static_cast<UINT>(i + 1);
         D3DBuf& b = static_cast<D3DBuf&>(*bufs[i]);
         if (k.uav[slot])
         {
            ID3D11UnorderedAccessView* u = b.uav.Get();
            ctx->CSSetUnorderedAccessViews(slot, 1, &u, nullptr);
         }
         else
         {
            ID3D11ShaderResourceView* s = b.srv.Get();
            ctx->CSSetShaderResources(slot, 1, &s);
         }
      }
      ctx->Dispatch(x, y, z);
      // Unbind, so the next dispatch may bind these buffers the other way.
      ID3D11UnorderedAccessView* nu[8] = {};
      ID3D11ShaderResourceView* ns[8] = {};
      ctx->CSSetUnorderedAccessViews(0, 8, nu, nullptr);
      ctx->CSSetShaderResources(0, 8, ns);
      return true;
   }

   bool ReadBack(Buffer& src, size_t bytes, void* dst, std::string& err) override
   {
      if (!staging || stagingBytes < bytes)
      {
         D3D11_BUFFER_DESC d = {};
         d.ByteWidth = static_cast<UINT>(std::max<size_t>(16, (bytes + 3) / 4 * 4));
         d.Usage = D3D11_USAGE_STAGING;
         d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         staging.Reset();
         HRESULT hr = dev->CreateBuffer(&d, nullptr, &staging);
         if (FAILED(hr))
         {
            err = "staging buffer: " + HrText(hr);
            return false;
         }
         stagingBytes = d.ByteWidth;
      }
      D3D11_BOX box = {0, 0, 0, static_cast<UINT>((bytes + 3) / 4 * 4), 1, 1};
      ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, static_cast<D3DBuf&>(src).b.Get(), 0, &box);
      D3D11_MAPPED_SUBRESOURCE ms;
      HRESULT hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
      if (FAILED(hr))
      {
         err = "read back: " + HrText(hr);
         return false;
      }
      std::memcpy(dst, ms.pData, bytes);
      ctx->Unmap(staging.Get(), 0);
      return true;
   }
};

} // namespace

std::unique_ptr<WidefieldGpuD3D11> WidefieldGpuD3D11::Create(std::string& info)
{
   auto be = std::make_unique<D3D11Backend>();
   D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
   D3D_FEATURE_LEVEL got;
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                  &be->dev, &got, &be->ctx);
   if (FAILED(hr))
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION,
                             &be->dev, &got, &be->ctx);
   if (FAILED(hr))
   {
      info = "no hardware Direct3D 11 device (" + HrText(hr) + ")";
      return nullptr;
   }
   ComPtr<IDXGIDevice> dxgiDev;
   ComPtr<IDXGIAdapter> adapter;
   DXGI_ADAPTER_DESC desc = {};
   std::string name = "Direct3D 11";
   if (SUCCEEDED(be->dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
       SUCCEEDED(adapter->GetDesc(&desc)))
   {
      name = Narrow(desc.Description);
      if (desc.VendorId == 0x1414)
      {
         info = "only a software adapter is available (" + name + ")";
         return nullptr;
      }
   }
   for (const auto& k : kWfGpuKernels)
   {
      ComPtr<ID3DBlob> blob, errors;
      hr = D3DCompile(k.hlsl, std::strlen(k.hlsl), k.name, nullptr, nullptr, k.name, "cs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &errors);
      if (FAILED(hr))
      {
         info = std::string("HLSL compile of ") + k.name + " failed: " +
                (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : HrText(hr));
         return nullptr;
      }
      Kernel kern;
      hr = be->dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &kern.cs);
      if (FAILED(hr))
      {
         info = std::string("CreateComputeShader ") + k.name + ": " + HrText(hr);
         return nullptr;
      }
      // Which bindings are UAVs: "register(uN)" in the generated HLSL.
      const std::string src(k.hlsl);
      for (int b = 1; b < 8; ++b)
         kern.uav[b] = src.find("register(u" + std::to_string(b) + ")") != std::string::npos;
      be->kernels[k.name] = kern;
   }
   D3D11_BUFFER_DESC cbd = {};
   cbd.ByteWidth = 64;
   cbd.Usage = D3D11_USAGE_DYNAMIC;
   cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   hr = be->dev->CreateBuffer(&cbd, nullptr, &be->cb);
   if (FAILED(hr))
   {
      info = "constant buffer: " + HrText(hr);
      return nullptr;
   }
   std::unique_ptr<WidefieldGpuD3D11> gpu(new WidefieldGpuD3D11(std::move(be)));
   std::string err;
   if (!gpu->SelfCheck(err))
   {
      info = name + " failed the WideField self-check (" + err + ")";
      return nullptr;
   }
   info = name;
   return gpu;
}

#else // !_WIN32

std::unique_ptr<WidefieldGpuD3D11> WidefieldGpuD3D11::Create(std::string& info)
{
   info = "the Direct3D 11 GPU path is Windows-only";
   return nullptr;
}

#endif

} // namespace sim
