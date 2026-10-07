///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMImageGeneration.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The glue between the MM camera device and the standalone
//                simulation engine (Simulation/ScopeMovie.h): the
//                always-running live producer thread, precomputed-stack
//                generation (Test rows), and frame delivery into ImgBuffer.
//                Every setting is read from the hub (Registry/SceneSettings.h).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "LiveClock.h"
#include "Simulation/CacheDir.h"
#include "Simulation/Parallel.h"
#include "Simulation/DyeLibrary.h"
#include "Simulation/SharedStageState.h"
#include "Simulation/Timing.h"
#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

///////////////////////////////////////////////////////////////////////////////
// Parameter snapshot / invalidation helpers
///////////////////////////////////////////////////////////////////////////////

sim::StackShapingFields CInSiliScopeCamera::ShapingFields(unsigned w, unsigned h) const
{
   double meanFactor = 1.0;
   sim::StackShapingFields out = isc::BuildShapingFields(St(), w, h, &meanFactor);
   if (!out.illum.empty())
   {
      std::ostringstream msg;
      msg << "Illumination profile: peak-normalized, keeps " << std::fixed << std::setprecision(1)
          << 100.0 * meanFactor << "% of the flat-field photon budget on average.";
      LogMessage(msg.str());
   }
   return out;
}

namespace {
// Live mode pre-loads this far around the FOV (all of the z column), in the
// time left before the next frame minus this slack.
constexpr double kCellFieldPrefetchMarginUm = 3.0;
constexpr double kCellFieldPrefetchSlackMs = 2.0;
} // namespace

// The WideField GPU host for this thread, when General_UseGpu is On and a
// usable Direct3D 11 device passes its self-check (General_GpuStatus says
// which, or why not). Created once per thread and kept.
sim::WidefieldGpuD3D11* CInSiliScopeCamera::WideFieldGpu(std::unique_ptr<sim::WidefieldGpuD3D11>& gpu, bool& tried)
{
   if (!St().useGpu.load())
   {
      SetGpuStatus("CPU (Renderer UseGpu is Off)");
      return nullptr;
   }
   thread_local std::string name; // this thread's host
   if (!gpu && !tried)
   {
      tried = true;
      std::string info;
      gpu = sim::WidefieldGpuD3D11::Create(info);
      if (!gpu)
      {
         SetGpuStatus("CPU (WideField: " + info + ")");
         LogMessage("WideField GPU unavailable, convolving on the CPU: " + info, false);
         return nullptr;
      }
      LogMessage("WideField GPU on " + info);
      name = info;
   }
   if (gpu)
      SetGpuStatus("GPU: " + name + " (WideField)");
   return gpu.get();
}

void CInSiliScopeCamera::RenderBrightfieldStack(std::vector<std::vector<uint16_t>>& stack, long stackLength,
                                                unsigned w, unsigned h, const sim::SimulationParams& params,
                                                const sim::CellFieldSettings& cellField, double stageXUm,
                                                double stageYUm, const sim::PixelOffsetMap& offsetMap,
                                                const sim::PixelGainMap& gainMap,
                                                const sim::PixelReadNoiseMap& readNoiseMap, uint32_t noiseSeed,
                                                const std::vector<sim::DriftNm>& drift)
{
   // A drifting sample: frames are the grid's image shifted in its spectrum
   // and interpolated on a focus grid (BrightfieldDriftFrames).
   const bool drifting = params.drift.On() && drift.size() >= static_cast<size_t>(stackLength);
   const sim::DriftBounds driftRange = drifting ? sim::DriftRange(drift) : sim::DriftBounds();
   auto t0 = std::chrono::steady_clock::now();
   std::string err;
   sim::CellFieldSource source;
   // As WideField: an armed z sequence gives frame f the position
   // seq[f % n]; otherwise the Z stage, read per batch of frames.
   const sim::SharedStageState::ZSequence zseq = Stg().GetZSequence();
   const bool useSeq = zseq.armed && !zseq.positions.empty();
   stackZSeqVersion_ = useSeq ? zseq.version : -1;
   auto focusOf = [&](double z) {
      return isc::CellFieldQueryFor(St(), stageXUm, stageYUm, z, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0).zCullCentreUm;
   };
   const sim::CellFieldQuery q =
      isc::CellFieldQueryFor(St(), stageXUm, stageYUm, 0.0, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
   sim::BrightfieldSpec spec = isc::BuildBrightfieldSpec(St(), params, q, w, h);
   if (drifting)
      spec.marginUm = spec.Resolved().marginUm +
                      (std::ceil(driftRange.MaxXyNm() / params.pixelSizeNm) + 1.0) * params.pixelSizeNm / 1000.0;
   sim::BrightfieldScene scene;
   bool ok = source.Configure(cellField, err) && scene.Update(source, spec, 1, err);
   if (!ok)
      LogMessage("BrightField: nothing rendered (" + err + ")", false);
   else
   {
      const sim::BrightfieldQuality bq = spec.Resolved();
      std::ostringstream m;
      m << "BrightField: quality " << spec.quality << ", " << scene.Sources() << " sources, " << scene.Slices()
        << " slices, " << scene.GridNx() << "x" << scene.GridNy() << " grid (upscaling " << bq.upscale
        << ", geometry samples " << bq.sub << ") at stage (" << stageXUm << ", " << stageYUm << ") um ("
        << std::fixed << std::setprecision(2)
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s setup)";
      LogMessage(m.str());
   }
   // Photons per pixel of the empty field per frame.
   const double flux = std::max(0.0, St().brightField[isc::BF_PHOTONS_PER_PX_PER_SEC].load()) * params.frameDurationSec;
   std::map<double, std::vector<float>> images; // world focus -> transmitted intensity
   auto imageAt = [&](double focus) -> const std::vector<float>* {
      auto it = images.find(focus);
      if (it != images.end())
         return &it->second;
      std::vector<float> img;
      if (!ok || !scene.Image(focus, img, err))
         return nullptr;
      for (float& v : img)
         v = static_cast<float>(v * flux);
      return &(images[focus] = std::move(img));
   };
   const sim::CameraNoiseParams cam = params.Camera();
   const unsigned nThreads = std::max(1u, std::min(std::thread::hardware_concurrency(), 32u));
   const long batch = static_cast<long>(nThreads) * 4;
   const std::vector<float> dark(static_cast<size_t>(w) * h, 0.0f);
   // Drift: the focus grid around each focus a frame can have.
   sim::BrightfieldDriftFrames driftFrames;
   std::map<double, size_t> driftBase;
   double driftZ = std::numeric_limits<double>::quiet_NaN();
   for (long f0 = 0; f0 < stackLength; f0 += batch)
   {
      const long f1 = std::min(stackLength, f0 + batch);
      std::vector<const std::vector<float>*> src(static_cast<size_t>(f1 - f0));
      std::vector<size_t> base(static_cast<size_t>(f1 - f0), 0);
      std::vector<bool> drawn(static_cast<size_t>(f1 - f0), false);
      const double zNow = Stg().zPositionUm.load();
      if (drifting && ok && (useSeq ? driftBase.empty() : zNow != driftZ))
      {
         std::vector<double> foci;
         driftBase.clear();
         for (double z : useSeq ? zseq.positions : std::vector<double>(1, zNow))
            if (!driftBase.count(focusOf(z)))
            {
               driftBase[focusOf(z)] = foci.size();
               foci.push_back(focusOf(z));
            }
         driftFrames.Begin(driftRange, foci);
         driftZ = zNow;
      }
      for (long f = f0; f < f1; ++f)
      {
         const double z = useSeq ? zseq.positions[static_cast<size_t>(f) % zseq.positions.size()] : zNow;
         if (drifting && ok)
         {
            const size_t b = driftBase[focusOf(z)];
            if (driftFrames.Refresh(scene, b, err))
            {
               base[static_cast<size_t>(f - f0)] = b;
               drawn[static_cast<size_t>(f - f0)] = true;
               src[static_cast<size_t>(f - f0)] = nullptr;
               continue;
            }
         }
         const std::vector<float>* img = drifting ? nullptr : imageAt(focusOf(z));
         src[static_cast<size_t>(f - f0)] = img ? img : &dark;
      }
      std::atomic<long> next{f0};
      auto worker = [&]() {
         std::vector<float> photons;
         ++sim::ParallelDepth(); // one frame per thread: no nested pools in the FFTs
         for (long f; (f = next.fetch_add(1)) < f1;)
         {
            const size_t k = static_cast<size_t>(f - f0);
            if (drawn[k])
            {
               driftFrames.Image(scene, base[k], drift[static_cast<size_t>(f)], photons);
               for (float& v : photons)
                  v = static_cast<float>(v * flux);
            }
            sim::ApplyNoiseChain(drawn[k] ? photons : *src[k], stack[static_cast<size_t>(f)], w, h, cam, offsetMap,
                                 gainMap, readNoiseMap, noiseSeed, static_cast<uint32_t>(f));
         }
         --sim::ParallelDepth();
      };
      std::vector<std::thread> pool;
      for (unsigned t = 1; t < nThreads; ++t)
         pool.emplace_back(worker);
      worker();
      for (std::thread& t : pool)
         t.join();
      stackFramesGenerated_ = f1;
   }
   if (!err.empty() && ok)
      LogMessage("BrightField: " + err, false);
}

bool CInSiliScopeCamera::PrepareGpu(std::unique_ptr<sim::GpuSimulator>& gpu, const sim::PsfKernelCache& cache,
                                 unsigned w, unsigned h, const sim::PixelOffsetMap& offsetMap,
                                 const sim::PixelGainMap& gainMap, const sim::PixelReadNoiseMap& readNoiseMap,
                                 const sim::StackShapingFields& shaping, const sim::SimulationParams& params)
{
   if (!St().useGpu.load())
   {
      SetGpuStatus("CPU (Renderer UseGpu is Off)");
      return false;
   }
   if (!cache.valid)
   {
      SetGpuStatus("CPU (the Gaussian PSF renders on the CPU; the GPU path is for diffraction PSF models)");
      return false;
   }
   if (cache.interpMode == sim::PsfInterpMode::Fft)
   {
      SetGpuStatus("CPU (PsfInterp=Fft has no GPU path)");
      return false;
   }
   std::string info;
   if (!gpu)
   {
      gpu = sim::GpuSimulator::Create(info);
      if (!gpu)
      {
         SetGpuStatus("CPU (" + info + ")");
         LogMessage("GPU unavailable, rendering on the CPU: " + info, false);
         return false;
      }
      SetGpuStatus("GPU: " + info);
      LogMessage("GPU simulation on " + info);
   }
   // Per-pixel background before the per-frame fade: the structured map (or
   // the flat value) times the illumination field -- what RenderPhotonImage
   // computes per pixel.
   std::vector<float> bg;
   if (!shaping.background.empty() || !shaping.illum.empty())
   {
      const size_t n = static_cast<size_t>(w) * h;
      bg.resize(n);
      for (size_t i = 0; i < n; ++i)
      {
         double v = shaping.background.size() == n ? shaping.background[i] : params.backgroundPhotons;
         if (shaping.illum.size() == n)
            v *= shaping.illum[i];
         bg[i] = static_cast<float>(v);
      }
   }
   std::string err;
   if (!gpu->SetKernel(cache, err) ||
       !gpu->SetStatic(w, h, offsetMap.offset, gainMap.gainPhotonsPerAdu, readNoiseMap.readNoiseElectrons, bg,
                       params.backgroundPhotons, params.Camera(), err))
   {
      SetGpuStatus("CPU (GPU setup failed: " + err + ")");
      LogMessage("GPU setup failed, rendering on the CPU: " + err, false);
      gpu.reset();
      return false;
   }
   return true;
}

void CInSiliScopeCamera::InvalidateStack()
{
   InvalidateStackOnly();
   liveConfigVersion_.fetch_add(1, std::memory_order_relaxed);
}

void CInSiliScopeCamera::InvalidateStackOnly()
{
   stackReady_ = false;
   endOfStackReached_ = false;
   playbackIndex_ = 0;
}

void CInSiliScopeCamera::ApplyFrameSizeChange()
{
   MMThreadGuard g(imgPixelsLock_);
   roiX_ = 0;
   roiY_ = 0;
   roiXSize_ = FullWidth();
   roiYSize_ = FullHeight();
   img_.Resize(roiXSize_, roiYSize_, 2);
   InvalidateStack();
}

///////////////////////////////////////////////////////////////////////////////
// Precomputed-stack mode
///////////////////////////////////////////////////////////////////////////////

void CInSiliScopeCamera::StartStackGeneration()
{
   if (stackGenerating_.load())
      return;
   if (stackGenThread_.joinable())
      stackGenThread_.join();

   stackGenerating_ = true;
   stackFramesGenerated_ = 0;
   stackReady_ = false;
   endOfStackReached_ = false;

   unsigned fullW = FullWidth();
   unsigned fullH = FullHeight();
   sim::SimulationParams params = isc::SnapshotParams(St());
   long seed = St().seed.load();
   long length = stackLength_;
   // CellField: the stack is generated for ONE stage pose, snapshot here;
   // moving the XY stage afterwards does not change it (spec/PORT.md 8).
   sim::CellFieldSettings cellField = isc::BuildCellFieldSettings(St());
   double stageX = 0.0, stageY = 0.0;
   Stg().PositionXyAt(sim::SharedStageState::Clock::now(), stageX, stageY);
   double stageZ = Stg().zPositionUm.load();

   stackGenThread_ = std::thread(&CInSiliScopeCamera::StackGenerationWorker, this, length, fullW, fullH, params, seed,
                                  cellField, stageX, stageY, stageZ,
                                  isc::LightModeOf(St()));
}

void CInSiliScopeCamera::LitRect(double stageXUm, double stageYUm, double& x0, double& y0, double& x1,
                                 double& y1) const
{
   // The margin plus 0.5 um: the mean-field grid (world-anchored cells, a
   // cell beyond the margin) and the history's 0.25 um tiles lie wholly in
   // the lit rect, so a frame scales every column's weight alike (the
   // scene's fast path) instead of re-convolving every frame.
   const double um = St().PixelSizeNm() / 1000.0, W = FullWidth() * um, H = FullHeight() * um;
   const double m = isc::kCellFieldMarginUm + 0.5;
   x0 = stageXUm - W / 2.0 - m;
   y0 = stageYUm - H / 2.0 - m;
   x1 = stageXUm + W / 2.0 + m;
   y1 = stageYUm + H / 2.0 + m;
}

std::function<double(double, double)> CInSiliScopeCamera::HistoryWeight(const sim::StackShapingFields& shaping,
                                                                        double stageXUm, double stageYUm) const
{
   const unsigned w = FullWidth(), h = FullHeight();
   if (shaping.illum.size() != static_cast<size_t>(w) * h)
      return {};
   const double um = St().PixelSizeNm() / 1000.0;
   const double ox = stageXUm - w * um / 2.0, oy = stageYUm - h * um / 2.0;
   const std::vector<float> illum = shaping.illum;
   return [illum, w, h, um, ox, oy](double x, double y) {
      const long px = std::min(static_cast<long>(w) - 1, std::max(0L, static_cast<long>(std::floor((x - ox) / um))));
      const long py = std::min(static_cast<long>(h) - 1, std::max(0L, static_cast<long>(std::floor((y - oy) / um))));
      return std::round(16.0 * illum[static_cast<size_t>(px) + static_cast<size_t>(w) * py]) / 16.0;
   };
}

void CInSiliScopeCamera::SyncHistoryWorld()
{
   const sim::CellFieldSettings world = isc::BuildCellFieldSettings(St());
   std::lock_guard<std::mutex> g(historyWorldMutex_);
   if (!historyHaveWorld_ || !world.SameWorld(historyWorld_))
   {
      if (historyHaveWorld_)
         LogMessage("Illumination history cleared (a new world: seed or cell parameters).");
      illumHistory_.Reset();
      historyWorld_ = world;
      historyHaveWorld_ = true;
   }
}

void CInSiliScopeCamera::StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH,
                                             sim::SimulationParams params, long seed,
                                             sim::CellFieldSettings cellField,
                                             double stageXUm, double stageYUm, double stageZUm, int light)
{
   const bool bf = light == isc::LIGHT_TRANS, both = light == isc::LIGHT_BOTH;
   // The noise maps' stream (the CellField dyes come from the core, not from it).
   std::mt19937_64 localRng(static_cast<uint64_t>(seed));
   // Illumination: a fixed field for the whole stack (no rng).
   sim::StackShapingFields shaping = ShapingFields(fullW, fullH);
   sim::PixelOffsetMap localOffsetMap;
   localOffsetMap.Generate(fullW, fullH, params.offsetAdu, params.offsetStdAdu, localRng);
   sim::PixelGainMap localGainMap;
   localGainMap.Generate(fullW, fullH, params.gainPhotonsPerAdu, params.pixelGainStdFraction, localRng);
   sim::PixelReadNoiseMap localReadNoiseMap;
   localReadNoiseMap.Generate(fullW, fullH, params.readNoiseElectrons, params.pixelReadNoiseStdFraction, localRng);

   std::vector<std::vector<uint16_t>> newStack(static_cast<size_t>(std::max(stackLength, 0L)));
   const uint32_t noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   const sim::CameraNoiseParams cam = params.Camera();
   const double decaySec = St().bgDecaySec.load();
   std::atomic<long> zClampedAll{0}, zRenderedAll{0};
   // An armed z sequence (hardware z stack): frame f is at position f mod n.
   const sim::SharedStageState::ZSequence stackSeq = Stg().GetZSequence();
   const bool stackUsesSeq = stackSeq.armed && !stackSeq.positions.empty();
   auto zOf = [&](long f) {
      // The ZStage read fresh per frame (as any live-adjustable parameter), or
      // the z sequence's position for this frame.
      return stackUsesSeq ? stackSeq.positions[static_cast<size_t>(f) % stackSeq.positions.size()]
                          : Stg().zPositionUm.load();
   };
   // The sample drift of every frame (zero at frame 0): one path per seed,
   // the cli's for the same settings (Simulation/Drift.h).
   const std::vector<sim::DriftNm> stackDrift =
      sim::DriftTrajectory(seed, stackLength, params.frameDurationSec, params.drift);
   auto startTime = std::chrono::steady_clock::now();
   bool gpuOk = false;
   std::string where = "the CPU";
   if (light == isc::LIGHT_NONE)
   {
      // No light source open: dark frames (offset, read noise, dark current).
      const std::vector<float> dark(static_cast<size_t>(fullW) * fullH, 0.0f);
      for (long f = 0; f < stackLength; ++f)
      {
         sim::ApplyNoiseChain(dark, newStack[static_cast<size_t>(f)], fullW, fullH, cam, localOffsetMap, localGainMap,
                              localReadNoiseMap, noiseSeed, static_cast<uint32_t>(f), true);
         stackFramesGenerated_ = f + 1;
      }
      where = "the CPU (dark: no light source open)";
   }
   else if (bf)
      RenderBrightfieldStack(newStack, stackLength, fullW, fullH, params, cellField, stageXUm, stageYUm,
                             localOffsetMap, localGainMap, localReadNoiseMap, noiseSeed, stackDrift);
   else
   {
      stackZSeqVersion_ = stackUsesSeq ? stackSeq.version : -1;
      const std::string zWarn = isc::CellFieldZRangeWarning(St());
      if (!zWarn.empty())
         LogMessage(zWarn, false);
      // Every structure's label in its mode through the light path: the
      // engine's fluorescence movie (Simulation/ScopeMovie.h), with this
      // camera's drift, illumination field, background fade and z sequence.
      // Each dye's clock is the illumination its place has had (a place
      // never lit starts at 0); the stack then adds its own frames there.
      // The drift goes in the spec: the movie moves its blinks, per-dye and
      // mean-field populations along the same path (zStageUm below is the
      // undrifted stage).
      sim::ScopeSpec spec = isc::BuildScopeSpec(St(), stageXUm, stageYUm, stageZUm, 0.0, stackLength);
      isc::AddDriftToSpec(spec, params.drift);
      SyncHistoryWorld();
      double lx0, ly0, lx1, ly1;
      LitRect(stageXUm, stageYUm, lx0, ly0, lx1, ly1);
      const sim::ClockSnapshot clock = illumHistory_.Snapshot(lx0, ly0, lx1, ly1);
      bool lit = false;
      // Both lights: the lamp's photons x its QE join each frame before the
      // noise (the BrightField stack's scene at each frame's focus; drifting:
      // its shifted image), so the frames render on the CPU.
      sim::CellFieldSource lampSource;
      sim::BrightfieldScene lampScene;
      sim::BrightfieldDriftFrames lampDrift;
      std::map<double, std::vector<float>> lampImages;
      std::vector<float> lampImg;
      double lampDriftFocus = std::numeric_limits<double>::quiet_NaN();
      bool lampOk = false;
      const bool lampDrifting = both && params.drift.On();
      const double lampFlux = std::max(0.0, St().brightField[isc::BF_PHOTONS_PER_PX_PER_SEC].load()) *
                              params.frameDurationSec * isc::BrightFieldQe(St());
      auto lampFocus = [&](double z) {
         return isc::CellFieldQueryFor(St(), stageXUm, stageYUm, z, fullW, fullH, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0,
                                       0.0).zCullCentreUm;
      };
      if (both)
      {
         std::string lampErr;
         const sim::CellFieldQuery q0 = isc::CellFieldQueryFor(St(), stageXUm, stageYUm, 0.0, fullW, fullH, params, 0.0,
                                                               0.0, 0.0, 0.0, 0, 0.0, 0.0);
         sim::BrightfieldSpec bs = isc::BuildBrightfieldSpec(St(), params, q0, fullW, fullH);
         if (lampDrifting)
            bs.marginUm = bs.Resolved().marginUm + (std::ceil(sim::DriftRange(stackDrift).MaxXyNm() / params.pixelSizeNm) +
                                                     1.0) * params.pixelSizeNm / 1000.0;
         lampOk = lampSource.Configure(cellField, lampErr) && lampScene.Update(lampSource, bs, 1, lampErr);
         if (!lampOk)
            LogMessage("BrightField (with the fluorescence): nothing rendered (" + lampErr + ")", false);
      }
      // Frame f's lamp photons (x QE) added to photons.
      auto addLamp = [&](long f, std::vector<float>& photons) {
         if (!lampOk)
            return;
         std::string lampErr;
         const double focus = lampFocus(zOf(f));
         const std::vector<float>* img = nullptr;
         if (lampDrifting)
         {
            if (focus != lampDriftFocus)
            {
               lampDrift.Begin(sim::DriftRange(stackDrift), std::vector<double>(1, focus));
               lampDriftFocus = focus;
            }
            if (lampDrift.Refresh(lampScene, 0, lampErr))
            {
               lampDrift.Image(lampScene, 0, stackDrift[static_cast<size_t>(f)], lampImg);
               img = &lampImg;
            }
         }
         else
         {
            auto it = lampImages.find(focus);
            if (it == lampImages.end() && lampScene.Image(focus, lampImg, lampErr))
               it = lampImages.emplace(focus, lampImg).first;
            img = it == lampImages.end() ? nullptr : &it->second;
         }
         if (img && img->size() == photons.size())
            for (size_t i = 0; i < photons.size(); ++i)
               photons[i] += static_cast<float>((*img)[i] * lampFlux);
      };
      sim::FluorescenceMovie fm;
      std::string err;
      std::unique_ptr<sim::WidefieldGpuD3D11> wfGpu;
      bool wfTried = false;
      // The mean-field scenes convolve on this thread's Direct3D 11 host.
      if (!fm.Begin(spec, false, err, St().useGpu.load() ? WideFieldGpu(wfGpu, wfTried) : nullptr, &clock))
         LogMessage("Fluorescence: nothing rendered (" + err + ")", false);
      else
      {
         // One blink group and no continuous population: the GPU splat + noise.
         const sim::FluorescenceSimplePlan plan = fm.SimplePlan();
         std::unique_ptr<sim::GpuSimulator> gpu;
         sim::SimulationParams gp = params;
         if (plan.ok)
         {
            gp.photonsPerBlink = plan.photonsPerBlink;
            gp.backgroundPhotons = plan.backgroundPhotons;
            gp.psfSigmaPx = plan.sigmaPx;
            gpuOk = plan.kernel && !both && PrepareGpu(gpu, *plan.kernel, fullW, fullH, localOffsetMap, localGainMap,
                                                       localReadNoiseMap, shaping, gp);
         }
         else if (St().useGpu.load() && !fm.HasPopulations())
            SetGpuStatus("CPU (the GPU splat is for one blink group without continuous populations)");
         {
            std::ostringstream msg;
            msg << "Fluorescence: " << (plan.ok ? std::to_string(plan.events->size()) + " blinks" : std::string("stack"))
                << " for " << stackLength << " frames at stage (" << stageXUm << ", " << stageYUm
                << ") um, the dyes at their places' illumination clocks (" << clock.At(stageXUm, stageYUm)
                << " s at the FOV centre)";
            LogMessage(msg.str());
         }
         if (gpuOk)
         {
            const std::vector<std::vector<uint32_t>> frameEvents = sim::BucketEventsByFrame(*plan.events, stackLength);
            const long batch = static_cast<long>(gpu->MaxBatchFrames());
            std::vector<sim::BlinkEvent> evs;
            long zc = 0, zt = 0;
            for (long f0 = 0; f0 < stackLength && gpuOk; f0 += batch)
            {
               const long f1 = std::min(stackLength, f0 + batch);
               std::vector<std::vector<sim::GpuSplatEmitter>> ems(static_cast<size_t>(f1 - f0));
               std::vector<uint32_t> frameIds;
               std::vector<double> bgScales;
               std::vector<std::vector<uint16_t>*> outs;
               for (long f = f0; f < f1; ++f)
               {
                  evs.clear();
                  for (uint32_t idx : frameEvents[static_cast<size_t>(f)])
                     evs.push_back((*plan.events)[idx]);
                  const sim::DriftNm& d = stackDrift[static_cast<size_t>(f)];
                  const double dx = d.x / params.pixelSizeNm, dy = d.y / params.pixelSizeNm;
                  sim::RenderExtras extras = shaping.Extras(f * params.frameDurationSec, decaySec);
                  sim::CollectGpuEmitters(evs, f, fullW, fullH, params.pixelSizeNm, gp.photonsPerBlink, dx, dy,
                                          *plan.kernel, zOf(f) - d.z / 1000.0, &extras,
                                          ems[static_cast<size_t>(f - f0)], &zc, &zt);
                  frameIds.push_back(static_cast<uint32_t>(f));
                  bgScales.push_back(extras.backgroundScale);
                  outs.push_back(&newStack[static_cast<size_t>(f)]);
               }
               if (!gpu->RenderFrames(ems, frameIds, bgScales, cam, noiseSeed, outs, err))
               {
                  LogMessage("GPU frame render failed, rendering the stack on the CPU: " + err, false);
                  SetGpuStatus("CPU (GPU render failed: " + err + ")");
                  gpuOk = false;
                  stackFramesGenerated_ = 0;
                  zc = zt = 0;
                  break;
               }
               stackFramesGenerated_ = f1;
            }
            zClampedAll += zc;
            zRenderedAll += zt;
            if (gpuOk)
            {
               where = "the GPU";
               lit = true;
            }
         }
         if (!gpuOk)
         {
            sim::FluorescenceFrameOptions opt;
            opt.zStageUm = zOf;   // the movie subtracts the spec's z drift
            if (!shaping.illum.empty())
               opt.illumField = &shaping.illum;
            if (decaySec > 0)
               opt.backgroundScale = [&](long f) { return sim::BackgroundFadeScale(f * params.frameDurationSec, decaySec); };
            std::vector<float> sum;
            opt.onPhotons = [&](long f, const std::vector<float>& photons) {
               const std::vector<float>* p = &photons;
               if (both)
               {
                  sum = photons;
                  addLamp(f, sum);
                  p = &sum;
               }
               sim::ApplyNoiseChain(*p, newStack[static_cast<size_t>(f)], fullW, fullH, cam, localOffsetMap,
                                    localGainMap, localReadNoiseMap, noiseSeed, static_cast<uint32_t>(f), true);
               stackFramesGenerated_ = f + 1;
               return true;
            };
            sim::ScopeMovieInfo info;
            if (!fm.Render([](long, const std::vector<uint16_t>&) { return true; }, info, err, nullptr, &opt))
               LogMessage("Fluorescence: " + err, false);
            else
            {
               LogMessage(info.description);
               lit = true;
            }
         }
      }
      if (lit)
         illumHistory_.Advance(lx0, ly0, lx1, ly1, stackLength * params.frameDurationSec,
                               HistoryWeight(shaping, stageXUm, stageYUm));
      for (std::vector<uint16_t>& fr : newStack)
         if (fr.size() != static_cast<size_t>(fullW) * fullH)
            fr.assign(static_cast<size_t>(fullW) * fullH, static_cast<uint16_t>(std::min(65535.0, std::max(0.0, params.offsetAdu))));
   }
   {
      double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
      std::ostringstream msg;
      msg << "Rendered " << stackLength << " frames in " << std::fixed << std::setprecision(2) << secs << " s on " << where;
      LogMessage(msg.str());
   }
   long zClampedTotal = zClampedAll.load(), zRenderedTotal = zRenderedAll.load();
   if (zClampedTotal > 0)
   {
      std::ostringstream warn;
      warn << zClampedTotal << "/" << zRenderedTotal << " emitter renders had a total z (stage offset + "
           << "dye depth) beyond the PSF kernel's own z range and were clamped to its end plane -- "
           << "widen PsfZRangeUm or narrow SimType_CellFieldZRangeUm.";
      LogMessage(warn.str(), false);
   }

   {
      MMThreadGuard g(imgPixelsLock_);
      stack_.swap(newStack);
      stackFrameW_ = fullW;
      stackFrameH_ = fullH;
      playbackIndex_ = 0;
      endOfStackReached_ = false;
   }

   stackReady_ = true;
   stackGenerating_ = false;
}

///////////////////////////////////////////////////////////////////////////////
// Live mode
///////////////////////////////////////////////////////////////////////////////

void CInSiliScopeCamera::StartLiveProducer()
{
   if (liveProducerRun_.load())
      return;
   if (liveProducerThread_.joinable())
      liveProducerThread_.join();

   liveFrameCounter_ = 0;
   liveDriftOriginFrame_ = 0;
   uint64_t liveSeed = static_cast<uint64_t>(St().seed.load()) ^ 0xABCDEF1234567890ULL;
   liveRng_.seed(liveSeed);

   {
      MMThreadGuard g(frontFrameLock_);
      liveFrameW_ = FullWidth();
      liveFrameH_ = FullHeight();
      frontFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);
      backFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);
   }
   liveFrameSeq_ = 0;
   lastConsumedLiveFrameSeq_ = -1;
   liveTakenSeq_ = -1;
   frameIntervalHistoryCount_ = 0;
   frameIntervalHistoryPos_ = 0;
   actualFrameIntervalMs_ = 0.0;

   liveProducerRun_ = true;
   liveProducerThread_ = std::thread(&CInSiliScopeCamera::LiveProducerLoop, this);
}

void CInSiliScopeCamera::StopLiveProducer()
{
   liveProducerRun_ = false;
   if (liveProducerThread_.joinable())
      liveProducerThread_.join();
}

void CInSiliScopeCamera::LiveProducerLoop()
{
   std::vector<float> photonImg;
   sim::PixelOffsetMap offsetMap;
   sim::PixelGainMap gainMap;
   sim::PixelReadNoiseMap readNoiseMap;
   // The illumination field -- rebuilt on the same config-version trigger as
   // everything else here.
   sim::StackShapingFields shaping;
   // GPU simulator for this thread (created on first use), the kernel it was
   // loaded with and the background it carries.
   std::unique_ptr<sim::GpuSimulator> gpu;
   uint64_t gpuKernelSerial = 0;
   double gpuBackground = -1.0;
   bool gpuFailed = false;
   // The mean-field scenes' Direct3D 11 host of this thread.
   std::unique_ptr<sim::WidefieldGpuD3D11> wfGpu;
   bool wfGpuTried = false;
   // Sentinel: guarantees the very first tick below rebuilds the offset map
   // and the rest of the cached config.
   long appliedConfigVersion = -1;
   long zClampedSinceRebuild = 0, zTotalSinceRebuild = 0;
   // The sample drift: one walker (Simulation/Drift.h) from the drift origin;
   // a Live/MDA sequence start restarts it at the next frame.
   sim::DriftWalker liveWalker;
   bool liveWalkerOn = false;
   long appliedDriftRestart = liveDriftRestart_.load();
   // BrightField with drift: the scene's anchor (the drift it was built at)
   // and the focus-grid spectra around its focus.
   sim::DriftNm bfAnchor;
   bool bfAnchored = false;
   sim::BrightfieldDriftFrames bfDrift;
   double bfDriftFocus = std::numeric_limits<double>::quiet_NaN();
   // The simulated time the labels' schedules are read at, which advances by
   // one frame duration per produced frame -- across config changes too, so
   // changing a camera setting does not un-bleach the sample. It starts
   // 0 (the fluorescence dyes read their own clocks from the illumination
   // history; this one is BrightField's and the prefetch's).
   double cellFieldTimeSec = 0.0;
   // BrightField: the cell field (configured on the rebuild trigger below),
   // the scene of the current pose (rebuilt when the pose or a setting
   // changes; a focus change only re-images) and the transmitted intensity of
   // the current frame.
   sim::CellFieldSource cellField;
   bool cellFieldOk = false;
   bool bfActive = false, bfErrLogged = false, flErrLogged = false;
   long cellFieldVersion = -1;   // the config version the cell field was configured at
   sim::BrightfieldScene bfScene;
   std::vector<float> bfImage;
   // Publishes a finished frame (front buffer, sequence counter, interval
   // statistics).
   auto publish = [this](std::vector<uint16_t>& frame, unsigned fw, unsigned fh, long epoch, long frameIndex,
                         long config, long light, LitFrame& lit,
                         sim::SharedStageState::Clock::time_point started) {
      const double renderMs =
         std::chrono::duration<double, std::milli>(sim::SharedStageState::Clock::now() - started).count();
      const double prevRender = liveRenderMs_.load(std::memory_order_relaxed);
      liveRenderMs_.store(prevRender > 0.0 ? 0.8 * prevRender + 0.2 * renderMs : renderMs, std::memory_order_relaxed);
      {
         MMThreadGuard g(frontFrameLock_);
         frontFrame_.swap(frame);
         std::swap(liveFrameLit_, lit);
         liveFrameStart_ = started;
         liveFramePublished_ = sim::SharedStageState::Clock::now();
         liveFrameW_ = fw;
         liveFrameH_ = fh;
         liveFrameEpoch_ = epoch;
         liveFrameConfig_ = config;
         liveFrameLight_ = light;
         // Under the lock: a consumer reads the frame and its number together.
         liveFrameSeq_.fetch_add(1, std::memory_order_relaxed);
      }
      {
         std::lock_guard<std::mutex> lk(liveCvMutex_);
      }
      liveCv_.notify_all();
      MM::MMTime publishTime = GetCurrentMMTime();
      if (frameIndex > 0)
      {
         double intervalMs = (publishTime - lastFramePublishTime_).getMsec();
         frameIntervalHistoryMs_[frameIntervalHistoryPos_ % kFrameIntervalWindowSize] = intervalMs;
         ++frameIntervalHistoryPos_;
         if (frameIntervalHistoryCount_ < kFrameIntervalWindowSize)
            ++frameIntervalHistoryCount_;
         double sum = 0.0;
         for (int i = 0; i < frameIntervalHistoryCount_; ++i)
            sum += frameIntervalHistoryMs_[i];
         actualFrameIntervalMs_.store(sum / frameIntervalHistoryCount_, std::memory_order_relaxed);
      }
      lastFramePublishTime_ = publishTime;
   };
   sim::CellFieldQuery bfLastQuery;
   bool bfQueried = false;
   // The frame schedule: frame k+1 starts one exposure after frame k was due
   // to start (an absolute schedule, so wake-up latency does not add up); a
   // frame that took longer than the exposure starts the next at once. A snap
   // (liveWakeNow_) starts a frame at once.
   isc::PreciseWaiter waiter;
   sim::SharedStageState::Clock::time_point scheduled = sim::SharedStageState::Clock::now();

   while (liveProducerRun_.load())
   {
      // A hardware z stack: the camera triggers the stage once per frame, so
      // a frame of this acquisition the consumer has not taken yet must not
      // be overwritten by the next position (it would be lost and every later
      // frame would sit one position off).
      const auto tZWait = sim::TimingClock::now();
      bool zWaited = false;
      for (;;)
      {
         if (!liveProducerRun_.load() || !liveSeqCapture_.load() || !liveSeqSkipStale_.load() ||
             liveTakenSeq_.load() >= liveFrameSeq_.load())
            break;
         {
            MMThreadGuard g(frontFrameLock_);
            if (liveFrameEpoch_ < liveSeqEpoch_.load())
               break;   // a frame from before this acquisition: skipped anyway
         }
         zWaited = true;
         std::unique_lock<std::mutex> lk(liveCvMutex_);
         liveCv_.wait_for(lk, std::chrono::milliseconds(2),
                          [this] { return liveTakenSeq_.load() >= liveFrameSeq_.load() || !liveProducerRun_.load(); });
      }
      if (zWaited)
         sim::TimingLog("live.zseq-wait", sim::TimingSince(tZWait));
      // Everything this frame reads (settings, pose, focus, clocks) is read
      // after this instant (GenerateNextFrameIntoImg: liveFrameStart_).
      const sim::SharedStageState::Clock::time_point frameStart = sim::SharedStageState::Clock::now();
      // Phase times of this frame (Simulation/Timing.h: ISC_TIMING, Test_ProfileCollect).
      auto tLap = sim::TimingClock::now();
      auto lap = [&tLap](const char* phase) {
         const auto now = sim::TimingClock::now();
         sim::TimingLog(phase, std::chrono::duration<double>(now - tLap).count());
         tLap = now;
      };
      sim::SimulationParams params = isc::SnapshotParams(St());
      unsigned w = FullWidth();
      unsigned h = FullHeight();

      // InvalidateStack() bumps liveConfigVersion_ from *every* property
      // handler that changes something affecting simulated frame content; a
      // bump refreshes the state cached across ticks: the static noise maps,
      // the illumination field and (BrightField) the cell field.
      long currentConfigVersion = liveConfigVersion_.load(std::memory_order_relaxed);
      if (currentConfigVersion != appliedConfigVersion || offsetMap.width != w || offsetMap.height != h)
      {
         offsetMap.Generate(w, h, params.offsetAdu, params.offsetStdAdu, liveRng_);
         gainMap.Generate(w, h, params.gainPhotonsPerAdu, params.pixelGainStdFraction, liveRng_);
         readNoiseMap.Generate(w, h, params.readNoiseElectrons, params.pixelReadNoiseStdFraction, liveRng_);
         shaping = ShapingFields(w, h);
         if (zClampedSinceRebuild > 0)
         {
            std::ostringstream warn;
            warn << zClampedSinceRebuild << "/" << zTotalSinceRebuild << " emitter renders had a total z "
                 << "(stage offset + dye depth) beyond the PSF kernel's own z range and were clamped "
                 << "to its end plane since the last config change.";
            LogMessage(warn.str(), false);
         }
         zClampedSinceRebuild = 0;
         zTotalSinceRebuild = 0;
         gpuKernelSerial = 0;   // reload the GPU (maps, background)
         gpuFailed = false;
         flErrLogged = false;
         if (!isc::BrightFieldSelected(St()))
         {
            const std::string zWarn = isc::CellFieldZRangeWarning(St());
            if (!zWarn.empty())
               LogMessage(zWarn, false);
         }
         appliedConfigVersion = currentConfigVersion;
      }
      // The light of this frame: the shutters of the light sources (a frame
      // rendered with other light is not taken, liveFrameLight_). BrightField
      // configures the cell field when it starts and after a setting changed.
      const long frameLight = St().lightVersion.load();
      const bool lightOn = isc::LightOn(St());
      const bool epiOn = St().epiOpen.load();
      bfActive = St().transOpen.load();   // the lamp: BrightField, alone or with the fluorescence
      if (bfActive && cellFieldVersion != appliedConfigVersion)
      {
         std::string err;
         cellFieldOk = cellField.Configure(isc::BuildCellFieldSettings(St()), err);
         if (!cellFieldOk)
            LogMessage("CellField unavailable: " + err, false);
         bfErrLogged = false;
         cellFieldVersion = appliedConfigVersion;
      }

      // The drift starts from zero at liveDriftOriginFrame_ (reset at
      // StartLiveProducer(), and here at the first frame after a Live/MDA
      // sequence start asked for it): the walker's steps of the frames since,
      // the stack's path for the same seed (each step with the current
      // settings). Off: no steps; switched on later, it starts from zero there.
      const long driftRestart = liveDriftRestart_.load();
      if (driftRestart != appliedDriftRestart)
      {
         liveDriftOriginFrame_ = liveFrameCounter_.load(std::memory_order_relaxed);
         appliedDriftRestart = driftRestart;
      }
      long framesSinceDriftOrigin = std::max(0L, liveFrameCounter_.load(std::memory_order_relaxed) -
                                                      liveDriftOriginFrame_.load(std::memory_order_relaxed));
      if (framesSinceDriftOrigin < liveWalker.Frame() || !params.drift.On() || !liveWalkerOn)
      {
         liveWalker = sim::DriftWalker(St().seed.load(), params.drift, framesSinceDriftOrigin);
         liveWalkerOn = params.drift.On();
      }
      liveWalker.SetSettings(params.drift);
      while (liveWalker.Frame() < framesSinceDriftOrigin)
         liveWalker.Step(params.frameDurationSec);
      const sim::DriftNm liveDrift = liveWalker.Position();
      // The fluorescence frame takes the drift as its pose: the FOV over the
      // moved sample (stage - d), the focal plane dz lower in it.
      const double dx = 0.0, dy = 0.0;
      // InSiliScopeZStage's current position, read fresh every tick -- or,
      // during a sequence acquisition with an armed z sequence, the
      // sequence's next position (one per frame).
      long frameEpoch = 0;
      double zOffsetUm = Stg().NextFrameZ(&frameEpoch);
      double sx = 0.0, sy = 0.0;
      Stg().PositionXyAt(sim::SharedStageState::Clock::now(), sx, sy);
      // The background fade restarts with the drift ramp.
      sim::RenderExtras extras =
         shaping.Extras(framesSinceDriftOrigin * params.frameDurationSec, St().bgDecaySec.load());
      // Counter-based noise: keyed by the live frame counter, on a seed of
      // its own (live mode was never meant to reproduce precomputed frames).
      const uint32_t liveNoiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(St().seed.load()) ^ 0x4C4E4F49ULL);
      const uint32_t noiseFrame = static_cast<uint32_t>(liveFrameCounter_.load(std::memory_order_relaxed));
      std::vector<uint16_t> nextFrame;
      sim::ScopeSpec spec;
      // The lit rect of this frame and its illumination clocks.
      double lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
      sim::ClockSnapshot clock;
      LitFrame lit;
      // The lamp's photons of this frame (x qe) into out. A new pose or setting
      // rebuilds the scene (seconds at high quality); a focus change re-images
      // it (one inverse FFT per source). Photons per pixel of the empty field
      // times the transmitted intensity (dark if the scene failed).
      auto lampInto = [&](std::vector<float>& out, double qe) {
         if (cellFieldOk && params.drift.On())
         {
            // With drift: the scene stays at an anchor pose (rebuilt when the
            // sample has moved 1 um from it) and each frame is its fine-grid
            // image shifted by the rest of the drift, the z drift interpolated
            // on a focus grid (BrightfieldDriftFrames).
            if (!bfAnchored || std::fabs(liveDrift.x - bfAnchor.x) > 1000.0 ||
                std::fabs(liveDrift.y - bfAnchor.y) > 1000.0)
            {
               bfAnchor = liveDrift;
               bfAnchored = true;
            }
            const sim::CellFieldQuery q =
               isc::CellFieldQueryFor(St(), sx - bfAnchor.x / 1000.0, sy - bfAnchor.y / 1000.0, zOffsetUm, w, h, params, 0.0, 0.0,
                                 0.0, 0.0, liveFrameCounter_, cellFieldTimeSec, params.frameDurationSec);
            std::string err;
            sim::BrightfieldSpec bs = isc::BuildBrightfieldSpec(St(), params, q, w, h);
            bs.marginUm = bs.Resolved().marginUm + 1.0 + params.pixelSizeNm / 1000.0;
            bool ok = bfScene.Update(cellField, bs, static_cast<uint64_t>(appliedConfigVersion), err);
            if (ok && q.zCullCentreUm != bfDriftFocus)
            {
               sim::DriftBounds zb;
               zb.zLo = -20000.0;
               zb.zHi = 20000.0;
               bfDrift.Begin(zb, std::vector<double>(1, q.zCullCentreUm));
               bfDriftFocus = q.zCullCentreUm;
            }
            ok = ok && bfDrift.Ensure(bfScene, 0, liveDrift.z, err);
            if (ok)
            {
               sim::DriftNm rest = liveDrift;
               rest.x -= bfAnchor.x;
               rest.y -= bfAnchor.y;
               bfDrift.Image(bfScene, 0, rest, bfImage);
            }
            else
            {
               bfImage.clear();
               if (!bfErrLogged)
                  LogMessage("BrightField: " + err, false);
               bfErrLogged = true;
            }
            bfLastQuery = q;
            bfQueried = true;
         }
         else if (cellFieldOk)
         {
            const sim::CellFieldQuery q = isc::CellFieldQueryFor(St(), sx, sy, zOffsetUm, w, h, params, dx, dy, dx, dy,
                                                            liveFrameCounter_, cellFieldTimeSec, params.frameDurationSec);
            std::string err;
            const sim::BrightfieldSpec bs = isc::BuildBrightfieldSpec(St(), params, q, w, h);
            const bool ok = bfScene.Update(cellField, bs, static_cast<uint64_t>(appliedConfigVersion), err) &&
                            bfScene.Image(q.zCullCentreUm, bfImage, err);
            if (!ok)
            {
               bfImage.clear();
               if (!bfErrLogged)
                  LogMessage("BrightField: " + err, false);
               bfErrLogged = true;
            }
            bfLastQuery = q;
            bfQueried = true;
         }
         const double flux = std::max(0.0, St().brightField[isc::BF_PHOTONS_PER_PX_PER_SEC].load()) *
                             params.frameDurationSec * qe;
         out.assign(static_cast<size_t>(w) * h, 0.0f);
         if (bfImage.size() == out.size())
            for (size_t i = 0; i < out.size(); ++i)
               out[i] = static_cast<float>(bfImage[i] * flux);
         lap("live.lamp");
      };
      lap("live.state");
      if (!lightOn)
      {
         // No light source open: a dark frame (offset, read noise, dark current).
         photonImg.assign(static_cast<size_t>(w) * h, 0.0f);
         sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                              liveNoiseSeed, noiseFrame, true);
         lap("live.noise");
      }
      else if (bfActive && !epiOn)
      {
         // BrightField alone: the lamp (the noise chain applies its QE).
         lampInto(photonImg, 1.0);
         sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                              liveNoiseSeed, noiseFrame, true);
         lap("live.noise");
      }
      else
      {
         // Fluorescence: this frame of the engine's movie at the current pose,
         // focus and time (one stage pose per frame; motion blur is ignored).
         spec = isc::BuildScopeSpec(St(), sx - liveDrift.x / 1000.0, sy - liveDrift.y / 1000.0, zOffsetUm - liveDrift.z / 1000.0,
                               0.0, 1);
         SyncHistoryWorld();
         LitRect(sx, sy, lx0, ly0, lx1, ly1);
         clock = illumHistory_.Snapshot(lx0, ly0, lx1, ly1);
         lap("live.spec+clock");
         sim::FluorescenceMovie fm;
         std::string err;
         bool rendered = false;
         const bool begun = fm.Begin(spec, false, err, St().useGpu.load() ? WideFieldGpu(wfGpu, wfGpuTried) : nullptr, &clock);
         lap("live.begin");
         if (!begun)
         {
            if (!flErrLogged)
               LogMessage("Fluorescence: " + err, false);
            flErrLogged = true;
         }
         else
         {
            const sim::FluorescenceSimplePlan plan = fm.SimplePlan();
            // The GPU splat applies the noise itself: not with the lamp's photons to add.
            if (plan.ok && plan.kernel && !gpuFailed && St().useGpu.load() && !bfActive)
            {
               sim::SimulationParams gp = params;
               gp.photonsPerBlink = plan.photonsPerBlink;
               gp.backgroundPhotons = plan.backgroundPhotons;
               if (plan.kernel->Serial() != gpuKernelSerial || plan.backgroundPhotons != gpuBackground)
               {
                  gpuKernelSerial = PrepareGpu(gpu, *plan.kernel, w, h, offsetMap, gainMap, readNoiseMap, shaping, gp)
                                       ? plan.kernel->Serial() : 0;
                  gpuBackground = plan.backgroundPhotons;
                  gpuFailed = gpuKernelSerial == 0;
               }
               if (gpuKernelSerial)
               {
                  std::vector<sim::GpuSplatEmitter> ems;
                  const auto tGpu = sim::TimingClock::now();
                  sim::CollectGpuEmitters(*plan.events, 0, w, h, params.pixelSizeNm, gp.photonsPerBlink, dx, dy,
                                          *plan.kernel, zOffsetUm, &extras, ems, &zClampedSinceRebuild,
                                          &zTotalSinceRebuild);
                  sim::TimingLog("gpu.collect", sim::TimingSince(tGpu));
                  const auto tGpuRender = sim::TimingClock::now();
                  rendered = gpu->RenderFrame(ems, params.Camera(), extras.backgroundScale, liveNoiseSeed, noiseFrame,
                                              nextFrame, err);
                  sim::TimingLog("gpu.splat+noise", sim::TimingSince(tGpuRender));
                  if (!rendered)
                  {
                     LogMessage("GPU frame render failed, continuing on the CPU: " + err, false);
                     SetGpuStatus("CPU (GPU render failed: " + err + ")");
                     gpuFailed = true;
                  }
               }
            }
            else if (!plan.ok && St().useGpu.load() && !gpuFailed && !fm.HasPopulations())
               SetGpuStatus("CPU (the GPU splat is for one blink group without continuous populations)");
            if (!rendered)
            {
               sim::FluorescenceFrameOptions opt;
               if (!shaping.illum.empty())
                  opt.illumField = &shaping.illum;
               const double bgScale = extras.backgroundScale;
               if (bgScale != 1.0)
                  opt.backgroundScale = [bgScale](long) { return bgScale; };
               opt.onPhotons = [&](long, const std::vector<float>& photons) {
                  photonImg = photons;
                  return true;
               };
               sim::ScopeMovieInfo info;
               if (fm.Render([](long, const std::vector<uint16_t>&) { return true; }, info, err, nullptr, &opt))
                  rendered = true;
               else if (!flErrLogged)
               {
                  LogMessage("Fluorescence: " + err, false);
                  flErrLogged = true;
               }
            }
         }
         lap("live.render");
         if (!rendered || nextFrame.size() != static_cast<size_t>(w) * h)
         {
            if (!rendered || photonImg.size() != static_cast<size_t>(w) * h)
               photonImg.assign(static_cast<size_t>(w) * h, 0.0f);
            if (bfActive)
            {
               // Both lights: the lamp's photons x the QE at its wavelength
               // (the fluorescence chain runs at QE 1).
               std::vector<float> lampPhotons;
               lampInto(lampPhotons, isc::BrightFieldQe(St()));
               for (size_t i = 0; i < photonImg.size() && i < lampPhotons.size(); ++i)
                  photonImg[i] += lampPhotons[i];
            }
            sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                                 liveNoiseSeed, noiseFrame, true);
         }
         lap("live.noise");
         // This frame lights the FOV and its margin for one exposure (counted
         // when it is taken).
         if (rendered)
         {
            lit.valid = true;
            lit.x0 = lx0;
            lit.y0 = ly0;
            lit.x1 = lx1;
            lit.y1 = ly1;
            lit.dtSec = params.frameDurationSec;
            lit.weight = HistoryWeight(shaping, sx, sy);
         }
      }
      cellFieldTimeSec += params.frameDurationSec;
      publish(nextFrame, w, h, frameEpoch, liveFrameCounter_.load(std::memory_order_relaxed), currentConfigVersion, frameLight,
              lit, frameStart);
      lap("live.publish");
      sim::TimingLog("live.frame", std::chrono::duration<double>(sim::SharedStageState::Clock::now() - frameStart).count());
      ++liveFrameCounter_;

      const double exposureMs = GetExposure();
      scheduled += std::chrono::duration_cast<sim::SharedStageState::Clock::duration>(
         std::chrono::duration<double, std::milli>(exposureMs));
      const auto nowAfter = sim::SharedStageState::Clock::now();
      if (scheduled < nowAfter)
         scheduled = nowAfter;   // rendering took longer than the exposure: no catch-up burst
      double sleepMs = std::chrono::duration<double, std::milli>(scheduled - nowAfter).count();
      // Spend the wait pre-loading the dyes a stage move would need next (the
      // whole z column and an xy margin around the FOV), so focusing and
      // nearby moves do not stall a frame on generating them.
      if (sleepMs > kCellFieldPrefetchSlackMs && !liveWakeNow_.load())
      {
         const auto tPrefetch = sim::SharedStageState::Clock::now();
         // Not with the lamp on: BrightField reads only the cells' geometry
         // (its scene is rebuilt on a pose change), and the dye prefetch of a
         // whole z column overran its budget by half a second (one work item
         // is checked against the budget only when it ends), stalling frames.
         if (!bfActive && !spec.empty())
         {
            spec["start-sec"] = clock.At(sx, sy);
            sim::PrefetchScope(spec, kCellFieldPrefetchMarginUm, sleepMs - kCellFieldPrefetchSlackMs);
         }
         const double took =
            std::chrono::duration<double, std::milli>(sim::SharedStageState::Clock::now() - tPrefetch).count();
         sim::TimingLog("live.prefetch", took / 1000.0);
         if (took > livePrefetchMaxMs_.load())
         {
            livePrefetchMaxMs_ = took;
            livePrefetchBudgetMs_ = sleepMs - kCellFieldPrefetchSlackMs;
         }
      }
      const auto tWait = sim::TimingClock::now();
      waiter.WaitUntil(scheduled, [this] { return !liveProducerRun_.load() || liveWakeNow_.load(); });
      sim::TimingLog("live.idle", sim::TimingSince(tWait));
      if (liveWakeNow_.exchange(false))
         scheduled = sim::SharedStageState::Clock::now();   // a snap: its frame starts now
   }
}

///////////////////////////////////////////////////////////////////////////////
// Frame delivery (shared by SnapImage / RunSequenceOnThread)
///////////////////////////////////////////////////////////////////////////////

void CInSiliScopeCamera::CropFullFrameIntoImg(const std::vector<uint16_t>& fullFrame, unsigned fullW, unsigned fullH)
{
   if (fullFrame.size() != static_cast<size_t>(fullW) * fullH)
      return;

   MMThreadGuard g(imgPixelsLock_);
   unsigned x = std::min(roiX_, fullW);
   unsigned y = std::min(roiY_, fullH);
   unsigned w = std::min(roiXSize_, fullW - x);
   unsigned h = std::min(roiYSize_, fullH - y);
   if (img_.Width() != w || img_.Height() != h || img_.Depth() != 2)
      img_.Resize(w, h, 2);

   unsigned char* dst = img_.GetPixelsRW();
   for (unsigned row = 0; row < h; ++row)
   {
      const uint16_t* srcRow = fullFrame.data() + static_cast<size_t>(y + row) * fullW + x;
      std::memcpy(dst + static_cast<size_t>(row) * w * 2, srcRow, static_cast<size_t>(w) * 2);
   }
}

bool CInSiliScopeCamera::GenerateNextFrameIntoImg(bool interruptible)
{
   if (acqMode_ == SMLM_MODE_LIVE)
   {
      // Wait for LiveProducerLoop to actually swap in a frame we haven't
      // consumed yet, rather than immediately copying frontFrame_: without
      // this, a consumer pulling faster than the producer's exposure-paced
      // tick (e.g. right after StartSequenceAcquisition, or a short
      // interval) would re-copy the still-current frontFrame_ and deliver
      // an exact duplicate. Waiting instead turns that into a timing
      // stutter -- the correct behavior, since the "duplicate" frame simply
      // hadn't been simulated yet.
      std::vector<uint16_t> frameCopy;
      unsigned w, h;
      long seq;
      LitFrame lit;
      // Only frames rendered with the settings of this moment: one already in
      // flight when a property changed would show the old settings. A snap
      // takes a frame started after it was called (one in flight would show
      // the stage pose before a move, and its illumination clocks would miss
      // the previous snap's light); a sequence acquisition, frames started
      // after it began.
      const long configNow = liveConfigVersion_.load(std::memory_order_relaxed);
      const long lightNow = St().lightVersion.load();
      const sim::SharedStageState::Clock::time_point takeAfter =
         interruptible ? sim::SharedStageState::Clock::time_point(
                            sim::SharedStageState::Clock::duration(liveSeqStartTicks_.load()))
                       : sim::SharedStageState::Clock::now();
      // A snap: a producer waiting for its next tick starts this frame now.
      if (!interruptible)
         liveWakeNow_ = true;
      const auto tTake = sim::TimingClock::now();
      sim::SharedStageState::Clock::time_point published;
      for (;;)
      {
         {
            MMThreadGuard g(frontFrameLock_);
            seq = liveFrameSeq_.load(std::memory_order_relaxed);
            // A z-sequence acquisition takes only frames rendered after it
            // started (their focus is the sequence's).
            // A sequence (interruptible) takes only frames rendered after its
            // drift restart.
            const bool stale = (interruptible && liveSeqSkipStale_.load() && liveFrameEpoch_ < liveSeqEpoch_.load()) ||
                               liveFrameConfig_ < configNow || liveFrameLight_ < lightNow ||
                               liveFrameStart_ < takeAfter;
            if (stale && seq != lastConsumedLiveFrameSeq_)
            {
               lastConsumedLiveFrameSeq_ = seq;
               liveTakenSeq_ = seq;
               liveCv_.notify_all();
            }
            else if (seq != lastConsumedLiveFrameSeq_)
            {
               frameCopy = frontFrame_;
               w = liveFrameW_;
               h = liveFrameH_;
               lit = liveFrameLit_;
               published = liveFramePublished_;
               break;
            }
         }
         if (!liveProducerRun_.load())
            return false;
         if (interruptible && thd_ && thd_->IsStopped())
            return false;
         // Woken by the next publish (the timeout only re-checks the stop flags).
         std::unique_lock<std::mutex> lk(liveCvMutex_);
         liveCv_.wait_for(lk, std::chrono::milliseconds(5),
                          [&] { return liveFrameSeq_.load() != seq || !liveProducerRun_.load(); });
      }
      lastConsumedLiveFrameSeq_ = seq;
      liveTakenSeq_ = seq;
      liveCv_.notify_all();   // a hardware z stack's producer waits for this
      const auto tTaken = sim::TimingClock::now();
      sim::TimingLog(interruptible ? "mm.wait-frame (sequence)" : "mm.wait-frame (snap)",
                     std::chrono::duration<double>(tTaken - tTake).count());
      sim::TimingLog("mm.frame-age", std::chrono::duration<double>(tTaken - published).count());
      CropFullFrameIntoImg(frameCopy, w, h);
      // The frame was taken: its light goes into the illumination history.
      if (lit.valid)
         illumHistory_.Advance(lit.x0, lit.y0, lit.x1, lit.y1, lit.dtSec, lit.weight);
      sim::TimingLog("mm.copy+history", sim::TimingSince(tTaken));
      return true;
   }

   // Precomputed mode: auto-trigger generation on first use so a user who
   // never touches GenerateStack still gets working frames. This wait can
   // take seconds for a large stack; when called from the sequence thread
   // (interruptible), bail out early if a stop was requested in the
   // meantime so StopSequenceAcquisition() isn't blocked for the whole
   // duration of generation -- generation itself keeps running in the
   // background and will be picked up by the next Snap/sequence start.
   if (!stackReady_.load())
   {
      StartStackGeneration();
      while (!stackReady_.load())
      {
         if (interruptible && thd_ && thd_->IsStopped())
            return false;
         CDeviceUtils::SleepMs(5);
      }
   }

   std::vector<uint16_t> frame;
   unsigned fullW, fullH;
   {
      MMThreadGuard g(imgPixelsLock_);
      if (stack_.empty())
         return false;
      if (playbackIndex_ >= static_cast<long>(stack_.size()))
         playbackIndex_ = stackLoop_ ? 0 : static_cast<long>(stack_.size()) - 1;

      long idx = playbackIndex_;
      frame = stack_[static_cast<size_t>(idx)];
      fullW = stackFrameW_;
      fullH = stackFrameH_;
      if (idx == static_cast<long>(stack_.size()) - 1 && !stackLoop_)
         endOfStackReached_ = true;
      playbackIndex_++;
   }
   CropFullFrameIntoImg(frame, fullW, fullH);
   return true;
}

///////////////////////////////////////////////////////////////////////////////
// Property handlers
///////////////////////////////////////////////////////////////////////////////

int CInSiliScopeCamera::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(binSize_);
   }
   else if (eAct == MM::AfterSet)
   {
      if (IsCapturing())
         return DEVICE_CAMERA_BUSY_ACQUIRING;

      long b;
      pProp->Get(b);
      if (b <= 0)
         return DEVICE_ERR;
      binSize_ = b;
      St().binning = b;
      ApplyFrameSizeChange();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnGenerateStack(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(0L);
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v != 0)
      {
         StartStackGeneration();
         pProp->Set(0L);
      }
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnStackStatus(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      if (stackGenerating_.load())
      {
         std::ostringstream os;
         os << "Generating " << stackFramesGenerated_.load() << "/" << stackLength_;
         pProp->Set(os.str().c_str());
      }
      else if (stackReady_.load())
      {
         std::ostringstream os;
         os << "Ready (" << stack_.size() << " frames)";
         pProp->Set(os.str().c_str());
      }
      else
      {
         pProp->Set("Idle");
      }
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnStackLength(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(stackLength_);
   else if (eAct == MM::AfterSet)
   {
      long v = 0;
      pProp->Get(v);
      v = std::max(1L, std::min(100000L, v));
      if (v != stackLength_)
      {
         stackLength_ = v;
         InvalidateStack();
      }
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(endOfStackReached_ ? "Yes" : "No");
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnActualFrameIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(actualFrameIntervalMs_.load(std::memory_order_relaxed));
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnProfileCollect(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(sim::TimingCollect().load() ? "On" : "Off");
   else if (eAct == MM::AfterSet)
   {
      std::string v;
      pProp->Get(v);
      sim::TimingCollect() = v == "On";
      sim::TimingProfileTake();   // start afresh
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnProfileWriteTo(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      std::string path;
      pProp->Get(path);
      if (path.empty())
         return DEVICE_OK;
      std::ofstream f(path, std::ios::binary);
      if (!f)
         return DEVICE_INVALID_PROPERTY_VALUE;
      f << sim::TimingProfileTake() << "\n";
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnLivePrefetchMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      std::ostringstream o;
      o << std::fixed << std::setprecision(1) << livePrefetchMaxMs_.exchange(0.0) << "/"
        << livePrefetchBudgetMs_.exchange(0.0);
      pProp->Set(o.str().c_str());
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnLiveRenderMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(liveRenderMs_.load(std::memory_order_relaxed));
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnExposureProperty(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      // Every rate is converted to a frame with the exposure (SnapshotParams),
      // so the precomputed stack must regenerate. The live loop reads it fresh
      // every tick and none of its cached state depends on it, so this skips
      // the live rebuild (it would recompute the PSF kernel for nothing).
      double v = 0;
      pProp->Get(v);
      St().exposureMs = v;
      InvalidateStackOnly();
   }
   return DEVICE_OK;
}

void CInSiliScopeCamera::SettingsChanged(isc::Invalidate what)
{
   if (what == isc::Invalidate::All)
      InvalidateStack();
   else if (what == isc::Invalidate::Stack)
      InvalidateStackOnly();
   else if (what == isc::Invalidate::Live)
      liveConfigVersion_.fetch_add(1, std::memory_order_relaxed);
}

void CInSiliScopeCamera::StartPsfPreload()
{
   if (psfPreloadThread_.joinable())
      psfPreloadThread_.join();
   if (static_cast<sim::PsfModelKind>(St().psfModel.load()) == sim::PsfModelKind::Gaussian)
      return;
   // The kernels of the current settings' dye states (the engine's prepare
   // movie: world, FOV blocks and kernels, no frames) on a background thread.
   double sx = 0.0, sy = 0.0;
   Stg().PositionXyAt(sim::SharedStageState::Clock::now(), sx, sy);
   sim::ScopeSpec spec = isc::BuildScopeSpec(St(), sx, sy, Stg().zPositionUm.load(), 60.0, 1);
   spec["prepare"] = 1;
   psfPreloadThread_ = std::thread([this, spec]() {
      const sim::TimingScope timing("init.preload (background)");
      LogMessage("PSF: preloading the kernels of the current settings in the background.");
      sim::ScopeMovieInfo info;
      std::string err;
      if (!sim::RenderScopeMovie(spec, [](long, const std::vector<uint16_t>&) { return true; }, info, err))
         LogMessage("PSF preload failed (the first frame computes the kernel instead): " + err, false);
   });
}

