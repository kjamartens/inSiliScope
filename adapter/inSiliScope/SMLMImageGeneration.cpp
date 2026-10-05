///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMImageGeneration.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Property handlers plus the thin glue between the MM camera
//                device and the standalone simulation engine
//                (Simulation/SMLMSimulation.h): precomputed-stack generation
//                (background thread), the always-running live producer
//                thread, and frame delivery into ImgBuffer.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "Simulation/CacheDir.h"
#include "Simulation/SharedStageState.h"
#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>

///////////////////////////////////////////////////////////////////////////////
// Parameter snapshot / invalidation helpers
///////////////////////////////////////////////////////////////////////////////

sim::SimulationParams CInSiliScopeCamera::SnapshotParams() const
{
   // BackgroundPhotonsPerSec and the dark current are rates (per second) at
   // the property level, so they scale with whatever the standard MM Exposure
   // is set to -- converted here to the frame-equivalent quantities the
   // (exposure-unaware) noise chain expects. The dyes' photons come from the
   // engine's FluorescenceMovie (ScopeProperties.cpp: BuildScopeSpec).
   double exposureMs = GetExposure();
   double expSec = exposureMs / 1000.0;
   if (expSec <= 0.0)
      expSec = 0.001;

   sim::SimulationParams p;
   p.pixelSizeNm = pixelSizeNm_.load();
   p.backgroundPhotons = backgroundPhotonsPerSec_.load() * expSec;
   // Fluorescence: the movie's photon images are already detected photons
   // (the camera's QE curve at each dye's emission is in the light path), so
   // the noise chain runs at QE 1, as the cli/viewer's. BrightField: the
   // curve's QE at the lamp wavelength.
   p.quantumEfficiency = BrightFieldSelected() ? BrightFieldQe() : 1.0;
   // Dark current is a rate (e-/pixel/s) like the other *PerSec properties,
   // converted here to its frame-equivalent electron count.
   p.darkCurrentElectronsPerFrame = darkCurrentPerSec_.load() * expSec;
   p.gainPhotonsPerAdu = gainPhotonsPerAdu_.load();
   p.offsetAdu = offsetAdu_.load();
   p.offsetStdAdu = offsetStdAdu_.load();
   p.readNoiseElectrons = readNoiseElectrons_.load();
   p.pixelGainStdFraction = pixelGainStdPct_.load() / 100.0;
   p.pixelReadNoiseStdFraction = pixelReadNoiseStdPct_.load() / 100.0;
   p.driftNmPerSecX = driftNmPerSecX_.load();
   p.driftAngleRad = sim::DriftAngleForSeed(randomSeed_);
   p.frameDurationSec = expSec;
   p.emccd = cameraEmccd_;
   p.emGain = EmGain();
   p.cicElectrons = cicElectrons_.load();
   p.bitDepth = bitDepth_;
   return p;
}

sim::PsfGeneratorRequest CInSiliScopeCamera::BuildPsfGeneratorRequest() const
{
   sim::PsfGeneratorRequest req;
   req.model = CurrentPsfModel();
   // The emission wavelength is the dye's (the engine's request hook sets it
   // per dye state, Initialize: SetScopePsfRequestHook); this placeholder is
   // the default microtubule dye's peak region.
   req.wavelengthNm = 670.0;
   req.na = psfNa_.load();
   req.immersionIndex = psfImmersionIndex_.load();
   req.pixelSizeNm = pixelSizeNm_.load();
   req.oversampling = psfOversampling_;

   // PsfKernelHalfWidthNm (100-20000 nm via its property limits) is a
   // physical half-width, rounded here to the nearest whole camera pixel
   // against the current pixel size -- so the rendered window covers the
   // same physical extent regardless of what PixelSizeNm is set to, which
   // a pixel-denominated property could not do.
   //
   // The result is then treated as a user-settable MINIMUM, auto-grown as
   // needed so the splatted kernel always comfortably covers the
   // first-order Airy ring regardless of NA/wavelength/pixel size. Without
   // this, a low-NA/long-wavelength combination -- both within the allowed
   // property ranges -- can put the first ring outside a small fixed
   // window, silently truncating it (the rendered spot then just looks like
   // a soft square blob with no visible ring, since SplatPsfKernel simply
   // never sees data beyond the window it's given). The margin (3x the
   // classic Rayleigh first-minimum radius, 0.61*lambda/NA) comfortably
   // clears the first bright secondary maximum, mirroring the
   // physics-derived Gaussian sigma (0.21 lambda/NA). Capped at 48 px regardless of physics to keep the
   // oversampled grid PSFGenerator computes from growing unboundedly.
   // (sim::PsfKernelHalfWidthPx, shared with the cli/viewer.)
   req.kernelHalfWidthPx =
      sim::PsfKernelHalfWidthPx(psfKernelHalfWidthNm_.load(), req.pixelSizeNm, req.wavelengthNm, req.na);

   // Real Z-stack (step 2): nz/zStepNm are derived from the user-facing
   // PsfZRangeUm/PsfZStepUm properties rather than hardcoded. The global
   // focus offset selecting a plane from this stack each frame comes from
   // the InSiliScopeZStage device (step 3) -- see RenderPhotonImage's
   // globalZOffsetUm parameter.
   double zRangeUm = psfZRangeUm_.load();
   double zStepUm = std::max(psfZStepUm_.load(), 0.001);
   req.nz = static_cast<int>(std::lround(zRangeUm / zStepUm)) + 1;
   req.zStepNm = zStepUm * 1000.0;

   // GibsonLanni-only (ignored by RichardsWolf); see the comments on
   // psfSampleIndex_/psfWorkingDistanceUm_/psfSampleDepthNm_ in
   // InSiliScopeCamera.h.
   req.sampleIndex = psfSampleIndex_.load();
   req.workingDistanceUm = psfWorkingDistanceUm_.load();
   req.sampleDepthNm = psfSampleDepthNm_.load();

   // GibsonLanniZernike-only (ignored otherwise); see the comment on
   // psfZernikeCoefficients_ in InSiliScopeCamera.h.
   // The property is space-separated (MMCore forbids commas in values);
   // PsfBridge.java wants commas. psfZernikeCoefficients_ only ever holds a
   // string that parsed OK (see OnPsfZernikeCoefficients).
   {
      bool ok = false;
      req.zernikeCoefficients =
         sim::FormatZernikeCoefficients(sim::ParseZernikeCoefficients(psfZernikeCoefficients_, ok), ',');
   }

   req.javaHome = psfGeneratorJavaHome_;
   req.interpMode = static_cast<sim::PsfInterpMode>(psfInterp_);
   req.maskType = static_cast<sim::PsfMaskType>(psfMaskType_);
   req.maskModes = psfMaskModes_;
   req.maskWaist = psfMaskWaist_.load();
   return req;
}

sim::StackShapingFields CInSiliScopeCamera::BuildShapingFields(unsigned w, unsigned h) const
{
   sim::StackShapingFields out;
   double meanFactor = 1.0;
   out.illum = sim::BuildIlluminationField(w, h, static_cast<sim::IllumProfile>(illumProfile_), illumFwhmPct_.load(),
                                           &meanFactor);
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
// Dyes up to this far outside the FOV are still rendered: their PSF tails
// reach in (spec/PORT.md 6.2; a few kernel half-widths of the in-focus core).
constexpr double kCellFieldMarginUm = 2.0;
// Live mode pre-loads this far around the FOV (all of the z column), in the
// time left before the next frame minus this slack.
constexpr double kCellFieldPrefetchMarginUm = 3.0;
constexpr double kCellFieldPrefetchSlackMs = 2.0;
} // namespace

sim::CellFieldSettings CInSiliScopeCamera::BuildCellFieldSettings() const
{
   sim::CellFieldSettings s;
   // Its own stream of RandomSeed ("CELL"), never the arrival/noise one.
   s.seed = static_cast<uint32_t>(static_cast<uint64_t>(randomSeed_) ^ 0x43454C4CULL);
   s.params = {
      {"chunkSize", cellField_[CF_CHUNK_SIZE_UM].load()},
      {"density", cellField_[CF_OCCUPANCY].load()},
      {"cellDiamMin", cellField_[CF_CELL_DIAM_MIN_UM].load()},
      {"cellDiamMax", cellField_[CF_CELL_DIAM_MAX_UM].load()},
      {"mtDensity", cellField_[CF_MT_DENSITY].load()},
      {"enablePacking", cellFieldPacking_ ? 1.0 : 0.0},
   };
   for (int i = 0; i < CF_COUNT; ++i)
      if (g_CellFieldCoreParam[i]) s.params.push_back({g_CellFieldCoreParam[i], cellField_[i].load()});
   // No labels: BrightField reads the geometry only (the fluorescence movie
   // sets its own labels from the spec, BuildScopeSpec).
   s.cacheDir = diskCacheMode_.load() >= 1 ? sim::DefaultCacheDir() : std::string();
   return s;
}

std::string CInSiliScopeCamera::CellFieldZRangeWarning() const
{
   if (CurrentPsfModel() == sim::PsfModelKind::Gaussian)
      return {};
   const double slab = cellField_[CF_Z_RANGE_UM].load(), kernel = psfZRangeUm_.load();
   if (slab > 0.0 && slab <= kernel)
      return {};
   std::ostringstream w;
   w << "CellField: SimType_CellFieldZRangeUm (" << (slab > 0.0 ? std::to_string(slab) + " um" : "0 = no limit")
     << ") exceeds PSFParam_PsfZRangeUm (" << kernel << " um): dyes beyond the kernel's range are drawn "
     << "on its end plane. Lower the former or widen the latter.";
   return w.str();
}

sim::CellFieldQuery CInSiliScopeCamera::CellFieldQueryFor(double stageX, double stageY, double zStageUm, unsigned w,
                                                       unsigned h, const sim::SimulationParams& params,
                                                       double drift0XPx, double drift0YPx, double drift1XPx,
                                                       double drift1YPx, long frameIndex, double tSec,
                                                       double spanSec) const
{
   sim::CellFieldQuery q;
   const double um = params.pixelSizeNm / 1000.0, W = w * um, H = h * um;
   // Stage position = world coordinate of the FOV centre (spec/PORT.md 7.3).
   q.originXUm = stageX - W / 2.0;
   q.originYUm = stageY - H / 2.0;
   // The renderer draws a dye at its FOV-relative position plus the drift,
   // so the dyes a frame can show sit that drift further back.
   const double dxLo = std::min(drift0XPx, drift1XPx) * um, dxHi = std::max(drift0XPx, drift1XPx) * um;
   const double dyLo = std::min(drift0YPx, drift1YPx) * um, dyHi = std::max(drift0YPx, drift1YPx) * um;
   q.x0Um = q.originXUm - dxHi - kCellFieldMarginUm;
   q.x1Um = q.originXUm + W - dxLo + kCellFieldMarginUm;
   q.y0Um = q.originYUm - dyHi - kCellFieldMarginUm;
   q.y1Um = q.originYUm + H - dyLo + kCellFieldMarginUm;
   // The renderer's defocus is zNm/1000 - zStage, so the plane in focus is
   // the world height focus + zStage: ZStage is the focal plane's height
   // above the coverslip, SimType_CellFieldFocusHeightUm an extra offset.
   const double focus = cellField_[CF_FOCUS_HEIGHT_UM].load();
   q.zRefUm = focus;
   q.zCullCentreUm = focus + zStageUm;
   // SimType_CellFieldZRangeUm: total slab around the focal plane whose dyes
   // are rendered (0 = no z limit); dyes outside it are culled, not clamped.
   q.zHalfRangeUm = std::max(0.0, cellField_[CF_Z_RANGE_UM].load()) / 2.0;
   q.frameIndex = frameIndex;
   q.tSec = tSec;
   q.spanSec = spanSec;
   q.frameSec = params.frameDurationSec;
   return q;
}

// The WideField GPU host for this thread, when General_UseGpu is On and a
// usable Direct3D 11 device passes its self-check (General_GpuStatus says
// which, or why not). Created once per thread and kept.
sim::WidefieldGpuD3D11* CInSiliScopeCamera::WideFieldGpu(std::unique_ptr<sim::WidefieldGpuD3D11>& gpu, bool& tried)
{
   if (!useGpu_)
   {
      SetGpuStatus("CPU (General_UseGpu is Off)");
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

sim::BrightfieldSpec CInSiliScopeCamera::BuildBrightfieldSpec(const sim::SimulationParams& params,
                                                              const sim::CellFieldQuery& q, unsigned w,
                                                              unsigned h) const
{
   sim::BrightfieldSpec s;
   s.originXUm = q.originXUm;
   s.originYUm = q.originYUm;
   s.width = w;
   s.height = h;
   s.pixelUm = params.pixelSizeNm / 1000.0;
   auto N = [&](int i) { return brightFieldNum_[i].load(); };
   s.quality = static_cast<int>(std::lround(std::min(4.0, std::max(1.0, N(BF_QUALITY)))));
   s.sources = static_cast<int>(std::lround(std::max(0.0, N(BF_SOURCES))));
   s.upscale = static_cast<int>(std::lround(std::max(0.0, N(BF_UPSCALING))));
   s.sub = static_cast<int>(std::lround(std::max(0.0, N(BF_GEOMETRY_SAMPLES))));
   s.sliceUm = N(BF_SLICE_UM) < 0 ? -1.0 : N(BF_SLICE_UM);
   s.condenserNa = std::max(0.0, N(BF_CONDENSER_NA));
   s.wavelengthNm = std::max(1.0, N(BF_WAVELENGTH_NM));
   s.na = std::max(0.01, psfNa_.load());
   s.nMedium = N(BF_INDEX_MEDIUM);
   s.nCytoplasm = N(BF_INDEX_CYTOPLASM);
   s.nNucleus = N(BF_INDEX_NUCLEUS);
   s.nMicrotubule = N(BF_INDEX_MICROTUBULE);
   s.absorptionPerUm = std::max(0.0, N(BF_ABSORPTION_PER_UM));
   s.zernike = sim::ZeroZernikeCoefficients();
   if (N(BF_ABERRATIONS) != 0 && CurrentPsfModel() == sim::PsfModelKind::GibsonLanniZernike)
   {
      bool ok = false;
      s.zernike = sim::ParseZernikeCoefficients(BuildPsfGeneratorRequest().zernikeCoefficients, ok);
   }
   return s;
}

void CInSiliScopeCamera::RenderBrightfieldStack(std::vector<std::vector<uint16_t>>& stack, long stackLength,
                                                unsigned w, unsigned h, const sim::SimulationParams& params,
                                                const sim::CellFieldSettings& cellField, double stageXUm,
                                                double stageYUm, const sim::PixelOffsetMap& offsetMap,
                                                const sim::PixelGainMap& gainMap,
                                                const sim::PixelReadNoiseMap& readNoiseMap, uint32_t noiseSeed)
{
   if (params.driftNmPerSecX > 0.0)
      LogMessage("BrightField: SimType_DriftNmPerSec is not applied in BrightField (yet).", false);
   auto t0 = std::chrono::steady_clock::now();
   std::string err;
   sim::CellFieldSource source;
   // As WideField: an armed z sequence gives frame f the position
   // seq[f % n]; otherwise the Z stage, read per batch of frames.
   const sim::SharedStageState::ZSequence zseq = sim::GetSharedStageState().GetZSequence();
   const bool useSeq = zseq.armed && !zseq.positions.empty();
   stackZSeqVersion_ = useSeq ? zseq.version : -1;
   auto focusOf = [&](double z) {
      return CellFieldQueryFor(stageXUm, stageYUm, z, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0).zCullCentreUm;
   };
   const sim::CellFieldQuery q =
      CellFieldQueryFor(stageXUm, stageYUm, 0.0, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
   const sim::BrightfieldSpec spec = BuildBrightfieldSpec(params, q, w, h);
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
   const double flux = std::max(0.0, brightFieldNum_[BF_PHOTONS_PER_PX_PER_SEC].load()) * params.frameDurationSec;
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
   for (long f0 = 0; f0 < stackLength; f0 += batch)
   {
      const long f1 = std::min(stackLength, f0 + batch);
      std::vector<const std::vector<float>*> src(static_cast<size_t>(f1 - f0));
      const double zNow = sim::GetSharedStageState().zPositionUm.load();
      for (long f = f0; f < f1; ++f)
      {
         const double z = useSeq ? zseq.positions[static_cast<size_t>(f) % zseq.positions.size()] : zNow;
         const std::vector<float>* img = imageAt(focusOf(z));
         src[static_cast<size_t>(f - f0)] = img ? img : &dark;
      }
      std::atomic<long> next{f0};
      auto worker = [&]() {
         for (long f; (f = next.fetch_add(1)) < f1;)
            sim::ApplyNoiseChain(*src[static_cast<size_t>(f - f0)], stack[static_cast<size_t>(f)], w, h, cam,
                                 offsetMap, gainMap, readNoiseMap, noiseSeed, static_cast<uint32_t>(f));
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

void CInSiliScopeCamera::SetGpuStatus(const std::string& s)
{
   std::lock_guard<std::mutex> lock(gpuStatusMutex_);
   gpuStatus_ = s;
}

bool CInSiliScopeCamera::PrepareGpu(std::unique_ptr<sim::GpuSimulator>& gpu, const sim::PsfKernelCache& cache,
                                 unsigned w, unsigned h, const sim::PixelOffsetMap& offsetMap,
                                 const sim::PixelGainMap& gainMap, const sim::PixelReadNoiseMap& readNoiseMap,
                                 const sim::StackShapingFields& shaping, const sim::SimulationParams& params)
{
   if (!useGpu_)
   {
      SetGpuStatus("CPU (General_UseGpu is Off)");
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
   sim::SimulationParams params = SnapshotParams();
   long seed = randomSeed_;
   long length = stackLength_;
   sim::PsfGeneratorRequest psfRequest = BuildPsfGeneratorRequest();
   // CellField: the stack is generated for ONE stage pose, snapshot here;
   // moving the XY stage afterwards does not change it (spec/PORT.md 8).
   sim::CellFieldSettings cellField = BuildCellFieldSettings();
   double stageX = 0.0, stageY = 0.0;
   sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), stageX, stageY);
   double stageZ = sim::GetSharedStageState().zPositionUm.load();

   stackGenThread_ = std::thread(&CInSiliScopeCamera::StackGenerationWorker, this, length, fullW, fullH, params, seed,
                                  psfRequest, cellField, stageX, stageY, stageZ, modality_.load());
}

void CInSiliScopeCamera::LitRect(double stageXUm, double stageYUm, double& x0, double& y0, double& x1,
                                 double& y1) const
{
   // The margin plus 0.5 um: the mean-field grid (world-anchored cells, a
   // cell beyond the margin) and the history's 0.25 um tiles lie wholly in
   // the lit rect, so a frame scales every column's weight alike (the
   // scene's fast path) instead of re-convolving every frame.
   const double um = pixelSizeNm_.load() / 1000.0, W = FullWidth() * um, H = FullHeight() * um;
   const double m = kCellFieldMarginUm + 0.5;
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
   const double um = pixelSizeNm_.load() / 1000.0;
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
   const sim::CellFieldSettings world = BuildCellFieldSettings();
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
                                             sim::PsfGeneratorRequest /*psfRequest*/, sim::CellFieldSettings cellField,
                                             double stageXUm, double stageYUm, double stageZUm, int modality)
{
   const bool bf = modality == 1;
   // The noise maps' stream (the CellField dyes come from the core, not from it).
   std::mt19937_64 localRng(static_cast<uint64_t>(seed));
   // Illumination: a fixed field for the whole stack (no rng).
   sim::StackShapingFields shaping = BuildShapingFields(fullW, fullH);
   sim::PixelOffsetMap localOffsetMap;
   localOffsetMap.Generate(fullW, fullH, params.offsetAdu, params.offsetStdAdu, localRng);
   sim::PixelGainMap localGainMap;
   localGainMap.Generate(fullW, fullH, params.gainPhotonsPerAdu, params.pixelGainStdFraction, localRng);
   sim::PixelReadNoiseMap localReadNoiseMap;
   localReadNoiseMap.Generate(fullW, fullH, params.readNoiseElectrons, params.pixelReadNoiseStdFraction, localRng);

   std::vector<std::vector<uint16_t>> newStack(static_cast<size_t>(std::max(stackLength, 0L)));
   const uint32_t noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   const sim::CameraNoiseParams cam = params.Camera();
   const double decaySec = bgDecaySec_.load();
   std::atomic<long> zClampedAll{0}, zRenderedAll{0};
   // An armed z sequence (hardware z stack): frame f is at position f mod n.
   const sim::SharedStageState::ZSequence stackSeq = sim::GetSharedStageState().GetZSequence();
   const bool stackUsesSeq = stackSeq.armed && !stackSeq.positions.empty();
   auto zOf = [&](long f) {
      // The ZStage read fresh per frame (as any live-adjustable parameter), or
      // the z sequence's position for this frame.
      return stackUsesSeq ? stackSeq.positions[static_cast<size_t>(f) % stackSeq.positions.size()]
                          : sim::GetSharedStageState().zPositionUm.load();
   };
   auto startTime = std::chrono::steady_clock::now();
   bool gpuOk = false;
   std::string where = "the CPU";
   if (bf)
      RenderBrightfieldStack(newStack, stackLength, fullW, fullH, params, cellField, stageXUm, stageYUm,
                             localOffsetMap, localGainMap, localReadNoiseMap, noiseSeed);
   else
   {
      stackZSeqVersion_ = stackUsesSeq ? stackSeq.version : -1;
      const std::string zWarn = CellFieldZRangeWarning();
      if (!zWarn.empty())
         LogMessage(zWarn, false);
      // Every structure's label in its mode through the light path: the
      // engine's fluorescence movie (Simulation/ScopeMovie.h), with this
      // camera's drift, illumination field, background fade and z sequence.
      // Each dye's clock is the illumination its place has had (a place
      // never lit starts at 0); the stack then adds its own frames there.
      const sim::ScopeSpec spec = BuildScopeSpec(stageXUm, stageYUm, stageZUm, 0.0, stackLength);
      SyncHistoryWorld();
      double lx0, ly0, lx1, ly1;
      LitRect(stageXUm, stageYUm, lx0, ly0, lx1, ly1);
      const sim::ClockSnapshot clock = illumHistory_.Snapshot(lx0, ly0, lx1, ly1);
      bool lit = false;
      sim::FluorescenceMovie fm;
      std::string err;
      std::unique_ptr<sim::WidefieldGpuD3D11> wfGpu;
      bool wfTried = false;
      // The mean-field scenes convolve on this thread's Direct3D 11 host.
      if (!fm.Begin(spec, false, err, useGpu_ ? WideFieldGpu(wfGpu, wfTried) : nullptr, &clock))
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
            gpuOk = plan.kernel && PrepareGpu(gpu, *plan.kernel, fullW, fullH, localOffsetMap, localGainMap,
                                              localReadNoiseMap, shaping, gp);
         }
         else if (useGpu_ && !fm.HasPopulations())
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
                  double dx = 0.0, dy = 0.0;
                  sim::ComputeDriftOffsetPx(f * params.frameDurationSec, params.driftNmPerSecX, params.driftAngleRad,
                                            params.pixelSizeNm, dx, dy);
                  sim::RenderExtras extras = shaping.Extras(f * params.frameDurationSec, decaySec);
                  sim::CollectGpuEmitters(evs, f, fullW, fullH, params.pixelSizeNm, gp.photonsPerBlink, dx, dy,
                                          *plan.kernel, zOf(f), &extras, ems[static_cast<size_t>(f - f0)], &zc, &zt);
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
            opt.zStageUm = zOf;
            opt.driftPx = [&](long f, double& dx, double& dy) {
               sim::ComputeDriftOffsetPx(f * params.frameDurationSec, params.driftNmPerSecX, params.driftAngleRad,
                                         params.pixelSizeNm, dx, dy);
            };
            if (!shaping.illum.empty())
               opt.illumField = &shaping.illum;
            if (decaySec > 0)
               opt.backgroundScale = [&](long f) { return sim::BackgroundFadeScale(f * params.frameDurationSec, decaySec); };
            opt.onPhotons = [&](long f, const std::vector<float>& photons) {
               sim::ApplyNoiseChain(photons, newStack[static_cast<size_t>(f)], fullW, fullH, cam, localOffsetMap,
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
   uint64_t liveSeed = static_cast<uint64_t>(randomSeed_) ^ 0xABCDEF1234567890ULL;
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
   sim::BrightfieldScene bfScene;
   std::vector<float> bfImage;
   // Publishes a finished frame (front buffer, sequence counter, interval
   // statistics).
   auto publish = [this](std::vector<uint16_t>& frame, unsigned fw, unsigned fh, long epoch, long frameIndex,
                         long config, LitFrame& lit) {
      {
         MMThreadGuard g(frontFrameLock_);
         frontFrame_.swap(frame);
         std::swap(liveFrameLit_, lit);
         liveFrameW_ = fw;
         liveFrameH_ = fh;
         liveFrameEpoch_ = epoch;
         liveFrameConfig_ = config;
      }
      liveFrameSeq_.fetch_add(1, std::memory_order_relaxed);
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

   while (liveProducerRun_.load())
   {
      MM::MMTime tickStart = GetCurrentMMTime();
      sim::SimulationParams params = SnapshotParams();
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
         shaping = BuildShapingFields(w, h);
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
         bfActive = BrightFieldSelected();
         if (bfActive)
         {
            std::string err;
            cellFieldOk = cellField.Configure(BuildCellFieldSettings(), err);
            if (!cellFieldOk)
               LogMessage("CellField unavailable: " + err, false);
            bfErrLogged = false;
            if (params.driftNmPerSecX > 0.0)
               LogMessage("BrightField: SimType_DriftNmPerSec is not applied in BrightField (yet).", false);
         }
         else
         {
            const std::string zWarn = CellFieldZRangeWarning();
            if (!zWarn.empty())
               LogMessage(zWarn, false);
         }
         appliedConfigVersion = currentConfigVersion;
      }

      // Drift ramps up from zero at liveDriftOriginFrame_ (reset at
      // StartLiveProducer() and at the start of every Live/MDA sequence
      // acquisition).
      long framesSinceDriftOrigin = std::max(0L, liveFrameCounter_.load(std::memory_order_relaxed) -
                                                      liveDriftOriginFrame_.load(std::memory_order_relaxed));
      double dx = 0.0, dy = 0.0;
      sim::ComputeDriftOffsetPx(framesSinceDriftOrigin * params.frameDurationSec, params.driftNmPerSecX,
                                 params.driftAngleRad, params.pixelSizeNm, dx, dy);
      // InSiliScopeZStage's current position, read fresh every tick -- or,
      // during a sequence acquisition with an armed z sequence, the
      // sequence's next position (one per frame).
      long frameEpoch = 0;
      double zOffsetUm = sim::GetSharedStageState().NextFrameZ(&frameEpoch);
      double sx = 0.0, sy = 0.0;
      sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), sx, sy);
      // The background fade restarts with the drift ramp.
      sim::RenderExtras extras =
         shaping.Extras(framesSinceDriftOrigin * params.frameDurationSec, bgDecaySec_.load());
      // Counter-based noise: keyed by the live frame counter, on a seed of
      // its own (live mode was never meant to reproduce precomputed frames).
      const uint32_t liveNoiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(randomSeed_) ^ 0x4C4E4F49ULL);
      const uint32_t noiseFrame = static_cast<uint32_t>(liveFrameCounter_.load(std::memory_order_relaxed));
      std::vector<uint16_t> nextFrame;
      sim::ScopeSpec spec;
      // The lit rect of this frame and its illumination clocks.
      double lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
      sim::ClockSnapshot clock;
      LitFrame lit;
      if (bfActive)
      {
         // A new pose or setting rebuilds the scene (seconds at high quality);
         // a focus change re-images it (one inverse FFT per source). The lamp:
         // photons per pixel of the empty field times the transmitted
         // intensity (dark if the scene failed); camera noise.
         if (cellFieldOk)
         {
            const sim::CellFieldQuery q = CellFieldQueryFor(sx, sy, zOffsetUm, w, h, params, dx, dy, dx, dy,
                                                            liveFrameCounter_, cellFieldTimeSec, params.frameDurationSec);
            std::string err;
            const sim::BrightfieldSpec bs = BuildBrightfieldSpec(params, q, w, h);
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
         const double flux = std::max(0.0, brightFieldNum_[BF_PHOTONS_PER_PX_PER_SEC].load()) * params.frameDurationSec;
         photonImg.assign(static_cast<size_t>(w) * h, 0.0f);
         if (bfImage.size() == photonImg.size())
            for (size_t i = 0; i < photonImg.size(); ++i)
               photonImg[i] = static_cast<float>(bfImage[i] * flux);
         sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                              liveNoiseSeed, noiseFrame, true);
      }
      else
      {
         // Fluorescence: this frame of the engine's movie at the current pose,
         // focus and time (one stage pose per frame; motion blur is ignored).
         spec = BuildScopeSpec(sx, sy, zOffsetUm, 0.0, 1);
         SyncHistoryWorld();
         LitRect(sx, sy, lx0, ly0, lx1, ly1);
         clock = illumHistory_.Snapshot(lx0, ly0, lx1, ly1);
         sim::FluorescenceMovie fm;
         std::string err;
         bool rendered = false;
         if (!fm.Begin(spec, false, err, useGpu_ ? WideFieldGpu(wfGpu, wfGpuTried) : nullptr, &clock))
         {
            if (!flErrLogged)
               LogMessage("Fluorescence: " + err, false);
            flErrLogged = true;
         }
         else
         {
            const sim::FluorescenceSimplePlan plan = fm.SimplePlan();
            if (plan.ok && plan.kernel && !gpuFailed && useGpu_)
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
                  sim::CollectGpuEmitters(*plan.events, 0, w, h, params.pixelSizeNm, gp.photonsPerBlink, dx, dy,
                                          *plan.kernel, zOffsetUm, &extras, ems, &zClampedSinceRebuild,
                                          &zTotalSinceRebuild);
                  rendered = gpu->RenderFrame(ems, params.Camera(), extras.backgroundScale, liveNoiseSeed, noiseFrame,
                                              nextFrame, err);
                  if (!rendered)
                  {
                     LogMessage("GPU frame render failed, continuing on the CPU: " + err, false);
                     SetGpuStatus("CPU (GPU render failed: " + err + ")");
                     gpuFailed = true;
                  }
               }
            }
            else if (!plan.ok && useGpu_ && !gpuFailed && !fm.HasPopulations())
               SetGpuStatus("CPU (the GPU splat is for one blink group without continuous populations)");
            if (!rendered)
            {
               sim::FluorescenceFrameOptions opt;
               opt.driftPx = [&](long, double& ddx, double& ddy) { ddx = dx; ddy = dy; };
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
         if (!rendered || nextFrame.size() != static_cast<size_t>(w) * h)
         {
            if (!rendered || photonImg.size() != static_cast<size_t>(w) * h)
               photonImg.assign(static_cast<size_t>(w) * h, 0.0f);
            sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                                 liveNoiseSeed, noiseFrame, true);
         }
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
      publish(nextFrame, w, h, frameEpoch, liveFrameCounter_.load(std::memory_order_relaxed), currentConfigVersion, lit);
      ++liveFrameCounter_;

      double exposureMs = GetExposure();
      MM::MMTime elapsed = GetCurrentMMTime() - tickStart;
      double sleepMs = exposureMs - elapsed.getMsec();
      // Spend the wait pre-loading the dyes a stage move would need next (the
      // whole z column and an xy margin around the FOV), so focusing and
      // nearby moves do not stall a frame on generating them.
      if (sleepMs > kCellFieldPrefetchSlackMs)
      {
         if (bfActive && cellFieldOk && bfQueried)
         {
            sim::CellFieldQuery next = bfLastQuery;
            next.tSec = cellFieldTimeSec;
            next.spanSec = params.frameDurationSec;
            cellField.Prefetch(next, kCellFieldPrefetchMarginUm, sleepMs - kCellFieldPrefetchSlackMs);
         }
         else if (!bfActive && !spec.empty())
         {
            spec["start-sec"] = clock.At(sx, sy);
            sim::PrefetchScope(spec, kCellFieldPrefetchMarginUm, sleepMs - kCellFieldPrefetchSlackMs);
         }
         sleepMs = exposureMs - (GetCurrentMMTime() - tickStart).getMsec();
      }
      if (sleepMs > 0.0)
         CDeviceUtils::SleepMs(static_cast<unsigned long>(sleepMs));
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
      // flight when a property changed would show the old settings.
      const long configNow = liveConfigVersion_.load(std::memory_order_relaxed);
      for (;;)
      {
         {
            MMThreadGuard g(frontFrameLock_);
            seq = liveFrameSeq_.load(std::memory_order_relaxed);
            // A z-sequence acquisition takes only frames rendered after it
            // started (their focus is the sequence's).
            const bool stale = (interruptible && liveSeqSkipStale_.load() && liveFrameEpoch_ < liveSeqEpoch_.load()) ||
                               liveFrameConfig_ < configNow;
            if (stale && seq != lastConsumedLiveFrameSeq_)
               lastConsumedLiveFrameSeq_ = seq;
            else if (seq != lastConsumedLiveFrameSeq_)
            {
               frameCopy = frontFrame_;
               w = liveFrameW_;
               h = liveFrameH_;
               lit = liveFrameLit_;
               break;
            }
         }
         if (!liveProducerRun_.load())
            return false;
         if (interruptible && thd_ && thd_->IsStopped())
            return false;
         CDeviceUtils::SleepMs(1);
      }
      lastConsumedLiveFrameSeq_ = seq;
      CropFullFrameIntoImg(frameCopy, w, h);
      // The frame was taken: its light goes into the illumination history.
      if (lit.valid)
         illumHistory_.Advance(lit.x0, lit.y0, lit.x1, lit.y1, lit.dtSec, lit.weight);
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

int CInSiliScopeCamera::OnAcqMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(acqMode_ == SMLM_MODE_LIVE ? g_AcqModeLive : g_AcqModePrecomputed);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      int newMode = (s == g_AcqModeLive) ? SMLM_MODE_LIVE : SMLM_MODE_PRECOMPUTED;
      if (newMode != acqMode_)
      {
         if (newMode == SMLM_MODE_LIVE)
         {
            acqMode_ = newMode;
            if (initialized_)
               StartLiveProducer();
         }
         else
         {
            StopLiveProducer();
            acqMode_ = newMode;
         }
      }
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* s = (cameraCCDXSize_ == 128) ? g_Fov128 : (cameraCCDXSize_ == 256) ? g_Fov256 : g_Fov512;
      pProp->Set(s);
   }
   else if (eAct == MM::AfterSet)
   {
      if (IsCapturing())
         return DEVICE_CAMERA_BUSY_ACQUIRING;

      std::string s;
      pProp->Get(s);
      if (s == g_Fov128) { cameraCCDXSize_ = 128; cameraCCDYSize_ = 128; }
      else if (s == g_Fov256) { cameraCCDXSize_ = 256; cameraCCDYSize_ = 256; }
      else { cameraCCDXSize_ = 512; cameraCCDYSize_ = 512; }
      // Live mode's producer thread naturally re-sizes its own buffers each
      // tick (ApplyNoiseChain resizes its output to the current w*h); no
      // buffer touch needed here beyond ROI/img_ + the config-version bump.
      ApplyFrameSizeChange();
   }
   return DEVICE_OK;
}

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

int CInSiliScopeCamera::OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(endOfStackReached_ ? "Yes" : "No");
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfNa(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfNa_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfNa_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPixelSizeNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(pixelSizeNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); pixelSizeNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBackgroundPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(backgroundPhotonsPerSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); backgroundPhotonsPerSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnQuantumEfficiency(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(quantumEfficiency_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); quantumEfficiency_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnDarkCurrentPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(darkCurrentPerSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); darkCurrentPerSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCameraGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(gainPhotonsPerAdu_.load());
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      gainPhotonsPerAdu_ = v;
      OnPropertyChanged(g_PropEmGain, std::to_string(EmGain()).c_str());   // derived from the gain
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCameraOffset(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(offsetAdu_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); offsetAdu_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnOffsetStd(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(offsetStdAdu_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); offsetStdAdu_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnReadNoise(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(readNoiseElectrons_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); readNoiseElectrons_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPixelGainStdPct(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(pixelGainStdPct_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); pixelGainStdPct_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPixelReadNoiseStdPct(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(pixelReadNoiseStdPct_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); pixelReadNoiseStdPct_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnDriftNmPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(driftNmPerSecX_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); driftNmPerSecX_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnRandomSeed(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(randomSeed_);
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      randomSeed_ = v;
      rng_.seed(static_cast<uint64_t>(randomSeed_));
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnActualFrameIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(actualFrameIntervalMs_.load(std::memory_order_relaxed));
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfModel(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_PsfModelGaussian, g_PsfModelRichardsWolf, g_PsfModelGibsonLanni,
                              g_PsfModelGibsonLanniZernike};
      pProp->Set(names[psfModel_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_PsfModelGaussian) psfModel_ = static_cast<int>(sim::PsfModelKind::Gaussian);
      else if (s == g_PsfModelRichardsWolf) psfModel_ = static_cast<int>(sim::PsfModelKind::RichardsWolf);
      else if (s == g_PsfModelGibsonLanni) psfModel_ = static_cast<int>(sim::PsfModelKind::GibsonLanni);
      else if (s == g_PsfModelGibsonLanniZernike) psfModel_ = static_cast<int>(sim::PsfModelKind::GibsonLanniZernike);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfImmersionIndex(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfImmersionIndex_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfImmersionIndex_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfOversampling(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(psfOversampling_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v < 1)
         v = 1;
      psfOversampling_ = static_cast<int>(v);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfKernelHalfWidthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfKernelHalfWidthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfKernelHalfWidthNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfGeneratorJavaHome(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(psfGeneratorJavaHome_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      pProp->Get(psfGeneratorJavaHome_);
      // Only takes effect before the first diffraction-PSF computation: the
      // embedded JVM is created once per process (JNI only supports one
      // JVM per process, see sim::EnsureJvmCreated) and reused for the
      // rest of this device adapter's lifetime, so changing this after
      // that first use has no effect until Micro-Manager/the process is
      // restarted. InvalidateStack() is still called for consistency with
      // every other PSF-affecting property, even though it's a no-op here
      // post-JVM-creation.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfZRangeUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfZRangeUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfZRangeUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfZStepUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfZStepUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfZStepUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfSampleIndex(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfSampleIndex_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfSampleIndex_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfWorkingDistanceUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfWorkingDistanceUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfWorkingDistanceUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfSampleDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfSampleDepthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfSampleDepthNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfZernikeCoefficients(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(psfZernikeCoefficients_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      bool ok = false;
      sim::ParseZernikeCoefficients(s, ok);
      // Same "reject a malformed/wrong-length list rather than silently
      // reinterpreting it" stance as sim::ParseZernikeCoefficients itself:
      // only accept the new text if it parses to exactly 15 or 28 numbers, else
      // leave the previous (valid) value in place and report the resolved
      // value back to the property browser via pProp->Set.
      if (ok)
         psfZernikeCoefficients_ = sim::FormatZernikeCoefficients(sim::ParseZernikeCoefficients(s, ok));
      pProp->Set(psfZernikeCoefficients_.c_str());
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfZernikePreset(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(psfZernikePreset_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      std::string name;
      pProp->Get(name);
      psfZernikePreset_ = name;
      psfZernikeCoefficients_ = sim::FormatZernikeCoefficients(sim::ZernikePresetCoefficients(name));
      // Keep the PsfZernikeCoefficients property (a separate, independently
      // user-editable property) in sync in the GUI/property-cache too --
      // AfterSet here only updates our own member, not that other
      // property's displayed value.
      OnPropertyChanged(g_PropPsfZernikeCoefficients, psfZernikeCoefficients_.c_str());
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfInterp(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_PsfInterpNearest, g_PsfInterpLinear, g_PsfInterpCubic, g_PsfInterpFft};
      pProp->Set(names[psfInterp_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_PsfInterpLinear) psfInterp_ = static_cast<int>(sim::PsfInterpMode::Linear);
      else if (s == g_PsfInterpCubic) psfInterp_ = static_cast<int>(sim::PsfInterpMode::Cubic);
      else if (s == g_PsfInterpFft) psfInterp_ = static_cast<int>(sim::PsfInterpMode::Fft);
      else psfInterp_ = static_cast<int>(sim::PsfInterpMode::Nearest);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnIllumFwhmPct(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(illumFwhmPct_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); illumFwhmPct_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnEmGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   // Read-only: the camera preset's pre-amplifier sensitivity / the gain (EmGain()).
   if (eAct == MM::BeforeGet) pProp->Set(EmGain());
   return DEVICE_OK;
}

double CInSiliScopeCamera::EmGain() const
{
   const double preamp = cameraPreset_ >= 0 && cameraPreset_ < static_cast<int>(sim::CameraIds().size())
      ? sim::CameraPreamp(sim::CameraAt(cameraPreset_)) : sim::kDefaultPreampElectronsPerAdu;
   return sim::EmGainFromGain(preamp, gainPhotonsPerAdu_.load());
}

int CInSiliScopeCamera::OnCicElectrons(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(cicElectrons_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); cicElectrons_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBgDecaySec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(bgDecaySec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); bgDecaySec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnIllumProfile(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_IllumFlat, g_IllumGaussian, g_IllumFlatTop};
      pProp->Set(names[illumProfile_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_IllumGaussian) illumProfile_ = static_cast<int>(sim::IllumProfile::Gaussian);
      else if (s == g_IllumFlatTop) illumProfile_ = static_cast<int>(sim::IllumProfile::FlatTop);
      else illumProfile_ = static_cast<int>(sim::IllumProfile::Flat);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCameraType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(cameraEmccd_ ? g_CameraTypeEmccd : g_CameraTypeScmos);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      cameraEmccd_ = (s == g_CameraTypeEmccd);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBitDepth(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(bitDepth_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      bitDepth_ = static_cast<int>(std::min(16L, std::max(8L, v)));
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnUseGpu(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(useGpu_ ? g_UseGpuOn : g_UseGpuOff);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      useGpu_ = (s == g_UseGpuOn);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnGpuStatus(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      std::lock_guard<std::mutex> lock(gpuStatusMutex_);
      pProp->Set(gpuStatus_.c_str());
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfMaskType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(psfMaskType_ == static_cast<int>(sim::PsfMaskType::DoubleHelix) ? g_PsfMaskDoubleHelix
                                                                                  : g_PsfMaskNone);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      psfMaskType_ = (s == g_PsfMaskDoubleHelix) ? static_cast<int>(sim::PsfMaskType::DoubleHelix)
                                                   : static_cast<int>(sim::PsfMaskType::None);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfMaskModes(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(psfMaskModes_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      psfMaskModes_ = static_cast<int>(std::min(8L, std::max(2L, v)));
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfMaskWaist(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfMaskWaist_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfMaskWaist_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnExposureProperty(MM::PropertyBase* /*pProp*/, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      // The dyes' rates and BackgroundPhotonsPerSec are all rates converted to frame-equivalent
      // values using this property's current value (SnapshotParams()) -- a
      // changed Exposure means the precomputed stack no longer reflects the
      // current settings and must regenerate. Live mode needs no equivalent
      // rebuild: LiveProducerLoop calls SnapshotParams() fresh every tick
      // regardless, and none of its version-gated cached state (offset map,
      // emitter pattern, PSF kernel) depends on exposure time -- so this
      // deliberately uses InvalidateStackOnly() rather than InvalidateStack(),
      // to avoid forcing a multi-second PSF-kernel recompute (diffraction
      // models) on every Live-mode exposure change.
      InvalidateStackOnly();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCellFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   if (index < 0 || index >= CF_COUNT)
      return DEVICE_INVALID_PROPERTY;
   if (eAct == MM::BeforeGet) pProp->Set(cellField_[index].load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); cellField_[index] = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCellFieldPacking(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(cellFieldPacking_ ? "On" : "Off");
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      cellFieldPacking_ = (s == "On");
      InvalidateStack();
   }
   return DEVICE_OK;
}

void CInSiliScopeCamera::StartPsfPreload()
{
   if (psfPreloadThread_.joinable())
      psfPreloadThread_.join();
   if (CurrentPsfModel() == sim::PsfModelKind::Gaussian)
      return;
   // The kernels of the current settings' dye states (the engine's prepare
   // movie: world, FOV blocks and kernels, no frames) on a background thread.
   double sx = 0.0, sy = 0.0;
   sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), sx, sy);
   sim::ScopeSpec spec = BuildScopeSpec(sx, sy, sim::GetSharedStageState().zPositionUm.load(), 60.0, 1);
   spec["prepare"] = 1;
   psfPreloadThread_ = std::thread([this, spec]() {
      LogMessage("PSF: preloading the kernels of the current settings in the background.");
      sim::ScopeMovieInfo info;
      std::string err;
      if (!sim::RenderScopeMovie(spec, [](long, const std::vector<uint16_t>&) { return true; }, info, err))
         LogMessage("PSF preload failed (the first frame computes the kernel instead): " + err, false);
   });
}

int CInSiliScopeCamera::OnDiskCache(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const int m = diskCacheMode_.load();
      pProp->Set(m <= 0 ? g_DiskCacheOff : m == 1 ? g_DiskCacheCells : g_DiskCacheCellsAndPsf);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      diskCacheMode_ = s == g_DiskCacheOff ? 0 : s == g_DiskCacheCellsAndPsf ? 2 : 1;
      sim::SetPsfKernelDiskCacheDir(diskCacheMode_.load() >= 2 ? sim::DefaultCacheDir() : std::string());
      // The sources pick the directory up with their next settings (a
      // cache only: no output changes, so no stack invalidation).
      liveConfigVersion_.fetch_add(1, std::memory_order_relaxed);
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnImagingModality(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(modality_.load() == 1 ? g_ModalityBrightField : g_ModalityFluorescence);
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      const int m = s == g_ModalityBrightField ? 1 : 0;
      const bool changed = modality_.exchange(m) != m;
      if (changed)
         ApplyModeGain();   // a camera whose gain depends on the imaging (an EMCCD preset)
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnWideFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   if (index < 0 || index >= WF_COUNT)
      return DEVICE_INVALID_PROPERTY;
   if (eAct == MM::BeforeGet)
   {
      if (index == WF_UPSCALING)
         pProp->Set(static_cast<long>(wideFieldNum_[index].load()));
      else
         pProp->Set(wideFieldNum_[index].load());
   }
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      wideFieldNum_[index] = v;
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBrightFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   if (index < 0 || index >= BF_COUNT)
      return DEVICE_INVALID_PROPERTY;
   const bool integer = index == BF_QUALITY || index == BF_SOURCES || index == BF_UPSCALING ||
                        index == BF_GEOMETRY_SAMPLES || index == BF_ABERRATIONS;
   if (eAct == MM::BeforeGet)
   {
      if (integer)
         pProp->Set(static_cast<long>(brightFieldNum_[index].load()));
      else
         pProp->Set(brightFieldNum_[index].load());
   }
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      brightFieldNum_[index] = v;
      InvalidateStack();
   }
   return DEVICE_OK;
}

