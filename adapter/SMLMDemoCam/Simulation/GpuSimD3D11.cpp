#include "GpuSimD3D11.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

using Microsoft::WRL::ComPtr;

// The fused splat + noise kernel. The RNG and noise transforms mirror
// SMLMCounterRng.h LINE FOR LINE (change one, change both); the splat
// mirrors SplatPsfKernel's block-sum read (PsfGeneratorBridge.cpp) and the
// noise chain mirrors ApplyNoiseChain (SMLMNoise.cpp).
const char kShaderSource[] = R"HLSL(
cbuffer Params : register(b0)
{
   uint W; uint H; uint NFrames; int CamRad;
   int Os; int Bw; int Bh; int Off;
   uint PlaneStride; uint Seed; uint Pad2; uint Emccd;
   float Pad3; float Qe; float Dark; float Cic;
   float EmGain; float MaxAdu; float Pad0; float Pad1;
};

// One frame of the batch: its emitters are Ems[emStart .. emStart+emCount).
struct FrameInfo
{
   uint emStart; uint emCount; uint frame; float bgScale;
};

struct Em
{
   int x0; int y0; int bx; int by;
   int plane; int nt; float photons; float pad;
   float4 wx; float4 wy;
};

StructuredBuffer<float> Sums : register(t0);
StructuredBuffer<Em> Ems : register(t1);
StructuredBuffer<float4> Pix : register(t2); // offset ADU, gain photons/ADU, read noise e-, background
StructuredBuffer<FrameInfo> Frames : register(t3);
RWStructuredBuffer<uint> Out : register(u0);

void Pcg4d(inout uint4 v)
{
   v = v * 1664525u + 1013904223u;
   v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
   v ^= v >> 16u;
   v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
}

static uint g_frame;
static uint g_pix;
static uint g_ctr;
static uint4 g_buf;
static uint g_k;

float Uniform()
{
   if (g_k == 4u)
   {
      uint4 v = uint4(Seed, g_frame, g_pix, g_ctr);
      Pcg4d(v);
      g_buf = v;
      g_ctr++;
      g_k = 0u;
   }
   uint x = g_buf[g_k];
   g_k++;
   return ((float)(x >> 9u) + 0.5f) * 1.1920928955078125e-7f;
}

float Gauss()
{
   float r = sqrt(-2.0f * log(Uniform()));
   return r * cos(6.283185307179586f * Uniform());
}

float Poisson(float l)
{
   if (l > 60.0f)
      return max(0.0f, floor(l + sqrt(l) * Gauss() + 0.5f));
   if (!(l > 0.0f))
      return 0.0f;
   float L = exp(-l);
   int k = 0;
   float p = 1.0f;
   [loop] do
   {
      k++;
      p *= Uniform();
   } while (p > L && k < 1000);
   return (float)(k - 1);
}

float GammaDraw(float k)
{
   float d = k - 1.0f / 3.0f, c = 1.0f / sqrt(9.0f * d);
   [loop] for (int it = 0; it < 1000; it++)
   {
      float x = Gauss(), t = 1.0f + c * x;
      if (t <= 0.0f)
         continue;
      float v = t * t * t, w = Uniform();
      if (w < 1.0f - 0.0331f * x * x * x * x)
         return d * v;
      if (log(w) < 0.5f * x * x + d * (1.0f - v + log(v)))
         return d * v;
   }
   return d;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
   if (id.x >= W || id.y >= H || id.z >= NFrames)
      return;
   uint i = id.y * W + id.x;
   float4 px = Pix[i];
   FrameInfo fi = Frames[id.z];

   // Splat: gather this frame's emitters, in list order, onto this pixel.
   float acc = px.w * fi.bgScale;
   int X = (int)id.x, Y = (int)id.y;
   [loop] for (uint e = fi.emStart; e < fi.emStart + fi.emCount; e++)
   {
      Em em = Ems[e];
      int dx = X - em.x0, dy = Y - em.y0;
      if (abs(dx) > CamRad || abs(dy) > CamRad)
         continue;
      int r0 = em.by + dy * Os + Off, c0 = em.bx + dx * Os + Off;
      uint base = (uint)em.plane * PlaneStride;
      float s = 0.0f;
      [loop] for (int j = 0; j < em.nt; j++)
      {
         int r = r0 + j;
         if (r < 0 || r >= Bh)
            continue;
         float row = 0.0f;
         [loop] for (int ii = 0; ii < em.nt; ii++)
         {
            int c = c0 + ii;
            if (c >= 0 && c < Bw)
               row += em.wx[ii] * Sums[base + (uint)(r * Bw + c)];
         }
         s += em.wy[j] * row;
      }
      acc += em.photons * s;
   }

   // Noise: this pixel's own counter stream.
   g_frame = fi.frame;
   g_pix = i;
   g_ctr = 0u;
   g_k = 4u;
   g_buf = uint4(0u, 0u, 0u, 0u);
   float photons = max(acc, 0.0f);
   float adu;
   if (Emccd != 0u)
   {
      float ne = Poisson(photons * Qe + Dark + Cic);
      float o = ne > 0.0f ? GammaDraw(ne) : 0.0f;
      adu = floor(px.x + (o + (px.z / EmGain) * Gauss()) / px.y + 0.5f);
      adu = clamp(adu, 0.0f, MaxAdu);
   }
   else
   {
      float ne = Poisson(photons * Qe + Dark);
      adu = px.x + (ne + px.z * Gauss()) / px.y;
      adu = floor(clamp(adu, 0.0f, 65535.0f) + 0.5f);
   }
   Out[id.z * W * H + i] = (uint)adu;
}
)HLSL";

struct alignas(16) ParamsCB
{
   uint32_t w, h, nFrames;
   int32_t camRad;
   int32_t os, bw, bh, off;
   uint32_t planeStride, seed, pad2, emccd;
   float pad3, qe, dark, cic;
   float emGain, maxAdu, pad0, pad1;
};
static_assert(sizeof(ParamsCB) == 80, "cbuffer layout");
static_assert(sizeof(GpuSplatEmitter) == 64, "emitter layout must match the HLSL struct Em");

struct FrameInfoCpu
{
   uint32_t emStart, emCount, frame;
   float bgScale;
};

// Output buffer budget: frames per dispatch = this / (4 bytes * pixels).
constexpr size_t kBatchBytes = 64ull << 20;
constexpr size_t kMaxBatchFrames = 64;

std::string Narrow(const wchar_t* w)
{
   std::string s;
   for (; *w; ++w)
      s += (*w < 128) ? static_cast<char>(*w) : '?';
   return s;
}

std::string HrText(HRESULT hr)
{
   std::ostringstream os;
   os << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
   return os.str();
}

} // namespace

struct GpuSimulator::Impl
{
   ComPtr<ID3D11Device> dev;
   ComPtr<ID3D11DeviceContext> ctx;
   ComPtr<ID3D11ComputeShader> cs;
   ComPtr<ID3D11Buffer> cb;

   ComPtr<ID3D11Buffer> sums;
   ComPtr<ID3D11ShaderResourceView> sumsSrv;
   int os = 1, bw = 0, bh = 0, off = 0, camRad = 0;
   uint32_t planeStride = 0;

   ComPtr<ID3D11Buffer> ems;
   ComPtr<ID3D11ShaderResourceView> emsSrv;
   size_t emsCapacity = 0;

   ComPtr<ID3D11Buffer> frames;
   ComPtr<ID3D11ShaderResourceView> framesSrv;
   size_t framesCapacity = 0;

   ComPtr<ID3D11Buffer> pix;
   ComPtr<ID3D11ShaderResourceView> pixSrv;
   ComPtr<ID3D11Buffer> out;
   ComPtr<ID3D11UnorderedAccessView> outUav;
   ComPtr<ID3D11Buffer> staging;
   unsigned w = 0, h = 0;

   bool MakeStructured(UINT stride, UINT count, const void* data, bool dynamic, ComPtr<ID3D11Buffer>& buf,
                       ComPtr<ID3D11ShaderResourceView>& srv, std::string& err)
   {
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = stride * std::max<UINT>(1, count);
      d.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      d.StructureByteStride = stride;
      std::vector<uint8_t> zeros;
      D3D11_SUBRESOURCE_DATA init = {};
      if (!data)
      {
         zeros.assign(d.ByteWidth, 0);
         data = zeros.data();
      }
      init.pSysMem = data;
      buf.Reset();
      srv.Reset();
      HRESULT hr = dev->CreateBuffer(&d, &init, &buf);
      if (FAILED(hr))
      {
         err = "CreateBuffer failed: " + HrText(hr);
         return false;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_UNKNOWN;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.FirstElement = 0;
      sd.Buffer.NumElements = std::max<UINT>(1, count);
      hr = dev->CreateShaderResourceView(buf.Get(), &sd, &srv);
      if (FAILED(hr))
      {
         err = "CreateShaderResourceView failed: " + HrText(hr);
         return false;
      }
      return true;
   }
};

GpuSimulator::GpuSimulator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuSimulator::~GpuSimulator() = default;

std::unique_ptr<GpuSimulator> GpuSimulator::Create(std::string& outInfo)
{
   auto impl = std::make_unique<Impl>();
   D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
   D3D_FEATURE_LEVEL got;
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                  &impl->dev, &got, &impl->ctx);
   if (FAILED(hr))
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION,
                             &impl->dev, &got, &impl->ctx);
   if (FAILED(hr))
   {
      outInfo = "no hardware Direct3D 11 device (" + HrText(hr) + ")";
      return nullptr;
   }

   ComPtr<IDXGIDevice> dxgiDev;
   ComPtr<IDXGIAdapter> adapter;
   DXGI_ADAPTER_DESC desc = {};
   if (SUCCEEDED(impl->dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
       SUCCEEDED(adapter->GetDesc(&desc)))
   {
      outInfo = Narrow(desc.Description);
      // 0x1414 = Microsoft: the WARP / Basic Render Driver software adapter.
      if (desc.VendorId == 0x1414)
      {
         outInfo = "only a software adapter is available (" + outInfo + ")";
         return nullptr;
      }
   }

   ComPtr<ID3DBlob> blob, errors;
   hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "SimSplatNoise", nullptr, nullptr, "main", "cs_5_0",
                   D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &errors);
   if (FAILED(hr))
   {
      outInfo = "HLSL compile failed: " +
                (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : HrText(hr));
      return nullptr;
   }
   hr = impl->dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &impl->cs);
   if (FAILED(hr))
   {
      outInfo = "CreateComputeShader failed: " + HrText(hr);
      return nullptr;
   }

   D3D11_BUFFER_DESC cbd = {};
   cbd.ByteWidth = sizeof(ParamsCB);
   cbd.Usage = D3D11_USAGE_DYNAMIC;
   cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   hr = impl->dev->CreateBuffer(&cbd, nullptr, &impl->cb);
   if (FAILED(hr))
   {
      outInfo = "constant buffer: " + HrText(hr);
      return nullptr;
   }
   return std::unique_ptr<GpuSimulator>(new GpuSimulator(std::move(impl)));
}

bool GpuSimulator::SetKernel(const PsfKernelCache& cache, std::string& outError)
{
   if (!cache.valid || cache.blockSums.empty())
   {
      outError = "no kernel block sums";
      return false;
   }
   Impl& m = *impl_;
   m.os = std::max(1, cache.oversampling);
   m.bw = cache.blockSumWidth;
   m.bh = cache.blockSumWidth;
   m.off = m.os - 1;
   m.camRad = cache.halfWidthOversampled / m.os;
   m.planeStride = static_cast<uint32_t>(m.bw) * static_cast<uint32_t>(m.bh);
   std::vector<float> all;
   all.reserve(static_cast<size_t>(m.planeStride) * cache.blockSums.size());
   for (const std::vector<float>& p : cache.blockSums)
      all.insert(all.end(), p.begin(), p.end());
   return m.MakeStructured(sizeof(float), static_cast<UINT>(all.size()), all.data(), false, m.sums, m.sumsSrv,
                           outError);
}

bool GpuSimulator::SetStatic(unsigned width, unsigned height, const std::vector<float>& offset,
                             const std::vector<float>& gain, const std::vector<float>& readNoise,
                             const std::vector<float>& background, double flatBg, const CameraNoiseParams& cam,
                             std::string& outError)
{
   Impl& m = *impl_;
   const size_t n = static_cast<size_t>(width) * height;
   std::vector<float> pix(4 * n);
   for (size_t i = 0; i < n; ++i)
   {
      pix[4 * i + 0] = offset.size() == n ? offset[i] : 0.0f;
      pix[4 * i + 1] = gain.size() == n ? gain[i] : static_cast<float>(cam.gainPhotonsPerAdu);
      pix[4 * i + 2] = readNoise.size() == n ? readNoise[i] : static_cast<float>(cam.readNoiseElectrons);
      pix[4 * i + 3] = background.size() == n ? background[i] : static_cast<float>(flatBg);
   }
   if (!m.MakeStructured(4 * sizeof(float), static_cast<UINT>(n), pix.data(), false, m.pix, m.pixSrv, outError))
      return false;

   if (m.w != width || m.h != height || !m.out)
   {
      const size_t batch = std::max<size_t>(1, std::min(kMaxBatchFrames, kBatchBytes / (sizeof(uint32_t) * n)));
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * n * batch);
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      d.StructureByteStride = sizeof(uint32_t);
      m.out.Reset();
      m.outUav.Reset();
      m.staging.Reset();
      HRESULT hr = m.dev->CreateBuffer(&d, nullptr, &m.out);
      if (FAILED(hr))
      {
         outError = "output buffer: " + HrText(hr);
         return false;
      }
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.Format = DXGI_FORMAT_UNKNOWN;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = static_cast<UINT>(n * batch);
      hr = m.dev->CreateUnorderedAccessView(m.out.Get(), &ud, &m.outUav);
      if (FAILED(hr))
      {
         outError = "output UAV: " + HrText(hr);
         return false;
      }
      D3D11_BUFFER_DESC sd = {};
      sd.ByteWidth = d.ByteWidth;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = m.dev->CreateBuffer(&sd, nullptr, &m.staging);
      if (FAILED(hr))
      {
         outError = "staging buffer: " + HrText(hr);
         return false;
      }
      m.w = width;
      m.h = height;
   }
   return true;
}

size_t GpuSimulator::MaxBatchFrames() const
{
   const size_t n = std::max<size_t>(1, static_cast<size_t>(impl_->w) * impl_->h);
   return std::max<size_t>(1, std::min(kMaxBatchFrames, kBatchBytes / (sizeof(uint32_t) * n)));
}

bool GpuSimulator::RenderFrame(const std::vector<GpuSplatEmitter>& emitters, const CameraNoiseParams& cam,
                               double backgroundScale, uint32_t noiseSeed, uint32_t frame,
                               std::vector<uint16_t>& out, std::string& outError)
{
   return RenderFrames({emitters}, {frame}, {backgroundScale}, cam, noiseSeed, {&out}, outError);
}

bool GpuSimulator::RenderFrames(const std::vector<std::vector<GpuSplatEmitter>>& emitters,
                                const std::vector<uint32_t>& frames, const std::vector<double>& backgroundScales,
                                const CameraNoiseParams& cam, uint32_t noiseSeed,
                                const std::vector<std::vector<uint16_t>*>& outs, std::string& outError)
{
   Impl& m = *impl_;
   if (!m.sumsSrv || !m.pixSrv || !m.outUav)
   {
      outError = "GPU simulator not set up";
      return false;
   }
   const size_t maxBatch = MaxBatchFrames();
   if (frames.size() > maxBatch)
   {
      // Split into dispatch-sized pieces.
      for (size_t s = 0; s < frames.size(); s += maxBatch)
      {
         size_t e = std::min(frames.size(), s + maxBatch);
         std::vector<std::vector<GpuSplatEmitter>> em(emitters.begin() + s, emitters.begin() + e);
         std::vector<uint32_t> fr(frames.begin() + s, frames.begin() + e);
         std::vector<double> bs(backgroundScales.begin() + s, backgroundScales.begin() + e);
         std::vector<std::vector<uint16_t>*> os(outs.begin() + s, outs.begin() + e);
         if (!RenderFrames(em, fr, bs, cam, noiseSeed, os, outError))
            return false;
      }
      return true;
   }
   const size_t nf = frames.size();
   if (nf == 0)
      return true;

   // Concatenate the batch's emitter lists; each frame gets its range.
   std::vector<GpuSplatEmitter> all;
   std::vector<FrameInfoCpu> info(nf);
   for (size_t k = 0; k < nf; ++k)
   {
      info[k].emStart = static_cast<uint32_t>(all.size());
      info[k].emCount = static_cast<uint32_t>(emitters[k].size());
      info[k].frame = frames[k];
      info[k].bgScale = static_cast<float>(backgroundScales[k]);
      all.insert(all.end(), emitters[k].begin(), emitters[k].end());
   }

   // Dynamic buffers, grown (by doubling) when needed.
   auto upload = [&](const void* data, size_t count, size_t stride, ComPtr<ID3D11Buffer>& buf,
                     ComPtr<ID3D11ShaderResourceView>& srv, size_t& capacity) {
      if (count > capacity || !buf)
      {
         size_t cap = std::max<size_t>(256, capacity);
         while (cap < count)
            cap *= 2;
         if (!m.MakeStructured(static_cast<UINT>(stride), static_cast<UINT>(cap), nullptr, true, buf, srv, outError))
            return false;
         capacity = cap;
      }
      if (count == 0)
         return true;
      D3D11_MAPPED_SUBRESOURCE ms;
      HRESULT hr = m.ctx->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
      if (FAILED(hr))
      {
         outError = "map: " + HrText(hr);
         return false;
      }
      std::memcpy(ms.pData, data, count * stride);
      m.ctx->Unmap(buf.Get(), 0);
      return true;
   };
   if (!upload(all.data(), all.size(), sizeof(GpuSplatEmitter), m.ems, m.emsSrv, m.emsCapacity) ||
       !upload(info.data(), info.size(), sizeof(FrameInfoCpu), m.frames, m.framesSrv, m.framesCapacity))
      return false;

   ParamsCB p = {};
   p.w = m.w;
   p.h = m.h;
   p.nFrames = static_cast<uint32_t>(nf);
   p.camRad = m.camRad;
   p.os = m.os;
   p.bw = m.bw;
   p.bh = m.bh;
   p.off = m.off;
   p.planeStride = m.planeStride;
   p.seed = noiseSeed;
   p.emccd = cam.emccd ? 1u : 0u;
   p.qe = static_cast<float>(cam.quantumEfficiency);
   p.dark = static_cast<float>(cam.darkCurrentElectrons);
   p.cic = static_cast<float>(cam.cicElectrons);
   p.emGain = static_cast<float>(std::max(1.0, cam.emGain));
   p.maxAdu = static_cast<float>(std::ldexp(1.0, std::min(16, std::max(1, cam.bitDepth))) - 1.0);
   {
      D3D11_MAPPED_SUBRESOURCE ms;
      HRESULT hr = m.ctx->Map(m.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
      if (FAILED(hr))
      {
         outError = "map constants: " + HrText(hr);
         return false;
      }
      std::memcpy(ms.pData, &p, sizeof(p));
      m.ctx->Unmap(m.cb.Get(), 0);
   }

   ID3D11ShaderResourceView* srvs[4] = {m.sumsSrv.Get(), m.emsSrv.Get(), m.pixSrv.Get(), m.framesSrv.Get()};
   ID3D11UnorderedAccessView* uavs[1] = {m.outUav.Get()};
   ID3D11Buffer* cbs[1] = {m.cb.Get()};
   m.ctx->CSSetShader(m.cs.Get(), nullptr, 0);
   m.ctx->CSSetShaderResources(0, 4, srvs);
   m.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
   m.ctx->CSSetConstantBuffers(0, 1, cbs);
   m.ctx->Dispatch((m.w + 15) / 16, (m.h + 15) / 16, static_cast<UINT>(nf));
   ID3D11UnorderedAccessView* nullUav[1] = {nullptr};
   m.ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
   const size_t n = static_cast<size_t>(m.w) * m.h;
   D3D11_BOX box = {0, 0, 0, static_cast<UINT>(sizeof(uint32_t) * n * nf), 1, 1};
   m.ctx->CopySubresourceRegion(m.staging.Get(), 0, 0, 0, 0, m.out.Get(), 0, &box);

   D3D11_MAPPED_SUBRESOURCE ms;
   HRESULT hr = m.ctx->Map(m.staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
   if (FAILED(hr))
   {
      outError = "read back: " + HrText(hr);
      return false;
   }
   const uint32_t* src = static_cast<const uint32_t*>(ms.pData);
   for (size_t k = 0; k < nf; ++k)
   {
      std::vector<uint16_t>& out = *outs[k];
      out.resize(n);
      for (size_t i = 0; i < n; ++i)
         out[i] = static_cast<uint16_t>(src[k * n + i]);
   }
   m.ctx->Unmap(m.staging.Get(), 0);
   return true;
}

#else // !_WIN32

struct GpuSimulator::Impl
{
};
GpuSimulator::GpuSimulator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuSimulator::~GpuSimulator() = default;
std::unique_ptr<GpuSimulator> GpuSimulator::Create(std::string& outInfo)
{
   outInfo = "the Direct3D 11 GPU path is Windows-only";
   return nullptr;
}
bool GpuSimulator::SetKernel(const PsfKernelCache&, std::string& e) { e = "unsupported"; return false; }
bool GpuSimulator::SetStatic(unsigned, unsigned, const std::vector<float>&, const std::vector<float>&,
                             const std::vector<float>&, const std::vector<float>&, double, const CameraNoiseParams&,
                             std::string& e)
{
   e = "unsupported";
   return false;
}
bool GpuSimulator::RenderFrame(const std::vector<GpuSplatEmitter>&, const CameraNoiseParams&, double, uint32_t,
                               uint32_t, std::vector<uint16_t>&, std::string& e)
{
   e = "unsupported";
   return false;
}
bool GpuSimulator::RenderFrames(const std::vector<std::vector<GpuSplatEmitter>>&, const std::vector<uint32_t>&,
                                const std::vector<double>&, const CameraNoiseParams&, uint32_t,
                                const std::vector<std::vector<uint16_t>*>&, std::string& e)
{
   e = "unsupported";
   return false;
}
size_t GpuSimulator::MaxBatchFrames() const { return 1; }

#endif

} // namespace sim
