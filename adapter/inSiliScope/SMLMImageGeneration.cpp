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
// LICENSE:       BSD (see license.txt)

#include "InSiliScopeCamera.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>

///////////////////////////////////////////////////////////////////////////////
// Parameter snapshot / invalidation helpers
///////////////////////////////////////////////////////////////////////////////

sim::SimulationParams CInSiliScopeCamera::SnapshotParams() const
{
   // EmitterDensityPerSec/PhotonsPerSecond/OnLifetimeSec/BackgroundPhotonsPerSec
   // are all expressed as rates (per second) at the property level, so they
   // scale correctly with whatever the standard MM Exposure is currently set
   // to -- convert them here to the frame-equivalent quantities the (unit-
   // agnostic, exposure-unaware) simulation engine expects.
   double exposureMs = GetExposure();
   double expSec = exposureMs / 1000.0;
   if (expSec <= 0.0)
      expSec = 0.001;

   sim::SimulationParams p;
   p.pixelSizeNm = pixelSizeNm_.load();
   p.photonsPerBlink = photonsPerSecond_.load() * expSec;
   // Clamp: an extreme (very short exposure)/(very long ON lifetime)
   // combination would otherwise blow up the lead-in window and event count
   // in GenerateAllEvents() (see SMLMSimulation.cpp), making stack
   // generation pathologically slow.
   p.onLifetimeFrames = std::min(onLifetimeSec_.load() / expSec, 20000.0);
   // General_EmitterDensityPerSec is the rate of blinks switching ON, per um^2
   // per second -- what a localization count measures -- whatever the
   // exposure, ON lifetime or bleaching. The engine's emitterDensity is the
   // steady-state density of ON emitters (arrivals per frame =
   // emitterDensity * area / onLifetimeFrames), i.e. rate x mean ON time.
   // (It used to be rate x exposure, which only equals that when the
   // exposure equals the ON lifetime: 20 ms frames with a 0.2 s ON time gave
   // a tenth of the set rate, and changing Exposure changed the density.)
   p.emitterDensity = emitterDensityPerSec_.load() * expSec * std::max(p.onLifetimeFrames, 0.01);
   p.backgroundPhotons = backgroundPhotonsPerSec_.load() * expSec;
   p.psfSigmaPx = ComputePsfSigmaPx();
   p.quantumEfficiency = quantumEfficiency_.load();
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
   p.blinkBleachProb = blinkBleachProb_.load();
   // Same clamp rationale as onLifetimeFrames above (lead-in window size).
   p.offLifetimeFrames = std::min(offLifetimeSec_.load() / expSec, 20000.0);
   p.photonCV = photonCV_.load();
   p.emccd = cameraEmccd_;
   p.emGain = emGain_.load();
   p.cicElectrons = cicElectrons_.load();
   p.bitDepth = bitDepth_;
   return p;
}

sim::PsfGeneratorRequest CInSiliScopeCamera::BuildPsfGeneratorRequest() const
{
   sim::PsfGeneratorRequest req;
   req.model = CurrentPsfModel();
   req.wavelengthNm = psfWavelengthNm_.load();
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
   // physics-derived approach ComputePsfSigmaPx() already uses for the
   // Gaussian renderer. Capped at 48 px regardless of physics to keep the
   // oversampled grid PSFGenerator computes from growing unboundedly.
   int requestedHalfWidthPx =
      static_cast<int>(std::lround(psfKernelHalfWidthNm_.load() / req.pixelSizeNm));
   requestedHalfWidthPx = std::max(requestedHalfWidthPx, 1);
   double na = req.na > 0.0 ? req.na : 0.01;
   double rayleighRadiusNm = 0.61 * req.wavelengthNm / na;
   int minHalfWidthPx = static_cast<int>(std::ceil(3.0 * rayleighRadiusNm / req.pixelSizeNm));
   minHalfWidthPx = std::min(std::max(minHalfWidthPx, 2), 48);
   req.kernelHalfWidthPx = std::max(requestedHalfWidthPx, minHalfWidthPx);

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

double CInSiliScopeCamera::ComputePsfSigmaPx() const
{
   // Gaussian approximation of a diffraction-limited widefield PSF (Zhang et
   // al. 2007): sigma ~= 0.21 * emission_wavelength / NA.
   double wavelengthNm = psfWavelengthNm_.load();
   double na = psfNa_.load();
   if (na <= 0.0)
      na = 0.01;
   double sigmaNm = 0.21 * wavelengthNm / na;
   double sigmaPx = sigmaNm / pixelSizeNm_.load();
   // Keep it in a sane rendering range regardless of extreme wavelength/NA/
   // pixel-size combinations.
   return std::min(std::max(sigmaPx, 0.3), 20.0);
}

sim::StructureParams CInSiliScopeCamera::BuildStructureParams() const
{
   sim::StructureParams sp;
   sp.zRangeNm = structureZRangeNm_.load();
   sp.structureSizeNm = structureSizeNm_.load();
   sp.labelingEfficiencyPct = labelingEfficiencyPct_.load();
   sp.nupRadiusNm = nupRadiusNm_.load();
   sp.nupCornerSpreadNm = nupCornerSpreadNm_.load();
   sp.nupRingSeparationNm = nupRingSeparationNm_.load();
   // NupLinkerMinNm/NupLinkerMaxNm have independent property limits (both
   // 0-30nm) so a user can set min > max; clamp here rather than in the
   // property handlers themselves, matching this codebase's existing
   // "clamp at the point of use, not the point of entry" convention (see
   // e.g. ComputePsfSigmaPx's own min/max clamp).
   double linkerMin = nupLinkerMinNm_.load();
   double linkerMax = nupLinkerMaxNm_.load();
   sp.nupLinkerMinNm = std::min(linkerMin, linkerMax);
   sp.nupLinkerMaxNm = std::max(linkerMin, linkerMax);
   sp.nupMembrane = static_cast<sim::MembraneOrientation>(nupMembrane_);
   sp.nupCount = nupCount_;
   sp.nupMinSpacingNm = nupMinSpacingNm_.load();
   sp.nupCurvatureNm = nupCurvatureNm_.load();
   return sp;
}

sim::StackShapingFields CInSiliScopeCamera::BuildShapingFields(const sim::EmitterModel& model, unsigned w, unsigned h,
                                                           const sim::SimulationParams& params, long seed) const
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

   double contrast = bgCellContrast_.load(), hazeWeight = bgHazeWeight_.load();
   if (params.backgroundPhotons > 0.0 && (contrast > 1.0 || hazeWeight > 0.0))
   {
      // Own streams (webSMLM's background rng is likewise derived from the
      // seed, separate from the emitter stream): one for the haze site
      // sample, one for the cell shape.
      std::vector<std::pair<double, double>> sitesPx;
      if (hazeWeight > 0.0 && CurrentPatternType() == sim::PATTERN_CELL_FIELD)
         LogMessage("Background_HazeWeight: no haze sites for the CellField pattern yet (flat haze-free "
                    "background map).", false);
      else if (hazeWeight > 0.0)
      {
         std::mt19937_64 siteRng(static_cast<uint64_t>(seed) ^ 0x48415A4553495445ULL); // "HAZESITE"
         double widthUm = w * params.pixelSizeNm / 1000.0, heightUm = h * params.pixelSizeNm / 1000.0;
         for (const sim::EmitterSite& s : model.SampleSitesForHaze(widthUm, heightUm, 20000, siteRng))
            sitesPx.emplace_back(s.xUm * 1000.0 / params.pixelSizeNm, s.yUm * 1000.0 / params.pixelSizeNm);
      }
      std::mt19937_64 bgRng(static_cast<uint64_t>(seed) ^ 0x4247524E44434C4CULL); // "BGRNDCLL"
      out.background = sim::BuildBackgroundMap(w, h, params.backgroundPhotons, contrast, hazeWeight,
                                               bgHazeWidthNm_.load() / params.pixelSizeNm, sitesPx, bgRng);
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
      {"labelEfficiency", cellField_[CF_LABELING_PCT_BLEACHING].load() / 100.0},
      {"labelNonBleaching", cellField_[CF_LABELING_PCT_NONBLEACHING].load() / 100.0},
      {"enablePacking", cellFieldPacking_ ? 1.0 : 0.0},
   };
   s.activationRatePerSec = cellField_[CF_MILLI_ACTIVATION_RATE].load() / 1000.0; // property in 1e-3/s
   s.onSec = std::max(1e-6, onLifetimeSec_.load());
   s.offSec = std::max(0.0, offLifetimeSec_.load());
   s.bleachProb = blinkBleachProb_.load();
   s.photonCV = photonCV_.load();
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

std::function<double(std::mt19937_64&)> CInSiliScopeCamera::OutOfFocusDepthSampler(const sim::PsfKernelCache& cache) const
{
   // Port of webSMLM's out-of-focus depth draw: |z| uniform in [zLo, zHi]
   // with a random sign, zLo = 300 nm (webSMLM uses max(its astigmatic
   // usable range, 300 nm); this project doesn't compute that range) and
   // zHi = OutOfFocusDepthNm, both clamped to the kernel's own half range.
   if (!cache.valid || cache.nz < 3)
      return {};
   double halfKernel = cache.zStepNm * (cache.nz - 1) / 2.0;
   double zLo = std::min(300.0, halfKernel);
   double zHi = std::min(std::max(outOfFocusDepthNm_.load(), zLo), halfKernel);
   return [zLo, zHi](std::mt19937_64& rng) {
      std::uniform_real_distribution<double> unif01(0.0, 1.0);
      double sign = unif01(rng) < 0.5 ? -1.0 : 1.0;
      return sign * (zLo + unif01(rng) * (zHi - zLo));
   };
}

namespace {
const char* const kWideFieldGpuStatus = "CPU (WideField: FFT convolution on the CPU; GPU path planned)";
} // namespace

sim::WidefieldSceneSpec CInSiliScopeCamera::BuildWidefieldSceneSpec(const sim::SimulationParams& params,
                                                                  const sim::CellFieldQuery& q) const
{
   sim::WidefieldSceneSpec s;
   const unsigned w = FullWidth(), h = FullHeight();
   s.originXUm = q.originXUm;
   s.originYUm = q.originYUm;
   s.width = w;
   s.height = h;
   s.pixelUm = params.pixelSizeNm / 1000.0;
   // The focal plane is the world height focus + ZStage (CellFieldQueryFor);
   // the dye slab is SimType_CellFieldZRangeUm around it (0 = every dye).
   s.focusWorldUm = q.zCullCentreUm;
   s.slabCentreUm = q.zCullCentreUm;
   s.slabHalfUm = q.zHalfRangeUm;
   s.grid.upscale = static_cast<int>(std::lround(std::min(4.0, std::max(1.0, wideFieldNum_[WF_UPSCALING].load()))));
   s.grid.zPlaneNm = std::min(500.0, std::max(5.0, wideFieldNum_[WF_Z_PLANE_NM].load()));
   s.marginUm = kCellFieldMarginUm;
   s.kernelCapUm = std::max(0.1, psfKernelHalfWidthNm_.load() / 1000.0);
   s.phot.excitationPhotonsPerUm2PerSec = std::max(0.0, wideFieldNum_[WF_EXCITATION].load());
   s.phot.quantumYield = std::min(1.0, std::max(0.0, wideFieldNum_[WF_QUANTUM_YIELD].load()));
   s.phot.photonBudget = std::max(0.0, wideFieldNum_[WF_PHOTON_BUDGET].load());
   s.phot.extinctionCoeff = std::max(0.0, wideFieldNum_[WF_EXTINCTION_COEFF].load());
   s.eta = sim::WidefieldCollectionEfficiency(psfNa_.load(), psfImmersionIndex_.load());
   s.exposureSec = params.frameDurationSec;
   return s;
}

std::unique_ptr<sim::WidefieldPsf> CInSiliScopeCamera::MakeWidefieldPsf(const sim::PsfKernelCache& cache,
                                                                        sim::WidefieldSceneSpec& spec) const
{
   if (cache.valid)
   {
      const int u = sim::VectorialWidefieldPsf::ValidUpscale(cache.oversampling, spec.grid.upscale);
      if (u != spec.grid.upscale)
      {
         std::ostringstream m;
         m << "WideField: General_WideFieldUpscaling " << spec.grid.upscale << " does not divide "
           << "PSFParam_PsfOversampling " << cache.oversampling << "; using " << u << ".";
         LogMessage(m.str(), false);
         spec.grid.upscale = u;
      }
      return std::unique_ptr<sim::WidefieldPsf>(new sim::VectorialWidefieldPsf(cache, u));
   }
   return std::unique_ptr<sim::WidefieldPsf>(new sim::GaussianWidefieldPsf(
      spec.pixelUm / std::max(1, spec.grid.upscale), psfWavelengthNm_.load(), psfNa_.load(), psfImmersionIndex_.load()));
}

void CInSiliScopeCamera::LogWidefieldPhotophysics(const sim::WidefieldSceneSpec& spec)
{
   const double kem = spec.phot.EmissionRatePerSec(1.0);
   std::ostringstream m;
   m << "WideField: sigma " << spec.phot.CrossSectionUm2() << " um^2, k_em " << kem << " photons/s at peak, t1/2 "
     << (spec.phot.Bleaches() ? std::to_string(spec.phot.HalfTimeSec(1.0)) + " s" : std::string("never (budget 0)"))
     << ", collection efficiency " << spec.eta << ", " << spec.eta * kem * spec.exposureSec
     << " photons/dye/frame reach the camera (before QE); non-bleaching dyes never bleach.";
   LogMessage(m.str());
}

void CInSiliScopeCamera::RenderWidefieldStack(std::vector<std::vector<uint16_t>>& stack, long stackLength, unsigned w,
                                              unsigned h, const sim::SimulationParams& params,
                                              const sim::CellFieldSettings& cellField, double stageXUm,
                                              double stageYUm, const sim::PsfKernelCache& psfCache,
                                              const sim::StackShapingFields& shaping,
                                              const sim::PixelOffsetMap& offsetMap, const sim::PixelGainMap& gainMap,
                                              const sim::PixelReadNoiseMap& readNoiseMap, uint32_t noiseSeed)
{
   SetGpuStatus(kWideFieldGpuStatus);
   if (params.driftNmPerSecX > 0.0)
      LogMessage("WideField: SimType_DriftNmPerSec is not applied in WideField (yet).", false);
   auto t0 = std::chrono::steady_clock::now();
   std::string err;
   sim::CellFieldSource source;
   double z = sim::GetSharedStageState().zPositionUm.load();
   sim::CellFieldQuery q = CellFieldQueryFor(stageXUm, stageYUm, z, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
   sim::WidefieldSceneSpec spec = BuildWidefieldSceneSpec(params, q);
   std::unique_ptr<sim::WidefieldPsf> psf = MakeWidefieldPsf(psfCache, spec);
   LogWidefieldPhotophysics(spec);
   const sim::SquareIllumination ill(w * spec.pixelUm, h * spec.pixelUm);
   sim::WidefieldScene scene;
   bool ok = source.Configure(cellField, err) && scene.Update(source, ill, spec, *psf, err);
   if (!ok)
      LogMessage("WideField: no dyes rendered (" + err + ")", false);
   else
   {
      std::ostringstream m;
      m << "WideField: " << scene.Dyes() << " dyes (" << scene.BleachingDyes() << " bleaching) at stage (" << stageXUm
        << ", " << stageYUm << ") um, " << scene.PsfPlanes() << " PSF planes, " << scene.FftSize() << "^2 FFT, "
        << (psfCache.valid ? "vectorial" : "Gaussian") << " PSF, upscaling " << spec.grid.upscale << " ("
        << std::fixed << std::setprecision(2)
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s setup)";
      LogMessage(m.str());
   }

   const sim::CameraNoiseParams cam = params.Camera();
   const double decaySec = bgDecaySec_.load();
   const std::vector<sim::BlinkEvent> none;
   const unsigned nThreads = std::max(1u, std::min(std::thread::hardware_concurrency(), 32u));
   const long batch = static_cast<long>(nThreads) * 4;
   long clampedMax = ok ? scene.ClampedDyes() : 0;
   std::vector<float> wb;
   for (long f0 = 0; f0 < stackLength; f0 += batch)
   {
      const long f1 = std::min(stackLength, f0 + batch);
      // The Z stage is read per batch of frames (a focus change re-derives the PSF planes).
      const double zNow = sim::GetSharedStageState().zPositionUm.load();
      if (ok && zNow != z)
      {
         z = zNow;
         q = CellFieldQueryFor(stageXUm, stageYUm, z, w, h, params, 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
         spec.focusWorldUm = spec.slabCentreUm = q.zCullCentreUm;
         ok = scene.Update(source, ill, spec, *psf, err);
         if (!ok)
            LogMessage("WideField: " + err, false);
         clampedMax = std::max(clampedMax, ok ? scene.ClampedDyes() : 0L);
      }
      // A fresh sample: frame f starts at dose f x dD. With a uniform pattern
      // every frame is a multiple of the batch's first (the fast path).
      std::vector<double> c(static_cast<size_t>(f1 - f0), 0.0);
      bool scalar = true;
      if (ok)
      {
         scene.FreshBleachWeights(static_cast<double>(f0), wb);
         double c0;
         if (!scene.ScalarOfBleaching(wb, c0))
            scene.SetBleachWeights(wb);
         for (long f = f0; f < f1 && scalar; ++f)
         {
            scene.FreshBleachWeights(static_cast<double>(f), wb);
            scalar = scene.ScalarOfBleaching(wb, c[static_cast<size_t>(f - f0)]);
         }
      }
      auto background = [&](long f, std::vector<float>& img) {
         sim::RenderExtras extras = shaping.Extras(f * params.frameDurationSec, decaySec);
         sim::RenderPhotonImage(img, w, h, none, f, params.pixelSizeNm, params.psfSigmaPx, params.photonsPerBlink,
                                params.backgroundPhotons, 0.0, 0.0, nullptr, 0.0, nullptr, nullptr, &extras);
      };
      if (!ok || scalar)
      {
         std::atomic<long> next{f0};
         auto worker = [&]() {
            std::vector<float> img;
            std::vector<sim::cfloat> scratch;
            for (long f; (f = next.fetch_add(1)) < f1;)
            {
               background(f, img);
               if (ok)
                  scene.RenderScaled(c[static_cast<size_t>(f - f0)], img, scratch);
               sim::ApplyNoiseChain(img, stack[static_cast<size_t>(f)], w, h, cam, offsetMap, gainMap, readNoiseMap,
                                    noiseSeed, static_cast<uint32_t>(f));
            }
         };
         std::vector<std::thread> pool;
         for (unsigned t = 1; t < nThreads; ++t)
            pool.emplace_back(worker);
         worker();
         for (std::thread& t : pool)
            t.join();
      }
      else
      {
         // A non-uniform pattern: each frame's bleaching spectrum on its own.
         std::vector<float> img;
         for (long f = f0; f < f1; ++f)
         {
            background(f, img);
            scene.FreshBleachWeights(static_cast<double>(f), wb);
            scene.RenderFrame(wb, img);
            sim::ApplyNoiseChain(img, stack[static_cast<size_t>(f)], w, h, cam, offsetMap, gainMap, readNoiseMap,
                                 noiseSeed, static_cast<uint32_t>(f));
         }
      }
      stackFramesGenerated_ = f1;
   }
   if (clampedMax > 0)
   {
      std::ostringstream m;
      m << "WideField: " << clampedMax << " dyes lay beyond the PSF kernel's z range and were drawn on its end plane "
        << "-- widen PSFParam_PsfZRangeUm or lower SimType_CellFieldZRangeUm.";
      LogMessage(m.str(), false);
   }
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
      SetGpuStatus("CPU (the Gaussian PSF renders on the CPU; the GPU path is for vectorial PSF models)");
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
   sim::SMLMPatternType patternType = CurrentPatternType();
   std::string customFile = customPointsFile_;
   std::vector<double> spacingsNm = resolutionSpacingsNm_;
   long seed = randomSeed_;
   long length = stackLength_;
   sim::PsfGeneratorRequest psfRequest = BuildPsfGeneratorRequest();
   sim::StructureParams structure = BuildStructureParams();
   // CellField: the stack is generated for ONE stage pose, snapshot here;
   // moving the XY stage afterwards does not change it (spec/PORT.md 8).
   sim::CellFieldSettings cellField = BuildCellFieldSettings();
   double stageX = 0.0, stageY = 0.0;
   sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), stageX, stageY);
   double stageZ = sim::GetSharedStageState().zPositionUm.load();

   stackGenThread_ = std::thread(&CInSiliScopeCamera::StackGenerationWorker, this, length, fullW, fullH, params,
                                  patternType, customFile, spacingsNm, seed, psfRequest, structure, cellField,
                                  stageX, stageY, stageZ, WideFieldSelected());
}

void CInSiliScopeCamera::StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH,
                                             sim::SimulationParams params, sim::SMLMPatternType patternType,
                                             std::string customPointsFile, std::vector<double> spacingsNm,
                                             long seed, sim::PsfGeneratorRequest psfRequest,
                                             sim::StructureParams structure, sim::CellFieldSettings cellField,
                                             double stageXUm, double stageYUm, double stageZUm, bool wideField)
{
   std::mt19937_64 localRng(static_cast<uint64_t>(seed));
   // Independent of localRng (see BuildStructurePattern's own doc comment
   // in Simulation/SMLMStructures.h): site-list generation/labeling can
   // never shift the arrival/noise stream above, at any StructureZRangeNm/
   // Pattern setting.
   uint64_t structureSeed = static_cast<uint64_t>(seed) ^ 0x5354525543545552ULL; // "STRUCTUR"

   double widthUm = fullW * params.pixelSizeNm / 1000.0;
   double heightUm = fullH * params.pixelSizeNm / 1000.0;

   sim::EmitterModel model;
   model.SetPattern(sim::CreatePattern(patternType, customPointsFile, spacingsNm, structure,
                                        widthUm, heightUm, structureSeed));
   model.Reseed(static_cast<uint64_t>(seed));

   // Warn up front (before rendering a single frame) if the structure's own
   // z extent will outrun the cached PSF kernel's z range -- catches a
   // StructureZRangeNm/PsfZRangeUm mismatch immediately rather than only
   // via the per-emitter clamp count below.
   if (psfRequest.model != sim::PsfModelKind::Gaussian)
   {
      double structureHalfRangeNm = sim::StructureZExtentNm(patternType, structure);
      double kernelHalfRangeNm = (psfRequest.nz > 1) ? (psfRequest.nz - 1) / 2.0 * psfRequest.zStepNm : 0.0;
      if (structureHalfRangeNm > kernelHalfRangeNm && kernelHalfRangeNm > 0.0)
      {
         std::ostringstream warn;
         warn << "Structure z extent (+/-" << structureHalfRangeNm << "nm) exceeds the PSF kernel's own z range "
              << "(+/-" << kernelHalfRangeNm << "nm) -- widen PsfZRangeUm or reduce StructureZRangeNm/"
              << "StructureSizeNm/NupRingSeparationNm/NupCurvatureNm.";
         LogMessage(warn.str(), false);
      }
   }

   const bool isCellField = patternType == sim::PATTERN_CELL_FIELD;
   // WideField renders every labelled dye (RenderWidefieldStack below), so
   // it needs no blinks.
   const bool wf = isCellField && wideField;
   if (wideField && !isCellField)
      LogMessage("General_ImagingModality = WideField applies to the CellField pattern only; rendering SuperRes.",
                 false);
   std::vector<sim::BlinkEvent> events;
   if (isCellField && !wf)
   {
      // Every blink of the frames' simulated time span [0, N * frameSec) in
      // one query; tStart/tEnd come out in frames (BucketEventsByFrame then
      // splits them as for any pattern).
      auto t0 = std::chrono::steady_clock::now();
      sim::CellFieldSource source;
      std::string err;
      double d1x = 0.0, d1y = 0.0;
      sim::ComputeDriftOffsetPx(stackLength * params.frameDurationSec, params.driftNmPerSecX, params.driftAngleRad,
                                params.pixelSizeNm, d1x, d1y);
      sim::CellFieldQuery q = CellFieldQueryFor(stageXUm, stageYUm, stageZUm, fullW, fullH, params, 0.0, 0.0, d1x,
                                                d1y, 0, 0.0, stackLength * params.frameDurationSec);
      const std::string zWarn = CellFieldZRangeWarning();
      if (!zWarn.empty())
         LogMessage(zWarn, false);
      if (!source.Configure(cellField, err) || !source.Events(q, events))
         LogMessage("CellField: no events (" + (err.empty() ? std::string("core query failed") : err) + ")", false);
      std::ostringstream msg;
      msg << "CellField: " << events.size() << " blinks for " << stackLength << " frames at stage (" << stageXUm
          << ", " << stageYUm << ") um, dyes "
          << (q.zHalfRangeUm > 0 ? "within +/-" + std::to_string(q.zHalfRangeUm) + " um of the focal plane"
                                 : std::string("at any z"))
          << " (dye activation from SimType_CellFieldMilliActivationRatePerDyePerSec; General_EmitterDensityPerSec "
          << "does not apply to this pattern) ("
          << std::fixed << std::setprecision(2)
          << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s)";
      LogMessage(msg.str());
   }
   else if (!isCellField)
   {
      events = model.GenerateAllEvents(stackLength, widthUm, heightUm, params, localRng);
   }

   // Illumination + structured background: fixed fields for the whole stack,
   // drawn from their OWN rng streams (see BuildShapingFields) -- the same
   // seed gives the same emitters and noise with or without them.
   sim::StackShapingFields shaping = BuildShapingFields(model, fullW, fullH, params, seed);

   sim::PixelOffsetMap localOffsetMap;
   localOffsetMap.Generate(fullW, fullH, params.offsetAdu, params.offsetStdAdu, localRng);
   sim::PixelGainMap localGainMap;
   localGainMap.Generate(fullW, fullH, params.gainPhotonsPerAdu, params.pixelGainStdFraction, localRng);
   sim::PixelReadNoiseMap localReadNoiseMap;
   localReadNoiseMap.Generate(fullW, fullH, params.readNoiseElectrons, params.pixelReadNoiseStdFraction, localRng);

   sim::PsfKernelCache localPsfCache;
   if (psfRequest.model != sim::PsfModelKind::Gaussian)
   {
      std::string err;
      // Runs on the stack-generation worker thread (not the MM device
      // thread) -- LogMessage is still safe to call from here (same
      // pattern already used a few lines below for the failure case).
      auto logCallback = [this](const std::string& msg) { this->LogMessage(msg); };
      if (!sim::ComputePsfKernelCache(psfRequest, localPsfCache, err, logCallback))
      {
         LogMessage("Vectorial PSF unavailable, falling back to Gaussian: " + err, false);
         localPsfCache = sim::PsfKernelCache();
      }
      else
      {
         std::string crlb = sim::DescribePsfCramerRao(localPsfCache, params.photonsPerBlink,
                                                      std::max(0.5, params.backgroundPhotons), params.pixelSizeNm);
         if (!crlb.empty())
            LogMessage(crlb);
      }
   }

   // Blinking out-of-focus emitters (Background_OutOfFocusRatio): the same
   // kinetics on the same structure at ratio x the density, on their own rng
   // stream, placed off focus -- appended to the in-focus events, rendered
   // through the same per-emitter plane lookup. Needs the kernel's z range,
   // hence after the PSF build.
   {
      double ratio = outOfFocusRatio_.load();
      if (ratio > 0.0 && isCellField)
      {
         LogMessage("Background_OutOfFocusRatio: not used by the CellField pattern (its dyes already sit at "
                    "their own depths).", false);
      }
      else if (ratio > 0.0)
      {
         auto zOf = OutOfFocusDepthSampler(localPsfCache);
         if (!zOf)
         {
            LogMessage("Background_OutOfFocusRatio > 0 needs a vectorial PsfModel with a z stack "
                       "(PsfZRangeUm > 0) -- out-of-focus emitters skipped.", false);
         }
         else
         {
            std::mt19937_64 oofRng(static_cast<uint64_t>(seed) ^ 0x4F55544F46464F43ULL); // "OUTOFFOC"
            std::vector<sim::BlinkEvent> oof =
               model.GenerateAllEvents(stackLength, widthUm, heightUm, params, oofRng, ratio, zOf);
            events.insert(events.end(), oof.begin(), oof.end());
         }
      }
   }

   std::vector<std::vector<uint16_t>> newStack(static_cast<size_t>(std::max(stackLength, 0L)));
   // Each frame renders only its own emitters, and its noise comes from the
   // counter-based stream keyed by (noiseSeed, frame, pixel) -- so frames are
   // independent and can be made in any order, on any thread or on the GPU,
   // with the same result.
   const std::vector<std::vector<uint32_t>> frameEvents = sim::BucketEventsByFrame(events, stackLength);
   const uint32_t noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   const sim::CameraNoiseParams cam = params.Camera();
   const double decaySec = bgDecaySec_.load();
   std::atomic<long> zClampedAll{0}, zRenderedAll{0};
   auto frameInputs = [&](long f, std::vector<sim::BlinkEvent>& evs, double& dx, double& dy, double& zOffsetUm) {
      evs.clear();
      for (uint32_t idx : frameEvents[static_cast<size_t>(f)])
         evs.push_back(events[idx]);
      sim::ComputeDriftOffsetPx(f * params.frameDurationSec, params.driftNmPerSecX, params.driftAngleRad,
                                 params.pixelSizeNm, dx, dy);
      // Read the InSiliScopeZStage device's current position fresh each frame,
      // same as any other live-adjustable parameter (see LiveProducerLoop).
      zOffsetUm = sim::GetSharedStageState().zPositionUm.load();
   };

   std::unique_ptr<sim::GpuSimulator> gpu;
   bool gpuOk = !wf && PrepareGpu(gpu, localPsfCache, fullW, fullH, localOffsetMap, localGainMap, localReadNoiseMap,
                                  shaping, params);
   auto startTime = std::chrono::steady_clock::now();
   if (wf)
      RenderWidefieldStack(newStack, stackLength, fullW, fullH, params, cellField, stageXUm, stageYUm, localPsfCache,
                           shaping, localOffsetMap, localGainMap, localReadNoiseMap, noiseSeed);
   if (gpuOk)
   {
      // Batches of frames per dispatch: one GPU round trip per batch rather
      // than per frame.
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
            double dx, dy, zOffsetUm;
            frameInputs(f, evs, dx, dy, zOffsetUm);
            sim::RenderExtras extras = shaping.Extras(f * params.frameDurationSec, decaySec);
            sim::CollectGpuEmitters(evs, f, fullW, fullH, params.pixelSizeNm, params.photonsPerBlink, dx, dy,
                                    localPsfCache, zOffsetUm, &extras, ems[static_cast<size_t>(f - f0)], &zc, &zt);
            frameIds.push_back(static_cast<uint32_t>(f));
            bgScales.push_back(extras.backgroundScale);
            outs.push_back(&newStack[static_cast<size_t>(f)]);
         }
         std::string err;
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
   }
   if (!gpuOk && !wf)
   {
      // Multi-threaded CPU path: an atomic frame counter hands out frames.
      std::atomic<long> nextFrame{0};
      std::atomic<long> done{0};
      auto worker = [&]() {
         std::vector<float> photonImg;
         std::vector<sim::BlinkEvent> evs;
         long zc = 0, zt = 0;
         for (long f; (f = nextFrame.fetch_add(1)) < stackLength;)
         {
            double dx, dy, zOffsetUm;
            frameInputs(f, evs, dx, dy, zOffsetUm);
            sim::RenderExtras extras = shaping.Extras(f * params.frameDurationSec, decaySec);
            sim::RenderPhotonImage(photonImg, fullW, fullH, evs, f, params.pixelSizeNm, params.psfSigmaPx,
                                   params.photonsPerBlink, params.backgroundPhotons, dx, dy,
                                   localPsfCache.valid ? &localPsfCache : nullptr, zOffsetUm, &zc, &zt, &extras);
            sim::ApplyNoiseChain(photonImg, newStack[static_cast<size_t>(f)], fullW, fullH, cam, localOffsetMap,
                                 localGainMap, localReadNoiseMap, noiseSeed, static_cast<uint32_t>(f));
            stackFramesGenerated_ = done.fetch_add(1) + 1;
         }
         zClampedAll += zc;
         zRenderedAll += zt;
      };
      unsigned nThreads = std::max(1u, std::min(std::thread::hardware_concurrency(), 32u));
      std::vector<std::thread> pool;
      for (unsigned t = 1; t < nThreads; ++t)
         pool.emplace_back(worker);
      worker();
      for (std::thread& t : pool)
         t.join();
   }
   {
      double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
      std::ostringstream msg;
      msg << "Rendered " << stackLength << " frames in " << std::fixed << std::setprecision(2) << secs << " s on "
          << (gpuOk ? "the GPU" : "the CPU (" + std::to_string(std::max(1u, std::min(std::thread::hardware_concurrency(), 32u))) + " threads)");
      LogMessage(msg.str());
   }
   long zClampedTotal = zClampedAll.load(), zRenderedTotal = zRenderedAll.load();
   if (zClampedTotal > 0)
   {
      std::ostringstream warn;
      warn << zClampedTotal << "/" << zRenderedTotal << " emitter renders had a total z (stage offset + "
           << "structure depth) beyond the PSF kernel's own z range and were clamped to its end plane -- "
           << "widen PsfZRangeUm or reduce StructureZRangeNm/StructureSizeNm/NupRingSeparationNm/"
           << "NupCurvatureNm.";
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
   // Independent structure-site rng stream, same rationale as
   // StackGenerationWorker's structureSeed -- distinct XOR constant so the
   // two modes' structure streams never collide even at the same
   // RandomSeed.
   uint64_t liveStructureSeed = static_cast<uint64_t>(randomSeed_) ^ 0x4C49564553545231ULL; // "LIVESTR1"
   double pxSizeNm = pixelSizeNm_.load();
   double widthUm = FullWidth() * pxSizeNm / 1000.0;
   double heightUm = FullHeight() * pxSizeNm / 1000.0;
   liveEmitterModel_.SetPattern(sim::CreatePattern(CurrentPatternType(), customPointsFile_, resolutionSpacingsNm_,
                                                    BuildStructureParams(), widthUm, heightUm, liveStructureSeed));
   liveEmitterModel_.Reseed(liveSeed);
   liveRng_.seed(liveSeed);
   liveOutOfFocusRng_.seed(static_cast<uint64_t>(randomSeed_) ^ 0x4C4956454F4F4631ULL); // "LIVEOOF1"

   liveEmitterModel_.ResetLive(widthUm, heightUm);
   liveOutOfFocusModel_.ResetLive(widthUm, heightUm);

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
   // Oversampled vectorial PSF kernel cache -- confined to this thread, same
   // as offsetMap, so no locking is needed. Rebuilt whenever
   // liveConfigVersion_ changes, same trigger as offsetMap/pattern below.
   sim::PsfKernelCache psfCache;
   // Illumination/background fields and the out-of-focus depth sampler --
   // rebuilt on the same config-version trigger as everything else here.
   sim::StackShapingFields shaping;
   std::function<double(std::mt19937_64&)> outOfFocusZ;
   // GPU simulator for this thread (created on first use) and whether the
   // current config renders on it.
   std::unique_ptr<sim::GpuSimulator> gpu;
   bool gpuOk = false;
   // Sentinel: guarantees the very first tick below rebuilds both the offset
   // map and the emitter pattern/site cache, even though StartLiveProducer()
   // already primed them moments earlier (harmless redundancy, and it means
   // this loop doesn't depend on that priming being correct).
   long appliedConfigVersion = -1;
   // Accumulated across ticks within one appliedConfigVersion and flushed
   // (logged + reset) whenever the config changes -- logging every tick at
   // live frame rates would flood the corelog, and the up-front
   // StructureZExtentNm check on rebuild already covers the common
   // "structure z extent misconfigured" case; this also catches "the Z
   // stage was driven out of range while streaming".
   long zClampedSinceRebuild = 0, zTotalSinceRebuild = 0;
   // CellField pattern: the world (configured on the rebuild trigger below)
   // and the simulated time its dye schedules are read at, which advances by
   // one frame duration per produced frame -- across config changes too, so
   // changing a camera setting does not un-bleach the sample.
   sim::CellFieldSource cellField;
   bool cellFieldOk = false;
   double cellFieldTimeSec = 0.0;
   // The last frame's query, for pre-loading around it in the idle time
   // before the next frame.
   sim::CellFieldQuery cellFieldLastQuery;
   bool cellFieldQueried = false;
   // WideField (CellField only): the scene, its PSF and a world-anchored
   // bleach map that persists across frames and config changes (so bleaching
   // a region, moving away and back finds it dim); reset on a world change
   // (seed, labelling, cell params) or a grid pitch change.
   bool wfActive = false, wfOk = false;
   sim::WidefieldScene wfScene;
   std::unique_ptr<sim::WidefieldPsf> wfPsf;
   int wfUpscale = 1;
   long wfPsfVersion = 0, wfWorldVersion = 0;
   sim::CellFieldSettings wfWorld;
   bool wfHaveWorld = false;
   sim::BleachField wfBleach;
   sim::WidefieldSceneSpec wfSpec;
   std::vector<float> wfDose, wfWeights;

   while (liveProducerRun_.load())
   {
      MM::MMTime tickStart = GetCurrentMMTime();
      sim::SimulationParams params = SnapshotParams();
      unsigned w = FullWidth();
      unsigned h = FullHeight();
      double widthUm = w * params.pixelSizeNm / 1000.0;
      double heightUm = h * params.pixelSizeNm / 1000.0;

      // InvalidateStack() bumps liveConfigVersion_ from *every* property
      // handler that changes something affecting simulated frame content --
      // density/lifetime/photon/background rates, PSF wavelength/NA, pixel
      // size, gain, offset, offset-std, read noise, drift, pattern,
      // CustomPointsFile, binning, FOV size, exposure, random seed. A single
      // version bump (of any of them) here triggers a refresh of both
      // pieces of state Live mode caches across ticks: the static
      // fixed-pattern offset map (must match the *current* offset/offset-
      // std or frame size) and the emitter model's pattern/candidate-site
      // cache (must match the *current* pattern/custom-points-file/pixel-
      // size/FOV-size). Using one counter for all of this -- rather than
      // comparing each dependent field individually -- means a newly added
      // property only needs to call InvalidateStack() to be correctly
      // picked up live; no per-property plumbing here.
      long currentConfigVersion = liveConfigVersion_.load(std::memory_order_relaxed);
      if (currentConfigVersion != appliedConfigVersion || offsetMap.width != w || offsetMap.height != h)
      {
         offsetMap.Generate(w, h, params.offsetAdu, params.offsetStdAdu, liveRng_);
         gainMap.Generate(w, h, params.gainPhotonsPerAdu, params.pixelGainStdFraction, liveRng_);
         readNoiseMap.Generate(w, h, params.readNoiseElectrons, params.pixelReadNoiseStdFraction, liveRng_);
         sim::StructureParams structure = BuildStructureParams();
         // Re-derived from currentConfigVersion (not a fixed constant) so
         // every rebuild gets an independent structure rng draw -- live
         // mode was never required to be reproducible (see liveRng_ itself),
         // so this only needs to avoid colliding with the arrival/noise
         // stream above, not to be deterministic across rebuilds.
         uint64_t liveStructureSeed = static_cast<uint64_t>(randomSeed_) ^ 0x4C49564553545231ULL ^
                                       static_cast<uint64_t>(currentConfigVersion);
         liveEmitterModel_.SetPattern(sim::CreatePattern(CurrentPatternType(), customPointsFile_,
                                                          resolutionSpacingsNm_, structure, widthUm, heightUm,
                                                          liveStructureSeed));
         // Same seed -> the identical site list, so the out-of-focus
         // population sits on the same structure.
         liveOutOfFocusModel_.SetPattern(sim::CreatePattern(CurrentPatternType(), customPointsFile_,
                                                             resolutionSpacingsNm_, structure, widthUm, heightUm,
                                                             liveStructureSeed));
         shaping = BuildShapingFields(liveEmitterModel_, w, h, params, randomSeed_);

         sim::PsfGeneratorRequest psfRequest = BuildPsfGeneratorRequest();
         if (psfRequest.model != sim::PsfModelKind::Gaussian)
         {
            std::string err;
            auto logCallback = [this](const std::string& msg) { this->LogMessage(msg); };
            if (!sim::ComputePsfKernelCache(psfRequest, psfCache, err, logCallback))
            {
               LogMessage("Vectorial PSF unavailable, falling back to Gaussian: " + err, false);
               psfCache = sim::PsfKernelCache();
            }
            else
            {
               std::string crlb = sim::DescribePsfCramerRao(psfCache, params.photonsPerBlink,
                                                            std::max(0.5, params.backgroundPhotons),
                                                            params.pixelSizeNm);
               if (!crlb.empty())
                  LogMessage(crlb);
               double structureHalfRangeNm = sim::StructureZExtentNm(CurrentPatternType(), structure);
               double kernelHalfRangeNm = (psfCache.nz > 1) ? (psfCache.nz - 1) / 2.0 * psfCache.zStepNm : 0.0;
               if (structureHalfRangeNm > kernelHalfRangeNm && kernelHalfRangeNm > 0.0)
               {
                  std::ostringstream warn;
                  warn << "Structure z extent (+/-" << structureHalfRangeNm << "nm) exceeds the PSF kernel's "
                       << "own z range (+/-" << kernelHalfRangeNm << "nm) -- widen PsfZRangeUm or reduce "
                       << "StructureZRangeNm/StructureSizeNm/NupRingSeparationNm/NupCurvatureNm.";
                  LogMessage(warn.str(), false);
               }
            }
         }
         else
         {
            psfCache = sim::PsfKernelCache();
         }

         if (zClampedSinceRebuild > 0)
         {
            std::ostringstream warn;
            warn << zClampedSinceRebuild << "/" << zTotalSinceRebuild << " emitter renders had a total z "
                 << "(stage offset + structure depth) beyond the PSF kernel's own z range and were clamped "
                 << "to its end plane since the last config change.";
            LogMessage(warn.str(), false);
         }
         zClampedSinceRebuild = 0;
         zTotalSinceRebuild = 0;

         gpuOk = PrepareGpu(gpu, psfCache, w, h, offsetMap, gainMap, readNoiseMap, shaping, params);

         outOfFocusZ = OutOfFocusDepthSampler(psfCache);
         if (CurrentPatternType() == sim::PATTERN_CELL_FIELD)
         {
            std::string err;
            cellFieldOk = cellField.Configure(BuildCellFieldSettings(), err);
            if (!cellFieldOk)
               LogMessage("CellField unavailable: " + err, false);
            else
               LogMessage("CellField: dye activation from SimType_CellFieldMilliActivationRatePerDyePerSec; "
                          "General_EmitterDensityPerSec does not apply to this pattern.");
            const std::string zWarn = CellFieldZRangeWarning();
            if (!zWarn.empty())
               LogMessage(zWarn, false);
            if (outOfFocusRatio_.load() > 0.0)
               LogMessage("Background_OutOfFocusRatio: not used by the CellField pattern (its dyes already sit "
                          "at their own depths).", false);
            outOfFocusZ = nullptr;
         }
         else if (outOfFocusRatio_.load() > 0.0 && !outOfFocusZ)
            LogMessage("Background_OutOfFocusRatio > 0 needs a vectorial PsfModel with a z stack "
                       "(PsfZRangeUm > 0) -- out-of-focus emitters skipped.", false);

         wfActive = WideFieldSelected() && CurrentPatternType() == sim::PATTERN_CELL_FIELD;
         if (WideFieldSelected() && !wfActive)
            LogMessage("General_ImagingModality = WideField applies to the CellField pattern only; rendering "
                       "SuperRes.", false);
         if (wfActive)
         {
            gpuOk = false;
            SetGpuStatus(kWideFieldGpuStatus);
            if (params.driftNmPerSecX > 0.0)
               LogMessage("WideField: SimType_DriftNmPerSec is not applied in WideField (yet).", false);
            sim::WidefieldSceneSpec base = BuildWidefieldSceneSpec(params, sim::CellFieldQuery());
            wfPsf = MakeWidefieldPsf(psfCache, base);
            wfUpscale = base.grid.upscale;
            ++wfPsfVersion;
            LogWidefieldPhotophysics(base);
            const sim::CellFieldSettings world = BuildCellFieldSettings();
            if (!wfHaveWorld || !world.SameWorld(wfWorld))
            {
               ++wfWorldVersion;
               if (!wfBleach.Empty())
                  LogMessage("WideField: new cell-field world, bleach map reset.");
               wfBleach.Reset(wfBleach.Pitch());
               wfWorld = world;
               wfHaveWorld = true;
            }
         }

         appliedConfigVersion = currentConfigVersion;
      }

      // Drift ramps up from zero at liveDriftOriginFrame_ (reset at
      // StartLiveProducer() and at the start of every Live/MDA sequence
      // acquisition) rather than from an absolute movie length, since a
      // live stream has no fixed end to normalize against.
      long framesSinceDriftOrigin = std::max(0L, liveFrameCounter_.load(std::memory_order_relaxed) -
                                                      liveDriftOriginFrame_.load(std::memory_order_relaxed));
      double dx = 0.0, dy = 0.0;
      sim::ComputeDriftOffsetPx(framesSinceDriftOrigin * params.frameDurationSec, params.driftNmPerSecX,
                                 params.driftAngleRad, params.pixelSizeNm, dx, dy);
      // InSiliScopeZStage's current position, read fresh every tick so moving
      // it live in Micro-Manager sharpens/blurs the rendered PSFs in
      // real time.
      double zOffsetUm = sim::GetSharedStageState().zPositionUm.load();

      std::vector<sim::BlinkEvent> events;
      if (CurrentPatternType() == sim::PATTERN_CELL_FIELD)
      {
         // One stage pose per produced frame (motion blur is ignored), a fresh
         // event per blink per frame with that frame's translation.
         if (cellFieldOk)
         {
            double sx = 0.0, sy = 0.0;
            sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), sx, sy);
            sim::CellFieldQuery q = CellFieldQueryFor(sx, sy, zOffsetUm, w, h, params, dx, dy, dx, dy,
                                                      liveFrameCounter_, cellFieldTimeSec, params.frameDurationSec);
            if (wfActive)
            {
               // The grid is rebuilt when the pose, slab or grid settings
               // change; the PSF planes when the focus does.
               wfSpec = BuildWidefieldSceneSpec(params, q);
               wfSpec.grid.upscale = wfUpscale;
               wfSpec.psfVersion = wfPsfVersion;
               wfSpec.worldVersion = wfWorldVersion;
               const sim::SquareIllumination ill(w * wfSpec.pixelUm, h * wfSpec.pixelUm);
               std::string err;
               const bool wasOk = wfOk;
               wfOk = wfPsf && wfScene.Update(cellField, ill, wfSpec, *wfPsf, err);
               if (!wfOk && wasOk)
                  LogMessage("WideField: " + err, false);
            }
            else if (!cellField.Events(q, events))
            {
               LogMessage("CellField: event query failed", false);
               cellFieldOk = false;
            }
            cellFieldLastQuery = q;
            cellFieldQueried = true;
         }
         cellFieldTimeSec += params.frameDurationSec;
      }
      else
      {
         events = liveEmitterModel_.AdvanceOneFrame(liveFrameCounter_, widthUm, heightUm, params, liveRng_);
         double outOfFocusRatio = outOfFocusRatio_.load();
         if (outOfFocusRatio > 0.0 && outOfFocusZ)
         {
            std::vector<sim::BlinkEvent> oof = liveOutOfFocusModel_.AdvanceOneFrame(
               liveFrameCounter_, widthUm, heightUm, params, liveOutOfFocusRng_, outOfFocusRatio, outOfFocusZ);
            events.insert(events.end(), oof.begin(), oof.end());
         }
      }
      // The background fade restarts with the drift ramp (at every Live/MDA
      // acquisition start), the live-mode analog of "frame 0".
      sim::RenderExtras extras =
         shaping.Extras(framesSinceDriftOrigin * params.frameDurationSec, bgDecaySec_.load());
      // Counter-based noise: keyed by the live frame counter, on a seed of
      // its own (live mode was never meant to reproduce precomputed frames).
      const uint32_t liveNoiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(randomSeed_) ^ 0x4C4E4F49ULL);
      const uint32_t noiseFrame = static_cast<uint32_t>(liveFrameCounter_.load(std::memory_order_relaxed));
      std::vector<uint16_t> nextFrame;
      bool rendered = false;
      if (wfActive)
      {
         // Background as SR, plus every labelled dye at its current dose; the
         // frame's dose is then deposited over the pattern's whole support.
         const std::vector<sim::BlinkEvent> none;
         sim::RenderPhotonImage(photonImg, w, h, none, liveFrameCounter_, params.pixelSizeNm, params.psfSigmaPx,
                                params.photonsPerBlink, params.backgroundPhotons, 0.0, 0.0, nullptr, 0.0, nullptr,
                                nullptr, &extras);
         if (cellFieldOk && wfOk)
         {
            const sim::WidefieldGridSpec& g = wfScene.Grid();
            if (wfBleach.Pitch() != g.pitchUm)
            {
               if (!wfBleach.Empty())
                  LogMessage("WideField: grid pitch changed, bleach map reset.");
               wfBleach.Reset(g.pitchUm);
            }
            wfBleach.DoseOver(g.x0Um, g.y0Um, g.nx, g.ny, wfDose);
            wfScene.BleachWeightsFromDose(wfDose, wfWeights);
            wfScene.RenderFrame(wfWeights, photonImg);
            if (wfSpec.phot.Bleaches())
               wfBleach.Deposit(sim::SquareIllumination(w * wfSpec.pixelUm, h * wfSpec.pixelUm), wfScene.AxisXUm(),
                                wfScene.AxisYUm(), wfSpec.phot.EmissionRatePerSec(1.0) * params.frameDurationSec);
         }
         sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                              liveNoiseSeed, noiseFrame);
         rendered = true;
      }
      if (!rendered && gpuOk && psfCache.valid)
      {
         std::vector<sim::GpuSplatEmitter> ems;
         sim::CollectGpuEmitters(events, liveFrameCounter_, w, h, params.pixelSizeNm, params.photonsPerBlink, dx,
                                 dy, psfCache, zOffsetUm, &extras, ems, &zClampedSinceRebuild,
                                 &zTotalSinceRebuild);
         std::string err;
         // Scalar camera settings (QE, gain...) are read per frame; the
         // static per-pixel buffer carries only the maps and background.
         rendered = gpu->RenderFrame(ems, params.Camera(), extras.backgroundScale, liveNoiseSeed, noiseFrame,
                                     nextFrame, err);
         if (!rendered)
         {
            LogMessage("GPU frame render failed, continuing on the CPU: " + err, false);
            SetGpuStatus("CPU (GPU render failed: " + err + ")");
            gpuOk = false;
         }
      }
      if (!rendered)
      {
         sim::RenderPhotonImage(photonImg, w, h, events, liveFrameCounter_, params.pixelSizeNm,
                                params.psfSigmaPx, params.photonsPerBlink, params.backgroundPhotons, dx, dy,
                                psfCache.valid ? &psfCache : nullptr, zOffsetUm,
                                &zClampedSinceRebuild, &zTotalSinceRebuild, &extras);
         sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.Camera(), offsetMap, gainMap, readNoiseMap,
                              liveNoiseSeed, noiseFrame);
      }

      {
         MMThreadGuard g(frontFrameLock_);
         frontFrame_.swap(nextFrame);
         liveFrameW_ = w;
         liveFrameH_ = h;
      }
      liveFrameSeq_.fetch_add(1, std::memory_order_relaxed);

      MM::MMTime publishTime = GetCurrentMMTime();
      if (liveFrameCounter_ > 0)
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

      ++liveFrameCounter_;

      double exposureMs = GetExposure();
      MM::MMTime elapsed = GetCurrentMMTime() - tickStart;
      double sleepMs = exposureMs - elapsed.getMsec();
      // CellField: spend the wait pre-loading the dyes a stage move would
      // need next (the whole z column and an xy margin around the FOV), so
      // focusing and nearby moves do not stall a frame on generating them.
      if (cellFieldOk && cellFieldQueried && CurrentPatternType() == sim::PATTERN_CELL_FIELD &&
          sleepMs > kCellFieldPrefetchSlackMs)
      {
         sim::CellFieldQuery next = cellFieldLastQuery;
         next.tSec = cellFieldTimeSec;
         next.spanSec = params.frameDurationSec;
         cellField.Prefetch(next, kCellFieldPrefetchMarginUm, sleepMs - kCellFieldPrefetchSlackMs);
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
      for (;;)
      {
         {
            MMThreadGuard g(frontFrameLock_);
            seq = liveFrameSeq_.load(std::memory_order_relaxed);
            if (seq != lastConsumedLiveFrameSeq_)
            {
               frameCopy = frontFrame_;
               w = liveFrameW_;
               h = liveFrameH_;
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

int CInSiliScopeCamera::OnPattern(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_PatternCircle,     g_PatternLines,   g_PatternGrid,
                              g_PatternRandom,     g_PatternCustom,  g_PatternSpiral,
                              g_PatternStar,       g_PatternHeart,   g_PatternResolutionTarget,
                              g_PatternTiltedPlane, g_PatternUniform3D, g_PatternShell, g_PatternNup,
                              g_PatternCalibration9Spots, g_PatternFilamentsRing, g_PatternCellField};
      pProp->Set(names[patternType_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_PatternCircle) patternType_ = sim::PATTERN_CIRCLE;
      else if (s == g_PatternLines) patternType_ = sim::PATTERN_LINES;
      else if (s == g_PatternGrid) patternType_ = sim::PATTERN_GRID;
      else if (s == g_PatternRandom) patternType_ = sim::PATTERN_RANDOM;
      else if (s == g_PatternCustom) patternType_ = sim::PATTERN_CUSTOM_POINTS;
      else if (s == g_PatternSpiral) patternType_ = sim::PATTERN_SPIRAL;
      else if (s == g_PatternStar) patternType_ = sim::PATTERN_STAR;
      else if (s == g_PatternHeart) patternType_ = sim::PATTERN_HEART;
      else if (s == g_PatternResolutionTarget) patternType_ = sim::PATTERN_RESOLUTION_TARGET;
      else if (s == g_PatternTiltedPlane) patternType_ = sim::PATTERN_TILTED_PLANE;
      else if (s == g_PatternUniform3D) patternType_ = sim::PATTERN_UNIFORM_3D;
      else if (s == g_PatternShell) patternType_ = sim::PATTERN_SHELL;
      else if (s == g_PatternNup) patternType_ = sim::PATTERN_NUP;
      else if (s == g_PatternCalibration9Spots) patternType_ = sim::PATTERN_CALIBRATION_9_SPOTS;
      else if (s == g_PatternFilamentsRing) patternType_ = sim::PATTERN_FILAMENTS_RING;
      else if (s == g_PatternCellField) patternType_ = sim::PATTERN_CELL_FIELD;

      // InvalidateStack() bumps liveConfigVersion_, which LiveProducerLoop
      // polls every tick and rebuilds liveEmitterModel_'s pattern from
      // accordingly -- no separate live-mode push needed here.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCustomPointsFile(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(customPointsFile_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      pProp->Get(customPointsFile_);
      // See OnPattern: InvalidateStack() alone is enough to get Live mode
      // to pick this up on its next tick.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnResolutionSpacingsNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      std::vector<double> parsed = sim::ParseResolutionSpacingsNm(s);
      if (!parsed.empty())
      {
         resolutionSpacingsNm_ = parsed;
         // See OnPattern: InvalidateStack() alone is enough to get Live
         // mode to pick this up on its next tick.
         InvalidateStack();
      }
      else
      {
         // Nothing valid parsed (empty string, garbage) -- leave the
         // existing list in place and reflect that back to the caller.
         pProp->Set(sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str());
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

int CInSiliScopeCamera::OnEmitterDensityPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(emitterDensityPerSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); emitterDensityPerSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPhotonsPerSecond(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(photonsPerSecond_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); photonsPerSecond_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnOnLifetimeSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(onLifetimeSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); onLifetimeSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPsfWavelengthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfWavelengthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfWavelengthNm_ = v; InvalidateStack(); }
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
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); gainPhotonsPerAdu_ = v; InvalidateStack(); }
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
      // Only takes effect before the first vectorial-PSF computation: the
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

int CInSiliScopeCamera::OnLabelingEfficiencyPct(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(labelingEfficiencyPct_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); labelingEfficiencyPct_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnStructureZRangeNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(structureZRangeNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); structureZRangeNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnStructureSizeNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(structureSizeNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); structureSizeNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupRadiusNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupRadiusNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupRadiusNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupCornerSpreadNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupCornerSpreadNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupCornerSpreadNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupRingSeparationNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupRingSeparationNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupRingSeparationNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupLinkerMinNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupLinkerMinNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupLinkerMinNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupLinkerMaxNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupLinkerMaxNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupLinkerMaxNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupMembraneType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(nupMembrane_ == static_cast<int>(sim::MembraneOrientation::TopDown) ? g_NupMembraneTopDown
                                                                                        : g_NupMembraneSideways);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      nupMembrane_ = (s == g_NupMembraneSideways) ? static_cast<int>(sim::MembraneOrientation::Sideways)
                                                    : static_cast<int>(sim::MembraneOrientation::TopDown);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupCount(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(nupCount_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v < 1)
         v = 1;
      nupCount_ = static_cast<int>(v);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupMinSpacingNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupMinSpacingNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupMinSpacingNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnNupCurvatureNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(nupCurvatureNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); nupCurvatureNm_ = v; InvalidateStack(); }
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

int CInSiliScopeCamera::OnBlinkBleachProb(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(blinkBleachProb_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); blinkBleachProb_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnOffLifetimeSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(offLifetimeSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); offLifetimeSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnPhotonCV(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(photonCV_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); photonCV_ = v; InvalidateStack(); }
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
   if (eAct == MM::BeforeGet) pProp->Set(emGain_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); emGain_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnCicElectrons(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(cicElectrons_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); cicElectrons_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBgCellContrast(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(bgCellContrast_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); bgCellContrast_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBgHazeWeight(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(bgHazeWeight_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); bgHazeWeight_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBgHazeWidthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(bgHazeWidthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); bgHazeWidthNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnBgDecaySec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(bgDecaySec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); bgDecaySec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnOutOfFocusRatio(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(outOfFocusRatio_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); outOfFocusRatio_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnOutOfFocusDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(outOfFocusDepthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); outOfFocusDepthNm_ = v; InvalidateStack(); }
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
      // EmitterDensityPerSec/OnLifetimeSec/PhotonsPerSecond/
      // BackgroundPhotonsPerSec are all rates converted to frame-equivalent
      // values using this property's current value (SnapshotParams()) -- a
      // changed Exposure means the precomputed stack no longer reflects the
      // current settings and must regenerate. Live mode needs no equivalent
      // rebuild: LiveProducerLoop calls SnapshotParams() fresh every tick
      // regardless, and none of its version-gated cached state (offset map,
      // emitter pattern, PSF kernel) depends on exposure time -- so this
      // deliberately uses InvalidateStackOnly() rather than InvalidateStack(),
      // to avoid forcing a multi-second PSF-kernel recompute (vectorial
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

int CInSiliScopeCamera::OnImagingModality(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(wideField_.load() ? g_ModalityWideField : g_ModalitySuperRes);
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      wideField_ = (s == g_ModalityWideField);
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
