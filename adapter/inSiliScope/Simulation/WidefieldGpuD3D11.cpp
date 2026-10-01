///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldGpuD3D11.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldGpuD3D11.h. Mirrors web/wf_gpu.js step for step.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "WidefieldGpuD3D11.h"

#include "Illumination.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
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

constexpr uint32_t kNone = 0xffffffffu;

uint32_t Bits(float f)
{
   uint32_t b;
   std::memcpy(&b, &f, 4);
   return b;
}

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
struct Buf
{
   ComPtr<ID3D11Buffer> b;
   ComPtr<ID3D11ShaderResourceView> srv;
   ComPtr<ID3D11UnorderedAccessView> uav;
   size_t bytes = 0;
};

struct Kernel
{
   ComPtr<ID3D11ComputeShader> cs;
   bool uav[8] = {false, false, false, false, false, false, false, false}; // binding i is a UAV (else SRV)
};

} // namespace

struct WidefieldGpuD3D11::Impl
{
   ComPtr<ID3D11Device> dev;
   ComPtr<ID3D11DeviceContext> ctx;
   ComPtr<ID3D11Buffer> cb; // 64 bytes of parameters
   std::map<std::string, Kernel> kernels;

   // Geometry and resident spectra (as wf_gpu.js).
   std::string geometry;
   unsigned NX = 0, NY = 0, W = 0;
   size_t SS = 0, PS = 0;
   Buf spec16, kspec, work, tw;
   unsigned slotCap = 0, kcap = 0, pairCap = 1;
   std::vector<unsigned> freeSlots;
   struct PlaneEntry
   {
      unsigned slot;
      unsigned long long used;
      float scale;
   };
   std::map<unsigned long long, PlaneEntry> planes;
   std::map<int, unsigned> kslots;
   unsigned long long clock = 0;
   unsigned long long residentGeometry = ~0ull;

   // Frames.
   unsigned fw = 0, fh = 0;
   Buf pix, frameOut;
   ComPtr<ID3D11Buffer> staging;
   size_t stagingBytes = 0;

   bool MakeBuf(size_t bytes, const void* data, Buf& out, std::string& err)
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
      out = Buf();
      HRESULT hr = dev->CreateBuffer(&d, data ? &init : nullptr, &out.b);
      if (FAILED(hr))
      {
         err = "CreateBuffer(" + std::to_string(bytes) + " bytes) failed: " + HrText(hr);
         return false;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sd.BufferEx.NumElements = static_cast<UINT>(bytes / 4);
      sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      hr = dev->CreateShaderResourceView(out.b.Get(), &sd, &out.srv);
      if (FAILED(hr))
      {
         err = "raw SRV: " + HrText(hr);
         return false;
      }
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = static_cast<UINT>(bytes / 4);
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      hr = dev->CreateUnorderedAccessView(out.b.Get(), &ud, &out.uav);
      if (FAILED(hr))
      {
         err = "raw UAV: " + HrText(hr);
         return false;
      }
      out.bytes = bytes;
      return true;
   }

   // Buffer holding `data` (bytes long, rounded up).
   bool Upload(const void* data, size_t bytes, Buf& out, std::string& err)
   {
      return MakeBuf(bytes, data, out, err);
   }

   void Write(Buf& b, size_t offset, const void* data, size_t bytes)
   {
      if (bytes == 0)
         return;
      D3D11_BOX box = {static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + bytes), 1, 1};
      ctx->UpdateSubresource(b.b.Get(), 0, &box, data, 0, 0);
   }

   bool Params(const uint32_t* u, size_t n, std::string& err)
   {
      uint32_t p[16] = {};
      std::memcpy(p, u, std::min<size_t>(n, 16) * 4);
      D3D11_MAPPED_SUBRESOURCE ms;
      HRESULT hr = ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
      if (FAILED(hr))
      {
         err = "map constants: " + HrText(hr);
         return false;
      }
      std::memcpy(ms.pData, p, sizeof p);
      ctx->Unmap(cb.Get(), 0);
      return true;
   }

   // One dispatch: parameters, then buffers at bindings 1.. in order.
   bool Dispatch(const char* name, std::initializer_list<uint32_t> params, std::initializer_list<Buf*> bufs,
                 unsigned x, unsigned y, unsigned z, std::string& err)
   {
      auto it = kernels.find(name);
      if (it == kernels.end())
      {
         err = std::string("no kernel ") + name;
         return false;
      }
      std::vector<uint32_t> p(params);
      if (!Params(p.data(), p.size(), err))
         return false;
      const Kernel& k = it->second;
      ctx->CSSetShader(k.cs.Get(), nullptr, 0);
      ID3D11Buffer* cbs[1] = {cb.Get()};
      ctx->CSSetConstantBuffers(0, 1, cbs);
      UINT slot = 1;
      for (Buf* b : bufs)
      {
         if (k.uav[slot])
         {
            ID3D11UnorderedAccessView* u = b->uav.Get();
            ctx->CSSetUnorderedAccessViews(slot, 1, &u, nullptr);
         }
         else
         {
            ID3D11ShaderResourceView* s = b->srv.Get();
            ctx->CSSetShaderResources(slot, 1, &s);
         }
         ++slot;
      }
      ctx->Dispatch(x, y, z);
      // Unbind, so the next dispatch may bind these buffers the other way.
      ID3D11UnorderedAccessView* nu[8] = {};
      ID3D11ShaderResourceView* ns[8] = {};
      ctx->CSSetUnorderedAccessViews(0, 8, nu, nullptr);
      ctx->CSSetShaderResources(0, 8, ns);
      return true;
   }

   bool ReadBack(Buf& src, size_t bytes, void* dst, std::string& err)
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
      ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src.b.Get(), 0, &box);
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

   static void Grid256(size_t count, unsigned& x, unsigned& y, unsigned& rowLen)
   {
      const size_t groups = std::max<size_t>(1, (count + 255) / 256);
      x = static_cast<unsigned>(std::min<size_t>(groups, 65535));
      y = static_cast<unsigned>((groups + x - 1) / x);
      rowLen = x * 256;
   }

   bool Reset(const WidefieldGpuJob& job, std::string& err)
   {
      NX = job.NX;
      NY = job.NY;
      W = NX / 2 + 1;
      SS = static_cast<size_t>(W) * NY;
      PS = static_cast<size_t>(NX) * NY;
      planes.clear();
      kslots.clear();
      kspec = Buf();
      kcap = 0;
      // Resident fp16 plane spectra (up to 512 MB), work grids (up to 256 MB).
      slotCap = static_cast<unsigned>(std::max<size_t>(4, (size_t(512) << 20) / (SS * 4)));
      if (!MakeBuf(slotCap * SS * 4, nullptr, spec16, err))
      {
         slotCap = static_cast<unsigned>(std::max<size_t>(4, (size_t(128) << 20) / (SS * 4)));
         if (!MakeBuf(slotCap * SS * 4, nullptr, spec16, err))
            return false;
      }
      freeSlots.clear();
      for (unsigned i = slotCap; i-- > 0;)
         freeSlots.push_back(i);
      pairCap = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(32, (size_t(256) << 20) / (PS * 8))));
      if (!MakeBuf(pairCap * PS * 8, nullptr, work, err))
         return false;
      std::vector<float> t(static_cast<size_t>(NX) + NY);
      const double pi = 3.14159265358979323846;
      for (int pass = 0; pass < 2; ++pass)
      {
         const unsigned n = pass == 0 ? NX : NY, off = pass == 0 ? 0 : NX / 2;
         for (unsigned k = 0; k < n / 2; ++k)
         {
            const double a = -2.0 * pi * k / n;
            t[2 * (off + k)] = static_cast<float>(std::cos(a));
            t[2 * (off + k) + 1] = static_cast<float>(std::sin(a));
         }
      }
      return Upload(t.data(), t.size() * 4, tw, err);
   }

   static unsigned Log2(unsigned n)
   {
      unsigned l = 0;
      while ((1u << l) < n)
         ++l;
      return l;
   }

   bool Fft2(unsigned pairs, bool inverse, std::string& err)
   {
      const uint32_t inv = inverse ? 1u : 0u;
      return Dispatch("wf_fft", {NX, Log2(NX), 1, NX, static_cast<uint32_t>(PS), inv, 0, 0}, {&work, &tw}, NY, 1, pairs,
                      err) &&
             Dispatch("wf_fft", {NY, Log2(NY), NX, 1, static_cast<uint32_t>(PS), inv, NX / 2, 0}, {&work, &tw}, NX, 1,
                      pairs, err);
   }

   bool EnsureKernels(const WidefieldGpuJob& job, std::string& err)
   {
      std::vector<const WidefieldGpuJob::Kernel*> missing;
      for (const WidefieldGpuJob::Kernel& k : job.kernels)
         if (!kslots.count(k.p))
            missing.push_back(&k);
      if (missing.empty())
         return true;
      const size_t need = kslots.size() + missing.size(), bytes = SS * 8;
      if (need > kcap)
      {
         const unsigned cap = static_cast<unsigned>(std::max<size_t>(need, std::max<size_t>(16, 2 * kcap)));
         Buf nb;
         if (!MakeBuf(cap * bytes, nullptr, nb, err))
            return false;
         if (kspec.b)
         {
            D3D11_BOX box = {0, 0, 0, static_cast<UINT>(kslots.size() * bytes), 1, 1};
            ctx->CopySubresourceRegion(nb.b.Get(), 0, 0, 0, 0, kspec.b.Get(), 0, &box);
         }
         kspec = nb;
         kcap = cap;
      }
      for (const WidefieldGpuJob::Kernel* k : missing)
      {
         const unsigned slot = static_cast<unsigned>(kslots.size());
         kslots[k->p] = slot;
         Write(kspec, slot * bytes, k->spec->data(), bytes);
      }
      return true;
   }

   bool EnsurePlanes(const WidefieldGpuJob& job, std::string& err)
   {
      const unsigned long long stamp = ++clock;
      for (const auto& ch : job.channels)
         for (const WidefieldGpuJob::Dep& d : ch)
         {
            auto it = planes.find(d.key);
            if (it != planes.end())
               it->second.used = stamp;
         }
      std::vector<const WidefieldGpuJob::Plane*> todo;
      for (const WidefieldGpuJob::Plane& p : job.planes)
         if (!planes.count(p.key) && !p.cells.empty())
            todo.push_back(&p);
      if (todo.size() > freeSlots.size())
      {
         std::vector<std::pair<unsigned long long, unsigned long long>> victims; // used, key
         for (const auto& kv : planes)
            if (kv.second.used < stamp)
               victims.push_back({kv.second.used, kv.first});
         std::sort(victims.begin(), victims.end());
         for (size_t v = 0; v < victims.size() && todo.size() > freeSlots.size(); ++v)
         {
            freeSlots.push_back(planes[victims[v].second].slot);
            planes.erase(victims[v].second);
         }
         if (todo.size() > freeSlots.size())
         {
            err = "GPU plane spectra do not fit";
            return false;
         }
      }
      for (const WidefieldGpuJob::Plane* p : todo)
      {
         planes[p->key] = {freeSlots.back(), stamp, p->absSum};
         freeSlots.pop_back();
      }
      for (size_t b0 = 0; b0 < todo.size(); b0 += 2 * pairCap)
      {
         const size_t b1 = std::min(todo.size(), b0 + 2 * pairCap);
         const unsigned pairs = static_cast<unsigned>((b1 - b0 + 1) / 2);
         std::vector<uint32_t> entries, slots(4 * pairs, 0);
         for (size_t i = b0; i < b1; ++i)
         {
            const WidefieldGpuJob::Plane& p = *todo[i];
            const uint32_t pair = static_cast<uint32_t>((i - b0) / 2), comp = static_cast<uint32_t>((i - b0) & 1);
            for (size_t j = 0; j < p.cells.size(); ++j)
            {
               entries.push_back(pair);
               entries.push_back(p.cells[j]);
               entries.push_back(Bits(p.values[j]));
               entries.push_back(comp);
            }
            slots[4 * pair + comp] = planes[p.key].slot;
            slots[4 * pair + 2 + comp] = Bits(1.0f / p.absSum);
            if (comp == 0)
               slots[4 * pair + 1] = kNone;
         }
         unsigned cx, cy, cl, sx, sy, sl;
         Grid256(pairs * PS, cx, cy, cl);
         if (!Dispatch("wf_clear", {static_cast<uint32_t>(pairs * PS), cl, 0, 0}, {&work}, cx, cy, 1, err))
            return false;
         const size_t n = entries.size() / 4;
         Buf eb, sb;
         if (!Upload(entries.data(), entries.size() * 4, eb, err) || !Upload(slots.data(), slots.size() * 4, sb, err))
            return false;
         Grid256(n, sx, sy, sl);
         if (!Dispatch("wf_scatter", {static_cast<uint32_t>(n), job.nx, NX, static_cast<uint32_t>(PS), sl, 0, 0, 0},
                       {&eb, &work}, sx, sy, 1, err) ||
             !Fft2(pairs, false, err) ||
             !Dispatch("wf_split", {NX, NY, W, static_cast<uint32_t>(SS), static_cast<uint32_t>(PS), pairs, 0, 0},
                       {&work, &sb, &spec16}, (W + 15) / 16, (NY + 15) / 16, pairs, err))
            return false;
      }
      return true;
   }
};

WidefieldGpuD3D11::WidefieldGpuD3D11(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WidefieldGpuD3D11::~WidefieldGpuD3D11() = default;

bool WidefieldGpuD3D11::HasPlane(unsigned long long geometry, unsigned long long key) const
{
   return impl_->residentGeometry == geometry && impl_->planes.count(key) != 0;
}

bool WidefieldGpuD3D11::Images(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err)
{
   try
   {
      return ImagesImpl(job, out, err);
   }
   catch (const std::exception& e)
   {
      err = std::string("exception: ") + e.what();
   }
   catch (...)
   {
      err = "exception";
   }
   out.clear();
   return false;
}

bool WidefieldGpuD3D11::ImagesImpl(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err)
{
   Impl& m = *impl_;
   out.clear();
   std::ostringstream geo;
   geo << job.geometry << ':' << job.NX << 'x' << job.NY << ':' << job.nx << 'x' << job.ny;
   if (geo.str() != m.geometry)
   {
      m.geometry = geo.str();
      m.residentGeometry = ~0ull;
      if (!m.Reset(job, err))
         return false;
   }
   m.residentGeometry = job.geometry;
   if (!m.EnsureKernels(job, err) || !m.EnsurePlanes(job, err))
      return false;
   const unsigned nChan = static_cast<unsigned>(job.channels.size());
   if (nChan == 0)
      return true;
   std::vector<uint32_t> depI, chans;
   std::vector<float> depW;
   for (const auto& ch : job.channels)
   {
      chans.push_back(static_cast<uint32_t>(depW.size() / 2));
      chans.push_back(static_cast<uint32_t>(ch.size()));
      for (const WidefieldGpuJob::Dep& d : ch)
      {
         auto pe = m.planes.find(d.key);
         auto k0 = m.kslots.find(d.p0), k1 = m.kslots.find(d.p1);
         if (pe == m.planes.end() || k0 == m.kslots.end() || (d.two && k1 == m.kslots.end()))
         {
            std::ostringstream os;
            os << "job refers to " << (pe == m.planes.end() ? "a plane" : "a kernel") << " it does not carry (key "
               << d.key << ", planes " << m.planes.size() << ", job planes " << job.planes.size() << ", p " << d.p0
               << "/" << d.p1 << ", kernels " << m.kslots.size() << ")";
            err = os.str();
            return false;
         }
         depI.push_back(pe->second.slot);
         depI.push_back(k0->second);
         depI.push_back(d.two ? k1->second : kNone);
         depI.push_back(Bits(pe->second.scale));
         depW.push_back(d.w0);
         depW.push_back(d.w1);
      }
   }
   if (depI.empty())
   {
      depI.assign(4, 0);
      depW.assign(2, 0.0f);
   }
   Buf chanSpec, bI, bW, bC, imgs;
   const size_t cs = static_cast<size_t>(job.cw) * job.ch;
   if (!m.MakeBuf(nChan * m.SS * 8, nullptr, chanSpec, err) || !m.Upload(depI.data(), depI.size() * 4, bI, err) ||
       !m.Upload(depW.data(), depW.size() * 4, bW, err) || !m.Upload(chans.data(), chans.size() * 4, bC, err) ||
       !m.MakeBuf(nChan * cs * 4, nullptr, imgs, err))
      return false;
   unsigned mx, my, ml;
   Impl::Grid256(m.SS, mx, my, ml);
   if (!m.Dispatch("wf_mac", {static_cast<uint32_t>(m.SS), nChan, ml, 0}, {&m.spec16, &m.kspec, &bI, &bW, &bC, &chanSpec},
                   mx, my, nChan, err))
      return false;
   std::vector<uint32_t> pairList;
   for (unsigned c = 0; c < nChan; c += 2)
   {
      pairList.push_back(c);
      pairList.push_back(c + 1 < nChan ? c + 1 : kNone);
   }
   const unsigned nPairs = static_cast<unsigned>(pairList.size() / 2);
   for (unsigned p0 = 0; p0 < nPairs; p0 += m.pairCap)
   {
      const unsigned pairs = std::min(m.pairCap, nPairs - p0);
      Buf pb;
      if (!m.Upload(pairList.data() + 2 * p0, pairs * 8, pb, err))
         return false;
      if (!m.Dispatch("wf_expand",
                      {m.NX, m.NY, m.W, static_cast<uint32_t>(m.SS), static_cast<uint32_t>(m.PS), pairs,
                       Bits(static_cast<float>(job.fracX)), Bits(static_cast<float>(job.fracY))},
                      {&chanSpec, &pb, &m.work}, (m.NX + 15) / 16, (m.NY + 15) / 16, pairs, err) ||
          !m.Fft2(pairs, true, err) ||
          !m.Dispatch("wf_crop",
                      {m.NX, static_cast<uint32_t>(m.PS), job.fovX0, job.fovY0, job.cw, job.ch,
                       Bits(1.0f / (static_cast<float>(m.NX) * static_cast<float>(m.NY))), 0},
                      {&m.work, &pb, &imgs}, (job.cw + 15) / 16, (job.ch + 15) / 16, pairs, err))
         return false;
   }
   std::vector<float> all(nChan * cs);
   if (!m.ReadBack(imgs, all.size() * 4, all.data(), err))
      return false;
   out.resize(nChan);
   for (unsigned c = 0; c < nChan; ++c)
      out[c].assign(all.begin() + c * cs, all.begin() + (c + 1) * cs);
   return true;
}

bool WidefieldGpuD3D11::SetFrameStatic(unsigned width, unsigned height, const std::vector<float>& offset,
                                       const std::vector<float>& gain, const std::vector<float>& readNoise,
                                       const std::vector<float>& background, double flatBg,
                                       const CameraNoiseParams& cam, std::string& err)
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
   m.fw = width;
   m.fh = height;
   return m.Upload(pix.data(), pix.size() * 4, m.pix, err);
}

bool WidefieldGpuD3D11::RenderFrames(const WidefieldImages& images, const std::vector<std::vector<double>>& coef,
                                     const std::vector<uint32_t>& frames, const std::vector<double>& bgScale,
                                     const CameraNoiseParams& cam, uint32_t seed,
                                     const std::vector<std::vector<uint16_t>*>& outs, std::string& err)
{
   Impl& m = *impl_;
   if (!m.pix.b || images.cw != m.fw * images.upscale || images.ch != m.fh * images.upscale)
   {
      err = "frame inputs not set for this size";
      return false;
   }
   const size_t cs = static_cast<size_t>(images.cw) * images.ch, n = static_cast<size_t>(m.fw) * m.fh;
   const bool hasP = !images.persistent.empty();
   const unsigned nB = static_cast<unsigned>(images.bleach.size());
   std::vector<float> all((hasP ? 1 : 0) * cs + nB * cs);
   if (hasP)
      std::copy(images.persistent.begin(), images.persistent.end(), all.begin());
   for (unsigned j = 0; j < nB; ++j)
      std::copy(images.bleach[j].begin(), images.bleach[j].end(), all.begin() + ((hasP ? 1 : 0) + j) * cs);
   if (all.empty())
      all.assign(1, 0.0f);
   Buf imgs;
   if (!m.Upload(all.data(), all.size() * 4, imgs, err))
      return false;
   // Batches bounded by 64 MB of output.
   const size_t batch = std::max<size_t>(1, std::min<size_t>(64, (size_t(64) << 20) / (4 * n)));
   if (!m.frameOut.b || m.frameOut.bytes < batch * n * 4)
      if (!m.MakeBuf(batch * n * 4, nullptr, m.frameOut, err))
         return false;
   const double maxAdu = std::ldexp(1.0, std::min(16, std::max(1, cam.bitDepth))) - 1.0;
   std::vector<uint32_t> host(batch * n);
   for (size_t f0 = 0; f0 < frames.size(); f0 += batch)
   {
      const size_t nf = std::min(batch, frames.size() - f0);
      std::vector<float> c(std::max<size_t>(1, nf * nB), 0.0f);
      std::vector<uint32_t> fr(2 * nf);
      for (size_t k = 0; k < nf; ++k)
      {
         for (unsigned j = 0; j < nB && j < coef[f0 + k].size(); ++j)
            c[k * nB + j] = static_cast<float>(coef[f0 + k][j]);
         fr[2 * k] = frames[f0 + k];
         fr[2 * k + 1] = Bits(static_cast<float>(bgScale[f0 + k]));
      }
      Buf bc, bf;
      if (!m.Upload(c.data(), c.size() * 4, bc, err) || !m.Upload(fr.data(), fr.size() * 4, bf, err))
         return false;
      if (!m.Dispatch("wf_frame",
                      {m.fw, m.fh, images.cw, images.upscale, nB, hasP ? 1u : 0u, static_cast<uint32_t>(nf), seed,
                       cam.emccd ? 1u : 0u, Bits(static_cast<float>(cam.quantumEfficiency)),
                       Bits(static_cast<float>(cam.darkCurrentElectrons)), Bits(static_cast<float>(cam.cicElectrons)),
                       Bits(static_cast<float>(std::max(1.0, cam.emGain))), Bits(static_cast<float>(maxAdu)), 0, 0},
                      {&imgs, &bc, &bf, &m.pix, &m.frameOut}, (m.fw + 15) / 16, (m.fh + 15) / 16,
                      static_cast<unsigned>(nf), err) ||
          !m.ReadBack(m.frameOut, nf * n * 4, host.data(), err))
         return false;
      for (size_t k = 0; k < nf; ++k)
      {
         std::vector<uint16_t>& o = *outs[f0 + k];
         o.resize(n);
         for (size_t i = 0; i < n; ++i)
            o[i] = static_cast<uint16_t>(host[k * n + i]);
      }
   }
   return true;
}

namespace {

// Self-check: a synthetic scene (random dyes over a few planes, Gaussian
// PSF, sub-cell shift, both populations) on the GPU against the CPU.
bool SelfCheck(WidefieldGpuD3D11& gpu, std::string& err)
{
   WidefieldSceneSpec s;
   s.originXUm = 3.037;
   s.originYUm = -1.981;
   s.width = s.height = 48;
   s.pixelUm = 0.1;
   s.focusWorldUm = s.slabCentreUm = 1.0;
   s.slabHalfUm = 1.0;
   s.grid.upscale = 1;
   s.eta = 0.3;
   SquareIllumination ill(4.8, 4.8);
   const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s);
   GaussianWidefieldPsf psf(g.pitchUm, 660, 1.4, 1.518);
   WidefieldDyeGrid G;
   G.spec = g;
   G.k0 = static_cast<long>(std::floor(0.7 / g.zPlaneUm));
   G.nz = 24;
   const size_t n = static_cast<size_t>(g.nx) * g.ny * G.nz;
   std::mt19937 rng(12345);
   G.bleaching.resize(n);
   G.persistent.resize(n);
   for (size_t i = 0; i < n; ++i)
   {
      G.bleaching[i] = rng() % 23 == 0 ? static_cast<float>(1 + rng() % 3) : 0.0f;
      G.persistent[i] = rng() % 17 == 0 ? 1.0f : 0.0f;
      G.nBleaching += static_cast<long>(G.bleaching[i]);
      G.nPersistent += static_cast<long>(G.persistent[i]);
   }
   WidefieldScene cpu;
   cpu.SetGpuMode(true);
   if (!cpu.UpdateFromGrid(G, ill, s, psf, err))
      return false;
   std::vector<float> wb;
   cpu.FreshBleachWeights(3, wb);
   cpu.SetBleachWeights(wb);
   WidefieldGpuJob job;
   if (!cpu.MakeGpuJob(job))
   {
      err = "self-check job";
      return false;
   }
   std::vector<std::vector<float>> imgs;
   if (!gpu.Images(job, imgs, err))
      return false;
   const WidefieldImages& ref = cpu.Images();
   std::vector<const std::vector<float>*> want;
   if (!ref.persistent.empty())
      want.push_back(&ref.persistent);
   for (const auto& b : ref.bleach)
      want.push_back(&b);
   if (imgs.size() != want.size())
   {
      err = "self-check: channel count";
      return false;
   }
   double e2 = 0.0, s2 = 0.0;
   for (size_t c = 0; c < want.size(); ++c)
      for (size_t i = 0; i < want[c]->size() && i < imgs[c].size(); ++i)
      {
         const double d = imgs[c][i] - (*want[c])[i];
         e2 += d * d;
         s2 += (*want[c])[i] * (*want[c])[i];
      }
   const double rms = s2 > 0 ? std::sqrt(e2 / s2) : 1.0;
   if (!(rms < 1e-3))
   {
      std::ostringstream os;
      os << "self-check: GPU images differ from the CPU's by " << rms << " rms";
      err = os.str();
      return false;
   }
   // Frames: the same noise chain as the CPU (float vs double transcendentals
   // may move a count at a boundary).
   CameraNoiseParams cam;
   cam.gainPhotonsPerAdu = 0.25;
   cam.readNoiseElectrons = 1.2;
   PixelOffsetMap off;
   PixelGainMap gm;
   PixelReadNoiseMap rn;
   std::mt19937_64 r64(7);
   off.Generate(s.width, s.height, 100.0, 0.5, r64);
   gm.Generate(s.width, s.height, cam.gainPhotonsPerAdu, 0.05, r64);
   rn.Generate(s.width, s.height, cam.readNoiseElectrons, 0.2, r64);
   if (!gpu.SetFrameStatic(s.width, s.height, off.offset, gm.gainPhotonsPerAdu, rn.readNoiseElectrons, {}, 2.0, cam, err))
      return false;
   std::vector<double> a;
   cpu.BleachCoefficients(wb, a);
   std::vector<uint16_t> gf;
   if (!gpu.RenderFrames(ref, {a}, {5}, {1.0}, cam, 99, {&gf}, err))
      return false;
   std::vector<float> ph(static_cast<size_t>(s.width) * s.height, 2.0f);
   ref.Render(a, ph);
   std::vector<uint16_t> cf;
   ApplyNoiseChain(ph, cf, s.width, s.height, cam, off, gm, rn, 99, 5);
   size_t same = 0;
   for (size_t i = 0; i < cf.size(); ++i)
      same += gf[i] == cf[i] ? 1 : 0;
   if (same < cf.size() * 9 / 10)
   {
      err = "self-check: GPU frame noise differs from the CPU's (" + std::to_string(same) + "/" +
            std::to_string(cf.size()) + " pixels equal)";
      return false;
   }
   return true;
}

} // namespace

std::unique_ptr<WidefieldGpuD3D11> WidefieldGpuD3D11::Create(std::string& info)
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
      info = "no hardware Direct3D 11 device (" + HrText(hr) + ")";
      return nullptr;
   }
   ComPtr<IDXGIDevice> dxgiDev;
   ComPtr<IDXGIAdapter> adapter;
   DXGI_ADAPTER_DESC desc = {};
   std::string name = "Direct3D 11";
   if (SUCCEEDED(impl->dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
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
      hr = impl->dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &kern.cs);
      if (FAILED(hr))
      {
         info = std::string("CreateComputeShader ") + k.name + ": " + HrText(hr);
         return nullptr;
      }
      // Which bindings are UAVs: "register(uN)" in the generated HLSL.
      const std::string src(k.hlsl);
      for (int b = 1; b < 8; ++b)
         kern.uav[b] = src.find("register(u" + std::to_string(b) + ")") != std::string::npos;
      impl->kernels[k.name] = kern;
   }
   D3D11_BUFFER_DESC cbd = {};
   cbd.ByteWidth = 64;
   cbd.Usage = D3D11_USAGE_DYNAMIC;
   cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   hr = impl->dev->CreateBuffer(&cbd, nullptr, &impl->cb);
   if (FAILED(hr))
   {
      info = "constant buffer: " + HrText(hr);
      return nullptr;
   }
   std::unique_ptr<WidefieldGpuD3D11> gpu(new WidefieldGpuD3D11(std::move(impl)));
   std::string err;
   if (!SelfCheck(*gpu, err))
   {
      info = name + " failed the WideField self-check (" + err + ")";
      return nullptr;
   }
   info = name;
   return gpu;
}

#else // !_WIN32

struct WidefieldGpuD3D11::Impl
{
};
WidefieldGpuD3D11::WidefieldGpuD3D11(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WidefieldGpuD3D11::~WidefieldGpuD3D11() = default;
std::unique_ptr<WidefieldGpuD3D11> WidefieldGpuD3D11::Create(std::string& info)
{
   info = "the Direct3D 11 GPU path is Windows-only";
   return nullptr;
}
bool WidefieldGpuD3D11::HasPlane(unsigned long long, unsigned long long) const { return false; }
bool WidefieldGpuD3D11::Images(const WidefieldGpuJob&, std::vector<std::vector<float>>&, std::string& e)
{
   e = "unsupported";
   return false;
}
bool WidefieldGpuD3D11::ImagesImpl(const WidefieldGpuJob&, std::vector<std::vector<float>>&, std::string& e)
{
   e = "unsupported";
   return false;
}
bool WidefieldGpuD3D11::SetFrameStatic(unsigned, unsigned, const std::vector<float>&, const std::vector<float>&,
                                       const std::vector<float>&, const std::vector<float>&, double,
                                       const CameraNoiseParams&, std::string& e)
{
   e = "unsupported";
   return false;
}
bool WidefieldGpuD3D11::RenderFrames(const WidefieldImages&, const std::vector<std::vector<double>>&,
                                     const std::vector<uint32_t>&, const std::vector<double>&,
                                     const CameraNoiseParams&, uint32_t, const std::vector<std::vector<uint16_t>*>&,
                                     std::string& e)
{
   e = "unsupported";
   return false;
}

#endif

} // namespace sim
