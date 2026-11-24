///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldGpuHost.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldGpuHost.h. Mirrors web/wf_gpu.js step for step.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "WidefieldGpuHost.h"

#include "Illumination.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <sstream>

namespace sim {

namespace {

constexpr uint32_t kNone = 0xffffffffu;

uint32_t Bits(float f)
{
   uint32_t b;
   std::memcpy(&b, &f, 4);
   return b;
}

using Buf = GpuBackend::BufPtr;

} // namespace

struct WidefieldGpuHost::State
{
   GpuBackend& gpu;
   explicit State(GpuBackend& g) : gpu(g) {}

   // The FFT grid (its size: shape) and the resident spectra (as wf_gpu.js): one pool of plane slots for the grid,
   // and per scene geometry (WidefieldGpuJob::geometry) its planes' slots and its kernel spectra, the kMaxGeometries
   // used last kept (2026-10-29: two mean-field scenes take turns, one per labelled target; with one geometry each job
   // dropped the other's planes and kernels, and a live WideField z step re-made ~500 plane spectra, 0.5 s).
   std::string shape;
   unsigned NX = 0, NY = 0, W = 0;
   size_t SS = 0, PS = 0;
   Buf spec16, work, tw;
   unsigned slotCap = 0, pairCap = 1;
   std::vector<unsigned> freeSlots;
   struct PlaneEntry
   {
      unsigned slot;
      unsigned long long used;
      float scale;
   };
   struct Resident
   {
      std::map<unsigned long long, PlaneEntry> planes;
      unsigned long long used = 0;
      size_t need = 0;          // the planes its last job needed (Fits)
      unsigned long long job = 0;   // that job's number
   };
   // Kernel spectra per kernel set (WidefieldGpuJob::kernelSet: the scenes of one store share them).
   struct KernelSet
   {
      std::map<int, unsigned> kslots;
      Buf kspec;
      unsigned kcap = 0;
      unsigned long long used = 0;
   };
   unsigned long long jobs = 0;   // jobs run after a Fits
   unsigned long long askGeometry = 0;
   size_t askNeed = 0;
   static constexpr unsigned long long kRecentJobs = 8;   // scenes served within this many jobs share the pool
   static constexpr size_t kMaxKernelSets = 4;
   std::map<unsigned long long, Resident> resident;
   std::map<unsigned long long, KernelSet> ksets;
   Resident* cur = nullptr;
   KernelSet* kcur = nullptr;
   unsigned long long clock = 0;

   // Frames.
   unsigned fw = 0, fh = 0;
   Buf pix, frameOut;

   // A buffer of mb MB, at most one binding.
   size_t PoolBytes(size_t mb) const { return std::min<size_t>(gpu.MaxBinding(), mb << 20); }

   // The kernel set's spectra (made, and the least recently used beyond kMaxKernelSets dropped).
   KernelSet& UseKernels(unsigned long long set)
   {
      KernelSet& k = ksets[set];
      k.used = ++clock;
      while (ksets.size() > kMaxKernelSets)
      {
         auto victim = ksets.end();
         for (auto it = ksets.begin(); it != ksets.end(); ++it)
            if (it->first != set && (victim == ksets.end() || it->second.used < victim->second.used))
               victim = it;
         ksets.erase(victim);
      }
      return ksets[set];
   }

   // The geometry's resident state (made, and the least recently used beyond MaxScenes dropped, its slots freed).
   Resident& Use(unsigned long long geometry)
   {
      Resident& r = resident[geometry];
      r.used = ++clock;
      while (resident.size() > std::max<size_t>(1, gpu.MaxScenes()))
      {
         auto victim = resident.end();
         for (auto it = resident.begin(); it != resident.end(); ++it)
            if (it->first != geometry && (victim == resident.end() || it->second.used < victim->second.used))
               victim = it;
         for (const auto& kv : victim->second.planes)
            freeSlots.push_back(kv.second.slot);
         resident.erase(victim);
      }
      return r;
   }

   bool Make(size_t bytes, const void* data, Buf& out, std::string& err)
   {
      out = gpu.Make(bytes, data, err);
      return out != nullptr;
   }

   // One dispatch: parameters, then buffers at bindings 1.. in order.
   bool Dispatch(const char* name, std::initializer_list<uint32_t> params, std::initializer_list<GpuBackend::Buffer*> bufs,
                 unsigned x, unsigned y, unsigned z, std::string& err)
   {
      std::vector<uint32_t> p(params);
      std::vector<GpuBackend::Buffer*> b(bufs);
      return gpu.Dispatch(name, p.data(), p.size(), b.data(), b.size(), x, y, z, err);
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
      resident.clear();
      ksets.clear();
      cur = nullptr;
      kcur = nullptr;
      spec16.reset();
      work.reset();
      // Resident fp16 plane spectra (the backend's pool sizes, largest first: 512 MB, else 128 MB in the browser and
      // the adapter), work grids (up to 256 MB).
      bool made = false;
      for (size_t mb : gpu.PoolMB())
      {
         slotCap = static_cast<unsigned>(std::max<size_t>(4, PoolBytes(mb) / (SS * 4)));
         std::string e;
         if (Make(slotCap * SS * 4, nullptr, spec16, e))
         {
            made = true;
            break;
         }
         err = e;
      }
      if (!made)
         return false;
      err.clear();
      freeSlots.clear();
      for (unsigned i = slotCap; i-- > 0;)
         freeSlots.push_back(i);
      pairCap = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(32, PoolBytes(256) / (PS * 8))));
      if (!Make(pairCap * PS * 8, nullptr, work, err))
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
      return Make(t.size() * 4, t.data(), tw, err);
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
      return Dispatch("wf_fft", {NX, Log2(NX), 1, NX, static_cast<uint32_t>(PS), inv, 0, 0}, {work.get(), tw.get()},
                      NY, 1, pairs, err) &&
             Dispatch("wf_fft", {NY, Log2(NY), NX, 1, static_cast<uint32_t>(PS), inv, NX / 2, 0},
                      {work.get(), tw.get()}, NX, 1, pairs, err);
   }

   bool EnsureKernels(const WidefieldGpuJob& job, std::string& err)
   {
      KernelSet& r = *kcur;
      std::vector<const WidefieldGpuJob::Kernel*> missing;
      for (const WidefieldGpuJob::Kernel& k : job.kernels)
         if (!r.kslots.count(k.p))
            missing.push_back(&k);
      if (missing.empty())
         return true;
      const size_t need = r.kslots.size() + missing.size(), bytes = SS * 8;
      if (need > r.kcap)
      {
         const unsigned cap = static_cast<unsigned>(std::max<size_t>(need, std::max<size_t>(16, 2 * r.kcap)));
         Buf nb;
         if (!Make(cap * bytes, nullptr, nb, err))
            return false;
         if (r.kspec && !r.kslots.empty() && !gpu.Copy(*nb, *r.kspec, r.kslots.size() * bytes, err))
            return false;
         r.kspec = nb;
         r.kcap = cap;
      }
      for (const WidefieldGpuJob::Kernel* k : missing)
      {
         const unsigned slot = static_cast<unsigned>(r.kslots.size());
         r.kslots[k->p] = slot;
         if (!gpu.Write(*r.kspec, slot * bytes, k->spec->data(), bytes, err))
            return false;
      }
      return true;
   }

   bool EnsurePlanes(const WidefieldGpuJob& job, std::string& err)
   {
      std::map<unsigned long long, PlaneEntry>& planes = cur->planes;
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
         // (A job that cannot fit evicts nothing: the other scenes keep their planes.)
         size_t evictable = 0;
         for (const auto& r : resident)
            for (const auto& kv : r.second.planes)
               evictable += kv.second.used < stamp ? 1 : 0;
         if (todo.size() > freeSlots.size() + evictable)
         {
            err = "GPU plane spectra do not fit";
            return false;
         }
         // Least recently used first, of any geometry (never this job's).
         struct Victim
         {
            unsigned long long used;
            std::map<unsigned long long, PlaneEntry>* owner;
            unsigned long long key;
            bool operator<(const Victim& o) const { return used < o.used; }
         };
         std::vector<Victim> victims;
         for (auto& r : resident)
            for (const auto& kv : r.second.planes)
               if (kv.second.used < stamp)
                  victims.push_back({kv.second.used, &r.second.planes, kv.first});
         std::sort(victims.begin(), victims.end());
         for (size_t v = 0; v < victims.size() && todo.size() > freeSlots.size(); ++v)
         {
            freeSlots.push_back((*victims[v].owner)[victims[v].key].slot);
            victims[v].owner->erase(victims[v].key);
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
         if (!Dispatch("wf_clear", {static_cast<uint32_t>(pairs * PS), cl, 0, 0}, {work.get()}, cx, cy, 1, err))
            return false;
         const size_t n = entries.size() / 4;
         Buf eb, sb;
         if (!Make(entries.size() * 4, entries.data(), eb, err) || !Make(slots.size() * 4, slots.data(), sb, err))
            return false;
         Grid256(n, sx, sy, sl);
         if (!Dispatch("wf_scatter", {static_cast<uint32_t>(n), job.nx, NX, static_cast<uint32_t>(PS), sl, 0, 0, 0},
                       {eb.get(), work.get()}, sx, sy, 1, err) ||
             !Fft2(pairs, false, err) ||
             !Dispatch("wf_split", {NX, NY, W, static_cast<uint32_t>(SS), static_cast<uint32_t>(PS), pairs, 0, 0},
                       {work.get(), sb.get(), spec16.get()}, (W + 15) / 16, (NY + 15) / 16, pairs, err))
            return false;
      }
      return true;
   }
};

WidefieldGpuHost::WidefieldGpuHost(std::shared_ptr<GpuBackend> backend)
   : backend_(std::move(backend)), s_(new State(*backend_))
{
}

WidefieldGpuHost::~WidefieldGpuHost() = default;

bool WidefieldGpuHost::HasPlane(unsigned long long geometry, unsigned long long key) const
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   auto it = s_->resident.find(geometry);
   return it != s_->resident.end() && it->second.planes.count(key) != 0;
}

bool WidefieldGpuHost::Fits(unsigned long long geometry, unsigned NX, unsigned NY, size_t planes)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   State& m = *s_;
   const size_t SS = static_cast<size_t>(NX / 2 + 1) * NY;
   const bool sameShape = m.NX == NX && m.NY == NY && m.slotCap > 0;
   // (Another shape resets the pool: Reset's first try.)
   const std::vector<size_t> pool = backend_->PoolMB();
   const size_t cap = sameShape ? m.slotCap : std::max<size_t>(4, m.PoolBytes(pool.empty() ? 512 : pool[0]) / (SS * 4));
   size_t others = 0;
   if (sameShape)
      for (const auto& r : m.resident)
         if (r.first != geometry && r.second.job + State::kRecentJobs > m.jobs && r.second.job > 0)
            others += r.second.need;
   if (planes + others > cap)
      return false;
   m.askGeometry = geometry;   // (recorded when its job runs: a job on another shape resets the pool first)
   m.askNeed = planes;
   return true;
}

bool WidefieldGpuHost::Images(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
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

bool WidefieldGpuHost::ImagesImpl(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err)
{
   State& m = *s_;
   out.clear();
   std::ostringstream shape;
   shape << job.NX << 'x' << job.NY;
   if (shape.str() != m.shape)
   {
      m.shape = shape.str();
      if (!m.Reset(job, err))
      {
         m.shape.clear();
         return false;
      }
   }
   m.cur = &m.Use(job.geometry);
   m.kcur = &m.UseKernels(job.kernelSet ? job.kernelSet : job.geometry);
   if (m.askGeometry == job.geometry)
   {
      m.cur->need = m.askNeed;
      m.cur->job = ++m.jobs;
      m.askGeometry = 0;
   }
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
         const State::Resident& r = *m.cur;
         const State::KernelSet& ks = *m.kcur;
         auto pe = r.planes.find(d.key);
         auto k0 = ks.kslots.find(d.p0), k1 = ks.kslots.find(d.p1);
         if (pe == r.planes.end() || k0 == ks.kslots.end() || (d.two && k1 == ks.kslots.end()))
         {
            std::ostringstream os;
            os << "job refers to " << (pe == r.planes.end() ? "a plane" : "a kernel") << " it does not carry (key "
               << d.key << ", planes " << r.planes.size() << ", job planes " << job.planes.size() << ", p " << d.p0
               << "/" << d.p1 << ", kernels " << ks.kslots.size() << ")";
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
   if (!m.Make(nChan * m.SS * 8, nullptr, chanSpec, err) || !m.Make(depI.size() * 4, depI.data(), bI, err) ||
       !m.Make(depW.size() * 4, depW.data(), bW, err) || !m.Make(chans.size() * 4, chans.data(), bC, err) ||
       !m.Make(nChan * cs * 4, nullptr, imgs, err))
      return false;
   unsigned mx, my, ml;
   State::Grid256(m.SS, mx, my, ml);
   if (!m.Dispatch("wf_mac", {static_cast<uint32_t>(m.SS), nChan, ml, 0},
                   {m.spec16.get(), m.kcur->kspec.get(), bI.get(), bW.get(), bC.get(), chanSpec.get()}, mx, my, nChan,
                   err))
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
      if (!m.Make(pairs * 8, pairList.data() + 2 * p0, pb, err))
         return false;
      if (!m.Dispatch("wf_expand",
                      {m.NX, m.NY, m.W, static_cast<uint32_t>(m.SS), static_cast<uint32_t>(m.PS), pairs,
                       Bits(static_cast<float>(job.fracX)), Bits(static_cast<float>(job.fracY))},
                      {chanSpec.get(), pb.get(), m.work.get()}, (m.NX + 15) / 16, (m.NY + 15) / 16, pairs, err) ||
          !m.Fft2(pairs, true, err) ||
          !m.Dispatch("wf_crop",
                      {m.NX, static_cast<uint32_t>(m.PS), job.fovX0, job.fovY0, job.cw, job.ch,
                       Bits(1.0f / (static_cast<float>(m.NX) * static_cast<float>(m.NY))), 0},
                      {m.work.get(), pb.get(), imgs.get()}, (job.cw + 15) / 16, (job.ch + 15) / 16, pairs, err))
         return false;
   }
   std::vector<float> all(nChan * cs);
   if (!m.gpu.ReadBack(*imgs, all.size() * 4, all.data(), err))
      return false;
   out.resize(nChan);
   for (unsigned c = 0; c < nChan; ++c)
      out[c].assign(all.begin() + c * cs, all.begin() + (c + 1) * cs);
   return true;
}

bool WidefieldGpuHost::SetFrameStatic(unsigned width, unsigned height, const std::vector<float>& offset,
                                      const std::vector<float>& gain, const std::vector<float>& readNoise,
                                      const std::vector<float>& background, double flatBg,
                                      const CameraNoiseParams& cam, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   State& m = *s_;
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
   return m.Make(pix.size() * 4, pix.data(), m.pix, err);
}

bool WidefieldGpuHost::RenderFrames(const WidefieldImages& images, const std::vector<std::vector<double>>& coef,
                                    const std::vector<uint32_t>& frames, const std::vector<double>& bgScale,
                                    const CameraNoiseParams& cam, uint32_t seed,
                                    const std::vector<std::vector<uint16_t>*>& outs, std::string& err)
{
   std::lock_guard<std::recursive_mutex> lock(backend_->mu);
   State& m = *s_;
   if (!m.pix || images.cw != m.fw * images.upscale || images.ch != m.fh * images.upscale)
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
   if (!m.Make(all.size() * 4, all.data(), imgs, err))
      return false;
   // Batches bounded by 64 MB of output.
   const size_t batch = std::max<size_t>(1, std::min<size_t>(64, (size_t(64) << 20) / (4 * n)));
   if (!m.frameOut || m.frameOut->bytes < batch * n * 4)
      if (!m.Make(batch * n * 4, nullptr, m.frameOut, err))
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
      if (!m.Make(c.size() * 4, c.data(), bc, err) || !m.Make(fr.size() * 4, fr.data(), bf, err))
         return false;
      if (!m.Dispatch("wf_frame",
                      {m.fw, m.fh, images.cw, images.upscale, nB, hasP ? 1u : 0u, static_cast<uint32_t>(nf), seed,
                       cam.emccd ? 1u : 0u, Bits(static_cast<float>(cam.quantumEfficiency)),
                       Bits(static_cast<float>(cam.darkCurrentElectrons)), Bits(static_cast<float>(cam.cicElectrons)),
                       Bits(static_cast<float>(std::max(1.0, cam.emGain))), Bits(static_cast<float>(maxAdu)), 0, 0},
                      {imgs.get(), bc.get(), bf.get(), m.pix.get(), m.frameOut.get()}, (m.fw + 15) / 16,
                      (m.fh + 15) / 16, static_cast<unsigned>(nf), err) ||
          !m.gpu.ReadBack(*m.frameOut, nf * n * 4, host.data(), err))
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

// Self-check: a synthetic scene (random dyes over a few planes, Gaussian
// PSF, sub-cell shift, both populations) on the GPU against the CPU.
bool WidefieldGpuHost::SelfCheck(std::string& err)
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
   if (!Images(job, imgs, err))
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
   if (!SetFrameStatic(s.width, s.height, off.offset, gm.gainPhotonsPerAdu, rn.readNoiseElectrons, {}, 2.0, cam, err))
      return false;
   std::vector<double> a;
   cpu.BleachCoefficients(wb, a);
   std::vector<uint16_t> gf;
   if (!RenderFrames(ref, {a}, {5}, {1.0}, cam, 99, {&gf}, err))
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

} // namespace sim
