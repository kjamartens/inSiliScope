///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See ScopeMovie.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "ScopeMovie.h"

#include "CacheDir.h"

#include "BrightfieldRender.h"
#include "Parallel.h"
#include "Timing.h"

#include "CellFieldSource.h"
#include "PsfGeneratorBridge.h"
#include "SMLMZernike.h"
#include "SMLMNoise.h"
#include "SMLMSimulation.h"
#include "WidefieldRender.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace sim {

const std::vector<ScopeOption>& ScopeMovieOptions()
{
   static const std::vector<ScopeOption> opts = {
      { "seed", 42, "SimType_RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)" },
      { "world-seed", -1, "cell-field world seed used as is (the viewer's seed); -1 = derive it from seed" },
      { "disk-cache", 1, "per-user cache on disk ($ISC_CACHE_DIR, else %LOCALAPPDATA%/inSiliScope/cache or ~/.cache/insiliscope): 0 = none, 1 = the packed cell positions (a few MB: a rerun with the same seed and cell parameters packs nothing), 2 = also the PSF kernel (one file, up to ~200 MB)" },
      { "prepare", 0, "1 = build the world and the PSF kernel only (warms the memo and the disk cache), no frames" },
      { "x", 0, "FOV centre x, world um (XY stage position)" },
      { "y", 0, "FOV centre y, world um" },
      { "z", 0.5, "Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)" },
      { "size", 128, "FOV width = height, pixels" },
      { "frames", 1000, "number of frames" },
      { "exposure-ms", 50, "frame duration, ms (simulated time per frame)" },
      { "start-sec", 0, "simulated time of the first frame, s" },
      { "pixel-nm", 100, "pixel size, nm" },
      { "photons-per-sec", 7500, "FluoParam_PhotonsPerSecond" },
      { "on-sec", 0.05, "FluoParam_OnLifetimeSec" },
      { "off-sec", 1.0, "FluoParam_OffLifetimeSec" },
      { "bleach-prob", 1.0, "FluoParam_BlinkBleachProb" },
      { "photon-cv", 0.5, "FluoParam_PhotonCV (per-blink log-normal brightness spread)" },
      { "background-per-sec", 0, "Background_BackgroundPhotonsPerSec (photons/pixel/s)" },
      { "wavelength-nm", 660, "PSFParam_PsfEmissionWavelengthNm (Gaussian sigma = 0.21 lambda / NA)" },
      { "na", 1.4, "PSFParam_PsfNa: numerical aperture" },
      { "focus-um", 0, "SimType_CellFieldFocusHeightUm (focus offset added to z)" },
      { "z-range-um", 7.0, "SimType_CellFieldZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)" },
      { "milli-activation-rate", 1.43, "SimType_CellFieldMilliActivationRatePerDyePerSec (per dark dye, 1e-3/s)" },
      { "labeling-pct-bleaching", 0, "SimType_CellFieldLabelingPctBleaching (bleaching dyes, % of lattice sites)" },
      { "labeling-pct-nonbleaching", 70, "SimType_CellFieldLabelingPctNonBleaching (persistent, DNA-PAINT-like sites)" },
      { "chunk-um", 26, "SimType_CellFieldChunkSizeUm" },
      { "occupancy", 0.33, "SimType_CellFieldOccupancy" },
      { "cell-diam-min-um", 25, "SimType_CellFieldCellDiameterMinUm" },
      { "cell-diam-max-um", 35, "SimType_CellFieldCellDiameterMaxUm" },
      { "mt-density", 0.9, "SimType_CellFieldMicrotubuleDensityPerUm2" },
      { "packing", 1, "SimType_CellFieldPacking (1 on, 0 off)" },
      { "qe", 0.85, "CamParam_QuantumEfficiency" },
      { "dark-per-sec", 1.03, "CamParam_DarkCurrentElectronsPerSec" },
      { "gain", 0.25, "CamParam_GainPhotonsPerADU" },
      { "offset", 100, "CamParam_OffsetADU" },
      { "offset-std", 0.5, "CamParam_OffsetStdADU" },
      { "read-noise", 1.2, "CamParam_ReadNoiseElectrons" },
      { "gain-std-pct", 0.5, "CamParam_GainStdPctPerPixel (per-pixel gain spread, PRNU)" },
      { "read-noise-std-pct", 20, "CamParam_ReadNoiseStdPctPerPixel" },
      { "modality", 0, "General_ImagingModality: 0 = SuperRes (blinks), 1 = WideField (all dyes), 2 = BrightField (transmitted light; names accepted)" },
      { "wf-upscale", 1, "General_WideFieldUpscaling: WideField grid cells per pixel, per axis (1-4)" },
      { "wf-plane-nm", 25, "General_WideFieldZPlaneNm: WideField dye plane thickness, nm" },
      { "wf-kernel-um", 7, "WideField PSF kernel radius cap, um" },
      { "wf-excitation-photons-per-um2-per-sec", 4e8, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec" },
      { "wf-quantum-yield", 0.7, "FluoParam_WideFieldQuantumYield" },
      { "wf-photon-budget", 5000, "FluoParam_WideFieldPhotonBudget: emitted photons per dye (0 = never bleaches)" },
      { "wf-extinction-coeff", 270000, "FluoParam_WideFieldExtinctionCoeff, M^-1 cm^-1" },
      { "bf-quality", 3, "General_BrightFieldQuality: speed vs precision, 1 (fast) .. 4 (precise); sets the four below unless given" },
      { "bf-sources", 0, "General_BrightFieldSources: condenser source points (0 = from bf-quality: 6/12/24/48)" },
      { "bf-upscale", 0, "General_BrightFieldUpscaling: optical grid cells per pixel, per axis (a minimum, raised to keep the grid pitch <= lambda / 4n; 0 = from bf-quality: 1)" },
      { "bf-sub", 0, "General_BrightFieldGeometrySamples: geometry samples per grid cell side (0 = from bf-quality: 1/1/2/2)" },
      { "bf-slice-um", -1, "General_BrightFieldSliceUm: multislice step, um; 0 = one thin slice (-1 = from bf-quality: 0/0.5/0.5/0.25)" },
      { "bf-margin-um", 0, "BrightField grid margin around the FOV, um (0 = from bf-quality: 3-5)" },
      { "bf-condenser-na", 0.55, "General_BrightFieldCondenserNa: illumination NA (0 = coherent)" },
      { "bf-wavelength-nm", 550, "General_BrightFieldWavelengthNm: illumination wavelength" },
      { "bf-photons-per-px-per-sec", 40000, "General_BrightFieldPhotonsPerPxPerSec: empty-field photons per pixel per second" },
      { "bf-aberrations", 1, "General_BrightFieldAberrations: 1 = the PSF's Zernike aberrations in the detection pupil, 0 = none" },
      { "bf-n-medium", 1.337, "SimType_CellFieldIndexMedium: refractive index of the medium" },
      { "bf-n-cytoplasm", 1.345, "SimType_CellFieldIndexCytoplasm" },
      { "bf-n-nucleus", 1.345, "SimType_CellFieldIndexNucleus" },
      { "bf-n-microtubule", 1.48, "SimType_CellFieldIndexMicrotubule (12.5 nm tubes)" },
      { "bf-absorption-per-um", 0, "SimType_CellFieldAbsorptionPerUm: intensity absorption of cell material, 1/um (unstained: 0)" },
      { "immersion-index", 1.518, "PSFParam_PsfImmersionIndex (PSF and WideField collection efficiency)" },
      { "psf-model", 3, "PSFParam_PsfModel: 0 = Gaussian, 3 = GibsonLanniZernike (names accepted; 1/2 need the adapter's JVM)" },
      { "psf-zernike-preset", 9, "PSFParam_PsfZernikePreset: index or name (0 None ... 9 MixedRealisticObjective ... 12)" },
      { "psf-mask", 0, "PSFParam_PsfMaskType: 0 = None, 1 = DoubleHelix (names accepted)" },
      { "psf-mask-modes", 5, "PSFParam_PsfMaskModes: double-helix Gauss-Laguerre modes (2-8)" },
      { "psf-mask-waist", 1.0, "PSFParam_PsfMaskWaist: double-helix waist, pupil radii" },
      { "psf-oversampling", 6, "PSFParam_PsfOversampling: kernel samples per camera pixel, per axis (1-16)" },
      { "psf-kernel-half-width-nm", 7000, "PSFParam_PsfKernelHalfWidthNm (a minimum: grown to 3x the Rayleigh radius)" },
      { "psf-z-range-um", 7.0, "PSFParam_PsfZRangeUm: span of the PSF z stack" },
      { "psf-z-step-um", 0.1, "PSFParam_PsfZStepUm: PSF z plane spacing" },
      { "psf-sample-index", 1.518, "PSFParam_PsfSampleIndex: sample refractive index (Gibson-Lanni)" },
      { "psf-working-distance-um", 150, "PSFParam_PsfWorkingDistanceUm (Gibson-Lanni ti0)" },
      { "psf-sample-depth-nm", 0, "PSFParam_PsfSampleDepthNm: emitter depth below the coverslip (Gibson-Lanni)" },
      { "psf-interp", 2, "PSFParam_PsfInterp: 0 Nearest, 1 Linear, 2 Cubic, 3 Fft (names accepted)" },
   };
   return opts;
}

namespace {

// "zern.<j>", j = 0..27: Zernike coefficient j in waves, replacing the
// preset's. Returns j, or -1.
int ZernikeKeyIndex(const std::string& name)
{
   if (name.compare(0, 5, "zern.") != 0 || name.size() < 6 || name.size() > 7)
      return -1;
   int j = 0;
   for (size_t i = 5; i < name.size(); ++i)
   {
      if (name[i] < '0' || name[i] > '9')
         return -1;
      j = j * 10 + (name[i] - '0');
   }
   return j < static_cast<int>(kNumZernike) ? j : -1;
}

} // namespace

bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value)
{
   if ((name.compare(0, 2, "p.") == 0 && name.size() > 2) || ZernikeKeyIndex(name) >= 0)
   {
      spec[name] = value;
      return true;
   }
   for (const ScopeOption& o : ScopeMovieOptions())
      if (name == o.name)
      {
         spec[name] = value;
         return true;
      }
   return false;
}

double ScopeSpecGet(const ScopeSpec& spec, const char* name)
{
   auto it = spec.find(name);
   if (it != spec.end())
      return it->second;
   for (const ScopeOption& o : ScopeMovieOptions())
      if (!std::strcmp(o.name, name))
         return o.value;
   return 0.0;
}

bool ScopeOptionValue(const std::string& name, const char* text, double& value)
{
   char* end = nullptr;
   value = std::strtod(text, &end);
   if (end != text && *end == 0)
      return true;
   if (name == "modality")
   {
      if (!std::strcmp(text, "SuperRes")) { value = 0; return true; }
      if (!std::strcmp(text, "WideField")) { value = 1; return true; }
      if (!std::strcmp(text, "BrightField")) { value = 2; return true; }
   }
   if (name == "psf-model")
   {
      const char* names[] = { "Gaussian", "RichardsWolf", "GibsonLanni", "GibsonLanniZernike" };
      for (int i = 0; i < 4; ++i)
         if (!std::strcmp(text, names[i])) { value = i; return true; }
   }
   if (name == "psf-mask")
   {
      if (!std::strcmp(text, "None")) { value = 0; return true; }
      if (!std::strcmp(text, "DoubleHelix")) { value = 1; return true; }
   }
   if (name == "psf-interp")
   {
      const char* names[] = { "Nearest", "Linear", "Cubic", "Fft" };
      for (int i = 0; i < 4; ++i)
         if (!std::strcmp(text, names[i])) { value = i; return true; }
   }
   if (name == "psf-zernike-preset")
   {
      const std::vector<std::string>& names = ZernikePresetNames();
      for (size_t i = 0; i < names.size(); ++i)
         if (names[i] == text) { value = static_cast<double>(i); return true; }
   }
   return false;
}

bool ParseScopeSpec(const std::string& text, ScopeSpec& spec, std::string& err)
{
   size_t i = 0;
   while (i < text.size())
   {
      while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == ';' || text[i] == '\n')) i++;
      size_t j = i;
      while (j < text.size() && text[j] != ' ' && text[j] != ',' && text[j] != ';' && text[j] != '\n') j++;
      if (j == i) break;
      const std::string tok = text.substr(i, j - i);
      const size_t eq = tok.find('=');
      double v = 0.0;
      if (eq == std::string::npos || !ScopeOptionValue(tok.substr(0, eq), tok.c_str() + eq + 1, v) ||
          !ScopeSpecSet(spec, tok.substr(0, eq), v))
      {
         err = "bad option '" + tok + "'";
         return false;
      }
      i = j;
   }
   return true;
}

void ScopeMovieDims(const ScopeSpec& spec, unsigned& w, unsigned& h, long& frames)
{
   w = h = static_cast<unsigned>(std::min(2048.0, std::max(1.0, ScopeSpecGet(spec, "size"))));
   frames = static_cast<long>(std::min(100000.0, std::max(1.0, ScopeSpecGet(spec, "frames"))));
   if (ScopeSpecGet(spec, "prepare") >= 1)
      frames = 0;   // prepare: the world and the PSF kernel only
}

namespace {

// The camera's noise maps and seeds for a RandomSeed (StackGenerationWorker).
struct NoiseSetup
{
   PixelOffsetMap offsetMap;
   PixelGainMap gainMap;
   PixelReadNoiseMap rnMap;
   uint32_t noiseSeed = 0;
   NoiseSetup(long seed, unsigned W, unsigned H, const SimulationParams& p)
   {
      std::mt19937_64 rng(static_cast<uint64_t>(seed));
      offsetMap.Generate(W, H, p.offsetAdu, p.offsetStdAdu, rng);
      gainMap.Generate(W, H, p.gainPhotonsPerAdu, p.pixelGainStdFraction, rng);
      rnMap.Generate(W, H, p.readNoiseElectrons, p.pixelReadNoiseStdFraction, rng);
      noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   }
};

} // namespace

struct ScopeSetup
{
   SimulationParams p;
   CellFieldSettings cf;
   CellFieldQuery q;
   unsigned W = 0, H = 0;
   long N = 0;
   long seed = 0;
   double expSec = 0.05, t0Sec = 0.0;
};

static ScopeSetup MakeScopeSetup(const ScopeSpec& spec)
{
   ScopeSetup S;
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };

   const long seed = static_cast<long>(O("seed"));
   unsigned W, H;
   long N;
   ScopeMovieDims(spec, W, H, N);
   const double expSec = std::max(1e-6, O("exposure-ms") / 1000.0);
   const double t0Sec = std::max(0.0, O("start-sec"));

   // Frame-equivalent parameters, as the camera's SnapshotParams().
   SimulationParams p;
   p.pixelSizeNm = O("pixel-nm");
   p.photonsPerBlink = O("photons-per-sec") * expSec;
   p.backgroundPhotons = O("background-per-sec") * expSec;
   p.psfSigmaPx = std::min(std::max(0.21 * O("wavelength-nm") / std::max(0.01, O("na")) / p.pixelSizeNm, 0.3), 20.0);
   p.quantumEfficiency = O("qe");
   p.darkCurrentElectronsPerFrame = O("dark-per-sec") * expSec;
   p.gainPhotonsPerAdu = O("gain");
   p.offsetAdu = O("offset");
   p.offsetStdAdu = O("offset-std");
   p.readNoiseElectrons = O("read-noise");
   p.pixelGainStdFraction = O("gain-std-pct") / 100.0;
   p.pixelReadNoiseStdFraction = O("read-noise-std-pct") / 100.0;
   p.frameDurationSec = expSec;

   CellFieldSettings cf;
   const double worldSeed = O("world-seed");
   cf.seed = worldSeed >= 0 ? static_cast<uint32_t>(static_cast<uint64_t>(worldSeed))
                            : static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x43454C4CULL);
   cf.cacheDir = O("disk-cache") >= 1 ? DefaultCacheDir() : std::string();
   SetPsfKernelDiskCacheDir(O("disk-cache") >= 2 ? DefaultCacheDir() : std::string());   // "" under Emscripten
   std::map<std::string, double> world = {
      { "chunkSize", O("chunk-um") }, { "density", O("occupancy") },
      { "cellDiamMin", O("cell-diam-min-um") }, { "cellDiamMax", O("cell-diam-max-um") },
      { "mtDensity", O("mt-density") }, { "labelEfficiency", O("labeling-pct-bleaching") / 100.0 },
      { "labelNonBleaching", O("labeling-pct-nonbleaching") / 100.0 },
      { "enablePacking", O("packing") != 0 ? 1.0 : 0.0 },
   };
   // p.* pass-through (known core names only), overriding the named ones.
   IscParams* probe = isc_params_new();
   for (const auto& kv : spec)
      if (kv.first.compare(0, 2, "p.") == 0 && isc_params_set(probe, kv.first.c_str() + 2, kv.second) == 0)
         world[kv.first.substr(2)] = kv.second;
   isc_params_free(probe);
   cf.params.assign(world.begin(), world.end());
   cf.activationRatePerSec = O("milli-activation-rate") / 1000.0;
   cf.onSec = std::max(1e-6, O("on-sec"));
   cf.offSec = std::max(0.0, O("off-sec"));
   cf.bleachProb = O("bleach-prob");
   cf.photonCV = O("photon-cv");

   // The camera's CellFieldQueryFor (no drift).
   const double um = p.pixelSizeNm / 1000.0, margin = 2.0;
   CellFieldQuery q;
   q.originXUm = O("x") - W * um / 2;
   q.originYUm = O("y") - H * um / 2;
   q.x0Um = q.originXUm - margin;
   q.x1Um = q.originXUm + W * um + margin;
   q.y0Um = q.originYUm - margin;
   q.y1Um = q.originYUm + H * um + margin;
   q.zRefUm = O("focus-um");
   q.zCullCentreUm = O("focus-um") + O("z");
   q.zHalfRangeUm = std::max(0.0, O("z-range-um")) / 2;
   q.frameSec = expSec;
   q.tSec = t0Sec;
   q.spanSec = N * expSec;
   q.frameIndex = 0;

   S.p = p;
   S.cf = cf;
   S.q = q;
   S.W = W;
   S.H = H;
   S.N = N;
   S.seed = seed;
   S.expSec = expSec;
   S.t0Sec = t0Sec;
   return S;
}

bool ScopePsfRequest(const ScopeSpec& spec, PsfGeneratorRequest& req, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const int model = static_cast<int>(O("psf-model"));
   if (model == 0)
      return false;
   if (model != 3)
   {
      err = "psf-model " + std::to_string(model) +
            ": only 0 (Gaussian) and 3 (GibsonLanniZernike) run here; RichardsWolf/GibsonLanni need the adapter's JVM";
      return false;
   }
   // As the camera's BuildPsfGeneratorRequest.
   req = PsfGeneratorRequest();
   req.model = PsfModelKind::GibsonLanniZernike;
   req.wavelengthNm = O("wavelength-nm");
   req.na = O("na");
   req.immersionIndex = O("immersion-index");
   req.pixelSizeNm = O("pixel-nm");
   req.oversampling = static_cast<int>(std::min(16.0, std::max(1.0, O("psf-oversampling"))));
   req.kernelHalfWidthPx =
      PsfKernelHalfWidthPx(std::min(20000.0, std::max(100.0, O("psf-kernel-half-width-nm"))), req.pixelSizeNm,
                           req.wavelengthNm, req.na);
   const double zStepUm = std::max(O("psf-z-step-um"), 0.001);
   req.nz = static_cast<int>(std::lround(std::max(0.0, O("psf-z-range-um")) / zStepUm)) + 1;
   req.zStepNm = zStepUm * 1000.0;
   req.sampleIndex = O("psf-sample-index");
   req.workingDistanceUm = O("psf-working-distance-um");
   req.sampleDepthNm = O("psf-sample-depth-nm");
   const std::vector<std::string>& presets = ZernikePresetNames();
   const int preset = static_cast<int>(O("psf-zernike-preset"));
   if (preset < 0 || preset >= static_cast<int>(presets.size()))
   {
      err = "psf-zernike-preset " + std::to_string(preset) + " out of range (0-" +
            std::to_string(presets.size() - 1) + ")";
      return false;
   }
   ZernikeCoefficients z = ZernikePresetCoefficients(presets[static_cast<size_t>(preset)]);
   for (const auto& kv : spec)
   {
      const int j = ZernikeKeyIndex(kv.first);
      if (j >= 0)
         z[static_cast<size_t>(j)] = kv.second;
   }
   req.zernikeCoefficients = FormatZernikeCoefficients(z, ',');
   req.maskType = O("psf-mask") == 1 ? PsfMaskType::DoubleHelix : PsfMaskType::None;
   req.maskModes = static_cast<int>(std::min(8.0, std::max(2.0, O("psf-mask-modes"))));
   req.maskWaist = O("psf-mask-waist");
   req.interpMode = static_cast<PsfInterpMode>(static_cast<int>(std::min(3.0, std::max(0.0, O("psf-interp")))));
   return true;
}

bool ScopePsfKernel(const ScopeSpec& spec, PsfKernelCache& cache, std::string& err)
{
   cache = PsfKernelCache();
   PsfGeneratorRequest req;
   err.clear();
   if (!ScopePsfRequest(spec, req, err))
      return err.empty(); // Gaussian: no kernel, not an error
   return ComputePsfKernelCache(req, cache, err);
}

namespace {
// Movies made by one process (the cli; the viewer's worker, one after the
// other) share one world, one WideField scene and one BrightField scene, so
// a repeat, or a new focus, frame count, PSF or noise setting, reuses the
// built cells, microtubules, dyes, dye tiles and spectra (2026-10-03: SR
// and WideField too; BrightField since 2026-10-02). The answers are the
// same as with fresh objects (every answer of the core is a pure function
// of seed, params and window; the caches are for speed only); a mutex keeps
// concurrent callers serial.
struct MovieCache
{
   std::mutex mutex;
   CellFieldSource source;
   CellFieldSettings world;
   bool haveWorld = false;
   uint64_t version = 0;
   BrightfieldScene brightfield;
   // WideField: the scene (dye tiles, kernel and plane spectra, images) and
   // the PSF it was built with; wfPsfVersion changes when the PSF object does.
   WidefieldScene widefield;
   std::unique_ptr<WidefieldPsf> wfPsf;
   PsfKernelCache wfPsfCache;
   int wfUpscale = 0;
   double wfGauss[4] = { 0, 0, 0, 0 };
   bool wfGpuMode = false, wfHasScene = false;
   long wfPsfVersion = 0;
};

MovieCache& SharedMovieCache()
{
   static MovieCache c;
   return c;
}

// Configures the shared source; bumps the version when the world changes.
bool ConfigureShared(MovieCache& c, const CellFieldSettings& cf, std::string& err)
{
   if (!c.haveWorld || !c.world.SameWorld(cf))
      ++c.version;
   c.world = cf;
   c.haveWorld = true;
   if (!c.source.Configure(cf, err))
   {
      c.haveWorld = false;
      return false;
   }
   return true;
}
} // namespace

// WideField: every labelled dye emits; a fresh sample (dose f x dD at frame
// f), square illumination over the FOV, the same PSF as SR, same noise.
struct WidefieldMovie::Impl
{
   ScopeSpec spec;
   ScopeSetup S;
   // The shared world and WideField scene (MovieCache), held for this
   // movie's lifetime: Begin locks, the destructor unlocks (the viewer's
   // isc_wf_begin .. isc_wf_end steps are one session).
   MovieCache& cache;
   std::unique_lock<std::mutex> lock;
   CellFieldSource& source;
   WidefieldSceneSpec ws;
   PsfKernelCache& psfCache;
   std::unique_ptr<WidefieldPsf>& psf;
   std::unique_ptr<SquareIllumination> ill;
   WidefieldScene& scene;
   double framesBefore0 = 0.0;
   std::chrono::steady_clock::time_point t0;
   double setupSec = 0.0;
   Impl()
      : cache(SharedMovieCache()), lock(cache.mutex, std::defer_lock), source(cache.source),
        psfCache(cache.wfPsfCache), psf(cache.wfPsf), scene(cache.widefield)
   {
   }
};

WidefieldMovie::WidefieldMovie() : impl_(new Impl) {}
WidefieldMovie::~WidefieldMovie() = default;

WidefieldScene& WidefieldMovie::Scene()
{
   return impl_->scene;
}

bool WidefieldMovie::Begin(const ScopeSpec& spec, bool gpuMode, std::string& err)
{
   Impl& m = *impl_;
   m.t0 = std::chrono::steady_clock::now();
   m.spec = spec;
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   m.S = MakeScopeSetup(spec);
   const ScopeSetup& S = m.S;
   auto tPhase = TimingClock::now();
   if (!m.lock.owns_lock() && !m.lock.try_lock())
   {
      err = "WideField: another movie is being rendered in this process";
      return false;
   }
   if (!ConfigureShared(m.cache, S.cf, err))
      return false;
   TimingLog("wf.configure", TimingSince(tPhase));
   const double um = S.p.pixelSizeNm / 1000.0;
   WidefieldSceneSpec& ws = m.ws;
   ws.originXUm = S.q.originXUm;
   ws.originYUm = S.q.originYUm;
   ws.width = S.W;
   ws.height = S.H;
   ws.pixelUm = um;
   ws.focusWorldUm = S.q.zCullCentreUm;
   ws.slabCentreUm = S.q.zCullCentreUm;
   ws.slabHalfUm = S.q.zHalfRangeUm;
   ws.grid.upscale = static_cast<int>(std::min(4.0, std::max(1.0, O("wf-upscale"))));
   ws.grid.zPlaneNm = std::min(500.0, std::max(5.0, O("wf-plane-nm")));
   ws.kernelCapUm = std::max(0.1, O("wf-kernel-um"));
   ws.phot.excitationPhotonsPerUm2PerSec = std::max(0.0, O("wf-excitation-photons-per-um2-per-sec"));
   ws.phot.quantumYield = std::min(1.0, std::max(0.0, O("wf-quantum-yield")));
   ws.phot.photonBudget = std::max(0.0, O("wf-photon-budget"));
   ws.phot.extinctionCoeff = std::max(0.0, O("wf-extinction-coeff"));
   ws.eta = WidefieldCollectionEfficiency(O("na"), O("immersion-index"));
   ws.exposureSec = S.p.frameDurationSec;
   ws.worldVersion = static_cast<long>(m.cache.version);
   m.ill.reset(new SquareIllumination(S.W * um, S.H * um));
   tPhase = TimingClock::now();
   PsfKernelCache kc;
   if (!ScopePsfKernel(spec, kc, err))
      return false;
   TimingLog("wf.psf-kernel", TimingSince(tPhase));
   if (kc.valid)
   {
      // As the camera's MakeWidefieldPsf: the upscale must divide the oversampling.
      ws.grid.upscale = KernelWidefieldPsf::ValidUpscale(kc.oversampling, ws.grid.upscale);
   }
   // The WidefieldPsf (and with it the scene's kernel spectra) is kept across
   // movies while the kernel stack (its serial) and the grid pitch, or the
   // Gaussian's parameters, are unchanged.
   MovieCache& c = m.cache;
   const double gauss[4] = { um / ws.grid.upscale, O("wavelength-nm"), O("na"), O("immersion-index") };
   const bool samePsf = c.wfPsf && c.wfPsfCache.valid == kc.valid &&
                        (kc.valid ? (c.wfPsfCache.Serial() == kc.Serial() && c.wfUpscale == ws.grid.upscale)
                                  : std::equal(gauss, gauss + 4, c.wfGauss));
   if (!samePsf)
   {
      c.wfPsfCache = kc;
      c.wfUpscale = ws.grid.upscale;
      std::copy(gauss, gauss + 4, c.wfGauss);
      if (kc.valid)
         c.wfPsf.reset(new KernelWidefieldPsf(c.wfPsfCache, ws.grid.upscale));
      else
         c.wfPsf.reset(new GaussianWidefieldPsf(gauss[0], gauss[1], gauss[2], gauss[3]));
      ++c.wfPsfVersion;
   }
   ws.psfVersion = c.wfPsfVersion;
   // The scene's FFT sizes depend on the mode (GPU: powers of two): a mode
   // change starts from a fresh scene.
   if (!c.wfHasScene || c.wfGpuMode != gpuMode)
   {
      c.widefield = WidefieldScene();
      c.widefield.SetGpuMode(gpuMode);
      c.wfGpuMode = gpuMode;
      c.wfHasScene = true;
   }
   m.scene.SetDeferImages(gpuMode);
   tPhase = TimingClock::now();
   if (!m.scene.Update(m.source, *m.ill, ws, *m.psf, err))
      return false;
   TimingLog("wf.scene-update", TimingSince(tPhase));
   // The bleach basis, anchored at the first frame (a job then has every
   // channel the frames need).
   m.framesBefore0 = std::max(0.0, O("start-sec")) / S.expSec;
   std::vector<float> wb;
   tPhase = TimingClock::now();
   m.scene.FreshBleachWeights(m.framesBefore0, wb);
   m.scene.SetBleachWeights(wb);
   TimingLog("wf.bleach-anchor", TimingSince(tPhase));
   m.setupSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.t0).count();
   TimingLog("wf.setup", m.setupSec);
   return true;
}

void WidefieldMovie::ComputeCpuImages()
{
   impl_->scene.ComputeCpuImages();
}

bool WidefieldMovie::Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                            ScopeMovieInfo& info, std::string& err)
{
   (void)err;
   Impl& m = *impl_;
   const ScopeSetup& S = m.S;
   const WidefieldSceneSpec& ws = m.ws;
   auto O = [&](const char* n) { return ScopeSpecGet(m.spec, n); };
   const SimulationParams& p = S.p;
   const unsigned W = S.W, H = S.H;
   const long N = S.N;
   // Frames that do not fit the basis re-anchor it on the CPU.
   m.scene.SetDeferImages(false);
   const double kem = ws.phot.EmissionRatePerSec(1.0);
   info.width = W;
   info.height = H;
   info.frames = N;
   info.dyes = m.scene.Dyes();
   info.halfTimeSec = ws.phot.HalfTimeSec(1.0);
   info.querySec = m.setupSec;
   char desc[768];
   std::snprintf(desc, sizeof desc,
                 "insiliscope modality=WideField seed=%ld world_seed=%u x=%g y=%g z=%g size=%u pixel_nm=%g "
                 "exposure_ms=%g start_sec=%g frames=%ld focus_um=%g dyes=%ld bleaching_dyes=%ld upscale=%d "
                 "plane_nm=%g excitation=%g qy=%g budget=%g eps=%g eta=%.4f k_em=%.4g t_half_s=%.4g "
                 "photons_per_dye_per_frame=%.4g psf=%s",
                 S.seed, S.cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, S.expSec * 1000, O("start-sec"), N,
                 O("focus-um"), m.scene.Dyes(), m.scene.BleachingDyes(), ws.grid.upscale, ws.grid.zPlaneNm,
                 ws.phot.excitationPhotonsPerUm2PerSec, ws.phot.quantumYield, ws.phot.photonBudget,
                 ws.phot.extinctionCoeff, ws.eta, kem, info.halfTimeSec, ws.eta * kem * S.expSec,
                 m.psfCache.valid ? "GibsonLanniZernike" : "Gaussian");
   info.description = desc;

   NoiseSetup noise(S.seed, W, H, p);
   const std::vector<BlinkEvent> none;
   TimingSum tWeights, tRender, tWrite, tAnchor;
   const unsigned long spawns0 = ParallelForSpawns().load();
   // Frames are independent given their bleach coefficients (counter-based
   // noise), so they render in parallel batches; a frame whose weights no
   // longer fit the anchored basis is rendered alone through RenderFrame,
   // which re-anchors exactly as the serial loop did, and the next batch
   // starts after it. Without bleaching dyes the weights are never read
   // (BleachCoefficients returns at once), so one vector serves every frame.
   const bool noBleaching = m.scene.BleachingDyes() == 0;
   std::vector<float> wb0;
   m.scene.FreshBleachWeights(m.framesBefore0, wb0);
#if defined(__EMSCRIPTEN__)
   const long batch = 1; // serial anyway: one frame of buffers
#else
   const long batch = std::max(32L, 4L * static_cast<long>(std::thread::hardware_concurrency()));
#endif
   std::vector<std::vector<float>> photons(static_cast<size_t>(std::min(batch, std::max(N, 1L))));
   std::vector<std::vector<uint16_t>> adu(photons.size());
   std::vector<std::vector<double>> coef(photons.size());
   std::vector<float> wb;
   const double zStage = O("z");
   bool more = true;
   for (long f = 0; f < N && more;)
   {
      // Frames f .. f+nb-1 fit the current basis; `misfit` says f+nb does not.
      long nb = 0;
      bool misfit = false;
      tWeights.Start();
      while (nb < batch && f + nb < N)
      {
         const std::vector<float>* use = &wb0;
         if (!noBleaching)
         {
            m.scene.FreshBleachWeights(m.framesBefore0 + (f + nb), wb);
            use = &wb;
         }
         if (!m.scene.BleachCoefficients(*use, coef[static_cast<size_t>(nb)]))
         {
            misfit = true;
            break;
         }
         ++nb;
      }
      tWeights.Stop();
      tRender.Start();
      ParallelFor(static_cast<unsigned>(nb), [&](unsigned k) {
         const long fr = f + static_cast<long>(k);
         RenderPhotonImage(photons[k], W, H, none, fr, p.pixelSizeNm, p.psfSigmaPx, p.photonsPerBlink,
                           p.backgroundPhotons, 0.0, 0.0, nullptr, zStage);
         m.scene.RenderCoefficients(coef[k], photons[k]);
         ApplyNoiseChain(photons[k], adu[k], W, H, p.Camera(), noise.offsetMap, noise.gainMap, noise.rnMap,
                         noise.noiseSeed, static_cast<uint32_t>(fr));
      });
      tRender.Stop();
      tWrite.Start();
      for (long k = 0; k < nb && more; k++)
         more = onFrame(f + k, adu[static_cast<size_t>(k)]);
      tWrite.Stop();
      f += nb;
      if (misfit && more && f < N)
      {
         // The frame that did not fit: re-anchor the basis on it (RenderFrame).
         tAnchor.Start();
         RenderPhotonImage(photons[0], W, H, none, f, p.pixelSizeNm, p.psfSigmaPx, p.photonsPerBlink,
                           p.backgroundPhotons, 0.0, 0.0, nullptr, zStage);
         m.scene.RenderFrame(wb, photons[0]);
         ApplyNoiseChain(photons[0], adu[0], W, H, p.Camera(), noise.offsetMap, noise.gainMap, noise.rnMap,
                         noise.noiseSeed, static_cast<uint32_t>(f));
         tAnchor.Stop();
         more = onFrame(f, adu[0]);
         f++;
      }
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.t0).count();
   tWeights.Log("wf.frame.bleach-weights");
   tRender.Log("wf.frame.render+noise (batches)");
   tAnchor.Log("wf.frame.re-anchor");
   tWrite.Log("wf.frame.onFrame");
   if (TimingEnabled())
   {
      char b[96];
      std::snprintf(b, sizeof b, "dyes %ld, ParallelFor spawns %lu", info.dyes, ParallelForSpawns().load() - spawns0);
      TimingLog("wf.total", info.totalSec, b);
   }
   return true;
}

bool ScopeBrightfieldSpec(const ScopeSpec& spec, BrightfieldSpec& bs, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const ScopeSetup S = MakeScopeSetup(spec);
   bs = BrightfieldSpec();
   bs.originXUm = S.q.originXUm;
   bs.originYUm = S.q.originYUm;
   bs.width = S.W;
   bs.height = S.H;
   bs.pixelUm = S.p.pixelSizeNm / 1000.0;
   bs.quality = static_cast<int>(std::min(4.0, std::max(1.0, O("bf-quality"))));
   bs.sources = static_cast<int>(std::min(1024.0, std::max(0.0, O("bf-sources"))));
   bs.upscale = static_cast<int>(std::min(8.0, std::max(0.0, O("bf-upscale"))));
   bs.sub = static_cast<int>(std::min(16.0, std::max(0.0, O("bf-sub"))));
   bs.sliceUm = O("bf-slice-um") < 0 ? -1.0 : std::max(0.0, O("bf-slice-um"));
   bs.marginUm = std::max(0.0, O("bf-margin-um"));
   bs.condenserNa = std::max(0.0, O("bf-condenser-na"));
   bs.wavelengthNm = std::max(1.0, O("bf-wavelength-nm"));
   bs.na = std::max(0.01, O("na"));
   bs.nMedium = O("bf-n-medium");
   bs.nCytoplasm = O("bf-n-cytoplasm");
   bs.nNucleus = O("bf-n-nucleus");
   bs.nMicrotubule = O("bf-n-microtubule");
   bs.absorptionPerUm = std::max(0.0, O("bf-absorption-per-um"));
   bs.zernike = ZeroZernikeCoefficients();
   if (O("bf-aberrations") != 0)
   {
      PsfGeneratorRequest req;
      std::string e;
      if (ScopePsfRequest(spec, req, e))
      {
         bool ok = false;
         bs.zernike = ParseZernikeCoefficients(req.zernikeCoefficients, ok);
      }
      else if (!e.empty())
      {
         err = e;
         return false;
      }
   }
   return true;
}

// The frames of a BrightField movie from its scene: the image at the spec's
// focus (computed, or assembled by SetImageFromSources), times the lamp,
// then the camera noise per frame.
static bool BrightfieldFrames(const ScopeSpec& spec, const ScopeSetup& S, const BrightfieldSpec& bs,
                              BrightfieldScene& scene, std::chrono::steady_clock::time_point t0,
                              unsigned long spawns0,
                              const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                              ScopeMovieInfo& info, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const SimulationParams& p = S.p;
   std::vector<float> trans;
   const double focusUm = S.q.zCullCentreUm;
   auto tPhase = TimingClock::now();
   if (!scene.Image(focusUm, trans, err))
      return false;
   TimingLog("bf.image", TimingSince(tPhase));
   const unsigned W = S.W, H = S.H;
   const long N = S.N;
   const double flux = std::max(0.0, O("bf-photons-per-px-per-sec")) * S.expSec;
   const BrightfieldQuality q = bs.Resolved();
   info.width = W;
   info.height = H;
   info.frames = N;
   info.querySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   char desc[768];
   std::snprintf(desc, sizeof desc,
                 "insiliscope modality=BrightField seed=%ld world_seed=%u x=%g y=%g z=%g size=%u pixel_nm=%g "
                 "exposure_ms=%g frames=%ld focus_um=%g quality=%d sources=%d upscale=%d sub=%d slices=%d grid=%ux%u "
                 "na=%g condenser_na=%g lambda_nm=%g n_medium=%g n_cytoplasm=%g n_nucleus=%g n_microtubule=%g "
                 "absorption_per_um=%g photons_per_px=%.4g setup_ms=%.0f image_ms=%.0f",
                 S.seed, S.cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, S.expSec * 1000, N, focusUm, bs.quality,
                 scene.Sources(), q.upscale, q.sub, scene.Slices(), scene.GridNx(), scene.GridNy(), bs.na,
                 bs.condenserNa, bs.wavelengthNm, bs.nMedium, bs.nCytoplasm, bs.nNucleus, bs.nMicrotubule,
                 bs.absorptionPerUm, flux, scene.SetupMs(), scene.LastImageMs());
   info.description = desc;
   NoiseSetup noise(S.seed, W, H, p);
   std::vector<float> photons(trans.size());
   for (size_t i = 0; i < trans.size(); ++i)
      photons[i] = static_cast<float>(trans[i] * flux);
   std::vector<uint16_t> adu;
   TimingSum tNoise, tWrite;
   for (long f = 0; f < N; f++)
   {
      tNoise.Start();
      ApplyNoiseChain(photons, adu, W, H, p.Camera(), noise.offsetMap, noise.gainMap, noise.rnMap, noise.noiseSeed,
                      static_cast<uint32_t>(f));
      tNoise.Stop();
      tWrite.Start();
      const bool more = onFrame(f, adu);
      tWrite.Stop();
      if (!more)
         break;
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   tNoise.Log("bf.frame.noise");
   tWrite.Log("bf.frame.onFrame");
   if (TimingEnabled())
   {
      char b[96];
      std::snprintf(b, sizeof b, "ParallelFor spawns %lu", ParallelForSpawns().load() - spawns0);
      TimingLog("bf.total", info.totalSec, b);
   }
   return true;
}

bool RenderBrightfieldMovie(const ScopeSpec& spec,
                            const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                            ScopeMovieInfo& info, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   const ScopeSetup S = MakeScopeSetup(spec);
   MovieCache& cache = SharedMovieCache();
   std::lock_guard<std::mutex> lock(cache.mutex);
   if (!ConfigureShared(cache, S.cf, err))
      return false;
   BrightfieldSpec bs;
   if (!ScopeBrightfieldSpec(spec, bs, err))
      return false;
   const unsigned long spawns0 = ParallelForSpawns().load();
   const auto tPhase = TimingClock::now();
   if (!cache.brightfield.Update(cache.source, bs, cache.version, err))
      return false;
   TimingLog("bf.scene-update", TimingSince(tPhase));
   return BrightfieldFrames(spec, S, bs, cache.brightfield, t0, spawns0, onFrame, info, err);
}

struct BrightfieldMovie::Impl
{
   ScopeSpec spec;
   ScopeSetup S;
   BrightfieldSpec bs;
   MovieCache* cache = nullptr;           // Begin: the shared scene, under lock
   std::unique_lock<std::mutex> lock;
   BrightfieldScene own;                  // BeginFromPhase: a scene of this object alone
   BrightfieldScene* scene = nullptr;
   std::chrono::steady_clock::time_point t0;
   unsigned long spawns0 = 0;
};

BrightfieldMovie::BrightfieldMovie() : impl_(new Impl) {}
BrightfieldMovie::~BrightfieldMovie() = default;

bool BrightfieldMovie::Begin(const ScopeSpec& spec, bool deferSources, std::string& err)
{
   Impl& m = *impl_;
   m.t0 = std::chrono::steady_clock::now();
   m.spec = spec;
   m.S = MakeScopeSetup(spec);
   if (!ScopeBrightfieldSpec(spec, m.bs, err))
      return false;
   m.cache = &SharedMovieCache();
   m.lock = std::unique_lock<std::mutex>(m.cache->mutex);
   if (!ConfigureShared(*m.cache, m.S.cf, err))
      return false;
   m.spawns0 = ParallelForSpawns().load();
   const auto tPhase = TimingClock::now();
   if (!m.cache->brightfield.Update(m.cache->source, m.bs, m.cache->version, deferSources, err))
      return false;
   TimingLog("bf.scene-update", TimingSince(tPhase));
   m.scene = &m.cache->brightfield;
   return true;
}

bool BrightfieldMovie::BeginFromPhase(const ScopeSpec& spec, int slices, double zTopUm, double objectZUm,
                                      const std::vector<float>& phase, const std::vector<float>& atten,
                                      std::string& err)
{
   Impl& m = *impl_;
   m.t0 = std::chrono::steady_clock::now();
   m.spec = spec;
   m.S = MakeScopeSetup(spec);
   if (!ScopeBrightfieldSpec(spec, m.bs, err))
      return false;
   if (!m.own.UpdateFromPhase(m.bs, slices, zTopUm, objectZUm, phase, atten, true, err))
      return false;
   m.scene = &m.own;
   return true;
}

unsigned BrightfieldMovie::Width() const { return impl_->S.W; }
unsigned BrightfieldMovie::Height() const { return impl_->S.H; }
long BrightfieldMovie::Frames() const { return impl_->S.N; }
int BrightfieldMovie::Sources() const { return impl_->scene ? impl_->scene->Sources() : 0; }
int BrightfieldMovie::Slices() const { return impl_->scene ? impl_->scene->Slices() : 0; }
unsigned BrightfieldMovie::GridNx() const { return impl_->scene ? impl_->scene->GridNx() : 0; }
unsigned BrightfieldMovie::GridNy() const { return impl_->scene ? impl_->scene->GridNy() : 0; }
double BrightfieldMovie::ZTopUm() const { return impl_->scene ? impl_->scene->ZTopUm() : 0.0; }
double BrightfieldMovie::ObjectZUm() const { return impl_->scene ? impl_->scene->ObjectZUm() : 0.0; }
const std::vector<float>& BrightfieldMovie::Phase() const
{
   static const std::vector<float> none;
   return impl_->scene ? impl_->scene->Phase() : none;
}
const std::vector<float>& BrightfieldMovie::Atten() const
{
   static const std::vector<float> none;
   return impl_->scene ? impl_->scene->Atten() : none;
}

bool BrightfieldMovie::SourceImage(int s, std::vector<float>& out, std::string& err)
{
   if (!impl_->scene)
   {
      err = "BrightField: not begun.";
      return false;
   }
   return impl_->scene->SourceImageAt(impl_->S.q.zCullCentreUm, s, out, err);
}

bool BrightfieldMovie::SetSourceImages(const float* slots)
{
   return impl_->scene && impl_->scene->SetImageFromSources(impl_->S.q.zCullCentreUm, slots);
}

bool BrightfieldMovie::ImageCached() const
{
   return impl_->scene && impl_->scene->HasImage(impl_->S.q.zCullCentreUm);
}

bool BrightfieldMovie::Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                              ScopeMovieInfo& info, std::string& err)
{
   Impl& m = *impl_;
   if (!m.scene)
   {
      err = "BrightField: not begun.";
      return false;
   }
   return BrightfieldFrames(m.spec, m.S, m.bs, *m.scene, m.t0, m.spawns0, onFrame, info, err);
}

namespace {

void AppendNum(std::string& s, double v)
{
   char b[32];
   std::snprintf(b, sizeof b, "%.4f", v);
   s += b;
}

} // namespace

bool ScopeGeometryJson(const ScopeSpec& spec, double sizeUm, bool detail, std::string& json, std::string& err)
{
   const ScopeSetup S = MakeScopeSetup(spec);
   CellFieldSource source;
   if (!source.Configure(S.cf, err))
      return false;
   IscWorld* w = source.World();
   const double cx = ScopeSpecGet(spec, "x"), cy = ScopeSpecGet(spec, "y"), h = std::max(0.1, sizeUm) / 2;
   const double x0 = cx - h, y0 = cy - h, x1 = cx + h, y1 = cy + h;
   std::vector<double> cells(static_cast<size_t>(ISC_CELL_STRIDE) * 256);
   int32_t n = isc_cells_in_window(w, x0, y0, x1, y1, cells.data(), 256);
   if (n > 256)
   {
      cells.resize(static_cast<size_t>(ISC_CELL_STRIDE) * n);
      n = isc_cells_in_window(w, x0, y0, x1, y1, cells.data(), n);
   }
   if (n < 0)
   {
      err = "cell query failed";
      return false;
   }
   json.clear();
   json += "{\"x0\":";
   AppendNum(json, x0);
   json += ",\"y0\":";
   AppendNum(json, y0);
   json += ",\"x1\":";
   AppendNum(json, x1);
   json += ",\"y1\":";
   AppendNum(json, y1);
   json += ",\"cells\":[";
   std::vector<double> buf;
   std::vector<int32_t> lens;
   for (int32_t i = 0; i < n; ++i)
   {
      const double* c = cells.data() + static_cast<size_t>(i) * ISC_CELL_STRIDE;
      // cx, cy, x, y, packRot, rOuter, height, nucOffX, nucOffY, nucRot, nucLong, nucShort, nucHeight, nucZ
      const int32_t ccx = static_cast<int32_t>(c[0]), ccy = static_cast<int32_t>(c[1]);
      const double px = c[2], py = c[3], rot = c[4], cr = std::cos(rot), sr = std::sin(rot);
      auto toWorld = [&](double lx, double ly, double& wx, double& wy) {
         wx = px + lx * cr - ly * sr;
         wy = py + lx * sr + ly * cr;
      };
      double wx, wy;
      if (i)
         json += ',';
      json += "{\"x\":";
      AppendNum(json, px);
      json += ",\"y\":";
      AppendNum(json, py);
      json += ",\"height\":";
      AppendNum(json, c[6]);
      // Footprint outline.
      const int32_t no = isc_cell_outline(w, ccx, ccy, nullptr, 0);
      buf.assign(static_cast<size_t>(std::max(0, no)) * 2, 0.0);
      if (no > 0)
         isc_cell_outline(w, ccx, ccy, buf.data(), no);
      json += ",\"outline\":[";
      for (int32_t k = 0; k < no; ++k)
      {
         toWorld(buf[2 * k], buf[2 * k + 1], wx, wy);
         json += k ? ",[" : "[";
         AppendNum(json, wx);
         json += ',';
         AppendNum(json, wy);
         json += ']';
      }
      json += "],\"nucleus\":{\"x\":";
      toWorld(c[7], c[8], wx, wy);
      AppendNum(json, wx);
      json += ",\"y\":";
      AppendNum(json, wy);
      json += ",\"rot\":";
      AppendNum(json, c[9] + rot);
      json += ",\"long\":";
      AppendNum(json, c[10]);
      json += ",\"short\":";
      AppendNum(json, c[11]);
      json += ",\"height\":";
      AppendNum(json, c[12]);
      json += ",\"z\":";
      AppendNum(json, c[13]);
      json += '}';
      if (detail)
      {
         // Cytoplasm height mesh: (rings+1) x n vertices x, y, h.
         int32_t dims[2] = { 0, 0 };
         int32_t nv = isc_cell_mesh(w, ccx, ccy, dims, nullptr, 0);
         buf.assign(static_cast<size_t>(std::max(0, nv)) * 3, 0.0);
         if (nv > 0)
            nv = isc_cell_mesh(w, ccx, ccy, dims, buf.data(), nv);
         json += ",\"mesh\":{\"rings\":" + std::to_string(dims[0]) + ",\"n\":" + std::to_string(dims[1]) + ",\"v\":[";
         for (int32_t k = 0; k < nv; ++k)
         {
            toWorld(buf[3 * k], buf[3 * k + 1], wx, wy);
            json += k ? ",[" : "[";
            AppendNum(json, wx);
            json += ',';
            AppendNum(json, wy);
            json += ',';
            AppendNum(json, buf[3 * k + 2]);
            json += ']';
         }
         json += "]}";
         // Microtubule centrelines.
         int32_t totalPts = 0;
         const int32_t nMt = isc_cell_microtubules(w, ccx, ccy, nullptr, 0, nullptr, 0, &totalPts);
         buf.assign(static_cast<size_t>(std::max(0, totalPts)) * 3, 0.0);
         lens.assign(static_cast<size_t>(std::max(0, nMt)), 0);
         if (nMt > 0)
            isc_cell_microtubules(w, ccx, ccy, buf.data(), totalPts, lens.data(), nMt, &totalPts);
         json += ",\"mts\":[";
         size_t p = 0;
         for (int32_t m = 0; m < nMt; ++m)
         {
            json += m ? ",[" : "[";
            for (int32_t k = 0; k < lens[static_cast<size_t>(m)]; ++k, ++p)
            {
               toWorld(buf[3 * p], buf[3 * p + 1], wx, wy);
               json += k ? ",[" : "[";
               AppendNum(json, wx);
               json += ',';
               AppendNum(json, wy);
               json += ',';
               AppendNum(json, buf[3 * p + 2]);
               json += ']';
            }
            json += ']';
         }
         json += ']';
      }
      json += '}';
   }
   json += "]}";
   return true;
}

// prepare=1: the shared world (MovieCache) and, for SR/WideField, the PSF
// kernel (ComputePsfKernelCache's memo and, with disk-cache 2, its file), so
// a movie that follows finds both ready. No frames.
static bool PrepareScope(const ScopeSpec& spec, ScopeMovieInfo& info, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   const ScopeSetup S = MakeScopeSetup(spec);
   MovieCache& cache = SharedMovieCache();
   std::lock_guard<std::mutex> lock(cache.mutex);
   if (!ConfigureShared(cache, S.cf, err))
      return false;
   // The FOV's cells: packs (or takes from the block store) the blocks the
   // movie's query will touch. Assets and dyes stay with the movie (they
   // depend on its z range and kinetics).
   if (isc_cells_in_window(cache.source.World(), S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, nullptr, 0) < 0)
   {
      err = "cell-field query failed";
      return false;
   }
   const double tWorld = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   if (ScopeSpecGet(spec, "modality") != 2)
   {
      PsfKernelCache kernel;
      if (!ScopePsfKernel(spec, kernel, err))
         return false;
   }
   info = ScopeMovieInfo();
   info.width = S.W;
   info.height = S.H;
   info.frames = 0;
   info.querySec = tWorld;
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   info.description = "prepared";
   return true;
}

bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   if (O("prepare") >= 1)
      return PrepareScope(spec, info, err);
   const auto t0 = std::chrono::steady_clock::now();
   if (O("modality") == 1)
   {
      WidefieldMovie wm;
      return wm.Begin(spec, false, err) && wm.Render(onFrame, info, err);
   }
   if (O("modality") == 2)
      return RenderBrightfieldMovie(spec, onFrame, info, err);
   const ScopeSetup S = MakeScopeSetup(spec);
   const SimulationParams& p = S.p;
   const CellFieldSettings& cf = S.cf;
   const CellFieldQuery& q = S.q;
   const unsigned W = S.W, H = S.H;
   const long N = S.N, seed = S.seed;
   const double expSec = S.expSec, t0Sec = S.t0Sec;
   std::vector<BlinkEvent> events;
   const unsigned long spawns0 = ParallelForSpawns().load();
   auto tPhase = TimingClock::now();
   // The shared world (MovieCache): a repeat movie, or one with other
   // imaging settings, reuses the built cells, microtubules and dyes.
   MovieCache& cache = SharedMovieCache();
   std::lock_guard<std::mutex> lock(cache.mutex);
   if (!ConfigureShared(cache, cf, err))
      return false;
   CellFieldSource& source = cache.source;
   TimingLog("sr.configure", TimingSince(tPhase));
   // The PSF kernel computes while the cell field is queried (as the
   // camera's stack generation does); serially under Emscripten.
   PsfKernelCache psfCache;
   std::string psfErr;
   bool psfOk = true;
   double psfSec = 0.0, eventsSec = 0.0;
   tPhase = TimingClock::now();
#if defined(__EMSCRIPTEN__)
   psfOk = ScopePsfKernel(spec, psfCache, psfErr);
   psfSec = TimingSince(tPhase);
   const auto tEv = TimingClock::now();
   const bool eventsOk = psfOk && source.Events(q, events);
   eventsSec = TimingSince(tEv);
#else
   std::thread psfThread([&]() {
      const auto tk = TimingClock::now();
      psfOk = ScopePsfKernel(spec, psfCache, psfErr);
      psfSec = TimingSince(tk);
   });
   const bool eventsOk = source.Events(q, events);
   eventsSec = TimingSince(tPhase);
   psfThread.join();
#endif
   TimingLog("sr.psf-kernel", psfSec, psfCache.valid ? "GibsonLanniZernike" : "Gaussian");
   TimingLog("sr.events-query", eventsSec);
   TimingLog("sr.psf+query-wall", TimingSince(tPhase));
   if (!psfOk)
   {
      err = psfErr;
      return false;
   }
   const PsfKernelCache* kernel = psfCache.valid ? &psfCache : nullptr;
   if (!eventsOk)
   {
      err = "cell-field event query failed";
      return false;
   }
   info.width = W;
   info.height = H;
   info.frames = N;
   info.blinks = events.size();
   info.querySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   char desc[512];
   std::snprintf(desc, sizeof desc,
                 "insiliscope seed=%ld world_seed=%u x=%g y=%g z=%g size=%u pixel_nm=%g exposure_ms=%g start_sec=%g "
                 "frames=%ld focus_um=%g activation_rate=%g on_sec=%g off_sec=%g bleach_prob=%g photons_per_sec=%g "
                 "photon_cv=%g psf=%s",
                 seed, cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, expSec * 1000, t0Sec, N, O("focus-um"),
                 cf.activationRatePerSec, cf.onSec, cf.offSec, cf.bleachProb, O("photons-per-sec"), cf.photonCV,
                 kernel ? "GibsonLanniZernike" : "Gaussian");
   info.description = desc;

   // Same streams as the camera's StackGenerationWorker: maps off
   // mt19937_64(seed), counter-based noise on seed ^ 0x9E3779B9.
   std::mt19937_64 rng(static_cast<uint64_t>(seed));
   PixelOffsetMap offsetMap;
   offsetMap.Generate(W, H, p.offsetAdu, p.offsetStdAdu, rng);
   PixelGainMap gainMap;
   gainMap.Generate(W, H, p.gainPhotonsPerAdu, p.pixelGainStdFraction, rng);
   PixelReadNoiseMap rnMap;
   rnMap.Generate(W, H, p.readNoiseElectrons, p.pixelReadNoiseStdFraction, rng);
   const uint32_t noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   tPhase = TimingClock::now();
   const std::vector<std::vector<uint32_t>> buckets = BucketEventsByFrame(events, N);
   TimingLog("sr.noise-maps+buckets", TimingSince(tPhase));
   TimingSum tRender, tWrite;

   // Frames are independent (own events, counter-based noise), so a batch
   // is made on all cores (serial under Emscripten) and handed over in order.
   const double zStage = O("z");
   // A batch of frames per ParallelFor: a few frames per core, so the idle
   // tail of a batch is a small share (serial under Emscripten: one at a time).
#if defined(__EMSCRIPTEN__)
   const long batch = 1;
#else
   const long batch = std::max(32L, 4L * static_cast<long>(std::thread::hardware_concurrency()));
#endif
   const size_t slots = static_cast<size_t>(std::min(batch, std::max(N, 1L)));
   std::vector<std::vector<uint16_t>> adu(slots);
   std::vector<std::vector<float>> photons(slots);   // per-slot buffers, kept across batches
   std::vector<std::vector<BlinkEvent>> fe(slots);
   for (long f0 = 0; f0 < N; f0 += batch)
   {
      const long nb = std::min(batch, N - f0);
      tRender.Start();
      ParallelFor(static_cast<unsigned>(nb), [&](unsigned k) {
         const long f = f0 + static_cast<long>(k);
         fe[k].clear();
         for (uint32_t i : buckets[static_cast<size_t>(f)])
            fe[k].push_back(events[i]);
         RenderPhotonImage(photons[k], W, H, fe[k], f, p.pixelSizeNm, p.psfSigmaPx, p.photonsPerBlink,
                           p.backgroundPhotons, 0.0, 0.0, kernel, zStage);
         ApplyNoiseChain(photons[k], adu[k], W, H, p.Camera(), offsetMap, gainMap, rnMap, noiseSeed,
                         static_cast<uint32_t>(f));
      });
      tRender.Stop();
      tWrite.Start();
      bool more = true;
      for (long k = 0; k < nb && more; k++)
         more = onFrame(f0 + k, adu[static_cast<size_t>(k)]);
      tWrite.Stop();
      if (!more)
         break;
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   tRender.Log("sr.render-batches");
   tWrite.Log("sr.onFrame");
   if (TimingEnabled())
   {
      char b[96];
      std::snprintf(b, sizeof b, "blinks %zu, ParallelFor spawns %lu", events.size(),
                    ParallelForSpawns().load() - spawns0);
      TimingLog("sr.total", info.totalSec, b);
   }
   return true;
}

} // namespace sim
