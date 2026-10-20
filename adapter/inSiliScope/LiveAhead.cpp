///////////////////////////////////////////////////////////////////////////////
// FILE:          LiveAhead.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Micro-Manager device adapter: the inSiliScope camera
//-----------------------------------------------------------------------------
// DESCRIPTION:   Render-ahead for live fluorescence (docs/dev/PERF_PASS.md phase
//                2): a helper thread renders a batch of the next live frames
//                with one FluorescenceMovie of that many frames -- one setup
//                and one events query per batch, the frames rendered side by
//                side on all cores (or in one GPU dispatch) -- while the
//                producer (LiveProducerLoop) hands the previous batch's frames
//                out one per exposure slot. The producer builds each job from
//                the state of the moment and drops queued frames and jobs on
//                any change of it (LiveBatchJob, CancelLiveAhead).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "Simulation/Timing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

void CInSiliScopeCamera::StartLiveAhead()
{
   std::lock_guard<std::mutex> g(liveAheadMutex_);
   if (liveAheadRun_)
      return;
   liveAheadRun_ = true;
   liveAheadJob_.reset();
   liveAheadDone_.reset();
   liveAheadBusy_ = false;
   liveAheadThread_ = std::thread(&CInSiliScopeCamera::LiveAheadLoop, this);
}

void CInSiliScopeCamera::StopLiveAhead()
{
   {
      std::lock_guard<std::mutex> g(liveAheadMutex_);
      if (!liveAheadRun_)
         return;
      liveAheadRun_ = false;
      liveAheadCancel_ = std::numeric_limits<long>::max();
   }
   liveAheadCv_.notify_all();
   if (liveAheadThread_.joinable())
      liveAheadThread_.join();
   std::lock_guard<std::mutex> g(liveAheadMutex_);
   liveAheadJob_.reset();
   liveAheadDone_.reset();
   liveAheadBusy_ = false;
   liveAheadCancel_ = liveAheadLastId_;   // ids keep growing across restarts
}

void CInSiliScopeCamera::SubmitLiveAhead(std::unique_ptr<LiveBatchJob> job)
{
   {
      std::lock_guard<std::mutex> g(liveAheadMutex_);
      liveAheadDone_.reset();
      liveAheadLastId_ = job->id;
      liveAheadJob_ = std::move(job);
   }
   liveAheadCv_.notify_all();
}

bool CInSiliScopeCamera::LiveAheadIdle()
{
   std::lock_guard<std::mutex> g(liveAheadMutex_);
   return !liveAheadBusy_ && !liveAheadJob_;
}

std::unique_ptr<CInSiliScopeCamera::LiveBatch> CInSiliScopeCamera::TakeLiveAhead(
   long id, const std::chrono::steady_clock::time_point* until, const std::function<bool()>& stop)
{
   std::unique_lock<std::mutex> g(liveAheadMutex_);
   auto ready = [&] {
      return !liveAheadRun_ || (liveAheadDone_ && liveAheadDone_->id == id) ||
             (!liveAheadBusy_ && !(liveAheadJob_ && liveAheadJob_->id == id)) || !liveProducerRun_.load() ||
             liveWakeNow_.load();
   };
   while (until && !ready())
   {
      const auto now = std::chrono::steady_clock::now();
      if (now >= *until)
         break;
      liveAheadCv_.wait_until(g, std::min(*until, now + std::chrono::milliseconds(2)));
      if (!ready() && stop)
      {
         g.unlock();
         const bool s = stop();
         g.lock();
         if (s)
            break;
      }
   }
   if (liveAheadDone_ && liveAheadDone_->id == id)
      return std::move(liveAheadDone_);
   return nullptr;
}

void CInSiliScopeCamera::CancelLiveAhead()
{
   std::lock_guard<std::mutex> g(liveAheadMutex_);
   // The queued job and the one rendering (it stops at its next frame).
   liveAheadCancel_ = std::max(liveAheadCancel_.load(), liveAheadLastId_);
   liveAheadJob_.reset();
   liveAheadDone_.reset();
}

bool CInSiliScopeCamera::PrepareLiveGpu(const sim::FluorescenceSimplePlan& plan, unsigned w, unsigned h,
                                        const LiveStatics& st, const sim::SimulationParams& params)
{
   LiveGpu& g = liveGpu_;
   if (!plan.ok || g.failedAt == st.version)
      return false;
   std::vector<const sim::PsfKernelCache*> kernels;
   std::vector<uint64_t> serials;
   for (const sim::FluorescenceSimpleGroup& sg : plan.groups)
   {
      if (!sg.kernel)
         return false;   // the Gaussian PSF: the CPU
      kernels.push_back(sg.kernel);
      serials.push_back(sg.kernel->SplatSerial());
   }
   const std::vector<float> noMap;
   const std::vector<float>& bgMap = plan.backgroundMap ? *plan.backgroundMap : noMap;
   if (g.splat && !g.kernelSerials.empty() && serials == g.kernelSerials && st.version == g.statics &&
       (plan.backgroundPhotons != g.background || bgMap != g.backgroundMap))
   {
      // Only the background moved (a shaped beam's map follows the focus): the per-pixel statics, not the kernels
      // (~0.4 s for the default kernel). On a failure the full setup below runs.
      const auto tBg = sim::TimingClock::now();
      sim::SimulationParams gp = params;
      gp.backgroundPhotons = plan.backgroundPhotons;
      std::string err;
      if (g.splat->SetStatic(w, h, st.offsetMap.offset, st.gainMap.gainPhotonsPerAdu, st.readNoiseMap.readNoiseElectrons,
                             bgMap.size() == static_cast<size_t>(w) * h ? bgMap : noMap, gp.backgroundPhotons,
                             gp.Camera(), err))
      {
         g.background = plan.backgroundPhotons;
         g.backgroundMap = bgMap;
         sim::TimingLog("gpu.prepare-background", sim::TimingSince(tBg));
         return true;
      }
   }
   if (serials != g.kernelSerials || plan.backgroundPhotons != g.background || st.version != g.statics ||
       bgMap != g.backgroundMap)
   {
      const auto tPrep = sim::TimingClock::now();
      sim::SimulationParams gp = params;
      gp.backgroundPhotons = plan.backgroundPhotons;
      g.kernelSerials.clear();
      if (PrepareGpu(g.splat, kernels, w, h, st.offsetMap, st.gainMap, st.readNoiseMap, plan.backgroundMap, gp))
         g.kernelSerials = serials;
      g.background = plan.backgroundPhotons;
      g.backgroundMap = bgMap;
      g.statics = st.version;
      if (g.kernelSerials.empty())
         g.failedAt = st.version;
      sim::TimingLog("gpu.prepare", sim::TimingSince(tPrep));
   }
   return !g.kernelSerials.empty();
}

void CInSiliScopeCamera::LiveAheadLoop()
{
   auto render = [&](const LiveBatchJob& job, LiveBatch& out) {
      const long K = static_cast<long>(job.fadeSec.size());
      const size_t n = static_cast<size_t>(job.w) * job.h;
      auto cancelled = [&] { return job.id <= liveAheadCancel_.load() || !liveAheadRun_; };
      out.id = job.id;
      out.firstSlot = job.firstSlot;
      out.frames.assign(static_cast<size_t>(K), std::vector<uint16_t>());
      const LiveStatics& st = *job.statics;
      // The shared GPU hosts for the whole batch (the producer renders on the
      // CPU meanwhile, should it need a frame).
      std::unique_lock<std::mutex> gpuLock(liveGpu_.m, std::defer_lock);
      if (job.useGpu)
         gpuLock.lock();
      sim::FluorescenceMovie fm;
      fm.CarryRunningImages(true);
      if (!fm.Begin(job.spec, false, out.err, job.useGpu ? WideFieldGpu(liveGpu_.wf, liveGpu_.wfTried) : nullptr,
                    &lightHistory_, &job.light))
         return;
      if (cancelled())
         return;
      const sim::FluorescenceSimplePlan plan = fm.SimplePlan();
      const sim::CameraNoiseParams cam = job.params.Camera();
      bool gpuReady = job.useGpu && PrepareLiveGpu(plan, job.w, job.h, st, job.params);
      // The continuous populations of the batch (CPU, in frame order), added
      // on the GPU before the noise.
      std::vector<std::vector<float>> popImg;
      if (gpuReady && plan.populations)
      {
         const auto tPops = sim::TimingClock::now();
         popImg.assign(static_cast<size_t>(K), std::vector<float>());
         sim::FluorescenceFrameOptions opt;
         opt.populationsOnly = true;
         opt.onPhotons = [&](long f, const std::vector<float>& photons) {
            if (cancelled() || f >= K)
               return false;
            popImg[static_cast<size_t>(f)] = photons;
            return true;
         };
         sim::ScopeMovieInfo info;
         std::string err;
         if (!fm.Render([](long, const std::vector<uint16_t>&) { return true; }, info, err, nullptr, &opt))
            return;
         for (const std::vector<float>& img : popImg)
            if (img.size() != n)
               return;   // cancelled
         sim::TimingLog("ahead.gpu.populations", sim::TimingSince(tPops));
      }
      if (gpuReady)
      {
         const auto tGpu = sim::TimingClock::now();
         std::vector<std::vector<std::vector<uint32_t>>> byFrame;   // per group
         for (const sim::FluorescenceSimpleGroup& sg : plan.groups)
            byFrame.push_back(sim::BucketEventsByFrame(*sg.events, K));
         std::vector<std::vector<sim::GpuSplatEmitter>> ems(static_cast<size_t>(K));
         std::vector<uint32_t> ids;
         std::vector<double> bgScales;
         std::vector<std::vector<uint16_t>*> outs;
         std::vector<const std::vector<float>*> extraPtrs;
         std::vector<sim::BlinkEvent> evs;
         for (long f = 0; f < K; ++f)
         {
            const sim::RenderExtras extras = st.shaping.Extras(job.fadeSec[static_cast<size_t>(f)], job.decaySec);
            for (size_t gi = 0; gi < plan.groups.size(); ++gi)
            {
               const sim::FluorescenceSimpleGroup& sg = plan.groups[gi];
               evs.clear();
               if (sg.splatFrame[static_cast<size_t>(f)])   // else binned or mean-field: in popImg
                  for (uint32_t idx : byFrame[gi][static_cast<size_t>(f)])
                     evs.push_back((*sg.events)[idx]);
               sim::CollectGpuEmitters(evs, f, job.w, job.h, job.params.pixelSizeNm, sg.photonsPerBlink, 0.0, 0.0,
                                       *sg.kernel, job.zOffsetUm, &extras, ems[static_cast<size_t>(f)], &out.zClamped,
                                       &out.zTotal, static_cast<int>(gi), sg.minPhotons);
            }
            ids.push_back(job.noiseBase + static_cast<uint32_t>(f));
            bgScales.push_back(extras.backgroundScale);
            outs.push_back(&out.frames[static_cast<size_t>(f)]);
            if (!popImg.empty())
               extraPtrs.push_back(&popImg[static_cast<size_t>(f)]);
         }
         sim::TimingLog("ahead.gpu.collect", sim::TimingSince(tGpu));
         if (cancelled())
            return;
         const auto tSplat = sim::TimingClock::now();
         std::string err;
         if (liveGpu_.splat->RenderFrames(ems, ids, bgScales, cam, job.noiseSeed, outs, err,
                                          popImg.empty() ? nullptr : &extraPtrs))
         {
            sim::TimingLog("ahead.gpu.splat+noise", sim::TimingSince(tSplat));
            out.ok = true;
            return;
         }
         LogMessage("GPU render-ahead failed, continuing on the CPU: " + err, false);
         SetGpuStatus("CPU (GPU render failed: " + err + ")");
         liveGpu_.failedAt = st.version;
         for (std::vector<uint16_t>& fr : out.frames)
            fr.clear();
      }
      sim::FluorescenceFrameOptions opt;
      opt.backgroundScale = [&](long f) {
         return sim::BackgroundFadeScale(job.fadeSec[static_cast<size_t>(f)], job.decaySec);
      };
      if (job.decaySec <= 0)
         opt.backgroundScale = nullptr;
      long done = 0;
      opt.onPhotons = [&](long f, const std::vector<float>& photons) {
         if (cancelled() || photons.size() != n)
            return false;
         sim::ApplyNoiseChain(photons, out.frames[static_cast<size_t>(f)], job.w, job.h, cam, st.offsetMap, st.gainMap,
                              st.readNoiseMap, job.noiseSeed, job.noiseBase + static_cast<uint32_t>(f), true);
         ++done;
         return true;
      };
      sim::ScopeMovieInfo info;
      const auto tCpu = sim::TimingClock::now();
      if (fm.Render([](long, const std::vector<uint16_t>&) { return true; }, info, out.err, nullptr, &opt) && done == K)
         out.ok = true;
      sim::TimingLog("ahead.cpu.render+noise", sim::TimingSince(tCpu));
   };

   std::unique_lock<std::mutex> g(liveAheadMutex_);
   for (;;)
   {
      liveAheadCv_.wait(g, [&] { return !liveAheadRun_ || liveAheadJob_; });
      if (!liveAheadRun_)
         return;
      std::unique_ptr<LiveBatchJob> job = std::move(liveAheadJob_);
      liveAheadBusy_ = true;
      g.unlock();
      std::unique_ptr<LiveBatch> out(new LiveBatch());
      const auto t0 = sim::TimingClock::now();
      if (job->id > liveAheadCancel_.load())
         render(*job, *out);
      out->ms = sim::TimingSince(t0) * 1000.0;
      if (out->ok)
      {
         char b[32];
         std::snprintf(b, sizeof b, "%zu frames", out->frames.size());
         sim::TimingLog("ahead.batch", sim::TimingSince(t0), b);
      }
      g.lock();
      liveAheadBusy_ = false;
      if (job->id > liveAheadCancel_.load())
         liveAheadDone_ = std::move(out);
      liveAheadCv_.notify_all();
   }
}
