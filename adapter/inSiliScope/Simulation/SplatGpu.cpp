// SPDX-License-Identifier: BSD-3-Clause
// The blink splat + camera noise on a GPU backend: SplatGpu.h.
#include "SplatGpu.h"

#include "SMLMSimulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>

namespace sim {

namespace {

// Output per dispatch: frames = this / (4 bytes x pixels), at most kMaxBatchFrames (the adapter's numbers).
constexpr size_t kBatchBytes = 64ull << 20;
constexpr size_t kMaxBatchFrames = 64;
constexpr size_t kKeepKernels = 8;   // resident buffers (a kernel past one binding: one per range of its planes)
constexpr uint32_t kNoExtra = 0xffffffffu;
constexpr uint32_t kTableWords = 32;   // KGeo[4] + KMisc[4]
constexpr uint32_t kFrameWords = 8;

uint32_t Bits(float f)
{
   uint32_t u;
   std::memcpy(&u, &f, 4);
   return u;
}

} // namespace

struct SplatGpuHost::State
{
   struct Resident
   {
      uint64_t serial = 0;
      int chunk = 0, planes = 0;   // a range of the kernel's planes (SetKernels)
      std::shared_ptr<const PsfKernelPlanes> hold;   // (its serial stays unique while held)
      GpuBackend::BufPtr sums;
      uint64_t used = 0;
   };
   std::vector<Resident> kernels;
   uint64_t tick = 0;
   uint32_t table[kTableWords] = {};
   GpuBackend::BufPtr bufs[kMaxKernels];
   bool haveKernels = false;
   // Per kernel an emitter names: its first table entry and planes per entry; split: the records are renumbered.
   int entry0[kMaxKernels] = {};
   int perChunk[kMaxKernels] = {};
   bool split = false;
   std::vector<uint32_t> spans;   // int32 pairs, the kernels' halo spans (one dummy pair)

   unsigned w = 0, h = 0;
   uint32_t pixelBase = 0;
   uint64_t statKey = 0;
   std::vector<uint32_t> pix;   // 4 words per pixel
   bool statDirty = true;
   GpuBackend::BufPtr stat, dyn, out, none;
   std::vector<uint32_t> dynWords;
   std::vector<uint32_t> outWords;
};

SplatGpuHost::SplatGpuHost(std::shared_ptr<GpuBackend> backend) : backend_(std::move(backend)), s_(new State) {}
SplatGpuHost::~SplatGpuHost() = default;

bool SplatGpuHost::SetKernels(const std::vector<const PsfKernelCache*>& kernels, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   State& m = *s_;
   if (kernels.size() > kMaxKernels)
   {
      err = "the GPU splat takes up to " + std::to_string(kMaxKernels) + " kernels, not " + std::to_string(kernels.size());
      return false;
   }
   std::memset(m.table, 0, sizeof m.table);
   std::vector<uint32_t> spans(2, 0);
   // The table's entries: a kernel whose block sums pass one binding (a default 7 um kernel is ~200 MB; WebGPU's
   // default binding limit 128 MB) takes one entry per range of planes, each with a buffer of its own.
   struct Held
   {
      uint64_t serial;
      int chunk;
   };
   std::vector<Held> held;   // by buffer slot
   int entries = 0;
   m.split = false;
   ++m.tick;
   for (size_t k = 0; k < kernels.size(); ++k)
   {
      const PsfKernelCache* cache = kernels[k];
      if (!cache || !cache->valid || !cache->data || cache->Planes().empty() ||
          cache->interpMode == PsfInterpMode::Fft)
      {
         err = "no kernel block sums";
         return false;
      }
      // The same planes and cut as an earlier kernel (groups at one wavelength): its entries.
      size_t same = 0;
      while (same < k && !(kernels[same]->Serial() == cache->Serial() &&
                           kernels[same]->SplatSerial() == cache->SplatSerial()))
         ++same;
      if (same < k)
      {
         m.entry0[k] = m.entry0[same];
         m.perChunk[k] = m.perChunk[same];
         m.split = m.split || m.entry0[k] != static_cast<int>(k);
         continue;
      }
      const int os = std::max(1, cache->oversampling);
      const int bw = cache->blockSumWidth;
      const int camRad = cache->halfWidthOversampled / os;
      const int nz = static_cast<int>(cache->Planes().size());
      const size_t planeBytes = static_cast<size_t>(bw) * bw * sizeof(float);
      const int fit = static_cast<int>(std::max<size_t>(1, backend_->MaxBinding() / std::max<size_t>(1, planeBytes)));
      const int chunks = (nz + fit - 1) / fit;
      const int per = (nz + chunks - 1) / chunks;
      if (entries + chunks > static_cast<int>(kMaxKernels))
      {
         err = "the kernels' block sums need more than " + std::to_string(kMaxKernels) + " buffers of at most " +
               std::to_string(backend_->MaxBinding() >> 20) + " MB";
         return false;
      }
      m.entry0[k] = entries;
      m.perChunk[k] = per;
      m.split = m.split || chunks > 1 || entries != static_cast<int>(k);
      const bool useSpans = cache->halo && cache->halo->camRad == camRad && cache->halo->nz == nz;
      const uint32_t spanBase = static_cast<uint32_t>(spans.size() / 2);
      if (useSpans)
         for (size_t i = 0; i < cache->halo->lo.size(); ++i)
         {
            spans.push_back(static_cast<uint32_t>(static_cast<int32_t>(cache->halo->lo[i])));
            spans.push_back(static_cast<uint32_t>(static_cast<int32_t>(cache->halo->hi[i])));
         }
      const uint64_t serial = cache->Serial();
      for (int c = 0; c < chunks; ++c)
      {
         const int e = entries++;
         const int z0 = c * per, z1 = std::min(nz, z0 + per);
         int32_t* geo = reinterpret_cast<int32_t*>(m.table + 4 * e);
         uint32_t* misc = m.table + 16 + 4 * e;
         geo[0] = camRad;
         geo[1] = os;
         geo[2] = bw;
         geo[3] = os - 1;
         misc[0] = static_cast<uint32_t>(bw) * static_cast<uint32_t>(bw);
         misc[1] = useSpans ? 1u : 0u;
         misc[2] = spanBase + static_cast<uint32_t>(z0) * static_cast<uint32_t>(2 * camRad + 1);
         size_t slot = 0;
         while (slot < held.size() && !(held[slot].serial == serial && held[slot].chunk == c))
            ++slot;
         misc[3] = static_cast<uint32_t>(slot);
         if (slot < held.size())
            continue;
         held.push_back({serial, c});
         auto it = std::find_if(m.kernels.begin(), m.kernels.end(), [&](const State::Resident& r) {
            return r.serial == serial && r.chunk == c && r.planes == z1 - z0;
         });
         if (it == m.kernels.end())
         {
            // Row-major per plane, as the shader reads them (the stack keeps the polyphase layout).
            std::vector<float> all;
            all.reserve(static_cast<size_t>(misc[0]) * static_cast<size_t>(z1 - z0));
            cache->PrepareSplat();
            for (int z = z0; z < z1; ++z)
            {
               const std::vector<float> pl = cache->BlockSums(z).RowMajor();
               all.insert(all.end(), pl.begin(), pl.end());
            }
            // The least recently used beyond kKeepKernels go first (not this call's).
            while (m.kernels.size() >= kKeepKernels)
            {
               auto old = std::min_element(m.kernels.begin(), m.kernels.end(),
                                           [](const State::Resident& x, const State::Resident& y) { return x.used < y.used; });
               if (old->used == m.tick)
                  break;
               m.kernels.erase(old);
            }
            State::Resident r;
            r.serial = serial;
            r.chunk = c;
            r.planes = z1 - z0;
            r.hold = cache->data;
            r.sums = backend_->Make(all.size() * sizeof(float), all.data(), err);
            if (!r.sums)
            {
               err = "kernel block sums: " + err;
               return false;
            }
            m.kernels.push_back(std::move(r));
            it = m.kernels.end() - 1;
         }
         it->used = m.tick;
         m.bufs[slot] = it->sums;
      }
   }
   if (held.empty())
   {
      // No kernel (a movie without splatted blinks: the noise alone): a buffer to bind.
      if (!m.none)
         m.none = backend_->Make(16, nullptr, err);
      if (!m.none)
         return false;
      m.bufs[0] = m.none;
   }
   for (size_t b = std::max<size_t>(1, held.size()); b < kMaxKernels; ++b)
      m.bufs[b] = m.bufs[0];   // (bound, never read)
   if (spans != m.spans)
   {
      m.spans = std::move(spans);
      m.statDirty = true;
   }
   m.haveKernels = true;
   return true;
}

bool SplatGpuHost::SetStatic(unsigned width, unsigned height, uint32_t pixelBase, const std::vector<float>& offset,
                             const std::vector<float>& gain, const std::vector<float>& readNoise,
                             const CameraNoiseParams& cam, uint64_t key, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   (void)err;
   State& m = *s_;
   if (key != 0 && key == m.statKey && width == m.w && height == m.h && pixelBase == m.pixelBase)
      return true;
   const size_t n = static_cast<size_t>(width) * height;
   m.pix.resize(4 * n);
   for (size_t i = 0; i < n; ++i)
   {
      m.pix[4 * i + 0] = Bits(offset.size() == n ? offset[i] : 0.0f);
      m.pix[4 * i + 1] = Bits(gain.size() == n ? gain[i] : static_cast<float>(cam.gainPhotonsPerAdu));
      m.pix[4 * i + 2] = Bits(readNoise.size() == n ? readNoise[i] : static_cast<float>(cam.readNoiseElectrons));
      m.pix[4 * i + 3] = 0;   // (the background comes in the extra image)
   }
   if (width != m.w || height != m.h)
      m.out.reset();
   m.w = width;
   m.h = height;
   m.pixelBase = pixelBase;
   m.statKey = key;
   m.statDirty = true;
   return true;
}

size_t SplatGpuHost::MaxBatchFrames() const
{
   const size_t n = std::max<size_t>(1, static_cast<size_t>(s_->w) * s_->h);
   return std::max<size_t>(1, std::min(kMaxBatchFrames, kBatchBytes / (sizeof(uint32_t) * n)));
}

bool SplatGpuHost::RenderFrames(const std::vector<std::vector<GpuSplatEmitter>>& emitters,
                                const std::vector<uint32_t>& frames, const std::vector<const std::vector<float>*>& extras,
                                const CameraNoiseParams& cam, uint32_t seed,
                                const std::vector<std::vector<uint16_t>*>& outs, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   State& m = *s_;
   GpuBackend& be = *backend_;
   if (!m.haveKernels || m.w == 0 || m.h == 0)
   {
      err = "the GPU splat is not set up";
      return false;
   }
   const size_t n = static_cast<size_t>(m.w) * m.h;
   const size_t maxBatch = MaxBatchFrames();
   if (m.statDirty || !m.stat)
   {
      std::vector<uint32_t> words(m.pix);
      words.insert(words.end(), m.spans.begin(), m.spans.end());
      m.stat = be.Make(words.size() * 4, words.data(), err);
      if (!m.stat)
      {
         err = "camera maps: " + err;
         return false;
      }
      m.statDirty = false;
   }
   if (!m.out)
   {
      m.out = be.Make(sizeof(uint32_t) * n * maxBatch, nullptr, err);
      if (!m.out)
      {
         err = "output: " + err;
         return false;
      }
   }
   for (size_t s0 = 0; s0 < frames.size(); s0 += maxBatch)
   {
      const size_t nf = std::min(maxBatch, frames.size() - s0);
      // Dyn: the kernel table, the frames, their emitters, their extra images.
      size_t emTotal = 0;
      for (size_t k = 0; k < nf; ++k)
         emTotal += emitters[s0 + k].size();
      const size_t emAt = kTableWords + kFrameWords * nf;
      size_t at = emAt + 16 * emTotal;
      std::vector<uint32_t>& d = m.dynWords;
      d.assign(at, 0);
      std::memcpy(d.data(), m.table, sizeof m.table);
      size_t em = 0;
      for (size_t k = 0; k < nf; ++k)
      {
         const std::vector<GpuSplatEmitter>& es = emitters[s0 + k];
         uint32_t* fi = d.data() + kTableWords + kFrameWords * k;
         fi[0] = static_cast<uint32_t>(em);
         fi[1] = static_cast<uint32_t>(es.size());
         fi[2] = frames[s0 + k];
         fi[3] = Bits(0.0f);
         fi[4] = kNoExtra;
         if (!es.empty())
            std::memcpy(d.data() + emAt + 16 * em, es.data(), es.size() * sizeof(GpuSplatEmitter));
         if (m.split)
            for (size_t j = 0; j < es.size(); ++j)
            {
               // (kernel, plane) -> (its entry for the plane's range, the plane within it)
               uint32_t& w = d[emAt + 16 * (em + j) + 4];
               const uint32_t kk = std::min<uint32_t>(w >> 16, kMaxKernels - 1), pl = w & 0xffffu;
               const uint32_t per = static_cast<uint32_t>(std::max(1, m.perChunk[kk])), c = pl / per;
               w = ((static_cast<uint32_t>(m.entry0[kk]) + c) << 16) | (pl - c * per);
            }
         em += es.size();
         const std::vector<float>* x = extras.size() > s0 + k ? extras[s0 + k] : nullptr;
         if (x)
         {
            if (x->size() != n)
            {
               err = "an extra photon image of the wrong size";
               return false;
            }
            fi = d.data() + kTableWords + kFrameWords * k;
            fi[4] = static_cast<uint32_t>(d.size());
            const size_t was = d.size();
            d.resize(was + n);
            std::memcpy(d.data() + was, x->data(), n * sizeof(float));
         }
      }
      if (!m.dyn || m.dyn->bytes < d.size() * 4)
      {
         size_t cap = std::max<size_t>(1 << 16, m.dyn ? m.dyn->bytes : 0);
         while (cap < d.size() * 4)
            cap *= 2;
         m.dyn.reset();
         m.dyn = be.Make(cap, nullptr, err);
         if (!m.dyn)
         {
            err = "frames: " + err;
            return false;
         }
      }
      if (!be.Write(*m.dyn, 0, d.data(), d.size() * 4, err))
         return false;
      uint32_t p[16] = {};
      p[0] = m.w;
      p[1] = m.h;
      p[2] = static_cast<uint32_t>(nf);
      p[3] = seed;
      p[4] = cam.emccd ? 1u : 0u;
      p[5] = Bits(static_cast<float>(cam.quantumEfficiency));
      p[6] = Bits(static_cast<float>(cam.darkCurrentElectrons));
      p[7] = Bits(static_cast<float>(cam.cicElectrons));
      p[8] = Bits(static_cast<float>(std::max(1.0, cam.emGain)));
      p[9] = Bits(static_cast<float>(std::ldexp(1.0, std::min(16, std::max(1, cam.bitDepth))) - 1.0));
      p[10] = m.pixelBase;
      p[11] = static_cast<uint32_t>(m.pix.size());
      p[12] = static_cast<uint32_t>(emAt);
      GpuBackend::Buffer* b[7] = {m.bufs[0].get(), m.bufs[1].get(), m.bufs[2].get(), m.bufs[3].get(),
                                  m.stat.get(),    m.dyn.get(),     m.out.get()};
      if (!be.Dispatch("splat_frames", p, 16, b, 7, (m.w + 15) / 16, (m.h + 15) / 16, static_cast<unsigned>(nf), err))
         return false;
      m.outWords.resize(n * nf);
      if (!be.ReadBack(*m.out, n * nf * 4, m.outWords.data(), err))
         return false;
      for (size_t k = 0; k < nf; ++k)
      {
         std::vector<uint16_t>& o = *outs[s0 + k];
         o.resize(n);
         const uint32_t* src = m.outWords.data() + k * n;
         for (size_t i = 0; i < n; ++i)
            o[i] = static_cast<uint16_t>(src[i]);
      }
   }
   return true;
}

bool SplatGpuHost::SelfCheck(std::string& err)
{
   // A small in-focus-ish kernel and 400 blinks on a 64 x 64 sensor, sCMOS and EMCCD.
   PsfGeneratorRequest req;
   req.model = PsfModelKind::GibsonLanniZernike;
   req.wavelengthNm = 670;
   req.na = 1.4;
   req.pixelSizeNm = 100;
   req.oversampling = 4;
   req.kernelHalfWidthPx = 6;
   req.nz = 5;
   req.zStepNm = 100;
   req.interpMode = PsfInterpMode::Cubic;
   PsfKernelCache kernel;
   if (!ComputePsfKernelCache(req, kernel, err) || !kernel.valid)
   {
      err = "no check kernel (" + err + ")";
      return false;
   }
   const unsigned W = 64, H = 64;
   const size_t n = static_cast<size_t>(W) * H;
   std::mt19937_64 rng(12345);
   std::uniform_real_distribution<double> u(0.0, 1.0);
   std::vector<BlinkEvent> evs(400);
   for (BlinkEvent& e : evs)
   {
      e.xUm = 6.4 * u(rng);
      e.yUm = 6.4 * u(rng);
      e.zNm = 300 * u(rng) - 150;
      e.tStart = 0.3 * u(rng) - 0.1;
      e.tEnd = e.tStart + 0.5 + u(rng);
      e.brightness = 0.5 + u(rng);
   }
   PixelOffsetMap off;
   PixelGainMap gain;
   PixelReadNoiseMap rn;
   off.Generate(W, H, 100, 2, rng);
   gain.Generate(W, H, 0.25, 0.005, rng);
   rn.Generate(W, H, 1.2, 0.2, rng);
   for (int emccd = 0; emccd < 2; ++emccd)
   {
      CameraNoiseParams cam;
      cam.emccd = emccd != 0;
      cam.quantumEfficiency = 0.85;
      cam.darkCurrentElectrons = 0.05;
      cam.gainPhotonsPerAdu = 0.25;
      cam.readNoiseElectrons = 1.2;
      std::vector<float> cpu(n, 0.0f);
      RenderPhotonImage(cpu, W, H, evs, 0, 100.0, 1.3, 1500.0, 20.0, 0.3, -0.2, &kernel, 0.05);
      std::vector<uint16_t> cpuAdu, gpuAdu;
      ApplyNoiseChain(cpu, cpuAdu, W, H, cam, off, gain, rn, 777u, 3u, false, 0);
      std::vector<GpuSplatEmitter> ems;
      CollectGpuEmitters(evs, 0, W, H, 100.0, 1500.0, 0.3, -0.2, kernel, 0.05, nullptr, ems);
      const std::vector<float> bg(n, 20.0f);
      if (!SetKernels({&kernel}, err) ||
          !SetStatic(W, H, 0, off.offset, gain.gainPhotonsPerAdu, rn.readNoiseElectrons, cam, 0, err) ||
          !RenderFrames({ems}, {3u}, {&bg}, cam, 777u, {&gpuAdu}, err))
         return false;
      size_t same = 0;
      double diff = 0, mean = 0;
      for (size_t i = 0; i < n; ++i)
      {
         same += cpuAdu[i] == gpuAdu[i];
         diff += std::abs(static_cast<double>(cpuAdu[i]) - gpuAdu[i]);
         mean += cpuAdu[i];
      }
      if (same < n * 9 / 10 || diff > 0.01 * mean)
      {
         err = std::string(emccd ? "EMCCD" : "sCMOS") + ": " + std::to_string(100.0 * same / n) +
               " % of the pixels equal, mean |diff| " + std::to_string(diff / n) + " ADU";
         return false;
      }
   }
   // (the next movie sets its own)
   s_->statKey = 0;
   return true;
}

std::unique_ptr<SplatGpuHost> MakeSplatGpuHost(std::shared_ptr<GpuBackend> backend, std::string& info)
{
   if (!backend)
      return nullptr;
   std::unique_ptr<SplatGpuHost> h(new SplatGpuHost(backend));
   std::string err;
   if (!h->SelfCheck(err))
   {
      info = backend->name + " failed the blink-splat self-check (" + err + ")";
      return nullptr;
   }
   info = backend->name;
   return h;
}

} // namespace sim
