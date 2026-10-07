///////////////////////////////////////////////////////////////////////////////
// FILE:          SceneSettings.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The settings as the engine's structures (SceneSettings.h).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "SceneSettings.h"

#include "../Simulation/CacheDir.h"
#include "../Simulation/DyeLibrary.h"
#include "../Simulation/Spectra.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace isc {

int Modality(const SceneState& s)
{
   return s.transOpen.load() && !s.epiOpen.load() ? 1 : 0;
}

sim::SimulationParams SnapshotParams(const SceneState& s)
{
   // BackgroundPhotonsPerSec and the dark current are rates (per second),
   // converted to the frame-equivalent quantities the (exposure-unaware) noise
   // chain expects. The dyes' photons come from the engine's FluorescenceMovie
   // (BuildScopeSpec).
   double expSec = s.exposureMs.load() / 1000.0;
   if (expSec <= 0.0)
      expSec = 0.001;

   sim::SimulationParams p;
   p.pixelSizeNm = s.PixelSizeNm();
   p.backgroundPhotons = s.backgroundPhotonsPerSec.load() * expSec;
   // Fluorescence: the movie's photon images are already detected photons
   // (the camera's QE curve at each dye's emission is in the light path), so
   // the noise chain runs at QE 1, as the cli/viewer's. BrightField: the
   // curve's QE at the lamp wavelength.
   p.quantumEfficiency = BrightFieldSelected(s) ? BrightFieldQe(s) : 1.0;
   p.darkCurrentElectronsPerFrame = s.darkCurrentPerSec.load() * expSec;
   p.gainPhotonsPerAdu = s.gainElectronsPerAdu.load();
   p.offsetAdu = s.offsetAdu.load();
   p.offsetStdAdu = s.offsetStdAdu.load();
   p.readNoiseElectrons = s.readNoiseElectrons.load();
   // The per-pixel gain and read-noise spreads are an sCMOS's (a column of
   // amplifiers per pixel); an EMCCD reads every pixel through one amplifier.
   const bool emccd = s.emccd.load() != 0;
   p.pixelGainStdFraction = emccd ? 0.0 : s.pixelGainStdPct.load() / 100.0;
   p.pixelReadNoiseStdFraction = emccd ? 0.0 : s.pixelReadNoiseStdPct.load() / 100.0;
   p.drift.xyNmPerSqrtSec = s.driftXyNmPerSqrtSec.load();
   p.drift.zNmPerSqrtSec = s.driftZNmPerSqrtSec.load();
   p.drift.xySpeedNmPerSec = s.directedDrift[DD_XY_SPEED].load();
   p.drift.zSpeedNmPerSec = s.directedDrift[DD_Z_SPEED].load();
   p.drift.xyAngleDeg = s.directedDrift[DD_XY_ANGLE].load();
   p.drift.zDirection = static_cast<int>(std::lround(s.directedDrift[DD_Z_DIRECTION].load()));
   p.drift.angleWanderDeg = s.directedDrift[DD_ANGLE_WANDER].load();
   p.drift.zAngleWanderDeg = s.directedDrift[DD_Z_ANGLE_WANDER].load();
   p.drift.speedWanderPct = s.directedDrift[DD_SPEED_WANDER].load();
   p.drift.wanderTimeSec = s.directedDrift[DD_WANDER_TIME].load();
   p.frameDurationSec = expSec;
   p.emccd = s.emccd.load() != 0;
   p.emGain = s.EmGain();
   p.cicElectrons = s.cicElectrons.load();
   p.bitDepth = s.bitDepth.load();
   return p;
}

sim::PsfGeneratorRequest BuildPsfGeneratorRequest(const SceneState& s)
{
   sim::PsfGeneratorRequest req;
   req.model = static_cast<sim::PsfModelKind>(s.psfModel.load());
   // The emission wavelength is the dye's (the engine's request hook sets it
   // per dye state); this placeholder is the default microtubule dye's peak
   // region.
   req.wavelengthNm = 670.0;
   req.na = s.na.load();
   req.immersionIndex = s.immersionIndex.load();
   req.pixelSizeNm = s.PixelSizeNm();
   req.oversampling = std::max(1, s.psfOversampling.load());
   // The kernel half-width is physical (nm), rounded to whole camera pixels
   // and grown to cover the first Airy ring (sim::PsfKernelHalfWidthPx,
   // shared with the cli/viewer).
   req.kernelHalfWidthPx =
      sim::PsfKernelHalfWidthPx(s.psfKernelHalfWidthNm.load(), req.pixelSizeNm, req.wavelengthNm, req.na);
   // The kernel's z stack; the focus selecting a plane per emitter comes from
   // the ZStage.
   const double zRangeUm = s.psfZRangeUm.load();
   const double zStepUm = std::max(s.psfZStepUm.load(), 0.001);
   req.nz = static_cast<int>(std::lround(zRangeUm / zStepUm)) + 1;
   req.zStepNm = zStepUm * 1000.0;
   // GibsonLanni-only (ignored by RichardsWolf).
   req.sampleIndex = s.psfSampleIndex.load();
   req.workingDistanceUm = s.workingDistanceUm.load();
   req.sampleDepthNm = s.psfSampleDepthNm.load();
   // GibsonLanniZernike-only: the coefficients (space-separated in MM, which
   // forbids commas; PsfBridge.java wants commas).
   {
      bool ok = false;
      req.zernikeCoefficients =
         sim::FormatZernikeCoefficients(sim::ParseZernikeCoefficients(s.ZernikeCoefficients(), ok), ',');
   }
   req.javaHome = s.JavaHome();
   req.interpMode = static_cast<sim::PsfInterpMode>(s.psfInterp.load());
   req.pupilSamples = s.psfPupilSamples.load();
   req.maskType = static_cast<sim::PsfMaskType>(s.psfMaskType.load());
   req.maskModes = s.psfMaskModes.load();
   req.maskWaist = s.psfMaskWaist.load();
   return req;
}

sim::StackShapingFields BuildShapingFields(const SceneState& s, unsigned w, unsigned h, double* meanFactor)
{
   sim::StackShapingFields out;
   double mean = 1.0;
   out.illum = sim::BuildIlluminationField(w, h, static_cast<sim::IllumProfile>(s.illumProfile.load()),
                                           s.illumFwhmPct.load(), &mean);
   if (meanFactor)
      *meanFactor = mean;
   return out;
}

sim::CellFieldSettings BuildCellFieldSettings(const SceneState& s)
{
   sim::CellFieldSettings c;
   // Its own stream of RandomSeed ("CELL"), never the arrival/noise one.
   c.seed = static_cast<uint32_t>(static_cast<uint64_t>(s.seed.load()) ^ 0x43454C4CULL);
   c.params = {
      {"chunkSize", s.cellField[CF_CHUNK_SIZE_UM].load()},
      {"density", s.cellField[CF_OCCUPANCY].load()},
      {"cellDiamMin", s.cellField[CF_CELL_DIAM_MIN_UM].load()},
      {"cellDiamMax", s.cellField[CF_CELL_DIAM_MAX_UM].load()},
      {"mtDensity", s.cellField[CF_MT_DENSITY].load()},
      {"enablePacking", s.cellFieldPacking.load() ? 1.0 : 0.0},
   };
   for (int i = 0; i < CF_COUNT; ++i)
      if (g_CellFieldCoreParam[i])
         c.params.push_back({g_CellFieldCoreParam[i], s.cellField[i].load()});
   // No labels: BrightField reads the geometry only (the fluorescence movie
   // sets its own labels from the spec, BuildScopeSpec).
   c.cacheDir = s.diskCacheMode.load() >= 1 ? sim::DefaultCacheDir() : std::string();
   return c;
}

std::string CellFieldZRangeWarning(const SceneState& s)
{
   if (static_cast<sim::PsfModelKind>(s.psfModel.load()) == sim::PsfModelKind::Gaussian)
      return {};
   const double slab = s.cellField[CF_Z_RANGE_UM].load(), kernel = s.psfZRangeUm.load();
   if (slab > 0.0 && slab <= kernel)
      return {};
   std::ostringstream w;
   w << "CellField: ZRangeUm (" << (slab > 0.0 ? std::to_string(slab) + " um" : "0 = no limit")
     << ") exceeds the Objective's PsfZRangeUm (" << kernel << " um): dyes beyond the kernel's range are drawn "
     << "on its end plane. Lower the former or widen the latter.";
   return w.str();
}

sim::CellFieldQuery CellFieldQueryFor(const SceneState& s, double stageX, double stageY, double zStageUm, unsigned w,
                                      unsigned h, const sim::SimulationParams& params, double drift0XPx,
                                      double drift0YPx, double drift1XPx, double drift1YPx, long frameIndex,
                                      double tSec, double spanSec)
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
   // above the coverslip, the focus height an extra offset.
   const double focus = s.cellField[CF_FOCUS_HEIGHT_UM].load();
   q.zRefUm = focus;
   q.zCullCentreUm = focus + zStageUm;
   // The z slab around the focal plane whose dyes are rendered (0 = no z
   // limit); dyes outside it are culled, not clamped.
   q.zHalfRangeUm = std::max(0.0, s.cellField[CF_Z_RANGE_UM].load()) / 2.0;
   q.frameIndex = frameIndex;
   q.tSec = tSec;
   q.spanSec = spanSec;
   q.frameSec = params.frameDurationSec;
   return q;
}

double BrightFieldQe(const SceneState& s)
{
   const int qc = static_cast<int>(s.Option("qe-curve"));
   if (qc >= 0 && qc < static_cast<int>(sim::CameraIds().size()))
      if (const sim::Spectrum* sp = sim::SpectrumByKey(sim::CameraAt(qc).qeCurve))
         return sim::SampleAt(*sp, s.brightField[BF_WAVELENGTH_NM].load());
   return s.quantumEfficiency.load();
}

sim::BrightfieldSpec BuildBrightfieldSpec(const SceneState& st, const sim::SimulationParams& params,
                                          const sim::CellFieldQuery& q, unsigned w, unsigned h)
{
   sim::BrightfieldSpec s;
   s.originXUm = q.originXUm;
   s.originYUm = q.originYUm;
   s.width = w;
   s.height = h;
   s.pixelUm = params.pixelSizeNm / 1000.0;
   auto N = [&](int i) { return st.brightField[i].load(); };
   s.quality = static_cast<int>(std::lround(std::min(4.0, std::max(1.0, N(BF_QUALITY)))));
   s.sources = static_cast<int>(std::lround(std::max(0.0, N(BF_SOURCES))));
   s.upscale = static_cast<int>(std::lround(std::max(0.0, N(BF_UPSCALING))));
   s.sub = static_cast<int>(std::lround(std::max(0.0, N(BF_GEOMETRY_SAMPLES))));
   s.sliceUm = N(BF_SLICE_UM) < 0 ? -1.0 : N(BF_SLICE_UM);
   s.condenserNa = std::max(0.0, N(BF_CONDENSER_NA));
   s.wavelengthNm = std::max(1.0, N(BF_WAVELENGTH_NM));
   s.na = std::max(0.01, st.na.load());
   s.nMedium = N(BF_INDEX_MEDIUM);
   s.nCytoplasm = N(BF_INDEX_CYTOPLASM);
   s.nNucleus = N(BF_INDEX_NUCLEUS);
   s.nMicrotubule = N(BF_INDEX_MICROTUBULE);
   s.absorptionPerUm = std::max(0.0, N(BF_ABSORPTION_PER_UM));
   s.zernike = sim::ZeroZernikeCoefficients();
   if (N(BF_ABERRATIONS) != 0 &&
       static_cast<sim::PsfModelKind>(st.psfModel.load()) == sim::PsfModelKind::GibsonLanniZernike)
   {
      bool ok = false;
      s.zernike = sim::ParseZernikeCoefficients(BuildPsfGeneratorRequest(st).zernikeCoefficients, ok);
   }
   return s;
}

void AddDriftToSpec(sim::ScopeSpec& spec, const sim::DriftSettings& d)
{
   spec["drift-xy-nm-per-sqrt-sec"] = d.xyNmPerSqrtSec;
   spec["drift-z-nm-per-sqrt-sec"] = d.zNmPerSqrtSec;
   spec["drift-xy-speed-nm-per-sec"] = d.xySpeedNmPerSec;
   spec["drift-z-speed-nm-per-sec"] = d.zSpeedNmPerSec;
   spec["drift-xy-angle-deg"] = d.xyAngleDeg;
   spec["drift-z-direction"] = d.zDirection;
   spec["drift-xy-angle-wander-deg"] = d.angleWanderDeg;
   spec["drift-z-angle-wander-deg"] = d.zAngleWanderDeg;
   spec["drift-speed-wander-pct"] = d.speedWanderPct;
   spec["drift-wander-time-sec"] = d.wanderTimeSec;
}

sim::ScopeSpec BuildScopeSpec(const SceneState& st, double stageXUm, double stageYUm, double zStageUm, double startSec,
                              long frames)
{
   sim::ScopeSpec s;
   s["seed"] = static_cast<double>(st.seed.load());
   s["disk-cache"] = st.diskCacheMode.load();
   s["x"] = stageXUm;
   s["y"] = stageYUm;
   s["z"] = zStageUm;
   s["size"] = static_cast<double>(st.WidthPx());
   s["frames"] = static_cast<double>(frames);
   s["exposure-ms"] = st.exposureMs.load();
   s["start-sec"] = startSec;
   s["pixel-nm"] = st.PixelSizeNm();
   s["background-per-sec"] = st.backgroundPhotonsPerSec.load();
   s["na"] = st.na.load();
   s["focus-um"] = st.cellField[CF_FOCUS_HEIGHT_UM].load();
   s["z-range-um"] = st.cellField[CF_Z_RANGE_UM].load();
   for (const auto& kv : st.Options())
      s[kv.first] = kv.second;
   for (const auto& kv : st.DyeEdits())
      s[kv.first] = kv.second;
   // The light path and camera are the properties' (no preset of the engine's own).
   s["light-preset"] = -1;
   s["camera-preset"] = st.cameraPreset.load();
   s["qe"] = st.quantumEfficiency.load();
   s["camera-type"] = st.emccd.load() ? 1 : 0;
   s["dark-per-sec"] = st.darkCurrentPerSec.load();
   s["gain"] = st.gainElectronsPerAdu.load();
   s["offset"] = st.offsetAdu.load();
   s["offset-std"] = st.offsetStdAdu.load();
   s["read-noise"] = st.readNoiseElectrons.load();
   s["gain-std-pct"] = st.pixelGainStdPct.load();
   s["read-noise-std-pct"] = st.pixelReadNoiseStdPct.load();
   s["em-gain"] = st.EmGain();
   s["cic"] = st.cicElectrons.load();
   s["bit-depth"] = st.bitDepth.load();
   // The cell field.
   s["chunk-um"] = st.cellField[CF_CHUNK_SIZE_UM].load();
   s["occupancy"] = st.cellField[CF_OCCUPANCY].load();
   s["cell-diam-min-um"] = st.cellField[CF_CELL_DIAM_MIN_UM].load();
   s["cell-diam-max-um"] = st.cellField[CF_CELL_DIAM_MAX_UM].load();
   s["mt-density"] = st.cellField[CF_MT_DENSITY].load();
   s["packing"] = st.cellFieldPacking.load() ? 1 : 0;
   for (int i = 0; i < CF_COUNT; ++i)
      if (g_CellFieldCoreParam[i])
         s[std::string("p.") + g_CellFieldCoreParam[i]] = st.cellField[i].load();
   // Imaging.
   s["modality"] = Modality(st);
   s["light-epi"] = st.epiOpen.load() ? 1 : 0;
   s["light-trans"] = st.transOpen.load() ? 1 : 0;
   s["wf-upscale"] = st.wideField[WF_UPSCALING].load();
   s["wf-plane-nm"] = st.wideField[WF_Z_PLANE_NM].load();
   s["wf-kernel-um"] = std::max(0.1, st.psfKernelHalfWidthNm.load() / 1000.0);
   // The PSF: the coefficients as zern.* over the None preset.
   s["immersion-index"] = st.immersionIndex.load();
   s["psf-model"] = st.psfModel.load();
   s["psf-zernike-preset"] = sim::IndexOf(sim::ZernikePresetNames(), "None");
   {
      bool ok = false;
      const sim::ZernikeCoefficients z = sim::ParseZernikeCoefficients(st.ZernikeCoefficients(), ok);
      for (size_t j = 0; j < z.size(); ++j)
         s["zern." + std::to_string(j)] = z[j];
   }
   s["psf-mask"] = st.psfMaskType.load();
   s["psf-mask-modes"] = st.psfMaskModes.load();
   s["psf-mask-waist"] = st.psfMaskWaist.load();
   s["psf-oversampling"] = st.psfOversampling.load();
   s["psf-kernel-half-width-nm"] = st.psfKernelHalfWidthNm.load();
   s["psf-z-range-um"] = st.psfZRangeUm.load();
   s["psf-z-step-um"] = st.psfZStepUm.load();
   s["psf-sample-index"] = st.psfSampleIndex.load();
   s["psf-working-distance-um"] = st.workingDistanceUm.load();
   s["psf-sample-depth-nm"] = st.psfSampleDepthNm.load();
   s["psf-interp"] = st.psfInterp.load();
   s["psf-pupil-samples"] = st.psfPupilSamples.load();
   s["psf-halo-cut"] = st.psfHaloCut.load();
   return s;
}

std::string ScopeSpecText(const sim::ScopeSpec& spec)
{
   std::ostringstream o;
   for (const auto& kv : spec)
   {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", kv.second);
      o << kv.first << '=' << buf << '\n';
   }
   return o.str();
}

} // namespace isc
