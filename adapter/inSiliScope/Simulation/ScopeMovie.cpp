///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See ScopeMovie.h. The twin of web/prototype/scope/
//                scope_movie.js (options, setup) and fluorescence.js (the
//                fluorescence movie), issue 16.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "ScopeMovie.h"
#include "ScopeResolved.h"

#include "CacheDir.h"

#include "BrightfieldRender.h"
#include "DyeLibrary.h"
#include "Illumination.h"
#include "LightPath.h"
#include "Parallel.h"
#include "Spectra.h"
#include "Timing.h"

#include "CellFieldSource.h"
#include "Drift.h"
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

namespace {

// The default 640 nm intensity, the DNA-PAINT presets' 1 kW/cm^2 (estimate; was
// 0.1607 until 2026-10-05, what gave ATTO 655 the single-dye default's 6375
// photoelectrons/s while ON; scope_movie.js DEFAULT_LASER_640_KW, issue 16).
constexpr double kDefaultLaser640KW = 1.0;
// k_on 1e6 /M/s x 1 nM = 1e-3 bindings per site per second (scope_movie.js
// DEFAULT_IMAGER_NM; *estimate*, 2026-10-07: was 1.43, the former default
// activation rate; 1 nM gives about the emitters per frame of the dSTORM and
// PALM typical labels).
constexpr double kDefaultImagerNm = 1.0;

// The structures' option prefixes: the targets of data/specimens.json (JS world.js STRUCTURES), index = ISC_STRUCT_*.
const char* StructurePrefix(int s)
{
   return TargetAt(s).prefix;
}

struct OptionTable
{
   std::vector<ScopeOption> opts;
   std::vector<std::string> strings;   // storage of the generated names and help texts
};

const OptionTable& Options()
{
   static const OptionTable t = [] {
      OptionTable o;
      o.strings.reserve(64);
      auto keep = [&](const std::string& s) {
         o.strings.push_back(s);
         return o.strings.back().c_str();
      };
      auto idx = [](const std::vector<std::string>& list, const char* id) { return static_cast<double>(IndexOf(list, id)); };
      std::vector<ScopeOption>& v = o.opts;
      v = {
         { "seed", 42, "Hub.RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)" },
         { "world-seed", -1, "cell-field world seed used as is (the viewer's seed); -1 = derive it from seed" },
         { "disk-cache", 1, "per-user cache on disk ($ISC_CACHE_DIR, else %LOCALAPPDATA%/inSiliScope/cache or ~/.cache/insiliscope): 0 = none, 1 = the packed cell positions (a few MB: a rerun with the same seed and cell parameters packs nothing), 2 = also the PSF kernel (one file, up to ~200 MB)" },
         { "prepare", 0, "1 = build the world and the PSF kernel only (warms the caches), no frames" },
         { "x", 0, "FOV centre x, world um (XY stage position)" },
         { "y", 0, "FOV centre y, world um" },
         { "z", 0.5, "Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)" },
         { "size", 128, "FOV width = height, pixels" },
         { "frames", 1000, "number of frames" },
         { "exposure-ms", 50, "frame duration, ms (simulated time per frame)" },
         { "start-sec", 60, "simulated time of the first frame after the illumination starts, s (60: past the dSTORM initial ON phase, near steady state)" },
         { "drift-xy-speed-nm-per-sec", 0, "SampleHolder.DriftXySpeedNmPerSec: directed sample drift, mean xy speed, nm/s (0 = none)" },
         { "drift-z-speed-nm-per-sec", 0, "SampleHolder.DriftZSpeedNmPerSec: directed sample drift, mean z speed, nm/s (its direction: drift-z-direction)" },
         { "drift-xy-angle-deg", -1, "SampleHolder.DriftXyAngleDeg: direction of the xy drift, deg from +x (-1 = random per seed; advanced)" },
         { "drift-z-direction", 0, "SampleHolder.DriftZDirection: direction of the z drift, 1 = away from the coverslip, -1 = towards it, 0 = random per seed (advanced)" },
         { "drift-xy-angle-wander-deg", 180, "SampleHolder.DriftXyAngleWanderDeg: the xy direction swings slowly within +/- this, deg (180 = any direction; advanced)" },
         { "drift-z-angle-wander-deg", 90, "SampleHolder.DriftZAngleWanderDeg: the z drift swings within +/- this, deg: speed x cos(angle), 90 = between full speed and still, 180 = also back (advanced)" },
         { "drift-speed-wander-pct", 0, "SampleHolder.DriftSpeedWanderPct: how much the xy and z drift strengths fluctuate, % RMS of the mean (advanced)" },
         { "drift-wander-time-sec", 60, "SampleHolder.DriftWanderTimeSec: how slowly direction and strength wander (correlation time), s (advanced)" },
         { "drift-xy-nm-per-sqrt-sec", 0, "SampleHolder.DriftXyNmPerSqrtSec: random-walk drift on top, RMS nm per axis after 1 s (advanced)" },
         { "drift-z-nm-per-sqrt-sec", 0, "SampleHolder.DriftZNmPerSqrtSec: random-walk drift in z, RMS nm after 1 s (advanced)" },
         { "pixel-nm", 100, "pixel size, nm" },
         { "background-per-sec", 0, "SampleHolder.BackgroundPhotonsPerSec (photons/pixel/s at the camera, x the QE at the emission filter centre)" },
         { "na", 1.4, "Objective.NA: numerical aperture" },
         { "focus-um", 0, "CellField.FocusHeightUm (focus offset added to z)" },
         { "z-range-um", 7.0, "CellField.ZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)" },
         { "specimen", 0, "SampleHolder: the mounted specimen (0 = CellField; data/specimens.json; names accepted)" },
         { "mode", -1, "Fluorophores Mode: the experiment's label mode, for every target whose <prefix>-mode is -2 (Global): 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField; -1 None (each target's own; names accepted)" },
         { "mt-dye", idx(DyeChoices(), "ATTO655"), "CellField Microtubules_Label: a library dye or Dye1..Dye3; -1 Typical = the microtubules' typical dye in their mode (data/dyes/library.json typicalLabels; names accepted)" },
         { "mt-mode", -1, "CellField Microtubules_Mode: -1 DyeDefault = the dye's default (Typical: the library's default mode), -2 Global = the mode option, 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField (names accepted)" },
         { "mt-label-pct", -1, "CellField Microtubules_LabelingPct: % of the binding sites (13 x 8 nm lattice, 1625 /um) that carry a label; -1 = the target's typical % in its mode (data/dyes/library.json typicalLabels: DNA-PAINT 70, dSTORM 3, PALM 25, WideField 70)" },
         { "mt-imager-nm", kDefaultImagerNm, "CellField.Microtubules_ImagerNm: DNA-PAINT imager concentration, nM (binding rate k_on x c; the free imager adds a uniform background -- taken as constant: no depletion by binding or bleaching, no exclusion from cells)" },
         { "mt-orient", 0, "CellField.Microtubules_Orientation: 0 Free (isotropic), 1 Fixed, 2 Random (no effect on the image yet)" },
         { "mt-orient-polar-deg", 90, "CellField.Microtubules_OrientPolarDeg: Fixed dipole angle from the microtubule axis" },
         { "mt-orient-azimuth-deg", 0, "CellField.Microtubules_OrientAzimuthDeg: Fixed dipole azimuth about the axis, from the radial direction" },
         { "mt-wobble-deg", 0, "CellField.Microtubules_WobbleConeDeg: fast wobble cone half-angle (Fixed, Random)" },
         { "mt-motion", 0, "CellField.Microtubules_Motion: 0 Static (single-particle tracking: future)" },
         { "dye1.source", idx(DyeIds(), "AF647"), "Fluorophores.Dye1_Source: library dye of slot 1 (dye1.<field> overrides it)" },
         { "dye2.source", idx(DyeIds(), "mEos3.2"), "Fluorophores.Dye2_Source" },
         { "dye3.source", idx(DyeIds(), "mEGFP"), "Fluorophores.Dye3_Source" },
      };
      for (int nm : LaserLines())
         v.push_back({ keep("laser-" + std::to_string(nm)), nm == 640 ? kDefaultLaser640KW : 0.0,
                       keep("Lasers.Laser" + std::to_string(nm) + "KWcm2: " + std::to_string(nm) +
                            " nm laser intensity at the sample, kW/cm^2 (0 = off)") });
      const std::vector<ScopeOption> rest = {
         { "laser-custom-nm", 0, "wavelength of an extra laser line, nm (0 = none; not in Micro-Manager)" },
         { "laser-custom", 0, "its intensity, kW/cm^2" },
         { "light-preset", -1, "Lasers.Preset: -1 = none (the laser/dichroic/filter options as given); auto = the light preset of the first structure's dye in its mode; or a preset name/index (data/dyes/light_path.json presets). A preset sets every laser-*, ex-filter, dichroic and em-filter the spec does not give." },
         { "illum-geometry", 0, "Lasers.IlluminationGeometry: 0 Epi (TIRF/HILO: future)" },
         { "chamber-height-um", 5, "Lasers.ChamberHeightUm: imager solution depth that adds to the DNA-PAINT background (Epi: the whole chamber; small by default, standing in for HILO/TIRF)" },
         { "ex-filter", idx(ExcitationFilterIds(), DefaultExcitationFilter()), "ExcitationFilter: laser clean-up filter in front of the dichroic; each laser line is scaled by its transmission there (names accepted)" },
         { "ex-lo-nm", 635, "ExcitationFilter Custom band pass, low edge" },
         { "ex-hi-nm", 645, "ExcitationFilter Custom band pass, high edge" },
         { "dichroic", idx(DichroicIds(), DefaultDichroic()), "Dichroic: reflects the lasers (R = 1 - T), transmits the emission (names accepted)" },
         { "dichroic-edge-nm", 650, "Dichroic.CustomEdgeNm: the Custom dichroic's long-pass edge" },
         { "em-filter", idx(EmissionFilterIds(), DefaultEmissionFilter()), "EmissionFilter (names accepted)" },
         { "em-lo-nm", 657.5, "EmissionFilter.CustomLoNm: Custom band pass, low edge" },
         { "em-hi-nm", 694.5, "EmissionFilter.CustomHiNm: Custom band pass, high edge" },
         { "chunk-um", 26, "CellField.ChunkSizeUm" },
         { "occupancy", 0.33, "CellField.Occupancy" },
         { "cell-diam-min-um", 25, "CellField.CellDiameterMinUm" },
         { "cell-diam-max-um", 35, "CellField.CellDiameterMaxUm" },
         { "mt-density", 0.9, "CellField.MicrotubuleDensityPerUm2" },
         { "packing", 1, "CellField.Packing (1 on, 0 off)" },
         { "camera-preset", idx(CameraIds(), DefaultCamera()), "Camera.CameraPreset: Kinetix22, iXonUltra897, Custom (names accepted; data/dyes/cameras.json)" },
         { "qe-curve", -1, "Camera.QeCurve: QE(lambda) of a camera (index/name), -1 = the preset's, Custom = flat at qe" },
         { "qe", 0.85, "Camera.QuantumEfficiency (the flat QE of the Custom curve)" },
         { "camera-type", -1, "Camera.CameraType: -1 = the preset's, 0 sCMOS, 1 EMCCD" },
         { "dark-per-sec", 1.03, "Camera.DarkCurrentElectronsPerSec" },
         { "gain", 0.25, "Camera.GainElectronsPerADU (electrons per ADU)" },
         { "offset", 100, "Camera.OffsetADU" },
         { "offset-std", 0.5, "Camera.OffsetStdADU" },
         { "read-noise", 1.2, "Camera.ReadNoiseElectrons" },
         { "gain-std-pct", 0.5, "Camera.sCMOS_GainStdPctPerPixel (per-pixel gain spread, PRNU)" },
         { "read-noise-std-pct", 20, "Camera.sCMOS_ReadNoiseStdPctPerPixel" },
         { "em-gain", -1, "Camera.EMCCD_EmGain (EMCCD): -1 = the pre-amplifier sensitivity of the camera preset (1 e-/ADU when it has none) / gain, as the viewer and Micro-Manager derive it; > 0 sets it" },
         { "cic", 0.002, "Camera.EMCCD_CicElectrons (EMCCD clock-induced charge, e-/pixel/frame)" },
         { "bit-depth", 16, "Camera.BitDepth (EMCCD)" },
         { "modality", 0, "0 = Fluorescence (every label in its mode), 1 = BrightField (transmitted light; names accepted); the shorthand for light-epi / light-trans (Micro-Manager: the Lasers and TransmittedLamp shutters)" },
         { "light-epi", -1, "Lasers shutter: 1 open, 0 closed, -1 = from modality (open in Fluorescence)" },
         { "light-trans", -1, "TransmittedLamp shutter: 1 open, 0 closed, -1 = from modality (open in BrightField). Both open: fluorescence + BrightField through one camera; none: dark frames" },
         { "wf-upscale", 1, "Renderer.WideFieldUpscaling: mean-field grid cells per pixel, per axis (1-4)" },
         { "wf-plane-nm", 25, "Renderer.WideFieldZPlaneNm: mean-field dye plane thickness, nm" },
         { "wf-kernel-um", 7, "mean-field PSF kernel radius cap, um" },
         { "mean-field-density-per-um2", 20, "Renderer.MeanFieldDensityPerUm2: a continuous population (WideField dyes, pre states, dSTORM initial ON) renders mean-field above this many emitting dyes per um^2 of the focal slab, per dye below" },
         { "mean-field-slab-nm", 500, "Renderer.MeanFieldSlabNm: that slab's thickness around the focal plane" },
         { "mean-field-max-emitters", 5000, "Renderer.MeanFieldMaxEmitters: and mean-field above this many emitting dyes in the z range (cost cap of the per-dye path)" },
         { "bf-quality", 3, "Renderer.BrightFieldQuality: speed vs precision, 1 (fast) .. 4 (precise); sets the four below unless given" },
         { "bf-sources", 0, "Renderer.BrightFieldSources: condenser source points (0 = from bf-quality: 6/12/24/48)" },
         { "bf-upscale", 0, "Renderer.BrightFieldUpscaling: optical grid cells per pixel, per axis (a minimum, raised to keep the grid pitch <= lambda / 4n; 0 = from bf-quality: 1)" },
         { "bf-sub", 0, "Renderer.BrightFieldGeometrySamples: geometry samples per grid cell side (0 = from bf-quality: 1/1/2/2)" },
         { "bf-slice-um", -1, "Renderer.BrightFieldSliceUm: multislice step, um; 0 = one thin slice (-1 = from bf-quality: 0/0.5/0.5/0.25)" },
         { "bf-margin-um", 0, "BrightField grid margin around the FOV, um (0 = from bf-quality: 3-5)" },
         { "bf-condenser-na", 0.4, "TransmittedLamp.CondenserNA: illumination NA (0 = coherent)" },
         { "bf-wavelength-nm", 550, "TransmittedLamp.WavelengthNm: illumination wavelength (the camera QE is read there)" },
         { "bf-photons-per-px-per-sec", 80000, "TransmittedLamp.IntensityPhotonsPerPxPerSec: empty-field photons per pixel per second" },
         { "bf-aberrations", 1, "TransmittedLamp.UseObjectiveAberrations: 1 = the PSF's Zernike aberrations in the detection pupil, 0 = none" },
         { "bf-n-medium", 1.337, "CellField.IndexMedium: refractive index of the medium" },
         { "bf-n-cytoplasm", 1.35, "CellField.IndexCytoplasm" },
         { "bf-n-nucleus", 1.35, "CellField.IndexNucleus" },
         { "bf-n-microtubule", 1.48, "CellField.IndexMicrotubule (12.5 nm tubes)" },
         { "bf-absorption-per-um", 0, "CellField.AbsorptionPerUm: intensity absorption of cell material, 1/um (unstained: 0)" },
         { "immersion-index", 1.518, "Objective.ImmersionIndex (PSF and collection efficiency)" },
         { "psf-model", 3, "Renderer.PsfModel: 0 = Gaussian, 3 = GibsonLanniZernike (names accepted; 1/2 need the adapter's JVM)" },
         { "psf-zernike-preset", 9, "Objective.ZernikePreset: index or name (0 None ... 9 MixedRealisticObjective ... 12)" },
         { "psf-mask", 0, "pupil mask: 0 = None, 1 = DoubleHelix (names accepted; not in Micro-Manager)" },
         { "psf-mask-modes", 5, "double-helix Gauss-Laguerre modes (2-8)" },
         { "psf-mask-waist", 1.0, "double-helix waist, pupil radii" },
         { "psf-oversampling", 6, "Renderer.PsfOversampling: kernel samples per camera pixel, per axis (1-16)" },
         { "psf-kernel-half-width-nm", 7000, "Objective.PsfKernelHalfWidthNm (a minimum: grown to 3x the Rayleigh radius)" },
         { "psf-z-range-um", 7.0, "Objective.PsfZRangeUm: span of the PSF z stack" },
         { "psf-z-step-um", 0.1, "Objective.PsfZStepUm: PSF z plane spacing" },
         { "psf-sample-index", 1.518, "SampleHolder.PsfSampleIndex: sample refractive index (Gibson-Lanni)" },
         { "psf-working-distance-um", 150, "Objective.WorkingDistanceUm (Gibson-Lanni ti0)" },
         { "psf-sample-depth-nm", 0, "SampleHolder.PsfSampleDepthNm: emitter depth below the coverslip (Gibson-Lanni)" },
         { "psf-pupil-samples", 0, "Renderer.PsfPupilSamples: pupil samples per axis of the PSF evaluation (0 = as the window needs; 64 = webSMLM)" },
         { "psf-halo-cut", 3e-6, "Renderer.PsfHaloCut: blink splats leave out camera pixels below this share of the emitter's photons (Quality Fast 1e-5, Realistic 3e-6, Exhaustive 0 = the whole kernel; WideField keeps the whole kernel)" },
         { "psf-interp", 2, "Renderer.PsfInterp: 0 Nearest, 1 Linear, 2 Cubic, 3 Fft (names accepted)" },
      };
      v.insert(v.end(), rest.begin(), rest.end());
      return o;
   }();
   return t;
}

// The option names whose values may be given by name, and the names.
const std::vector<std::string>* OptionNames(const std::string& name)
{
   static const std::map<std::string, std::vector<std::string>> names = [] {
      std::map<std::string, std::vector<std::string>> m;
      std::vector<std::string> lightPresets = { "auto" };
      for (const std::string& id : LightPresetIds())
         lightPresets.push_back(id);
      m["modality"] = { "Fluorescence", "BrightField" };
      m["psf-model"] = { "Gaussian", "RichardsWolf", "GibsonLanni", "GibsonLanniZernike" };
      m["psf-mask"] = { "None", "DoubleHelix" };
      m["psf-interp"] = { "Nearest", "Linear", "Cubic", "Fft" };
      m["psf-zernike-preset"] = ZernikePresetNames();
      m["mt-dye"] = DyeChoices();
      m["mt-mode"] = DyeModeNames();
      m["mode"] = DyeModeNames();
      m["specimen"] = SpecimenIds();
      m["mt-orient"] = { "Free", "Fixed", "Random" };
      m["mt-motion"] = { "Static" };
      for (const char* k : { "dye1.source", "dye2.source", "dye3.source" })
         m[k] = DyeIds();
      m["ex-filter"] = ExcitationFilterIds();
      m["dichroic"] = DichroicIds();
      m["em-filter"] = EmissionFilterIds();
      m["light-preset"] = lightPresets;
      m["camera-preset"] = CameraIds();
      m["qe-curve"] = CameraIds();
      m["camera-type"] = { "sCMOS", "EMCCD" };
      m["illum-geometry"] = { "Epi" };
      return m;
   }();
   auto it = names.find(name);
   return it == names.end() ? nullptr : &it->second;
}

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

// A dye override key `<prefix>-dye.<field>` (structure = its index) or
// `dye<N>.<field>` (slot N - 1, structure -1). JS DYE_OVERRIDE.
struct OverrideKey
{
   int structure = -1, slot = -1;
   std::string field;
};
bool ParseOverrideKey(const std::string& k, OverrideKey& out)
{
   const size_t dot = k.find('.');
   if (dot == std::string::npos || dot + 1 >= k.size())
      return false;
   const std::string head = k.substr(0, dot), field = k.substr(dot + 1);
   for (char c : field)
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
         return false;
   out = OverrideKey();
   out.field = field;
   if (head.size() == 4 && head.compare(0, 3, "dye") == 0 && head[3] >= '1' && head[3] <= '3')
   {
      out.slot = head[3] - '1';
      return true;
   }
   if (head.size() > 4 && head.compare(head.size() - 4, 4, "-dye") == 0)
   {
      const std::string prefix = head.substr(0, head.size() - 4);
      for (char c : prefix)
         if (c < 'a' || c > 'z')
            return false;
      for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
         if (prefix == StructurePrefix(s))
            out.structure = s;
      return true;   // an unknown prefix: a bad option (ScopeSpecSet)
   }
   return false;
}

bool IsNamedOption(const std::string& name)
{
   for (const ScopeOption& o : Options().opts)
      if (name == o.name)
         return true;
   return false;
}

} // namespace

const std::vector<ScopeOption>& ScopeMovieOptions()
{
   return Options().opts;
}

bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value)
{
   if ((name.compare(0, 2, "p.") == 0 && name.size() > 2) || ZernikeKeyIndex(name) >= 0 || IsNamedOption(name))
   {
      spec[name] = value;
      return true;
   }
   OverrideKey ok;
   if (ParseOverrideKey(name, ok) && (ok.slot >= 0 || ok.structure >= 0) &&
       IndexOf(DyeFieldNames(), ok.field) >= 0)
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
   for (const ScopeOption& o : Options().opts)
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
   // The special values: a target's Typical dye, its DyeDefault / Global mode; no global mode.
   auto endsWith = [&](const char* suffix) {
      const size_t n = std::strlen(suffix);
      return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
   };
   const std::string t = text;
   if ((endsWith("-dye") && t == "Typical") || (endsWith("-mode") && t == "DyeDefault") || (name == "mode" && t == "None"))
   {
      value = -1;
      return true;
   }
   if (endsWith("-mode") && t == "Global")
   {
      value = -2;
      return true;
   }
   if (const std::vector<std::string>* names = OptionNames(name))
   {
      const int i = IndexOf(*names, text);
      if (i >= 0)
      {
         value = i;
         return true;
      }
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

double KernelWavelengthNm(double lambdaNm)
{
   return 2 * std::floor(lambdaNm / 2 + 0.5);
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

// The dye slots and per-structure overrides of a spec (JS dyeOverrides).
void SpecOverrides(const ScopeSpec& spec, std::vector<DyeSlot>& slots, std::vector<DyeOverrides>& byStructure)
{
   slots.assign(3, DyeSlot());
   for (int n = 0; n < 3; ++n)
   {
      const std::string k = "dye" + std::to_string(n + 1) + ".source";
      slots[static_cast<size_t>(n)].source = static_cast<int>(ScopeSpecGet(spec, k.c_str()));
   }
   byStructure.assign(ISC_STRUCT_COUNT, DyeOverrides());
   for (const auto& kv : spec)
   {
      OverrideKey ok;
      if (!ParseOverrideKey(kv.first, ok) || ok.field == "source")
         continue;
      if (ok.slot >= 0)
         slots[static_cast<size_t>(ok.slot)].overrides[ok.field] = kv.second;
      else if (ok.structure >= 0)
         byStructure[static_cast<size_t>(ok.structure)][ok.field] = kv.second;
   }
}

std::string Opt(const char* prefix, const char* name)
{
   return std::string(prefix) + "-" + name;
}

} // namespace

// A structure's dye choice and mode request as the spec gives them, resolved (JS structureDyeChoice): <prefix>-mode
// -2 = the global mode option (-1 there: none); <prefix>-dye -1 = the target's typical dye in that mode (the library's
// default mode when none is asked).
static void StructureDyeChoice(const ScopeSpec& spec, int s, int& choice, int& mode)
{
   auto O = [&](const std::string& n) { return ScopeSpecGet(spec, n.c_str()); };
   const char* P = StructurePrefix(s);
   mode = static_cast<int>(O(Opt(P, "mode")));
   if (mode == -2)
      mode = static_cast<int>(O("mode"));
   if (mode < -1)
      mode = -1;
   choice = static_cast<int>(O(Opt(P, "dye")));
   if (choice == -1)
   {
      if (mode < 0)
         mode = DefaultDyeMode();
      choice = TargetAt(s).typicalDye[mode];
   }
}

bool ScopeStructureDye(const ScopeSpec& spec, int s, EffectiveDye& eff, int& choice, std::string& err)
{
   if (s < 0 || s >= ISC_STRUCT_COUNT)
   {
      err = "structure " + std::to_string(s) + " out of range";
      return false;
   }
   std::vector<DyeSlot> slots;
   std::vector<DyeOverrides> byStructure;
   SpecOverrides(spec, slots, byStructure);
   int mode = -1;
   StructureDyeChoice(spec, s, choice, mode);
   return MakeEffectiveDye(choice, slots, byStructure[static_cast<size_t>(s)], mode, eff, err);
}

double ScopeStructureLabelingPct(const ScopeSpec& spec, int s, int mode)
{
   const double pct = ScopeSpecGet(spec, Opt(StructurePrefix(s), "label-pct").c_str());
   return pct >= 0 ? pct : TargetAt(s).typicalPct[mode];
}

// The camera of a spec: the preset's values for every option the spec does
// not set (JS scopeCamera).
struct ScopeCamera
{
   std::string preset;
   int qeCurve = 0;
   double qeFlat = 0.85;
   bool emccd = false;
   double darkPerSec = 0, gainPhotonsPerAdu = 1, offsetAdu = 100, offsetStdAdu = 0, readNoiseElectrons = 0;
   double gainStdFraction = 0, readNoiseStdFraction = 0, emGain = 300, cicElectrons = 0, bitDepth = 16;
};

void ScopeLights(const ScopeSpec& spec, bool& epi, bool& trans)
{
   const double e = ScopeSpecGet(spec, "light-epi"), t = ScopeSpecGet(spec, "light-trans");
   const bool brightField = ScopeSpecGet(spec, "modality") == 1;
   epi = e >= 0 ? e != 0 : !brightField;
   trans = t >= 0 ? t != 0 : brightField;
}

// The preset's gain depends on the imaging (CameraPresetGain): the lamp on
// (BrightField, alone or with fluorescence), or every structure in WideField
// mode (JS wideFieldOrBrightField).
static bool WideFieldOrBrightField(const ScopeSpec& spec, bool& out, std::string& err)
{
   bool epi = false;
   ScopeLights(spec, epi, out);
   if (out)
      return true;
   out = true;
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
   {
      EffectiveDye eff;
      int choice = 0;
      if (!ScopeStructureDye(spec, s, eff, choice, err))
         return false;
      out = out && DyeModeNames()[static_cast<size_t>(eff.mode)] == "WideField";
   }
   return true;
}

static bool MakeScopeCamera(const ScopeSpec& spec, ScopeCamera& c, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const int pi = static_cast<int>(O("camera-preset"));
   if (pi < 0 || pi >= static_cast<int>(CameraIds().size()))
   {
      err = "camera preset " + std::to_string(pi) + " out of range";
      return false;
   }
   const CameraData& preset = CameraAt(pi);
   bool wide = false;
   if (!WideFieldOrBrightField(spec, wide, err))
      return false;
   auto C = [&](const char* n, CameraField f) {
      const double v = f == CAM_GAIN ? CameraPresetGain(preset, wide) : preset.v[f];
      return spec.count(n) || std::isnan(v) ? O(n) : v;
   };
   c.preset = preset.id;
   c.emccd = O("camera-type") >= 0 ? O("camera-type") == 1 : (preset.type && !std::strcmp(preset.type, "EMCCD"));
   c.qeCurve = O("qe-curve") >= 0 ? static_cast<int>(O("qe-curve")) : pi;
   c.qeFlat = C("qe", CAM_QE);
   c.darkPerSec = C("dark-per-sec", CAM_DARK);
   c.gainPhotonsPerAdu = C("gain", CAM_GAIN);
   c.offsetAdu = C("offset", CAM_OFFSET);
   c.offsetStdAdu = C("offset-std", CAM_OFFSET_STD);
   c.readNoiseElectrons = C("read-noise", CAM_READ_NOISE);
   c.gainStdFraction = C("gain-std-pct", CAM_GAIN_STD_PCT) / 100.0;
   c.readNoiseStdFraction = C("read-noise-std-pct", CAM_READ_NOISE_STD_PCT) / 100.0;
   c.emGain = O("em-gain") > 0 ? O("em-gain") : EmGainFromGain(CameraPreamp(preset), c.gainPhotonsPerAdu);
   c.cicElectrons = C("cic", CAM_CIC);
   c.bitDepth = C("bit-depth", CAM_BIT_DEPTH);
   return true;
}

// The light preset a spec asks for (JS scopeLightPreset): nullptr for none.
static bool ScopeLightPreset(const ScopeSpec& spec, const LightPresetData*& out, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   out = nullptr;
   const int i = static_cast<int>(O("light-preset"));
   if (i < 0)
      return true;
   std::string id;
   if (i == 0)
   {
      EffectiveDye eff;
      int choice = 0;
      if (!ScopeStructureDye(spec, 0, eff, choice, err))
         return false;
      id = eff.dye.modes[eff.mode].lightPreset ? eff.dye.modes[eff.mode].lightPreset : "";
   }
   else if (i - 1 < static_cast<int>(LightPresetIds().size()))
      id = LightPresetIds()[static_cast<size_t>(i - 1)];
   out = FindLightPreset(id);
   if (!out)
   {
      err = "light preset " + std::to_string(i) + " out of range";
      return false;
   }
   return true;
}

// The light path of a spec (JS scopeLightPath): the light preset's values for
// the options the spec does not give.
static bool MakeScopeLightPath(const ScopeSpec& specIn, const ScopeCamera& camera, LightPath& lp, std::string& err)
{
   const LightPresetData* q = nullptr;
   if (!ScopeLightPreset(specIn, q, err))
      return false;
   ScopeSpec spec = specIn;
   const std::vector<int>& lines = LaserLines();
   if (q)
   {
      for (size_t l = 0; l < lines.size(); ++l)
      {
         const std::string k = "laser-" + std::to_string(lines[l]);
         if (!spec.count(k))
            spec[k] = q->lasers[l];
      }
      if (!spec.count("ex-filter"))
         spec["ex-filter"] = IndexOf(ExcitationFilterIds(), q->excitationFilter);
      if (!spec.count("dichroic"))
         spec["dichroic"] = IndexOf(DichroicIds(), q->dichroic);
      if (!spec.count("em-filter"))
         spec["em-filter"] = IndexOf(EmissionFilterIds(), q->emissionFilter);
   }
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   LightPathSettings s;
   for (int nm : lines)
      s.lasers.push_back({ static_cast<double>(nm), std::max(0.0, O(("laser-" + std::to_string(nm)).c_str())) });
   if (O("laser-custom-nm") > 0)
      s.lasers.push_back({ O("laser-custom-nm"), std::max(0.0, O("laser-custom")) });
   s.excitationFilter = static_cast<int>(O("ex-filter"));
   s.exLoNm = O("ex-lo-nm");
   s.exHiNm = O("ex-hi-nm");
   s.dichroic = static_cast<int>(O("dichroic"));
   s.dichroicEdgeNm = O("dichroic-edge-nm");
   s.emissionFilter = static_cast<int>(O("em-filter"));
   s.emLoNm = O("em-lo-nm");
   s.emHiNm = O("em-hi-nm");
   s.qeCurve = camera.qeCurve;
   s.qeFlat = camera.qeFlat;
   s.na = O("na");
   s.immersionIndex = O("immersion-index");
   s.chamberHeightUm = std::max(0.0, O("chamber-height-um"));
   return MakeLightPath(s, lp, err);
}

// Per structure: its effective dye, mode, world label and photophysics (JS scopeLabels).
static bool MakeScopeLabels(const ScopeSpec& spec, const LightPath& lp, std::vector<LabelPhysics>& labels,
                            std::string& err)
{
   auto O = [&](const std::string& n) { return ScopeSpecGet(spec, n.c_str()); };
   labels.assign(ISC_STRUCT_COUNT, LabelPhysics());
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
   {
      const char* P = StructurePrefix(s);
      EffectiveDye eff;
      int choice = 0;
      if (!ScopeStructureDye(spec, s, eff, choice, err))
         return false;
      const double pct = ScopeStructureLabelingPct(spec, s, eff.mode);
      LabelPhysicsOptions o;
      o.density = std::min(1.0, std::max(0.0, pct / 100));
      o.imagerNm = O(Opt(P, "imager-nm"));
      o.orientationMode = static_cast<int>(O(Opt(P, "orient")));
      o.polarDeg = O(Opt(P, "orient-polar-deg"));
      o.azimuthDeg = O(Opt(P, "orient-azimuth-deg"));
      o.wobbleDeg = O(Opt(P, "wobble-deg"));
      o.motion = static_cast<int>(O(Opt(P, "motion")));
      if (!MakeLabelPhysics(eff, lp, o, labels[static_cast<size_t>(s)], err))
         return false;
   }
   return true;
}

// The kinetic inputs of a spec (ScopeMovie.h KineticEnv): the light keys (the
// lasers, the light preset, excitation filter and dichroic, the imager
// concentrations) and the dye keys (dye choices, modes, overrides).
static bool IsLightKey(const std::string& k)
{
   auto ends = [&](const char* e) {
      const size_t n = std::strlen(e);
      return k.size() >= n && k.compare(k.size() - n, n, e) == 0;
   };
   return k.compare(0, 6, "laser-") == 0 || k == "light-preset" || k == "ex-filter" || k == "ex-lo-nm" ||
          k == "ex-hi-nm" || k == "dichroic" || k == "dichroic-edge-nm" || ends("-imager-nm");
}
static bool IsDyeKey(const std::string& k)
{
   auto ends = [&](const char* e) {
      const size_t n = std::strlen(e);
      return k.size() >= n && k.compare(k.size() - n, n, e) == 0;
   };
   OverrideKey ok;
   return k == "mode" || ends("-mode") || ends("-dye") || (ParseOverrideKey(k, ok) && (ok.slot >= 0 || ok.structure >= 0));
}

KineticEnv ScopeKineticEnv(const ScopeSpec& spec)
{
   KineticEnv e;
   for (const auto& kv : spec)
      if (IsLightKey(kv.first) || IsDyeKey(kv.first))
         e.insert(kv);
   return e;
}

double ScopeSampleIntensityKwCm2(const ScopeSpec& spec)
{
   ScopeCamera camera;
   LightPath lp;
   std::string err;
   if (!MakeScopeCamera(spec, camera, err) || !MakeScopeLightPath(spec, camera, lp, err))
      return 0.0;
   return LaserIntensityIn(lp, 0.0, 1e9);
}

// The spec a past epoch's rates come from: the current spec with the epoch's
// light keys, and its dye keys too when every structure's dye and mode are
// the current ones (an override change acts from then on); a dye or mode
// changed since: the current dye under the past light (spec/ALGORITHM.md).
static ScopeSpec EnvSpec(const ScopeSpec& cur, const KineticEnv& env)
{
   ScopeSpec out;
   bool sameDyes = true;
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
   {
      int c0 = 0, m0 = 0, c1 = 0, m1 = 0;
      StructureDyeChoice(cur, s, c0, m0);
      StructureDyeChoice(env, s, c1, m1);
      sameDyes = sameDyes && c0 == c1 && m0 == m1;
   }
   for (const auto& kv : cur)
      if (!IsLightKey(kv.first) && !(sameDyes && IsDyeKey(kv.first)))
         out.insert(kv);
   for (const auto& kv : env)
      if (IsLightKey(kv.first) || sameDyes)
         out[kv.first] = kv.second;
   return out;
}

struct ScopeSetup
{
   SimulationParams p;
   CellFieldSettings cf;
   CellFieldQuery q;
   unsigned W = 0, H = 0;
   long N = 0;
   long seed = 0;
   double expSec = 0.05, t0Sec = 0.0;
   // Sample drift: the per-frame displacement (nm, zero at frame 0) and its range.
   std::vector<DriftNm> drift;
   DriftBounds driftRange;
   bool driftOn = false;
   ScopeCamera camera;
   LightPath lp;
   std::vector<LabelPhysics> labels;
   double meanFieldDensityPerUm2 = 20, meanFieldSlabNm = 500, meanFieldMaxEmitters = 5000;
};

// JS scopeSetup: frame-equivalent parameters, world settings, the FOV query,
// the camera, light path and labels.
static bool MakeScopeSetup(const ScopeSpec& spec, ScopeSetup& S, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };

   const long seed = static_cast<long>(O("seed"));
   unsigned W, H;
   long N;
   ScopeMovieDims(spec, W, H, N);
   const double expSec = std::max(1e-6, O("exposure-ms") / 1000.0);
   const double t0Sec = std::max(0.0, O("start-sec"));
   if (static_cast<int>(O("specimen")) != 0)
   {
      err = "specimen " + std::to_string(static_cast<int>(O("specimen"))) + ": only the CellField (0) exists";
      return false;
   }
   if (!MakeScopeCamera(spec, S.camera, err) || !MakeScopeLightPath(spec, S.camera, S.lp, err) ||
       !MakeScopeLabels(spec, S.lp, S.labels, err))
      return false;
   bool epi = false, trans = false;
   ScopeLights(spec, epi, trans);
   const bool brightField = trans && !epi;   // both: the fluorescence chain at QE 1, the lamp's photons x its QE
   const ScopeCamera& c = S.camera;

   // Fluorescence: the photon image is already in detected photons (QE(lambda)
   // in each dye's detected fraction), so the noise chain runs at QE 1; the
   // flat background takes the QE at the emission filter's centre.
   // BrightField: the QE at its lamp wavelength.
   const double bgQe = BackgroundQe(S.lp);
   SimulationParams p;
   p.pixelSizeNm = O("pixel-nm");
   p.backgroundPhotons = O("background-per-sec") * expSec * bgQe;
   p.quantumEfficiency = brightField ? SampleAt(S.lp.qe, O("bf-wavelength-nm")) : 1.0;
   p.darkCurrentElectronsPerFrame = c.darkPerSec * expSec;
   p.gainPhotonsPerAdu = c.gainPhotonsPerAdu;
   p.offsetAdu = c.offsetAdu;
   p.offsetStdAdu = c.offsetStdAdu;
   p.readNoiseElectrons = c.readNoiseElectrons;
   // The per-pixel gain and read-noise spreads are an sCMOS's (an amplifier
   // per pixel); an EMCCD reads every pixel through one (JS scopeSetup).
   p.pixelGainStdFraction = c.emccd ? 0.0 : c.gainStdFraction;
   p.pixelReadNoiseStdFraction = c.emccd ? 0.0 : c.readNoiseStdFraction;
   p.emccd = c.emccd;
   p.emGain = c.emGain;
   p.cicElectrons = c.cicElectrons;
   p.bitDepth = static_cast<int>(c.bitDepth);
   p.frameDurationSec = expSec;
   p.drift.xyNmPerSqrtSec = std::max(0.0, O("drift-xy-nm-per-sqrt-sec"));
   p.drift.zNmPerSqrtSec = std::max(0.0, O("drift-z-nm-per-sqrt-sec"));
   p.drift.xySpeedNmPerSec = std::max(0.0, O("drift-xy-speed-nm-per-sec"));
   p.drift.zSpeedNmPerSec = O("drift-z-speed-nm-per-sec");
   p.drift.xyAngleDeg = O("drift-xy-angle-deg");
   p.drift.zDirection = static_cast<int>(std::lround(O("drift-z-direction")));
   p.drift.angleWanderDeg = std::max(0.0, O("drift-xy-angle-wander-deg"));
   p.drift.zAngleWanderDeg = std::max(0.0, O("drift-z-angle-wander-deg"));
   p.drift.speedWanderPct = std::max(0.0, O("drift-speed-wander-pct"));
   p.drift.wanderTimeSec = std::max(0.0, O("drift-wander-time-sec"));

   CellFieldSettings cf;
   const double worldSeed = O("world-seed");
   cf.seed = worldSeed >= 0 ? static_cast<uint32_t>(static_cast<uint64_t>(worldSeed))
                            : static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x43454C4CULL);
   cf.cacheDir = O("disk-cache") >= 1 ? DefaultCacheDir() : std::string();
   SetPsfKernelDiskCacheDir(O("disk-cache") >= 2 ? DefaultCacheDir() : std::string());   // "" under Emscripten
   std::map<std::string, double> world = {
      { "chunkSize", O("chunk-um") }, { "density", O("occupancy") },
      { "cellDiamMin", O("cell-diam-min-um") }, { "cellDiamMax", O("cell-diam-max-um") },
      { "mtDensity", O("mt-density") },
      { "enablePacking", O("packing") != 0 ? 1.0 : 0.0 },
   };
   // p.* pass-through (known core names only), overriding the named ones.
   IscParams* probe = isc_params_new();
   for (const auto& kv : spec)
      if (kv.first.compare(0, 2, "p.") == 0 && isc_params_set(probe, kv.first.c_str() + 2, kv.second) == 0)
         world[kv.first.substr(2)] = kv.second;
   isc_params_free(probe);
   cf.params.assign(world.begin(), world.end());
   for (const LabelPhysics& l : S.labels)
      cf.labels.push_back(l.label);

   // The camera's CellFieldQueryFor (drift: grown below).
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
   S.meanFieldDensityPerUm2 = std::max(0.0, O("mean-field-density-per-um2"));
   S.meanFieldSlabNm = std::max(0.0, O("mean-field-slab-nm"));
   S.meanFieldMaxEmitters = std::max(0.0, O("mean-field-max-emitters"));
   // Sample drift: the per-frame path, and the query rect grown so every
   // frame's dyes are in it (the renderer adds the drift; the dyes a frame can
   // show sit that far back) and the z window by the largest |dz| (the focus
   // itself stays: the mean-field scenes and BrightField use it).
   S.driftOn = p.drift.On();
   if (S.driftOn)
   {
      S.drift = DriftTrajectory(seed, N, expSec, p.drift);
      S.driftRange = DriftRange(S.drift);
      const DriftBounds& b = S.driftRange;
      S.q.x0Um -= b.xHi / 1000.0;
      S.q.x1Um -= b.xLo / 1000.0;
      S.q.y0Um -= b.yHi / 1000.0;
      S.q.y1Um -= b.yLo / 1000.0;
      if (S.q.zHalfRangeUm > 0.0)
         S.q.zHalfRangeUm += std::max(-b.zLo, b.zHi) / 1000.0;
   }
   return true;
}

// info.driftNm: x, y, z per frame (empty without drift).
static void DriftInfo(const ScopeSetup& S, ScopeMovieInfo& info)
{
   info.driftNm.clear();
   for (const DriftNm& d : S.drift)
   {
      info.driftNm.push_back(d.x);
      info.driftNm.push_back(d.y);
      info.driftNm.push_back(d.z);
   }
}

// The drift margin: the xy range in whole pixels plus one (um).
static double DriftMarginUm(const ScopeSetup& S)
{
   return S.driftOn ? (std::ceil(S.driftRange.MaxXyNm() / S.p.pixelSizeNm) + 1.0) * S.p.pixelSizeNm / 1000.0 : 0.0;
}

namespace {
std::mutex g_psfHookMutex;
ScopePsfHook g_psfHook;
} // namespace

void SetScopePsfRequestHook(ScopePsfHook hook)
{
   std::lock_guard<std::mutex> g(g_psfHookMutex);
   g_psfHook = std::move(hook);
}

bool ScopeLabelState(const ScopeSpec& spec, int structure, bool pre, ScopeStateReadout& out, std::string& err)
{
   ScopeSetup S;
   out = ScopeStateReadout();
   if (!MakeScopeSetup(spec, S, err))
      return false;
   if (structure < 0 || structure >= static_cast<int>(S.labels.size()))
   {
      err = "no such structure";
      return false;
   }
   const LabelPhysics& L = S.labels[static_cast<size_t>(structure)];
   const StatePhysics& st = pre ? L.pre : L.main;
   out.emits = st.emits;
   out.detectedFraction = st.detectedFraction;
   out.lambdaNm = st.lambdaNm;
   out.detectedPerSec = st.detectedPerSec;
   out.onSecNow = L.onSecNow;
   return true;
}

bool ScopePsfRequest(const ScopeSpec& spec, double wavelengthNm, PsfGeneratorRequest& req, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const int model = static_cast<int>(O("psf-model"));
   if (model == 0)
      return false;
   if (model == 1 || model == 2)
   {
      ScopePsfHook hook;
      {
         std::lock_guard<std::mutex> g(g_psfHookMutex);
         hook = g_psfHook;
      }
      if (hook)
      {
         if (hook(spec, wavelengthNm, req))
            return true;
         err = "psf-model " + std::to_string(model) + ": the host could not build the request";
         return false;
      }
   }
   if (model != 3)
   {
      err = "psf-model " + std::to_string(model) +
            ": only 0 (Gaussian) and 3 (GibsonLanniZernike) run here; RichardsWolf/GibsonLanni need the adapter's JVM";
      return false;
   }
   // As the camera's BuildPsfGeneratorRequest.
   req = PsfGeneratorRequest();
   req.model = PsfModelKind::GibsonLanniZernike;
   req.wavelengthNm = wavelengthNm;
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
   req.pupilSamples = static_cast<int>(std::max(0.0, O("psf-pupil-samples")));
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

bool ScopePsfKernel(const ScopeSpec& spec, double wavelengthNm, PsfKernelCache& cache, std::string& err)
{
   cache = PsfKernelCache();
   PsfGeneratorRequest req;
   err.clear();
   if (!ScopePsfRequest(spec, wavelengthNm, req, err))
      return err.empty(); // Gaussian: no kernel, not an error
   return ComputePsfKernelCache(req, cache, err);
}

namespace {

// Movies made by one process (the cli; the viewer's worker, one after the
// other) share one world, one BrightField scene and the mean-field scenes,
// so a repeat, or a new focus, frame count, PSF or noise setting, reuses the
// built cells, microtubules, dyes, dye tiles and spectra. The answers are
// the same as with fresh objects (every answer of the core is a pure
// function of seed, params, labels and window; the caches are for speed
// only); a mutex keeps concurrent callers serial.
struct MeanFieldSlot
{
   WidefieldScene scene;
   std::unique_ptr<WidefieldPsf> psf;
   PsfKernelCache psfCache;
   int upscale = 0;
   double gauss[4] = { 0, 0, 0, 0 };
   bool gpuMode = false, has = false;
   long psfVersion = 0;
};

struct MovieCache
{
   std::mutex mutex;
   CellFieldSource source;
   CellFieldSettings world;
   bool haveWorld = false;
   uint64_t version = 0;      // the cells (seed, params): the BrightField scene
   uint64_t dyeVersion = 0;   // the dyes too (labels): the mean-field dye tiles
   BrightfieldScene brightfield;
   // Mean-field scenes by population (structure x 4 + state).
   std::map<int, std::unique_ptr<MeanFieldSlot>> meanField;
   // Dye counts (Density3d, one bin) by (box, structure mask) at dyeVersion:
   // a count is a pure function of the world, the labels and the box, so a
   // live movie per frame at an unchanged pose counts once (the most recent
   // kDyeCountEntries boxes).
   struct DyeCount
   {
      double box[6];
      int mask;
      long n;
   };
   std::vector<DyeCount> dyeCounts;
   uint64_t dyeCountVersion = 0;
   static constexpr size_t kDyeCountEntries = 16;
   long CountDyes(double x0, double y0, double x1, double y1, double zMin, double zMax, int mask)
   {
      if (dyeCountVersion != dyeVersion)
      {
         dyeCounts.clear();
         dyeCountVersion = dyeVersion;
      }
      const double box[6] = { x0, y0, x1, y1, zMin, zMax };
      for (const DyeCount& c : dyeCounts)
         if (c.mask == mask && std::equal(box, box + 6, c.box))
            return c.n;
      float one = 0;
      const long n = source.Density3d(x0, y0, x1, y1, zMin, zMax, 1, 1, 1, mask, &one);
      if (n >= 0)
      {
         if (dyeCounts.size() >= kDyeCountEntries)
            dyeCounts.erase(dyeCounts.begin());
         DyeCount c;
         std::copy(box, box + 6, c.box);
         c.mask = mask;
         c.n = n;
         dyeCounts.push_back(c);
      }
      return n;
   }
   // Live: the per-dye populations' running images by population (structure x
   // 4 + state), carried from one movie to the next at the same geometry
   // (FluorescenceMovie::CarryRunningImages): the image, its geometry key and
   // the dyes in it (by identity, sorted).
   struct RunningImage
   {
      std::vector<uint64_t> key;
      std::vector<double> acc;
      std::vector<std::pair<uint64_t, BlinkEvent>> in;
      long carried = 0;   // movies since the last full build
   };
   std::map<int, RunningImage> running;
};

MovieCache& SharedMovieCache()
{
   static MovieCache c;
   return c;
}

// Configures the shared source; bumps the versions when the world (and for
// dyeVersion the labels) change.
bool ConfigureShared(MovieCache& c, const CellFieldSettings& cf, std::string& err)
{
   if (!c.haveWorld || !c.world.SameWorld(cf))
      ++c.version;
   if (!c.haveWorld || !c.world.SameWorld(cf) || !c.world.SameLabels(cf))
      ++c.dyeVersion;
   c.world = cf;
   c.haveWorld = true;
   if (!c.source.Configure(cf, err))
   {
      c.haveWorld = false;
      return false;
   }
   return true;
}

constexpr double kMinDetectedFraction = 1e-4;   // a (structure, state) detected less than this is skipped (no kernel)

double GaussianSigmaPx(double lambdaNm, double na, double pixelNm)
{
   return std::min(std::max(0.21 * lambdaNm / std::max(0.01, na) / pixelNm, 0.3), 20.0);
}

} // namespace

bool MakeScopePsfPreview(const ScopeSpec& spec, ScopePsfPreview& out, std::string& err)
{
   ScopeStateReadout st;
   if (!ScopeLabelState(spec, 0, false, st, err))
      return false;
   if (!st.emits)
   {
      ScopeStateReadout pre;
      if (ScopeLabelState(spec, 0, true, pre, err) && pre.emits)
         st = pre;
      err.clear();
   }
   out = ScopePsfPreview();
   out.lambdaNm = KernelWavelengthNm(st.emits ? st.lambdaNm : 670.0);
   PsfKernelCache cache;
   if (!ScopePsfKernel(spec, out.lambdaNm, cache, err))
      return false;
   if (cache.valid)
   {
      out.oversampling = std::max(1, cache.oversampling);
      out.size = cache.sizeOversampled;
      out.nz = cache.nz;
      out.zStepNm = cache.zStepNm;
      const int camRad = cache.halfWidthOversampled / out.oversampling;
      const int n = 2 * camRad + 1;
      out.camSize = n;
      const size_t P = static_cast<size_t>(out.size) * out.size, C = static_cast<size_t>(n) * n;
      out.planes.resize(P * out.nz);
      out.cams.assign(C * out.nz, 0.0f);
      std::vector<float> img;
      for (int z = 0; z < out.nz; ++z)
      {
         std::copy(cache.Planes()[static_cast<size_t>(z)].begin(), cache.Planes()[static_cast<size_t>(z)].end(),
                   out.planes.begin() + static_cast<std::ptrdiff_t>(P * z));
         img.assign(C, 0.0f);
         SplatPlan plan;
         if (PlanSplat(cache, z, camRad, camRad, 1.0, cache.interpMode, plan))
            SplatRows(img, static_cast<unsigned>(n), static_cast<unsigned>(n), 0, n, cache, plan, 1.0);
         std::copy(img.begin(), img.end(), out.cams.begin() + static_cast<std::ptrdiff_t>(C * z));
      }
      return true;
   }
   // Gaussian: no defocus, one plane.
   const double sigma = GaussianSigmaPx(out.lambdaNm, ScopeSpecGet(spec, "na"), ScopeSpecGet(spec, "pixel-nm"));
   const int os = static_cast<int>(std::min(16.0, std::max(1.0, ScopeSpecGet(spec, "psf-oversampling"))));
   const int camRad = static_cast<int>(std::ceil(4 * sigma)) + 1, n = 2 * camRad + 1;
   out.gaussian = true;
   out.oversampling = os;
   out.size = n * os;
   out.nz = 1;
   out.camSize = n;
   out.planes.resize(static_cast<size_t>(out.size) * out.size);
   for (int y = 0; y < out.size; ++y)
      for (int x = 0; x < out.size; ++x)
      {
         const double dx = (x + 0.5) / os - 0.5 - camRad, dy = (y + 0.5) / os - 0.5 - camRad;
         out.planes[static_cast<size_t>(y) * out.size + x] = static_cast<float>(std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma)));
      }
   out.cams.assign(static_cast<size_t>(n) * n, 0.0f);
   RenderGaussianPSF(out.cams, static_cast<unsigned>(n), static_cast<unsigned>(n), camRad, camRad, sigma, 1.0);
   return true;
}

namespace {

// Mean detected photons per dye of a decaying population in [t0, t1): rate x
// the integral of exp(-lambda t).
double MeanPhotons(double ratePerSec, double lambda, double t0, double t1)
{
   return lambda > 0 ? ratePerSec * (std::exp(-lambda * t0) - std::exp(-lambda * t1)) / lambda
                     : ratePerSec * (t1 - t0);
}

// A clock history's rates (ABI 11): the core's kinetics rows (tStart, then
// ISC_KIN_COUNT per structure) and each segment's physics. multi false: the
// movie's own env from 0, i.e. no history (the label's kinetics, today's
// arithmetic everywhere).
struct HistInfo
{
   bool multi = false;
   std::vector<double> rows, tStart;
   std::vector<const std::vector<LabelPhysics>*> phys;
};
using EnvPhysicsMemo = std::map<const KineticEnv*, std::vector<LabelPhysics>>;
bool MakeHistInfo(const ScopeSpec& spec, const ScopeCamera& camera, const KineticEnv& curEnv,
                  const std::vector<ClockSegment>& segs, EnvPhysicsMemo& memo, HistInfo& out, std::string& err)
{
   out = HistInfo();
   if (segs.empty() || (segs.size() == 1 && segs[0].env && *segs[0].env == curEnv))
      return true;
   out.multi = true;
   for (const ClockSegment& seg : segs)
   {
      if (!seg.env)
      {
         err = "clock history without an env";
         return false;
      }
      auto it = memo.find(seg.env);
      if (it == memo.end())
      {
         const ScopeSpec es = EnvSpec(spec, *seg.env);
         LightPath lp;
         std::vector<LabelPhysics> labels;
         if (!MakeScopeLightPath(es, camera, lp, err) || !MakeScopeLabels(es, lp, labels, err))
            return false;
         it = memo.emplace(seg.env, std::move(labels)).first;
      }
      out.tStart.push_back(seg.tStart);
      out.phys.push_back(&it->second);
      out.rows.push_back(seg.tStart);
      for (int st = 0; st < ISC_STRUCT_COUNT; ++st)
      {
         const std::vector<double>& l = it->second[static_cast<size_t>(st)].label;
         for (int k = 0; k < ISC_KIN_COUNT; ++k)
            out.rows.push_back(l[static_cast<size_t>(ISC_LABEL_ACTIVATION_RATE + k)]);
      }
   }
   return true;
}

// A (structure, state) with its PSF and detected photons per frame.
struct Group
{
   int structure = 0;
   bool pre = false;       // role 'pre' (PALM pre state), else 'main'
   double lambdaNm = 0;
   PsfKernelCache kernel;  // !valid: the Gaussian of sigmaPx
   // kernel with the halo cut (psf-halo-cut): the blinks' splats. The
   // continuous populations keep kernel (spec/ALGORITHM.md "PSF halo cut").
   PsfKernelCache blinkKernel;
   double sigmaPx = 1;
   double detectedFraction = 0, detectedPerSec = 0, perFrame = 0;
};

// A continuous population (WideField dyes, a PALM pre state, the dSTORM
// initial ON): mean-field or per dye per frame.
struct Population
{
   int structure = 0, state = 0;
   size_t group = 0;
   double lambda = 0, budget = 0, emissionPerSec = 0, rate = 0;
   long nZ = 0, nSlab = 0;
   bool meanFieldAt0 = false, needWindows = false;
   // A rate history in some region (HistInfo multi): the largest survival at
   // frame 0 over the regions (the mean-field switch reads it instead of tMin).
   bool histMulti = false;
   double maxSurvival = 1.0;
   // A host clock (DyeClock): the mean-field image is frame 0's photons with
   // each grid column at its own clock (wb0: mean photons per dye per column);
   // frame f is it x exp(-lambda f exposure).
   bool weighted = false;
   std::vector<float> wb0;
   std::vector<BlinkEvent> wins;
   MeanFieldSlot* slot = nullptr;   // its mean-field scene (meanFieldAt0)
   WidefieldSceneSpec ws;           // the scene's spec at the spec's focus
   std::vector<float> image;        // W x H photons per (photon per dye), at the spec's z
   std::map<double, std::vector<float>> imageAt;   // ... at other z (a host's z sequence)
   std::vector<double> acc;         // the running image of the per-dye path
   long accFrame = -1;
   double accZ = 0;                 // the z the running image was made at
   double accDx = 0, accDy = 0;     // ... and the sample drift (px)
   // Drift: the mean-field images (with spectra) at the focus-grid foci, by
   // world focus, and the bleach-basis coefficients of the weighted scene.
   std::map<double, WidefieldImages> driftImages;
   std::vector<double> driftCoef;
   long meanFieldFrames = 0, perDyeFrames = 0;
};

// A population's decay rate under a label's physics (per second): the
// WideField dyes' bleaching, the dSTORM initial ON's end, the PALM pre state's
// activation and bleaching.
double PopulationLambda(int state, const LabelPhysics& L)
{
   if (state == ISC_STATE_ALWAYS_ON)
      return L.photonBudget > 0 ? L.main.emissionPerSec / L.photonBudget : 0;
   if (state == ISC_STATE_INITIAL_ON)
      return L.initialOnSec > 0 ? 1 / L.initialOnSec : std::numeric_limits<double>::infinity();
   return L.kActPerSec + (L.prePhotonBudget > 0 ? L.pre.emissionPerSec / L.prePhotonBudget : 0);
}

// ... its photon-budget bleaching alone (a dye's own end: aux x budget / emission).
double PopulationBleachRate(int state, const LabelPhysics& L)
{
   if (state == ISC_STATE_ALWAYS_ON)
      return L.photonBudget > 0 ? L.main.emissionPerSec / L.photonBudget : 0;
   if (state == ISC_STATE_PRE)
      return L.prePhotonBudget > 0 ? L.pre.emissionPerSec / L.prePhotonBudget : 0;
   return 0;
}

// exp(-the population's hazard integrated over the history up to clock T).
double HistSurvival(const HistInfo& h, int structure, int state, double T)
{
   double L = 0;
   const size_t n = h.tStart.size();
   for (size_t i = 0; i < n; ++i)
   {
      const double s1 = i + 1 < n ? h.tStart[i + 1] : std::numeric_limits<double>::infinity();
      const double d = std::min(T, s1) - h.tStart[i];
      if (!(d > 0))
         break;
      const double lam = PopulationLambda(state, (*h.phys[i])[static_cast<size_t>(structure)]);
      if (lam > 0)
         L += lam * d;
   }
   return std::exp(-L);
}

// The clock at which a dye of unit-exponential draw aux has used its photon
// budget over the history (infinity: never).
double HistBleachEnd(const HistInfo& h, int structure, int state, double aux)
{
   double acc = 0;
   const size_t n = h.tStart.size();
   for (size_t i = 0; i < n; ++i)
   {
      const double lb = PopulationBleachRate(state, (*h.phys[i])[static_cast<size_t>(structure)]);
      if (!(lb > 0))
         continue;
      const double s1 = i + 1 < n ? h.tStart[i + 1] : std::numeric_limits<double>::infinity();
      const double d = lb * (s1 - h.tStart[i]);
      if (acc + d >= aux)
         return h.tStart[i] + (aux - acc) / lb;
      acc += d;
   }
   return std::numeric_limits<double>::infinity();
}

const char* PopulationName(int state)
{
   return state == ISC_STATE_ALWAYS_ON ? "WideField dyes" : state == ISC_STATE_INITIAL_ON ? "initial ON" : "pre state";
}

// The running image's unit splats (sign +1 or -1) in double, each pixel's
// term rounded to float first (JS splatRows / renderGaussian into a
// Float64Array: img += fround(sign x sum)).
void GaussianUnitInto(std::vector<double>& img, unsigned width, unsigned height, double xPx, double yPx,
                      double sigmaPx, double totalPhotons)
{
   if (totalPhotons == 0.0 || !(sigmaPx > 0.0))
      return;
   constexpr double kPi = 3.14159265358979323846;
   const int rad = static_cast<int>(std::ceil(3.0 * sigmaPx));
   const int cx = static_cast<int>(std::lround(xPx)), cy = static_cast<int>(std::lround(yPx));
   const double amplitude = totalPhotons / (2.0 * kPi * sigmaPx * sigmaPx);
   const double twoSigmaSq = 2.0 * sigmaPx * sigmaPx;
   const int yLo = std::max(0, cy - rad), yHi = std::min(static_cast<int>(height) - 1, cy + rad);
   const int xLo = std::max(0, cx - rad), xHi = std::min(static_cast<int>(width) - 1, cx + rad);
   for (int py = yLo; py <= yHi; ++py)
   {
      const double ddy = py - yPx;
      double* row = img.data() + static_cast<size_t>(py) * width;
      for (int px = xLo; px <= xHi; ++px)
      {
         const double ddx = px - xPx;
         row[px] += static_cast<double>(static_cast<float>(amplitude * std::exp(-(ddx * ddx + ddy * ddy) / twoSigmaSq)));
      }
   }
}

void SplatUnitInto(std::vector<double>& img, unsigned width, unsigned height, const PsfKernelCache& cache,
                   const SplatPlan& plan, double sign, int yLo = 0, int yHi = -1)
{
   if (yHi < 0)
      yHi = static_cast<int>(height);
   const int os = std::max(1, cache.oversampling);
   const int camRad = cache.halfWidthOversampled / os;
   const int off = os - 1, bw = cache.blockSumWidth;
   const SplatSetupResult& st = plan.st;
   const int NT = st.nTaps;
   const float* B = plan.B;
   const int dyLo = std::max(-camRad, yLo - st.y0), dyHi = std::min(camRad, yHi - 1 - st.y0);
   const int dxLo = std::max(-camRad, -st.x0), dxHi = std::min(camRad, static_cast<int>(width) - 1 - st.x0);
   for (int dy = dyLo; dy <= dyHi; ++dy)
   {
      const int r0 = st.by + dy * os + off;
      const int jLo = std::max(0, -r0), jHi = std::min(NT, bw - r0);
      double* rowOut = img.data() + static_cast<size_t>(st.y0 + dy) * width;
      for (int dx = dxLo; dx <= dxHi; ++dx)
      {
         const int c0 = st.bx + dx * os + off;
         const int iLo = std::max(0, -c0), iHi = std::min(NT, bw - c0);
         double sum = 0.0;
         for (int j = jLo; j < jHi; ++j)
         {
            const float* brow = B + static_cast<size_t>(r0 + j) * bw;
            double row = 0.0;
            for (int i = iLo; i < iHi; ++i)
               row += st.wx[i] * brow[c0 + i];
            sum += st.wy[j] * row;
         }
         rowOut[st.x0 + dx] += static_cast<double>(static_cast<float>(sign * sum));
      }
   }
}

// One unit-photon emitter (sign +1 or -1) into img at its plane (JS splatUnit).
void SplatUnit(std::vector<double>& img, unsigned W, unsigned H, const BlinkEvent& e, const Group& g, double zStage,
               double pixelNm, double sign, double dxPx = 0.0, double dyPx = 0.0)
{
   const double xPx = e.xUm * 1000.0 / pixelNm + dxPx, yPx = e.yUm * 1000.0 / pixelNm + dyPx;
   if (!g.kernel.valid)
   {
      GaussianUnitInto(img, W, H, xPx, yPx, g.sigmaPx, sign);
      return;
   }
   SplatPlan plan;
   if (PlanSplat(g.kernel, g.kernel.NearestZIndex(e.zNm / 1000.0 - zStage), xPx, yPx, 1.0, g.kernel.interpMode, plan))
      SplatUnitInto(img, W, H, g.kernel, plan, sign);
}

std::string JsonEscape(const std::string& s)
{
   std::string o;
   for (char c : s)
   {
      if (c == '"' || c == '\\')
         o += '\\';
      o += c;
   }
   return o;
}

} // namespace

// ---- the fluorescence movie (JS fluorescence.js renderFluorescenceMovie) ----
struct FluorescenceMovie::Impl
{
   ScopeSpec spec;
   ScopeSetup S;
   MovieCache& cache;
   std::unique_lock<std::mutex> lock;
   std::vector<Group> groups;
   std::vector<BlinkEvent> events;                    // blinks of structures with a main group
   std::vector<std::vector<BlinkEvent>> byGroup;
   std::vector<std::vector<std::vector<uint32_t>>> buckets;
   std::vector<Population> pops;
   bool anyBlinks = false;
   double imagerPerFrame = 0, bg = 0, fovArea = 0, zStage = 0;
   std::chrono::steady_clock::time_point t0;
   double setupSec = 0;
   bool gpuMode = false;
   WidefieldAccelerator* accel = nullptr;
   const DyeClock* clock = nullptr;   // per-region dye clocks (Begin only)
   bool carry = false;                // CarryRunningImages
   double tMin = 0;                   // the smallest clock in the query (the mean-field switch)
   // The rate histories of the clock (Begin only): by history id, and the
   // physics of each env met.
   KineticEnv curEnv;
   EnvPhysicsMemo envPhys;
   std::map<uint32_t, HistInfo> hists;
   const HistInfo* HistOf(uint32_t h, std::string& err)
   {
      auto it = hists.find(h);
      if (it != hists.end())
         return &it->second;
      std::vector<ClockSegment> segs;
      if (clock)
         clock->Segments(h, segs);
      HistInfo info;
      if (!MakeHistInfo(spec, S.camera, curEnv, segs, envPhys, info, err))
         return nullptr;
      return &(hists[h] = std::move(info));
   }
   std::vector<Population*> sceneQueue;   // the populations whose mean-field scene images are pending (GPU mode)
   Impl() : cache(SharedMovieCache()), lock(cache.mutex, std::defer_lock) {}

   bool IsMeanField(const Population& p, long f) const
   {
      const double tMid = (clock ? tMin : S.t0Sec) + (f + 0.5) * S.expSec;
      const double P = p.histMulti ? p.maxSurvival * std::exp(-p.lambda * ((f + 0.5) * S.expSec))
                                   : std::exp(-p.lambda * tMid);
      return p.nSlab * P / fovArea > S.meanFieldDensityPerUm2 || p.nZ * P > S.meanFieldMaxEmitters;
   }
   bool BuildMeanField(Population& p, std::string& err);
   void TakeImage(Population& p);
   // Weighted (clock) mean-field: frame 0's photons of the scene's grid.
   void WeightedImage(Population& p, std::vector<float>& img);
   // The mean-field image at another stage z (refocused scene; cached).
   const std::vector<float>& ImageAt(Population& p, double z);
   void AdvanceAcc(Population& p, long f, double z, double dxPx = 0.0, double dyPx = 0.0);
   // Drift: the mean-field image of frame f (stage z, drift d) into img (+=):
   // the scene's images at the two focus-grid foci around z - dz, their
   // spectra interpolated and shifted by d (frame 0's photons for a weighted
   // population, else photons per (photon per dye)).
   void DriftMeanField(Population& p, double z, const DriftNm& d, std::vector<float>& img);
   DriftFocusGrid driftGrid;
};

FluorescenceMovie::FluorescenceMovie() : impl_(new Impl) {}
void FluorescenceMovie::CarryRunningImages(bool on)
{
   impl_->carry = on;
}
FluorescenceMovie::~FluorescenceMovie() = default;

// The mean-field image of the population's structure (JS meanFieldImage):
// every fluorescent dye with weight 1 on the world-anchored grid, convolved
// with the group's PSF, binned: photons per (photon per dye). Uniform
// illumination over the FOV and its 2 um margin (as the per-dye path's
// query rect). In GPU mode the images are left to SetImages /
// ComputeCpuImages.
bool FluorescenceMovie::Impl::BuildMeanField(Population& p, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const Group& g = groups[p.group];
   const double um = S.p.pixelSizeNm / 1000.0;
   std::unique_ptr<MeanFieldSlot>& slotPtr = cache.meanField[p.structure * 4 + p.state];
   if (!slotPtr)
      slotPtr.reset(new MeanFieldSlot());
   MeanFieldSlot& slot = *slotPtr;
   WidefieldSceneSpec ws;
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
   // Drift: the frames are the image shifted, so the lit square and the grid
   // grow by the xy drift (the dyes it brings within the kernel's reach).
   ws.marginUm = 2.0 + DriftMarginUm(S);
   ws.kernelCapUm = std::max(0.1, O("wf-kernel-um"));
   ws.eta = 1.0;
   ws.exposureSec = S.expSec;
   ws.unitDose = 1.0;
   // With a host clock the dyes go to the scene's weighted (bleaching)
   // channel, one weight per grid column; else unit weights.
   p.weighted = clock != nullptr;
   ws.bleachingMask = p.weighted ? 1 << p.structure : 0;
   ws.persistentMask = p.weighted ? 0 : 1 << p.structure;
   ws.worldVersion = static_cast<long>(cache.dyeVersion);
   if (g.kernel.valid)
      ws.grid.upscale = KernelWidefieldPsf::ValidUpscale(g.kernel.oversampling, ws.grid.upscale);
   // The WidefieldPsf (and with it the scene's kernel spectra) is kept across
   // movies while the kernel stack (its serial) and the grid pitch, or the
   // Gaussian's parameters, are unchanged.
   const double gauss[4] = { um / ws.grid.upscale, g.lambdaNm, O("na"), O("immersion-index") };
   const bool samePsf = slot.psf && slot.psfCache.valid == g.kernel.valid &&
                        (g.kernel.valid ? (slot.psfCache.Serial() == g.kernel.Serial() && slot.upscale == ws.grid.upscale)
                                        : std::equal(gauss, gauss + 4, slot.gauss));
   if (!samePsf)
   {
      slot.psfCache = g.kernel;
      slot.upscale = ws.grid.upscale;
      std::copy(gauss, gauss + 4, slot.gauss);
      if (g.kernel.valid)
         slot.psf.reset(new KernelWidefieldPsf(slot.psfCache, ws.grid.upscale));
      else
         slot.psf.reset(new GaussianWidefieldPsf(gauss[0], gauss[1], gauss[2], gauss[3]));
      ++slot.psfVersion;
   }
   ws.psfVersion = slot.psfVersion;
   // The scene's FFT sizes depend on the mode (GPU: powers of two): a mode
   // change starts from a fresh scene.
   const bool sceneGpu = gpuMode || accel != nullptr;
   if (!slot.has || slot.gpuMode != sceneGpu)
   {
      slot.scene = WidefieldScene();
      slot.scene.SetGpuMode(sceneGpu);
      slot.gpuMode = sceneGpu;
      slot.has = true;
   }
   // Drift keeps the spectra (focus work on the CPU: the accelerator returns images only).
   slot.scene.SetKeepSpectra(S.driftOn);
   slot.scene.SetAccelerator(S.driftOn ? nullptr : accel);
   slot.scene.SetDeferImages(gpuMode && !p.weighted && !S.driftOn);
   const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
   const auto tPhase = TimingClock::now();
   if (!slot.scene.Update(cache.source, ill, ws, *slot.psf, err))
      return false;
   TimingLog("fl.mean-field-scene", TimingSince(tPhase));
   p.slot = &slot;
   p.ws = ws;
   if (p.weighted)
   {
      auto tW = TimingClock::now();
      // Frame 0's mean photons per dye at each grid column's clock (neighbouring
      // columns mostly share a clock: its value is reused, the same number).
      const WidefieldGridSpec& gr = slot.scene.Grid();
      p.wb0.assign(static_cast<size_t>(gr.nx) * gr.ny, 0.0f);
      // With a rate history: rate x survival to t x the frame's fraction.
      double lastT = std::numeric_limits<double>::quiet_NaN();
      uint32_t lastH = 0;
      float lastW = 0.0f;
      for (unsigned j = 0; j < gr.ny; ++j)
         for (unsigned i = 0; i < gr.nx; ++i)
         {
            double t = 0;
            uint32_t hid = 0;
            clock->KeyAt(gr.x0Um + (i + 0.5) * gr.pitchUm, gr.y0Um + (j + 0.5) * gr.pitchUm, t, hid);
            if (!(t == lastT && hid == lastH))
            {
               lastT = t;
               lastH = hid;
               const HistInfo* hi = HistOf(hid, err);
               if (!hi)
                  return false;
               if (!hi->multi)
                  lastW = static_cast<float>(MeanPhotons(p.rate, p.lambda, t, t + S.expSec));
               else
               {
                  const double sv = HistSurvival(*hi, p.structure, p.state, t);
                  lastW = static_cast<float>(p.lambda > 0 ? p.rate * sv * (1 - std::exp(-p.lambda * S.expSec)) / p.lambda
                                                          : p.rate * sv * S.expSec);
               }
            }
            p.wb0[i + static_cast<size_t>(gr.nx) * j] = lastW;
         }
      TimingLog("fl.mean-field-weights", TimingSince(tW));
      tW = TimingClock::now();
      WeightedImage(p, p.image);
      TimingLog("fl.mean-field-render", TimingSince(tW));
   }
   else if (!gpuMode)
      TakeImage(p);
   return true;
}

void FluorescenceMovie::Impl::WeightedImage(Population& p, std::vector<float>& img)
{
   img.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   p.slot->scene.RenderFrame(p.wb0, img);
}

const std::vector<float>& FluorescenceMovie::Impl::ImageAt(Population& p, double z)
{
   if (z == zStage || !p.slot)
      return p.image;
   auto it = p.imageAt.find(z);
   if (it != p.imageAt.end())
      return it->second;
   WidefieldSceneSpec ws = p.ws;
   ws.focusWorldUm = ws.slabCentreUm = S.q.zRefUm + z;
   const double um = S.p.pixelSizeNm / 1000.0;
   const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
   std::vector<float>& img = p.imageAt[z];
   img.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   std::string err;
   p.slot->scene.SetDeferImages(false);
   if (p.slot->scene.Update(cache.source, ill, ws, *p.slot->psf, err))
   {
      if (p.weighted)
         WeightedImage(p, img);
      else
         p.slot->scene.Images().Render({}, img);
   }
   return img;
}

void FluorescenceMovie::Impl::DriftMeanField(Population& p, double z, const DriftNm& d, std::vector<float>& img)
{
   if (!p.slot)
      return;
   int k = 0;
   double w = 0.0;
   driftGrid.Weights(d.z, k, w);
   const double um = S.p.pixelSizeNm / 1000.0;
   auto imagesAt = [&](int kk) -> const WidefieldImages* {
      // The sample dz higher: the focal plane dz lower in it.
      const double focus = S.q.zRefUm + z - driftGrid.DzNm(kk) / 1000.0;
      auto it = p.driftImages.find(focus);
      if (it != p.driftImages.end())
         return &it->second;
      WidefieldSceneSpec ws = p.ws;
      ws.focusWorldUm = ws.slabCentreUm = focus;
      const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
      std::string err;
      p.slot->scene.SetDeferImages(false);
      if (!p.slot->scene.Update(cache.source, ill, ws, *p.slot->psf, err))
         return nullptr;
      if (p.weighted)
      {
         // Anchor the bleach basis at frame 0's weights; its coefficients.
         std::vector<float> scratch;
         p.slot->scene.RenderFrame(p.wb0, scratch);
         if (!p.slot->scene.BleachCoefficients(p.wb0, p.driftCoef))
            p.driftCoef = p.slot->scene.AnchorCoefficients();
      }
      else if (!p.slot->scene.Images().HasSpectra())
         p.slot->scene.ComputeCpuImages();
      return &(p.driftImages[focus] = p.slot->scene.Images());
   };
   const WidefieldImages* i0 = imagesAt(k);
   const WidefieldImages* i1 = (w != 0.0 && k + 1 < driftGrid.n) ? imagesAt(k + 1) : nullptr;
   if (!i0)
      return;
   const double pitchNm = p.slot->scene.Grid().pitchUm * 1000.0;
   RenderShiftedImages(*i0, i1, i1 ? w : 0.0, p.weighted ? p.driftCoef : std::vector<double>(), d.x / pitchNm,
                       d.y / pitchNm, img);
}

void FluorescenceMovie::Impl::TakeImage(Population& p)
{
   p.image.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   p.slot->scene.Images().Render({}, p.image);
}

// A dye's identity in a running image (its position and aux draw; the query
// origin is part of the image's key).
uint64_t RunningDyeId(const BlinkEvent& e)
{
   uint64_t h = 1469598103934665603ull;
   for (double v : { e.xUm, e.yUm, e.zNm, e.aux })
   {
      uint64_t b;
      std::memcpy(&b, &v, sizeof b);
      h = (h ^ b) * 1099511628211ull;
      h ^= h >> 29;
   }
   return h;
}

// The unit splats of dyes (sign each) into img: with a kernel and many dyes on
// row bands on all cores, each band adding every dye in order, so every pixel
// sums the same terms in the same order as one thread (bit-identical).
void SplatUnits(std::vector<double>& img, unsigned W, unsigned H, const std::vector<const BlinkEvent*>& dyes,
                const std::vector<double>& signs, const Group& g, double zStage, double pixelNm, double dxPx,
                double dyPx)
{
   const size_t n = dyes.size();
   if (!g.kernel.valid || n < 16)
   {
      for (size_t i = 0; i < n; ++i)
         SplatUnit(img, W, H, *dyes[i], g, zStage, pixelNm, signs[i], dxPx, dyPx);
      return;
   }
   std::vector<SplatPlan> plans(n);
   std::vector<char> ok(n, 0);
   ParallelFor(static_cast<unsigned>(n), [&](unsigned i) {
      const BlinkEvent& e = *dyes[i];
      const double xPx = e.xUm * 1000.0 / pixelNm + dxPx, yPx = e.yUm * 1000.0 / pixelNm + dyPx;
      ok[i] = PlanSplat(g.kernel, g.kernel.NearestZIndex(e.zNm / 1000.0 - zStage), xPx, yPx, 1.0,
                        g.kernel.interpMode, plans[i]);
   });
   const unsigned bands = std::max(1u, std::min(H, 64u));
   ParallelFor(bands, [&](unsigned b) {
      const int y0 = static_cast<int>(static_cast<uint64_t>(H) * b / bands);
      const int y1 = static_cast<int>(static_cast<uint64_t>(H) * (b + 1) / bands);
      for (size_t i = 0; i < n; ++i)
         if (ok[i])
            SplatUnitInto(img, W, H, g.kernel, plans[i], signs[i], y0, y1);
   });
}

void FluorescenceMovie::Impl::AdvanceAcc(Population& p, long f, double z, double dxPx, double dyPx)
{
   const Group& g = groups[p.group];
   const double pixelNm = S.p.pixelSizeNm;
   if (p.accFrame >= 0 && (p.accZ != z || p.accDx != dxPx || p.accDy != dyPx))
      p.accFrame = -1;   // another focal plane or a moved sample: start the running image afresh
   p.accZ = z;
   p.accDx = dxPx;
   p.accDy = dyPx;
   std::vector<const BlinkEvent*> dyes;
   std::vector<double> signs;
   auto covers = [](const BlinkEvent& e, long fr) { return e.tStart <= fr && e.tEnd >= fr + 1; };
   if (p.accFrame < 0)
   {
      // Live: the last movie's running image at this geometry, updated by the
      // dyes that entered or left it.
      std::vector<uint64_t> key;
      MovieCache::RunningImage* run = nullptr;
      if (carry)
      {
         auto bits = [&](double v) {
            uint64_t b;
            std::memcpy(&b, &v, sizeof b);
            key.push_back(b);
         };
         for (double v : { static_cast<double>(S.W), static_cast<double>(S.H), pixelNm, S.q.originXUm,
                           S.q.originYUm, z, dxPx, dyPx, g.sigmaPx, static_cast<double>(g.kernel.valid),
                           static_cast<double>(g.kernel.interpMode) })
            bits(v);
         key.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g.kernel.data.get())));
         key.push_back(cache.version);
         key.push_back(cache.dyeVersion);
         run = &cache.running[p.structure * 4 + p.state];
      }
      std::vector<std::pair<uint64_t, const BlinkEvent*>> now;
      for (const BlinkEvent& e : p.wins)
         if (covers(e, f))
            now.push_back({ run ? RunningDyeId(e) : 0, &e });
      auto byId = [](const std::pair<uint64_t, const BlinkEvent*>& a, const std::pair<uint64_t, const BlinkEvent*>& b) {
         return a.first < b.first;
      };
      bool built = false;
      if (run && run->key == key && run->carried < 256 && run->acc.size() == static_cast<size_t>(S.W) * S.H)
      {
         std::vector<std::pair<uint64_t, const BlinkEvent*>> sorted = now;
         std::sort(sorted.begin(), sorted.end(), byId);
         size_t i = 0, j = 0;
         while (i < run->in.size() || j < sorted.size())
         {
            if (j == sorted.size() || (i < run->in.size() && run->in[i].first < sorted[j].first))
            {
               dyes.push_back(&run->in[i++].second);
               signs.push_back(-1);
            }
            else if (i == run->in.size() || sorted[j].first < run->in[i].first)
            {
               dyes.push_back(sorted[j++].second);
               signs.push_back(1);
            }
            else
            {
               ++i;
               ++j;
            }
         }
         if (dyes.size() < now.size())   // fewer changes than a fresh build
         {
            p.acc.swap(run->acc);
            SplatUnits(p.acc, S.W, S.H, dyes, signs, g, z, pixelNm, dxPx, dyPx);
            run->carried++;
            built = true;
         }
         dyes.clear();
         signs.clear();
      }
      if (!built)
      {
         // In the windows' order (the reference's summation order).
         p.acc.assign(static_cast<size_t>(S.W) * S.H, 0.0);
         for (const auto& d : now)
         {
            dyes.push_back(d.second);
            signs.push_back(1);
         }
         SplatUnits(p.acc, S.W, S.H, dyes, signs, g, z, pixelNm, dxPx, dyPx);
         if (run)
            run->carried = 0;
      }
      if (run)
      {
         std::sort(now.begin(), now.end(), byId);
         run->key = key;
         run->in.clear();
         run->in.reserve(now.size());
         for (const auto& d : now)
            run->in.push_back({ d.first, *d.second });
         run->acc = p.acc;   // the image at frame f (later frames of this movie update p.acc only)
      }
   }
   else
   {
      for (const BlinkEvent& e : p.wins)
      {
         const bool was = covers(e, p.accFrame), isNow = covers(e, f);
         if (was != isNow)
         {
            dyes.push_back(&e);
            signs.push_back(isNow ? 1 : -1);
         }
      }
      SplatUnits(p.acc, S.W, S.H, dyes, signs, g, z, pixelNm, dxPx, dyPx);
   }
   p.accFrame = f;
}

bool FluorescenceMovie::Begin(const ScopeSpec& spec, bool gpuMode, std::string& err, WidefieldAccelerator* accel,
                              const DyeClock* clock)
{
   Impl& m = *impl_;
   m.t0 = std::chrono::steady_clock::now();
   m.spec = spec;
   m.gpuMode = gpuMode;
   m.accel = accel;
   m.clock = clock;
   if (!MakeScopeSetup(spec, m.S, err))
      return false;
   TimingLog("fl.make-setup", TimingSince(m.t0));
   const ScopeSetup& S = m.S;
   const double pixelNm = S.p.pixelSizeNm, um = pixelNm / 1000.0;
   m.zStage = ScopeSpecGet(spec, "z");
   auto tPhase = TimingClock::now();
#if defined(__EMSCRIPTEN__)
   // One thread: a second movie while a session is open (between its begin and
   // end) would wait for itself.
   if (!m.lock.owns_lock() && !m.lock.try_lock())
   {
      err = "another movie is being rendered in this process";
      return false;
   }
#else
   if (!m.lock.owns_lock())
      m.lock.lock();
#endif
   if (!ConfigureShared(m.cache, S.cf, err))
      return false;
   CellFieldSource& source = m.cache.source;
   TimingLog("fl.configure", TimingSince(tPhase));
   m.curEnv = ScopeKineticEnv(spec);
   m.envPhys.clear();
   m.hists.clear();
   // The dye clocks: one region at start-sec over the whole query, or the
   // host clock's regions (each queried at its own time over its bounding
   // box, keeping only the dyes whose position has that clock).
   std::vector<ClockRegion> regions;
   tPhase = TimingClock::now();
   if (clock)
   {
      clock->Regions(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, regions);
      m.tMin = regions.empty() ? 0.0 : regions[0].tSec;
      for (const ClockRegion& r : regions)
         m.tMin = std::min(m.tMin, r.tSec);
   }
   else
   {
      ClockRegion r;
      r.tSec = S.q.tSec;
      r.x0Um = S.q.x0Um;
      r.y0Um = S.q.y0Um;
      r.x1Um = S.q.x1Um;
      r.y1Um = S.q.y1Um;
      regions.push_back(r);
   }
   TimingLog("fl.clock-regions", TimingSince(tPhase), std::to_string(regions.size()).c_str());
   // Their rate histories (ABI 11): the kinetics rows of each region's past
   // (segments in other envs: their light path and labels).
   tPhase = TimingClock::now();
   size_t multiRegions = 0;
   for (const ClockRegion& r : regions)
   {
      const HistInfo* hi = clock ? m.HistOf(r.history, err) : nullptr;
      if (clock && !hi)
         return false;
      multiRegions += hi && hi->multi;
   }
   TimingLog("fl.history", TimingSince(tPhase), std::to_string(multiRegions).c_str());
   auto useHistory = [&](const ClockRegion& r) {
      std::string e;
      const HistInfo* hi = clock ? m.HistOf(r.history, e) : nullptr;
      if (hi && hi->multi)
         return source.SetKineticsHistory(hi->rows.data(), static_cast<int>(hi->tStart.size()));
      return source.SetKineticsHistory(nullptr, 0);
   };
   auto regionQuery = [&](const ClockRegion& r) {
      CellFieldQuery q = S.q;
      q.tSec = r.tSec;
      q.x0Um = std::max(S.q.x0Um, r.x0Um);
      q.y0Um = std::max(S.q.y0Um, r.y0Um);
      q.x1Um = std::min(S.q.x1Um, r.x1Um);
      q.y1Um = std::min(S.q.y1Um, r.y1Um);
      return q;
   };
   auto inRegion = [&](const BlinkEvent& e, const ClockRegion& r) {
      if (!clock)
         return true;
      double t = 0;
      uint32_t h = 0;
      clock->KeyAt(e.xUm + S.q.originXUm, e.yUm + S.q.originYUm, t, h);
      return t == r.tSec && h == r.history;
   };
   auto regionEvents = [&](bool continuous, std::vector<BlinkEvent>& out, std::vector<const ClockRegion*>* regionOf) {
      for (const ClockRegion& r : regions)
      {
         const CellFieldQuery q = regionQuery(r);
         if (!(q.x1Um > q.x0Um && q.y1Um > q.y0Um))
            continue;
         std::vector<BlinkEvent> got;
         if (!useHistory(r) || !(continuous ? source.Continuous(q, got) : source.Events(q, got)))
            return false;
         for (const BlinkEvent& e : got)
            if (inRegion(e, r))
            {
               out.push_back(e);
               if (regionOf)
                  regionOf->push_back(&r);
            }
      }
      source.SetKineticsHistory(nullptr, 0);
      return true;
   };

   // ---- groups: (structure, state) with their PSF and detected photons per frame ----
   struct Want { int s; bool pre; const StatePhysics* st; };
   std::vector<Want> wanted;
   for (int s = 0; s < static_cast<int>(S.labels.size()); ++s)
      for (bool pre : { false, true })
      {
         const StatePhysics& st = pre ? S.labels[static_cast<size_t>(s)].pre : S.labels[static_cast<size_t>(s)].main;
         if (st.emits && st.detectedFraction >= kMinDetectedFraction && st.detectedPerSec > 0)
            wanted.push_back({ s, pre, &st });
      }
   SetPsfKernelMemoEntries(wanted.size() + 1);
   m.groups.resize(wanted.size());
   for (size_t i = 0; i < wanted.size(); ++i)
   {
      Group& g = m.groups[i];
      g.structure = wanted[i].s;
      g.pre = wanted[i].pre;
      g.lambdaNm = KernelWavelengthNm(wanted[i].st->lambdaNm);
      g.sigmaPx = GaussianSigmaPx(g.lambdaNm, ScopeSpecGet(spec, "na"), pixelNm);
      g.detectedFraction = wanted[i].st->detectedFraction;
      g.detectedPerSec = wanted[i].st->detectedPerSec;
      g.perFrame = wanted[i].st->detectedPerSec * S.expSec;
   }
   auto groupOf = [&](int s, bool pre) -> int {
      for (size_t i = 0; i < m.groups.size(); ++i)
         if (m.groups[i].structure == s && m.groups[i].pre == pre)
            return static_cast<int>(i);
      return -1;
   };
   m.anyBlinks = false;
   for (const LabelPhysics& l : S.labels)
      m.anyBlinks = m.anyBlinks || l.mode != MODE_WIDEFIELD;

   // The PSF kernels compute while the blinks are queried (as the camera's
   // stack generation does); serially under Emscripten.
   std::string psfErr;
   bool psfOk = true, eventsOk = true;
   double psfSec = 0.0, eventsSec = 0.0;
   auto kernels = [&]() {
      const auto tk = TimingClock::now();
      for (Group& g : m.groups)
      {
         if (psfOk && !ScopePsfKernel(spec, g.lambdaNm, g.kernel, psfErr))
            psfOk = false;
         if (psfOk)
            g.blinkKernel = WithHaloCut(g.kernel, ScopeSpecGet(spec, "psf-halo-cut"));
      }
      psfSec = TimingSince(tk);
   };
   std::vector<BlinkEvent> all;
   tPhase = TimingClock::now();
#if defined(__EMSCRIPTEN__)
   kernels();
   const auto tEv = TimingClock::now();
   if (psfOk && m.anyBlinks)
      eventsOk = regionEvents(false, all, nullptr);
   eventsSec = TimingSince(tEv);
#else
   std::thread psfThread(kernels);
   if (m.anyBlinks)
      eventsOk = regionEvents(false, all, nullptr);
   eventsSec = TimingSince(tPhase);
   psfThread.join();
#endif
   TimingLog("fl.psf-kernels", psfSec, std::to_string(m.groups.size()).c_str());
   TimingLog("fl.events-query", eventsSec);
   if (!psfOk)
   {
      err = psfErr;
      return false;
   }
   if (!eventsOk)
   {
      err = "cell-field event query failed";
      return false;
   }
   tPhase = TimingClock::now();
   m.events.clear();
   for (const BlinkEvent& e : all)
      if (groupOf(e.structure, false) >= 0)
         m.events.push_back(e);
   std::vector<BlinkEvent>().swap(all);
   m.byGroup.assign(m.groups.size(), {});
   m.buckets.assign(m.groups.size(), {});
   for (size_t gi = 0; gi < m.groups.size(); ++gi)
   {
      if (!m.groups[gi].pre)
         for (const BlinkEvent& e : m.events)
            if (e.structure == m.groups[gi].structure)
               m.byGroup[gi].push_back(e);
      m.buckets[gi] = BucketEventsByFrame(m.byGroup[gi], S.N);
   }
   TimingLog("fl.events-sort", TimingSince(tPhase), std::to_string(m.events.size()).c_str());

   // ---- continuous populations ----
   // Counted from the structure's dyes (every dye has the window from t = 0):
   // nZ in the query rect and z range, nSlab in the FOV within the focal slab.
   const double inf = std::numeric_limits<double>::infinity();
   const double focus = S.q.zCullCentreUm, slabHalf = S.meanFieldSlabNm / 2000;
   m.fovArea = S.W * S.H * um * um;
   const double zMin = S.q.zHalfRangeUm > 0 ? focus - S.q.zHalfRangeUm : -inf;
   const double zMax = S.q.zHalfRangeUm > 0 ? focus + S.q.zHalfRangeUm : inf;
   m.pops.clear();
   tPhase = TimingClock::now();
   for (int s = 0; s < static_cast<int>(S.labels.size()); ++s)
   {
      const LabelPhysics& L = S.labels[static_cast<size_t>(s)];
      std::vector<int> states;
      if (L.mode == MODE_WIDEFIELD)
         states.push_back(ISC_STATE_ALWAYS_ON);
      if (L.mode == MODE_DSTORM && L.initialOnSec > 0)
         states.push_back(ISC_STATE_INITIAL_ON);
      if (L.mode == MODE_PALM && L.preState)
         states.push_back(ISC_STATE_PRE);
      if (states.empty())
         continue;
      const double ox = S.q.originXUm, oy = S.q.originYUm;
      const long nZ = m.cache.CountDyes(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, 1 << s);
      const double sLo = std::max(zMin, focus - slabHalf), sHi = std::min(zMax, focus + slabHalf);
      const long nSlab = sHi > sLo ? m.cache.CountDyes(ox, oy, ox + S.W * um, oy + S.H * um, sLo, sHi, 1 << s) : 0;
      if (nZ < 0 || nSlab < 0)
      {
         err = "cell-field dye count failed";
         return false;
      }
      for (int state : states)
      {
         const bool pre = state == ISC_STATE_PRE;
         const int gi = groupOf(s, pre);
         if (gi < 0 || !nZ)
            continue;
         const StatePhysics& st = pre ? L.pre : L.main;
         Population p;
         p.structure = s;
         p.state = state;
         p.group = static_cast<size_t>(gi);
         if (state == ISC_STATE_ALWAYS_ON)
         {
            p.budget = L.photonBudget;
            p.lambda = p.budget > 0 ? st.emissionPerSec / p.budget : 0;
         }
         else if (state == ISC_STATE_INITIAL_ON)
            p.lambda = 1 / L.initialOnSec;
         else
         {
            p.budget = L.prePhotonBudget;
            p.lambda = L.kActPerSec + (p.budget > 0 ? st.emissionPerSec / p.budget : 0);
         }
         p.emissionPerSec = st.emissionPerSec;
         p.rate = st.detectedPerSec;
         p.nZ = nZ;
         p.nSlab = nSlab;
         // Rate histories: the largest survival at frame 0 over the regions.
         if (clock)
         {
            double maxS = 0;
            for (const ClockRegion& r : regions)
            {
               const HistInfo* hi = m.HistOf(r.history, err);
               if (!hi)
                  return false;
               p.histMulti = p.histMulti || hi->multi;
               maxS = std::max(maxS, hi->multi ? HistSurvival(*hi, s, state, r.tSec) : std::exp(-p.lambda * r.tSec));
            }
            p.maxSurvival = maxS;
         }
         m.pops.push_back(std::move(p));
      }
   }
   // The per-dye windows in frames (fetched once, for every population that
   // needs them): a dye with a photon budget bleaches at aux x budget /
   // emission rate.
   TimingLog("fl.dye-counts", TimingSince(tPhase));
   tPhase = TimingClock::now();
   std::vector<BlinkEvent> windows;
   std::vector<const ClockRegion*> windowRegion;   // each window's region (its dye's clock at frame 0, its history)
   bool haveWindows = false;
   for (Population& p : m.pops)
   {
      p.meanFieldAt0 = m.IsMeanField(p, 0);
      p.needWindows = !m.IsMeanField(p, S.N - 1);
      if (!p.needWindows)
         continue;   // mean-field to the end: no windows needed
      if (!haveWindows)
      {
         if (!regionEvents(true, windows, &windowRegion))
         {
            err = "cell-field window query failed";
            return false;
         }
         haveWindows = true;
      }
      for (size_t wi = 0; wi < windows.size(); ++wi)
      {
         const BlinkEvent& e = windows[wi];
         if (e.structure != p.structure || e.state != p.state)
            continue;
         const ClockRegion& reg = *windowRegion[wi];
         const double tq = reg.tSec;
         double tEndSec = (e.tEnd - S.q.frameIndex) * S.expSec + tq;   // back to seconds (Continuous mapped it)
         if (p.budget > 0)
         {
            // The photon budget: used up at aux x budget / emission, or over a
            // rate history at the clock where the walk reaches aux.
            const HistInfo* hi = clock ? m.HistOf(reg.history, err) : nullptr;
            tEndSec = std::min(tEndSec, hi && hi->multi ? HistBleachEnd(*hi, p.structure, p.state, e.aux)
                                                        : e.aux * p.budget / p.emissionPerSec);
         }
         const double b = (tEndSec - tq) / S.expSec;
         if (b > 0)
         {
            BlinkEvent w = e;
            w.tStart = (0 - tq) / S.expSec;
            w.tEnd = b;
            p.wins.push_back(w);
         }
      }
   }
   TimingLog("fl.windows", TimingSince(tPhase), std::to_string(windows.size()).c_str());
   // The mean-field images of the populations that start mean-field.
   m.sceneQueue.clear();
   for (Population& p : m.pops)
      if (p.meanFieldAt0)
      {
         if (!m.BuildMeanField(p, err))
            return false;
         if (gpuMode)
            m.sceneQueue.push_back(&p);
      }
   m.imagerPerFrame = 0;
   for (const LabelPhysics& L : S.labels)
      m.imagerPerFrame = m.imagerPerFrame + L.ImagerBackgroundPerPxPerSec(um);
   m.imagerPerFrame *= S.expSec;
   m.bg = S.p.backgroundPhotons + m.imagerPerFrame;
   m.setupSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.t0).count();
   TimingLog("fl.setup", m.setupSec);
   return true;
}

bool FluorescenceMovie::HasPopulations() const
{
   return !impl_->pops.empty();
}


int FluorescenceMovie::MeanFieldScenes() const
{
   return static_cast<int>(impl_->sceneQueue.size());
}

WidefieldScene& FluorescenceMovie::MeanFieldScene(int i)
{
   return impl_->sceneQueue[static_cast<size_t>(i)]->slot->scene;
}

bool FluorescenceMovie::SetMeanFieldImages(int i, std::vector<std::vector<float>>& images)
{
   Impl& m = *impl_;
   if (i < 0 || i >= static_cast<int>(m.sceneQueue.size()))
      return false;
   Population& p = *m.sceneQueue[static_cast<size_t>(i)];
   if (!p.slot->scene.SetImages(images))
      return false;
   m.TakeImage(p);
   return true;
}

void FluorescenceMovie::ComputeCpuImages()
{
   Impl& m = *impl_;
   for (Population* p : m.sceneQueue)
   {
      if (!p->image.empty())
         continue;
      p->slot->scene.ComputeCpuImages();
      m.TakeImage(*p);
   }
}

FluorescenceSimplePlan FluorescenceMovie::SimplePlan() const
{
   const Impl& m = *impl_;
   FluorescenceSimplePlan sp;
   // The blink groups: the main groups of the blinking labels (a WideField
   // label's main group only lights its population).
   int blinkGroup = -1, blinkGroups = 0;
   for (size_t gi = 0; gi < m.groups.size(); ++gi)
   {
      const Group& g = m.groups[gi];
      if (g.pre || m.S.labels[static_cast<size_t>(g.structure)].mode == MODE_WIDEFIELD)
         continue;
      blinkGroup = static_cast<int>(gi);
      ++blinkGroups;
   }
   if (blinkGroups != 1)
      return sp;
   const Group& g = m.groups[static_cast<size_t>(blinkGroup)];
   if (m.byGroup[static_cast<size_t>(blinkGroup)].size() != m.events.size())
      return sp;   // blinks of another group (none should be)
   sp.ok = true;
   sp.kernel = g.blinkKernel.valid ? &g.blinkKernel : nullptr;
   sp.photonsPerBlink = g.perFrame;
   sp.sigmaPx = g.sigmaPx;
   sp.backgroundPhotons = m.bg;
   sp.events = &m.byGroup[static_cast<size_t>(blinkGroup)];
   sp.populations = !m.pops.empty();
   return sp;
}

bool FluorescenceMovie::Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                               ScopeMovieInfo& info, std::string& err, const ScopeProgress* progress,
                               const FluorescenceFrameOptions* options)
{
   Impl& m = *impl_;
   const FluorescenceFrameOptions none;
   const FluorescenceFrameOptions& opt = options ? *options : none;
   const ScopeSetup& S = m.S;
   // Drift (the spec's; a host's own hooks win): the sample moved by d, the
   // focal plane dz lower in it.
   const bool drift = S.driftOn && !opt.driftPx;
   auto zAt = [&](long f) {
      const double z = opt.zStageUm ? opt.zStageUm(f) : m.zStage;
      return drift ? z - S.drift[static_cast<size_t>(f)].z / 1000.0 : z;
   };
   auto zBaseAt = [&](long f) { return opt.zStageUm ? opt.zStageUm(f) : m.zStage; };
   if (drift)
      m.driftGrid = DriftFocusGrid::For(S.driftRange);
   const unsigned W = S.W, H = S.H;
   const long N = S.N;
   const size_t n = static_cast<size_t>(W) * H;
   const double pixelNm = S.p.pixelSizeNm;
   for (Population& p : m.pops)
      if (p.meanFieldAt0 && p.image.empty())
      {
         p.slot->scene.ComputeCpuImages();   // GPU mode with no images handed over
         m.TakeImage(p);
      }
   (void)err;
   info.width = W;
   info.height = H;
   info.frames = N;
   info.blinks = m.events.size();
   info.dyes = 0;
   for (const Population& p : m.pops)
      info.dyes += p.nZ;
   info.halfTimeSec = std::numeric_limits<double>::infinity();
   info.querySec = m.setupSec;
   {
      auto O = [&](const char* k) { return ScopeSpecGet(m.spec, k); };
      std::string d;
      char b[512];
      std::snprintf(b, sizeof b,
                    "insiliscope modality=Fluorescence seed=%ld world_seed=%u x=%g y=%g z=%g size=%u pixel_nm=%g "
                    "exposure_ms=%g start_sec=%g frames=%ld focus_um=%g lasers=",
                    S.seed, S.cf.seed, O("x"), O("y"), O("z"), W, pixelNm, S.expSec * 1000, S.t0Sec, N, O("focus-um"));
      d = b;
      for (size_t l = 0; l < S.lp.lasers.size(); ++l)
      {
         std::snprintf(b, sizeof b, "%s%g:%g", l ? "," : "", S.lp.lasers[l].nm, S.lp.lasers[l].kWPerCm2);
         d += b;
      }
      d += " dichroic=" + std::string(DichroicIds()[static_cast<size_t>(O("dichroic"))]);
      for (size_t s = 0; s < S.labels.size(); ++s)
      {
         const LabelPhysics& L = S.labels[s];
         std::snprintf(b, sizeof b, " label%zu=%s:%s:%.4g%%:kact=%.4g", s, L.eff.dye.id, DyeModeNames()[static_cast<size_t>(L.mode)].c_str(),
                       100 * L.label[ISC_LABEL_DENSITY], L.kActPerSec);
         d += b;
      }
      for (const Group& g : m.groups)
      {
         std::snprintf(b, sizeof b, " group=%d:%s:%gnm:%.4g_photons_per_frame:%.4g_detected", g.structure,
                       g.pre ? "pre" : "main", g.lambdaNm, g.perFrame, g.detectedFraction);
         d += b;
      }
      std::snprintf(b, sizeof b, " imager_per_px_per_frame=%.4g psf=%s", m.imagerPerFrame,
                    !m.groups.empty() && m.groups[0].kernel.valid ? "GibsonLanniZernike" : "Gaussian");
      d += b;
      if (S.driftOn)
      {
         const DriftSettings& ds = S.p.drift;
         std::snprintf(b, sizeof b, " drift_xy_speed=%g drift_z_speed=%g drift_angle=%g drift_z_direction=%d "
                       "drift_angle_wander=%g drift_z_angle_wander=%g "
                       "drift_speed_wander=%g drift_wander_time=%g drift_xy_rms=%g drift_z_rms=%g",
                       ds.xySpeedNmPerSec, ds.zSpeedNmPerSec, ds.xyAngleDeg, ds.zDirection, ds.angleWanderDeg,
                       ds.zAngleWanderDeg, ds.speedWanderPct,
                       ds.wanderTimeSec, ds.xyNmPerSqrtSec, ds.zNmPerSqrtSec);
         d += b;
      }
      info.description = d;
   }
   DriftInfo(S, info);

   // The noise maps only when the movie applies the noise (a host with
   // onPhotons adds its own: a live frame skips building three maps).
   std::unique_ptr<NoiseSetup> noisePtr;
   if (!opt.onPhotons)
      noisePtr.reset(new NoiseSetup(S.seed, W, H, S.p));
   const CameraNoiseParams cam = S.p.Camera();
   TimingSum tBlinks, tPops, tNoise, tWrite;
   const unsigned long spawns0 = ParallelForSpawns().load();
   // Frames are independent but for the per-dye populations' running images:
   // the blinks and the noise of a batch of frames are made on all cores
   // (serial under Emscripten), the continuous populations in frame order.
#if defined(__EMSCRIPTEN__)
   const long batch = 1;
#else
   const long batch = std::max(32L, 4L * static_cast<long>(std::thread::hardware_concurrency()));
#endif
   const size_t slots = static_cast<size_t>(std::min(batch, std::max(N, 1L)));
   std::vector<std::vector<uint16_t>> adu(slots);
   std::vector<std::vector<float>> photons(slots);
   std::vector<std::vector<BlinkEvent>> fe(slots);
   std::vector<long> frameBlinks(slots);
   long blinks = 0;
   RenderExtras into;
   into.accumulate = true;
   // The continuous populations' per-dye splats run frame by frame: in bands
   // of rows on all cores (the same pixels as serial, RenderPhotonImage).
   into.parallel = true;
   RenderExtras blinkInto = into;   // the blinks also see the host's illumination field
   const bool shaped = opt.illumField && opt.illumField->size() == n;
   if (shaped)
      blinkInto.illumField = opt.illumField;
#if defined(__EMSCRIPTEN__)
   const long cores = 1;
#else
   const long cores = std::max(1L, static_cast<long>(std::thread::hardware_concurrency()));
#endif
   const bool popsOnly = opt.populationsOnly && opt.onPhotons;
   for (long f0 = 0; f0 < N; f0 += batch)
   {
      const long nb = std::min(batch, N - f0);
      tBlinks.Start();
      // The blinks: frame per core, or -- fewer frames than cores (a live
      // frame, a render-ahead batch) -- one frame after the other, each in
      // bands of rows on all cores.
      const bool bands = nb < cores;
      blinkInto.parallel = bands;
      auto blinkFrame = [&](unsigned k) {
         const long f = f0 + static_cast<long>(k);
         frameBlinks[k] = 0;
         if (popsOnly)
         {
            photons[k].assign(n, 0.0f);   // the host splats the background and the blinks
            return;
         }
         const double bgScale = opt.backgroundScale ? opt.backgroundScale(f) : 1.0;
         if (!shaped && bgScale == 1.0)
            photons[k].assign(n, static_cast<float>(m.bg));
         else
         {
            // The adapter's shaped background: x the illumination field, x the fade.
            photons[k].resize(n);
            for (size_t i = 0; i < n; ++i)
            {
               double b = m.bg;
               if (shaped)
                  b *= (*opt.illumField)[i];
               photons[k][i] = static_cast<float>(b * bgScale);
            }
         }
         double dx = 0.0, dy = 0.0;
         if (opt.driftPx)
            opt.driftPx(f, dx, dy);
         else if (drift)
         {
            dx = S.drift[static_cast<size_t>(f)].x / pixelNm;
            dy = S.drift[static_cast<size_t>(f)].y / pixelNm;
         }
         const double zf = zAt(f);
         frameBlinks[k] = 0;
         for (size_t gi = 0; gi < m.groups.size(); ++gi)
         {
            const Group& g = m.groups[gi];
            if (g.pre)
               continue;
            fe[k].clear();
            for (uint32_t i : m.buckets[gi][static_cast<size_t>(f)])
               fe[k].push_back(m.byGroup[gi][i]);
            frameBlinks[k] += static_cast<long>(fe[k].size());
            RenderPhotonImage(photons[k], W, H, fe[k], f, pixelNm, g.sigmaPx, g.perFrame, 0.0, dx, dy,
                              g.blinkKernel.valid ? &g.blinkKernel : nullptr, zf, nullptr, nullptr, &blinkInto);
         }
      };
      if (bands)
         for (long k = 0; k < nb; ++k)
            blinkFrame(static_cast<unsigned>(k));
      else
         ParallelFor(static_cast<unsigned>(nb), blinkFrame);
      tBlinks.Stop();
      // The continuous populations, frame by frame.
      std::vector<std::vector<std::string>> paths(static_cast<size_t>(nb));
      tPops.Start();
      for (long k = 0; k < nb; ++k)
      {
         const long f = f0 + k;
         const double zf = zAt(f);
         std::vector<float>& img = photons[static_cast<size_t>(k)];
         for (Population& p : m.pops)
         {
            const Group& g = m.groups[p.group];
            const double tf0 = S.t0Sec + f * S.expSec, tf1 = tf0 + S.expSec;
            if (m.IsMeanField(p, f))
            {
               if (p.image.empty())
               {
                  // A population that turns mean-field later: never (they only
                  // decay), but keep the frame valid.
                  continue;
               }
               std::vector<float> driftImage;
               if (drift)
               {
                  driftImage.assign(n, 0.0f);
                  m.DriftMeanField(p, zBaseAt(f), S.drift[static_cast<size_t>(f)], driftImage);
               }
               const std::vector<float>& image = drift ? driftImage : m.ImageAt(p, zf);
               if (p.weighted)
               {
                  // Frame 0 at each column's clock, every clock f exposures later.
                  const double sc = p.lambda > 0 ? std::exp(-p.lambda * f * S.expSec) : 1.0;
                  for (size_t i = 0; i < n; ++i)
                     img[i] += static_cast<float>(sc * image[i]);
               }
               else
               {
                  const float mp = static_cast<float>(MeanPhotons(p.rate, p.lambda, tf0, tf1));
                  for (size_t i = 0; i < n; ++i)
                     img[i] += static_cast<float>(static_cast<double>(mp) * image[i]);
               }
               p.meanFieldFrames++;
               paths[static_cast<size_t>(k)].push_back(std::string(PopulationName(p.state)) + ": mean-field (FFT)");
            }
            else
            {
               double ddx = 0.0, ddy = 0.0;
               if (opt.driftPx)
                  opt.driftPx(f, ddx, ddy);
               else if (drift)
               {
                  ddx = S.drift[static_cast<size_t>(f)].x / pixelNm;
                  ddy = S.drift[static_cast<size_t>(f)].y / pixelNm;
               }
               m.AdvanceAcc(p, f, zf, ddx, ddy);
               const double perFrame = p.rate * S.expSec;
               for (size_t i = 0; i < n; ++i)
                  if (p.acc[i] != 0)
                     img[i] += static_cast<float>(perFrame * p.acc[i]);
               // Windows that start or end inside this frame: their overlap.
               std::vector<BlinkEvent> partial;
               for (const BlinkEvent& e : p.wins)
                  if (e.tStart < f + 1 && e.tEnd > f && !(e.tStart <= f && e.tEnd >= f + 1))
                     partial.push_back(e);
               RenderPhotonImage(img, W, H, partial, f, pixelNm, g.sigmaPx, perFrame, 0.0, ddx, ddy,
                                 g.kernel.valid ? &g.kernel : nullptr, zf, nullptr, nullptr, &into);
               p.perDyeFrames++;
               paths[static_cast<size_t>(k)].push_back(std::string(PopulationName(p.state)) + ": per dye (" +
                                                       std::to_string(p.wins.size()) + " windows)");
            }
         }
      }
      tPops.Stop();
      tNoise.Start();
      if (!opt.onPhotons)
         ParallelFor(static_cast<unsigned>(nb), [&](unsigned k) {
            ApplyNoiseChain(photons[k], adu[k], W, H, cam, noisePtr->offsetMap, noisePtr->gainMap, noisePtr->rnMap,
                            noisePtr->noiseSeed, static_cast<uint32_t>(f0 + static_cast<long>(k)));
         });
      tNoise.Stop();
      tWrite.Start();
      bool more = true;
      for (long k = 0; k < nb && more; k++)
      {
         blinks += frameBlinks[static_cast<size_t>(k)];
         more = opt.onPhotons ? opt.onPhotons(f0 + k, photons[static_cast<size_t>(k)])
                              : onFrame(f0 + k, adu[static_cast<size_t>(k)]);
         if (progress && *progress)
         {
            // Which backend drew this frame: the SMLM splat for blinks, mean-field or per dye per population.
            std::string j = "{\"frame\":" + std::to_string(f0 + k) + ",\"frames\":" + std::to_string(N) +
                            ",\"blinks\":" + std::to_string(frameBlinks[static_cast<size_t>(k)]) + ",\"backends\":[";
            bool first = true;
            if (m.anyBlinks)
            {
               j += "\"SMLM: " + std::to_string(frameBlinks[static_cast<size_t>(k)]) + " blinks (splat)\"";
               first = false;
            }
            for (const std::string& s : paths[static_cast<size_t>(k)])
            {
               j += (first ? "\"" : ",\"") + JsonEscape(s) + "\"";
               first = false;
            }
            j += "]}";
            (*progress)("frames", static_cast<double>(f0 + k + 1) / N, j);
         }
      }
      tWrite.Stop();
      if (!more)
         break;
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.t0).count();
   tBlinks.Log("fl.frame.blinks (batches)");
   tPops.Log("fl.frame.continuous");
   tNoise.Log("fl.frame.noise (batches)");
   tWrite.Log("fl.frame.onFrame");
   if (TimingEnabled())
   {
      char b[128];
      std::snprintf(b, sizeof b, "blinks %ld, groups %zu, populations %zu, ParallelFor spawns %lu", blinks,
                    m.groups.size(), m.pops.size(), ParallelForSpawns().load() - spawns0);
      TimingLog("fl.total", info.totalSec, b);
   }
   return true;
}

bool ScopeBrightfieldSpec(const ScopeSpec& spec, BrightfieldSpec& bs, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
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
   // A drifting sample: the frames are the grid's image shifted, so the
   // margin grows by the xy drift (plus a pixel).
   if (S.driftOn)
      bs.marginUm = bs.Resolved().marginUm + (std::ceil(S.driftRange.MaxXyNm() / S.p.pixelSizeNm) + 1.0) *
                                                 S.p.pixelSizeNm / 1000.0;
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
      if (ScopePsfRequest(spec, bs.wavelengthNm, req, e))
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

// The lamp's photons per frame (BrightfieldFrames, the combined light): the
// image at the spec's focus (computed, or assembled by SetImageFromSources)
// times bf-photons-per-px-per-sec x exposure; a drifting sample: the
// fine-grid spectra on the drift's focus grid, a shifted, interpolated image
// per frame (JS brightfieldPhotons).
struct BrightfieldPhotons
{
   BrightfieldDriftFrames driftFrames;
   std::vector<float> trans, photons;
   double flux = 0.0;
   bool drift = false;

   bool Begin(const ScopeSpec& spec, const ScopeSetup& S, BrightfieldScene& scene, std::string& err)
   {
      const double focusUm = S.q.zCullCentreUm;
      auto tPhase = TimingClock::now();
      drift = S.driftOn;
      if (drift)
      {
         driftFrames.Begin(S.driftRange, std::vector<double>(1, focusUm));
         if (!driftFrames.Refresh(scene, 0, err))
            return false;
      }
      else if (!scene.Image(focusUm, trans, err))
         return false;
      TimingLog("bf.image", TimingSince(tPhase));
      flux = std::max(0.0, ScopeSpecGet(spec, "bf-photons-per-px-per-sec")) * S.expSec;
      photons.resize(trans.size());
      for (size_t i = 0; i < trans.size(); ++i)
         photons[i] = static_cast<float>(trans[i] * flux);
      return true;
   }
   // Frame f's photons (valid until the next call).
   const std::vector<float>& At(BrightfieldScene& scene, const ScopeSetup& S, long f)
   {
      if (drift)
      {
         driftFrames.Image(scene, 0, S.drift[static_cast<size_t>(f)], trans);
         photons.resize(trans.size());
         for (size_t i = 0; i < trans.size(); ++i)
            photons[i] = static_cast<float>(trans[i] * flux);
      }
      return photons;
   }
};

// The frames of a BrightField movie from its scene: the lamp's photons, then
// the camera noise per frame.
static bool BrightfieldFrames(const ScopeSpec& spec, const ScopeSetup& S, const BrightfieldSpec& bs,
                              BrightfieldScene& scene, std::chrono::steady_clock::time_point t0,
                              unsigned long spawns0,
                              const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                              ScopeMovieInfo& info, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const SimulationParams& p = S.p;
   const double focusUm = S.q.zCullCentreUm;
   BrightfieldPhotons lamp;
   if (!lamp.Begin(spec, S, scene, err))
      return false;
   const unsigned W = S.W, H = S.H;
   const long N = S.N;
   const double flux = lamp.flux;
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
                 "absorption_per_um=%g photons_per_px=%.4g drift_xy=%g drift_z=%g setup_ms=%.0f image_ms=%.0f",
                 S.seed, S.cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, S.expSec * 1000, N, focusUm, bs.quality,
                 scene.Sources(), q.upscale, q.sub, scene.Slices(), scene.GridNx(), scene.GridNy(), bs.na,
                 bs.condenserNa, bs.wavelengthNm, bs.nMedium, bs.nCytoplasm, bs.nNucleus, bs.nMicrotubule,
                 bs.absorptionPerUm, flux, p.drift.xyNmPerSqrtSec, p.drift.zNmPerSqrtSec, scene.SetupMs(),
                 scene.LastImageMs());
   info.description = desc;
   DriftInfo(S, info);
   NoiseSetup noise(S.seed, W, H, p);
   std::vector<uint16_t> adu;
   TimingSum tNoise, tWrite;
   for (long f = 0; f < N; f++)
   {
      const std::vector<float>& photons = lamp.At(scene, S, f);
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
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
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
   if (!MakeScopeSetup(spec, m.S, err) || !ScopeBrightfieldSpec(spec, m.bs, err))
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
   if (!MakeScopeSetup(spec, m.S, err) || !ScopeBrightfieldSpec(spec, m.bs, err))
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
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
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


// prepare=1: the shared world (MovieCache) and, for Fluorescence, the PSF
// kernels of the labels' states (ComputePsfKernelCache's memo and, with
// disk-cache 2, its file), so a movie that follows finds both ready. No
// frames (JS renderScopeMovie's prepare).
static bool PrepareScope(const ScopeSpec& spec, ScopeMovieInfo& info, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   MovieCache& cache = SharedMovieCache();
   std::lock_guard<std::mutex> lock(cache.mutex);
   if (!ConfigureShared(cache, S.cf, err))
      return false;
   // The FOV's cells: packs (or takes from the block store) the blocks the
   // movie's query will touch. Assets and dyes stay with the movie (they
   // depend on its z range and labels).
   if (isc_cells_in_window(cache.source.World(), S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, nullptr, 0) < 0)
   {
      err = "cell-field query failed";
      return false;
   }
   const double tWorld = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   bool anyKernel = false;
   bool epi = false, trans = false;
   ScopeLights(spec, epi, trans);
   if (epi)
   {
      std::vector<double> lambdas;
      for (const LabelPhysics& l : S.labels)
         for (const StatePhysics* st : { &l.main, &l.pre })
            if (st->emits && st->detectedFraction > 0)
               lambdas.push_back(KernelWavelengthNm(st->lambdaNm));
      SetPsfKernelMemoEntries(lambdas.size() + 1);
      for (double lambda : lambdas)
      {
         PsfKernelCache kernel;
         if (!ScopePsfKernel(spec, lambda, kernel, err))
            return false;
         anyKernel = anyKernel || kernel.valid;
      }
   }
   info = ScopeMovieInfo();
   info.width = S.W;
   info.height = S.H;
   info.frames = 0;
   info.querySec = tWorld;
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   info.description = anyKernel ? "prepared GibsonLanniZernike" : "prepared Gaussian";
   return true;
}

bool PrefetchScope(const ScopeSpec& specIn, double marginUm, double budgetMs, const DyeClock* clock)
{
   ScopeSpec spec = specIn;
   std::vector<ClockSegment> segs;
   if (clock)
   {
      // The FOV centre's clock and rate history.
      double t = 0;
      uint32_t h = 0;
      clock->KeyAt(ScopeSpecGet(spec, "x"), ScopeSpecGet(spec, "y"), t, h);
      spec["start-sec"] = t;
      clock->Segments(h, segs);
   }
   ScopeSetup S;
   std::string err;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   HistInfo hi;
   EnvPhysicsMemo memo;
   if (!MakeHistInfo(spec, S.camera, ScopeKineticEnv(spec), segs, memo, hi, err))
      return false;
   MovieCache& cache = SharedMovieCache();
   std::unique_lock<std::mutex> lock(cache.mutex, std::try_to_lock);
   if (!lock.owns_lock() || !cache.haveWorld || !cache.world.SameWorld(S.cf))
      return false;
   const bool histOk = hi.multi ? cache.source.SetKineticsHistory(hi.rows.data(), static_cast<int>(hi.tStart.size()))
                                : cache.source.SetKineticsHistory(nullptr, 0);
   const bool ok = histOk && cache.source.Prefetch(S.q, marginUm, budgetMs);
   cache.source.SetKineticsHistory(nullptr, 0);
   return ok;
}

// No light: the camera's noise on zero photons (dark current, read noise,
// offset; JS renderDarkMovie).
static bool RenderDarkMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                            ScopeMovieInfo& info, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   info = ScopeMovieInfo();
   info.width = S.W;
   info.height = S.H;
   info.frames = S.N;
   char desc[256];
   std::snprintf(desc, sizeof desc, "insiliscope light=none seed=%ld size=%u pixel_nm=%g exposure_ms=%g frames=%ld",
                 S.seed, S.W, S.p.pixelSizeNm, S.expSec * 1000, S.N);
   info.description = desc;
   NoiseSetup noise(S.seed, S.W, S.H, S.p);
   const std::vector<float> photons(static_cast<size_t>(S.W) * S.H, 0.0f);
   std::vector<uint16_t> adu;
   for (long f = 0; f < S.N; f++)
   {
      ApplyNoiseChain(photons, adu, S.W, S.H, S.p.Camera(), noise.offsetMap, noise.gainMap, noise.rnMap,
                      noise.noiseSeed, static_cast<uint32_t>(f));
      if (!onFrame(f, adu))
         break;
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   return true;
}

// Both lights (JS renderCombinedMovie): the fluorescence movie's photons (its
// noise chain runs at QE 1) plus the lamp's photons x the camera's QE at the
// lamp wavelength, then that one noise chain. The fluorescence movie holds
// the shared cache (world, BrightField scene) for this thread while it lives.
static bool RenderCombinedMovie(const ScopeSpec& spec,
                                const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                                ScopeMovieInfo& info, std::string& err, const ScopeProgress* progress)
{
   FluorescenceMovie fm;
   ScopeSetup S;
   BrightfieldSpec bs;
   if (!fm.Begin(spec, false, err) || !MakeScopeSetup(spec, S, err) || !ScopeBrightfieldSpec(spec, bs, err))
      return false;
   MovieCache& cache = SharedMovieCache();
   if (!cache.brightfield.Update(cache.source, bs, cache.version, err))
      return false;
   BrightfieldPhotons lamp;
   if (!lamp.Begin(spec, S, cache.brightfield, err))
      return false;
   const double qeLamp = SampleAt(S.lp.qe, ScopeSpecGet(spec, "bf-wavelength-nm"));
   NoiseSetup noise(S.seed, S.W, S.H, S.p);
   const CameraNoiseParams cam = S.p.Camera();
   std::vector<float> sum;
   std::vector<uint16_t> adu;
   FluorescenceFrameOptions opt;
   opt.onPhotons = [&](long f, const std::vector<float>& fl) {
      const std::vector<float>& bf = lamp.At(cache.brightfield, S, f);
      sum.resize(fl.size());
      for (size_t i = 0; i < fl.size(); ++i)
         sum[i] = fl[i] + static_cast<float>(bf[i] * qeLamp);
      ApplyNoiseChain(sum, adu, S.W, S.H, cam, noise.offsetMap, noise.gainMap, noise.rnMap, noise.noiseSeed,
                      static_cast<uint32_t>(f));
      return onFrame(f, adu);
   };
   if (!fm.Render(onFrame, info, err, progress, &opt))
      return false;
   char b[96];
   std::snprintf(b, sizeof b, " light=epi+trans bf_photons_per_px=%.4g bf_qe=%.4g", lamp.flux, qeLamp);
   info.description += b;
   return true;
}

bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err, const ScopeProgress* progress)
{
   if (ScopeSpecGet(spec, "prepare") >= 1)
      return PrepareScope(spec, info, err);
   bool epi = false, trans = false;
   ScopeLights(spec, epi, trans);
   if (!epi && !trans)
      return RenderDarkMovie(spec, onFrame, info, err);
   if (epi && trans)
      return RenderCombinedMovie(spec, onFrame, info, err, progress);
   if (trans)
      return RenderBrightfieldMovie(spec, onFrame, info, err);
   FluorescenceMovie fm;
   return fm.Begin(spec, false, err) && fm.Render(onFrame, info, err, progress);
}

// ---- read-only views for diagnostics (ScopeResolved.h) ----

bool ScopeResolve(const ScopeSpec& spec, ScopeResolved& out, std::string& err)
{
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   out = ScopeResolved();
   out.W = S.W;
   out.H = S.H;
   out.N = S.N;
   out.seed = S.seed;
   out.expSec = S.expSec;
   out.t0Sec = S.t0Sec;
   const ScopeCamera& c = S.camera;
   ScopeResolvedCamera& o = out.camera;
   o.preset = c.preset;
   o.qeCurve = c.qeCurve;
   o.qeFlat = c.qeFlat;
   o.emccd = c.emccd;
   o.darkPerSec = c.darkPerSec;
   o.gainElectronsPerAdu = c.gainPhotonsPerAdu;
   o.offsetAdu = c.offsetAdu;
   o.offsetStdAdu = c.offsetStdAdu;
   o.readNoiseElectrons = c.readNoiseElectrons;
   o.gainStdFraction = c.gainStdFraction;
   o.readNoiseStdFraction = c.readNoiseStdFraction;
   o.emGain = c.emGain;
   o.cicElectrons = c.cicElectrons;
   o.bitDepth = c.bitDepth;
   out.p = S.p;
   out.lp = S.lp;
   out.labels = S.labels;
   out.cf = S.cf;
   out.q = S.q;
   out.drift = S.drift;
   return true;
}

bool RenderScopePhotons(const ScopeSpec& spec, const ScopePhotonSink& onPhotons, ScopeMovieInfo& info,
                        std::string& err)
{
   const auto noFrames = [](long, const std::vector<uint16_t>&) { return true; };
   bool epi = false, trans = false;
   ScopeLights(spec, epi, trans);
   if (!epi && !trans)
   {
      // RenderDarkMovie without its noise chain: zero photons.
      const auto t0 = std::chrono::steady_clock::now();
      ScopeSetup S;
      if (!MakeScopeSetup(spec, S, err))
         return false;
      info = ScopeMovieInfo();
      info.width = S.W;
      info.height = S.H;
      info.frames = S.N;
      const std::vector<float> zero(static_cast<size_t>(S.W) * S.H, 0.0f);
      for (long f = 0; f < S.N; f++)
         if (!onPhotons(f, zero))
            break;
      info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      return true;
   }
   if (epi && trans)
   {
      // RenderCombinedMovie up to its noise chain.
      FluorescenceMovie fm;
      ScopeSetup S;
      BrightfieldSpec bs;
      if (!fm.Begin(spec, false, err) || !MakeScopeSetup(spec, S, err) || !ScopeBrightfieldSpec(spec, bs, err))
         return false;
      MovieCache& cache = SharedMovieCache();
      if (!cache.brightfield.Update(cache.source, bs, cache.version, err))
         return false;
      BrightfieldPhotons lamp;
      if (!lamp.Begin(spec, S, cache.brightfield, err))
         return false;
      const double qeLamp = SampleAt(S.lp.qe, ScopeSpecGet(spec, "bf-wavelength-nm"));
      std::vector<float> sum;
      FluorescenceFrameOptions opt;
      opt.onPhotons = [&](long f, const std::vector<float>& fl) {
         const std::vector<float>& bf = lamp.At(cache.brightfield, S, f);
         sum.resize(fl.size());
         for (size_t i = 0; i < fl.size(); ++i)
            sum[i] = fl[i] + static_cast<float>(bf[i] * qeLamp);
         return onPhotons(f, sum);
      };
      return fm.Render(noFrames, info, err, nullptr, &opt);
   }
   if (trans)
   {
      // RenderBrightfieldMovie / BrightfieldFrames up to the noise chain: the lamp's photons per frame.
      const auto t0 = std::chrono::steady_clock::now();
      ScopeSetup S;
      if (!MakeScopeSetup(spec, S, err))
         return false;
      MovieCache& cache = SharedMovieCache();
      std::lock_guard<std::mutex> lock(cache.mutex);
      if (!ConfigureShared(cache, S.cf, err))
         return false;
      BrightfieldSpec bs;
      if (!ScopeBrightfieldSpec(spec, bs, err) || !cache.brightfield.Update(cache.source, bs, cache.version, err))
         return false;
      BrightfieldPhotons lamp;
      if (!lamp.Begin(spec, S, cache.brightfield, err))
         return false;
      info = ScopeMovieInfo();
      info.width = S.W;
      info.height = S.H;
      info.frames = S.N;
      info.querySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      DriftInfo(S, info);
      for (long f = 0; f < S.N; f++)
         if (!onPhotons(f, lamp.At(cache.brightfield, S, f)))
            break;
      info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      return true;
   }
   FluorescenceMovie fm;
   FluorescenceFrameOptions opt;
   opt.onPhotons = onPhotons;
   return fm.Begin(spec, false, err) && fm.Render(noFrames, info, err, nullptr, &opt);
}

} // namespace sim
