///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See ScopeMovie.h (issue 16).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "ScopeMovie.h"
#include "Fiducials.h"
#include "ScopeResolved.h"

#include "CacheDir.h"

#include "BinnedBlinks.h"
#include "BlinkExpectation.h"
#include "BrightfieldRender.h"
#include "DyeLibrary.h"
#include "Excitation.h"
#include "Illumination.h"
#include "LightHistory.h"
#include "LightPath.h"
#include "Parallel.h"
#include "Spectra.h"
#include "Timing.h"

#include "SampleSource.h"
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
#include <deque>
#include <cstring>
#include <limits>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace sim {

namespace {

// The default 640 nm intensity, the DNA-PAINT presets' 1 kW/cm^2 (estimate; was
// 0.1607 until 2026-10-05, what gave ATTO 655 the single-dye default's 6375
// photoelectrons/s while ON; scope_movie.js DEFAULT_LASER_640_KW, issue 16).
constexpr double kDefaultLaser640KW = 1.0;

// The structures' option prefixes: the targets of data/specimens.json (JS world.js STRUCTURES), index = ISC_STRUCT_*.
const char* StructurePrefix(int s)
{
   return TargetAt(s).prefix;
}

// The texts of a target's label options (JS scope_movie.js TARGET_TEXT; a new target adds its entry).
struct TargetText
{
   const char* id;
   const char* dev;
   const char* row;
   const char* what;
   const char* sites;
   const char* axis;
};
const TargetText kTargetText[] = {
   { "microtubules", "CellField", "Microtubules", "the microtubules'",
     "% of the binding sites (13 x 8 nm lattice, 1625 /um) that carry a label", "the microtubule axis" },
   { "npcs", "CellField", "NuclearPores", "the nuclear pores'", "% of the 64 Nup96 sites per pore that carry a label",
     "the pore axis" },
   { "lamin", "CellField", "NuclearLamina", "the nuclear lamina's", "% of the lamina points that carry a label",
     "the envelope normal" },
   { "docking", "Nanorulers", "Docking", "the docking strands'",
     "% of the designed docking strands incorporated (each strand on its own: the incorporation efficiency)",
     "the rod axis" },
};
const TargetText& TextOf(const TargetData& t)
{
   for (const TargetText& x : kTargetText)
      if (!std::strcmp(x.id, t.id))
         return x;
   throw std::runtime_error(std::string("ScopeMovie.cpp: no option texts for target '") + t.id + "' (kTargetText)");
}

// The DNA-origami defaults (core params origami*; JS origami.js ORIGAMI_DEFAULTS).
constexpr double kOrigamiDensity = 1.2, kOrigamiMinSpacingNm = 0, kOrigamiLpNm = 1500, kOrigamiStrandsPerSpot = 5,
                 kOrigamiAttachNm = 7, kOrigamiLinkerNm = 3;

// The design ids (data/origami/designs.json), from the core.
std::vector<std::string> OrigamiDesignIds()
{
   std::vector<std::string> ids;
   for (int32_t i = 0; i < isc_origami_design_count(); ++i)
      ids.push_back(isc_origami_design_id(i));
   return ids;
}

// The part of a settings file a named option belongs to (ScopeOptionPart; docs/dev/SAVE_SETTINGS.md has the table).
// By name, so a new option without a rule here stops Options() at its first use.
const char* PartOf(const std::string& n)
{
   auto starts = [&](const char* p) { return n.compare(0, std::strlen(p), p) == 0; };
   auto in = [&](std::initializer_list<const char*> list) {
      for (const char* k : list)
         if (n == k)
            return true;
      return false;
   };
   if (in({ "disk-cache", "prepare", "illum-geometry" }))
      return "host";
   if (in({ "x", "y", "z", "focus-um", "frames", "exposure-ms", "start-sec" }) || starts("drift-"))
      return "measurement";
   for (int s = 0; s < TargetCount(); ++s)
      if (starts((std::string(TargetAt(s).prefix) + "-").c_str()))
         return "sample";
   // the hardware: objective and its PSF, camera, beam shape, the parts given by their numbers (a custom line or filter)
   if (in({ "na", "immersion-index", "pixel-nm", "psf-model", "psf-zernike-preset", "psf-mask", "psf-mask-modes",
            "psf-mask-waist", "psf-working-distance-um", "camera-preset", "qe-curve", "qe", "camera-type", "dark-per-sec",
            "gain", "offset", "offset-std", "read-noise", "gain-std-pct", "read-noise-std-pct", "cic", "bit-depth",
            "illum-profile", "illum-fwhm", "laser-custom-nm", "ex-lo-nm", "ex-hi-nm", "dichroic-edge-nm", "em-lo-nm",
            "em-hi-nm", "bf-condenser-na", "bf-wavelength-nm", "bf-aberrations", "fid-blocking-od" }))
      return "instrument";
   // the sample: the specimen and its world, labels and dyes, beads, the medium and the chamber
   if (in({ "seed", "world-seed", "specimen", "mode", "chunk-um", "occupancy", "cell-diam-min-um", "cell-diam-max-um",
            "packing", "chamber-height-um", "background-per-sec", "bf-absorption-per-um", "psf-sample-index",
            "psf-sample-depth-nm", "dye1.source", "dye2.source", "dye3.source" }) ||
       starts("origami-") || starts("fid-") || starts("bf-n-") || (starts("dye") && n.find('.') != std::string::npos) ||
       (n.size() > 8 && n.compare(n.size() - 8, 8, "-density") == 0))
      return "sample";
   // how the instrument is driven: the light, the filters in place, the camera's settings, the render quality
   if (starts("laser-") || starts("wf-") || starts("mean-field-") || starts("blink-") || starts("bf-") ||
       (starts("psf-") && !starts("psf-sample-")) ||
       in({ "light-preset", "ex-filter", "dichroic", "em-filter", "illum-bfp", "modality", "light-epi", "light-trans",
            "size", "em-gain", "z-range-um" }))
      return "settings";
   return nullptr;
}

struct OptionTable
{
   std::vector<ScopeOption> opts;
   std::deque<std::string> strings;   // storage of the generated names and help texts (a deque: push_back moves no element, so the c_str() handed out stay valid)
};

const OptionTable& Options()
{
   static const OptionTable t = [] {
      OptionTable o;
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
         { "specimen", 0, "SampleHolder: the mounted specimen (0 = CellField, 1 = Nanorulers; data/specimens.json; names accepted)" },
         { "mode", -1, "fallback label mode for every target whose <prefix>-mode is -2 (Global): 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField; -1 None (each target's own; names accepted). Each target has its own mode (<prefix>-mode); Micro-Manager has no experiment-wide mode since issue 15" },
      };
      // Each target's label (data/specimens.json; Micro-Manager: the specimen device's <Row>_* rows, Fluorophores):
      // <prefix>-dye, -mode, -label-pct, -imager-nm, -orient*, -wobble-deg, -motion, and for a target with a linkage
      // model (library.json linkagePresets.defaults) -binder, -binder-nm, -binder-flex-nm, -linker-min-nm,
      // -linker-max-nm. The default dye: the target's typical dye in the library's default mode, or None for a target
      // with defaultNone (the lamina: off until labelled; JS targetLabelOptions).
      for (int s = 0; s < TargetCount(); ++s)
      {
         const TargetData& tg = TargetAt(s);
         const TargetText& T = TextOf(tg);
         const std::string P = tg.prefix, D = std::string(T.dev) + "." + T.row;
         std::string pcts;
         for (int m = 0; m < MODE_COUNT; ++m)
         {
            char b[64];
            std::snprintf(b, sizeof b, "%s%s %g", m ? ", " : "", DyeModeNames()[static_cast<size_t>(m)].c_str(), tg.typicalPct[m]);
            pcts += b;
         }
         v.push_back({ keep(P + "-dye"), static_cast<double>(tg.defaultNone ? DyeNoneChoice() : tg.typicalDye[DefaultDyeMode()]),
                       keep(D + "_Label: a library dye, Dye1..Dye3, or None (no label: the target is off); -1 Typical = " + T.what +
                            " typical dye in their mode (data/dyes/library.json typicalLabels; names accepted)") });
         v.push_back({ keep(P + "-mode"), -1,
                       keep(D + "_Mode: -1 DyeDefault = the dye's default (Typical: the library's default mode), -2 Global = the mode option, 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField (names accepted)") });
         v.push_back({ keep(P + "-label-pct"), -1,
                       keep(D + "_LabelingPct: " + T.sites + " (0: none, the target is off); -1 = the target's typical % in its mode (typicalLabels: " + pcts + ")") });
         // The target's default imager concentration and DNA-PAINT binding kinetics (data/specimens.json paint:
         // microtubules 1 nM, pores 2.5 nM, docking strands 20 nM; estimates).
         v.push_back({ keep(P + "-imager-nm"), tg.paintImagerNm,
                       keep(D + "_ImagerNanoMol: DNA-PAINT imager concentration, nanomolar (binding rate k_on x c; the free imager adds a uniform background -- taken as constant: no depletion by binding or bleaching, no exclusion from cells)") });
         v.push_back({ keep(P + "-paint-kon"), tg.paintKonPerMPerSec,
                       keep(D + "_KonPerMolarPerSec: DNA-PAINT imager association rate k_on, /M/s (the target's docking sequence; data/specimens.json paint)") });
         v.push_back({ keep(P + "-paint-on-sec"), tg.paintOnSec,
                       keep(D + "_BoundTimeSec: DNA-PAINT mean bound time of an imager, s (exponential; half-time = ln 2 x this; data/specimens.json paint)") });
         v.push_back({ keep(P + "-orient"), 0,
                       keep(D + "_Orientation: 0 Free (isotropic), 1 Fixed, 2 Random (no effect on the image yet)") });
         v.push_back({ keep(P + "-orient-polar-deg"), 90, keep(D + "_OrientPolarDeg: Fixed dipole angle from " + T.axis) });
         v.push_back({ keep(P + "-orient-azimuth-deg"), 0,
                       keep(D + "_OrientAzimuthDeg: Fixed dipole azimuth about the axis, from the radial direction") });
         v.push_back({ keep(P + "-wobble-deg"), 0, keep(D + "_WobbleConeDeg: fast wobble cone half-angle (Fixed, Random)") });
         v.push_back({ keep(P + "-motion"), 0, keep(D + "_Motion: 0 Static (single-particle tracking: future)") });
         if (tg.linkageDefault < 0)
            continue;
         std::string list;
         for (size_t i = 0; i < LinkagePresetIds().size(); ++i)
            list += (i ? ", " : "") + std::to_string(i) + " " + LinkagePresetIds()[i];
         const std::string def = LinkagePresetIds()[static_cast<size_t>(tg.linkageDefault)];
         v.push_back({ keep(P + "-binder"), static_cast<double>(tg.linkageDefault),
                       keep(D + "_Binder: how the dye is attached (data/dyes/library.json linkagePresets; default " + def + "): " + list +
                            " (0 Legacy = the structure's numbers before the linkage model; names accepted)") });
         v.push_back({ keep(P + "-binder-nm"), -1,
                       keep(D + "_BinderNm: the probe (binder) reaches this far from the binding site along the structure's axis, nm (-1 = the preset's)") });
         v.push_back({ keep(P + "-binder-flex-nm"), -1,
                       keep(D + "_BinderFlexNm: the probe's flexibility: its end lies uniformly in a ball of this radius about that point, nm (-1 = the preset's)") });
         v.push_back({ keep(P + "-linker-min-nm"), -1,
                       keep(D + "_LinkerMinNm: shortest chemical linker from the probe to the dye, nm (-1 = the preset's)") });
         v.push_back({ keep(P + "-linker-max-nm"), -1,
                       keep(D + "_LinkerMaxNm: longest linker, nm (the length is uniform in volume between the two, the direction uniform; -1 = the preset's)") });
      }
      const std::vector<ScopeOption> slots = {
         { "dye1.source", idx(DyeIds(), "AF647"), "Fluorophores.Dye1_Source: library dye of slot 1 (dye1.<field> overrides it)" },
         { "dye2.source", idx(DyeIds(), "mEos3.2"), "Fluorophores.Dye2_Source" },
         { "dye3.source", idx(DyeIds(), "mEGFP"), "Fluorophores.Dye3_Source" },
      };
      v.insert(v.end(), slots.begin(), slots.end());
      for (int nm : LaserLines())
         v.push_back({ keep("laser-" + std::to_string(nm)), nm == 640 ? kDefaultLaser640KW : 0.0,
                       keep("Lasers.Laser" + std::to_string(nm) + "KWcm2: " + std::to_string(nm) +
                            " nm laser intensity at the sample, kW/cm^2 (0 = off)") });
      const std::vector<ScopeOption> rest = {
         { "laser-custom-nm", 0, "wavelength of an extra laser line, nm (0 = none; not in Micro-Manager)" },
         { "laser-custom", 0, "its intensity, kW/cm^2" },
         { "light-preset", 0, "Lasers.Preset: auto (0, the default) = the light presets of the labelled targets' dyes in their modes combined: each laser line at its highest power over them, the filters of the one preset, or the multi-band set (light_path.json multiTarget) when the presets' filters differ; -1 = none (the laser/dichroic/filter options as given); or a preset name/index (data/dyes/light_path.json presets). A preset sets every laser-*, ex-filter, dichroic and em-filter the spec does not give." },
         { "illum-profile", 1, "Lasers.IlluminationProfile: the cross-section of the beam, 0 Flat, 1 Gaussian (the default), 2 FlatTop (a soft-edged disc; names accepted)" },
         { "illum-fwhm", 100, "Lasers.IlluminationFwhmPct: the FWHM of the beam (Gaussian, FlatTop), % of the width of the field of view" },
         { "illum-bfp", 0, "Lasers.BfpPosition: the laser spot in the back focal plane, from its centre (0, epi) to the critical angle (1); between: HILO, above: TIRF up to the pupil edge NA / n_medium (beyond it no light reaches the sample). Unset: that of the light preset (DNA-PAINT: HILO; the nanorulers: TIRF)" },
         { "illum-geometry", 0, "deprecated, ignored (was Lasers.IlluminationGeometry: Epi; illum-bfp replaces it)" },
         { "chamber-height-um", 100, "Lasers.ChamberHeightUm: depth of the imager solution over the coverslip (estimate: a tape/flow chamber), um; the DNA-PAINT free-imager background is its column integral of the excitation (epi: all of it; HILO: the sheet; TIRF: ~ the penetration depth)" },
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
         { "mt-density", 0.9, "CellField.MicrotubuleDensityPerUm2 (0: no microtubules, the target is off)" },
         { "npc-density", 8, "CellField.NuclearPoreDensityPerUm2: nuclear pores per um^2 of nuclear envelope (8: chosen by the user 2026-10-13, was 12; measured 5-8.6 of maeshima2006 and dultz2010; 0: no pores, the target is off; the pore geometry: p.npcMinDistNm, p.npcRadiusNm, p.npcRingSepNm, p.npcCornerSpreadNm)" },
         { "lamin-density", 2000, "CellField.NuclearLaminaDensityPerUm2: nuclear lamina points per um^2 of nuclear envelope (2000: estimate, 2026-10-27, was 50; 100 .. 10000 in the viewer and MM; 0: no lamina; it is unlabelled by default: lamin-dye None; p.laminNpcExclNm: no point within half of it of a pore centre, default 40 nm)" },
         { "packing", 1, "CellField.Packing (1 on, 0 off)" },
         { "origami-design", 0, "Nanorulers.Design: the DNA-origami design (data/origami/designs.json; names accepted)" },
         { "origami-spacing-nm", 80, "Nanorulers.SpacingNm: lattice step between neighbouring spots, nm; the body grows with it (a rod keeps 20 nm ends; the UIs offer 640-5)" },
         { "origami-density", kOrigamiDensity, "Nanorulers.DensityPerUm2: molecules per um^2 before the minimum spacing thins them" },
         { "origami-min-spacing-nm", kOrigamiMinSpacingNm, "Nanorulers.MinSpacingNm: no two molecule centres closer than this, nm (0 = no thinning)" },
         { "origami-bend", 1, "Nanorulers.Bending: 1 = the design's in-plane worm-like-chain bending, 0 = rigid" },
         { "origami-lp-nm", kOrigamiLpNm, "Nanorulers.PersistenceLengthNm: bending persistence length, nm (estimate; measured helix bundles 0.74-8.2 um)" },
         { "origami-strands-per-spot", kOrigamiStrandsPerSpot, "Nanorulers.StrandsPerSpot: ring positions 0..N-1 of every spot carry a docking strand (the nanoruler has 5, a 2D origami 1)" },
         { "origami-attach-nm", kOrigamiAttachNm, "Nanorulers.AttachHeightNm: the rod's bottom above the glass, nm" },
         { "origami-linker-nm", kOrigamiLinkerNm, "Nanorulers.LinkerNm: the dye lies within this distance of its strand's anchor, outward, nm" },
         { "origami-roll", 0, "Nanorulers.Roll: 0 Uniform (each rod landed at a random roll about its axis), 1 Fixed (origami-roll-deg; names accepted)" },
         { "origami-roll-deg", 0, "Nanorulers.RollDeg: the fixed roll, deg" },
         { "origami-heights", 0, "Nanorulers.KeepHeights: 0 = 2D (every strand at the rod axis height), 1 = the ring's heights (3D)" },
         // ---- fiducial beads on any sample (Micro-Manager: Fiducials; core params fid*, Fiducials.h; data/fiducials) ----
         { "fid-type", idx(FiducialTypeIds(), FiducialDefaults().type), keep([] {
              std::string h = "Fiducials.Type: the bead (data/fiducials/types.json; names accepted): ";
              for (size_t i = 0; i < FiducialTypeIds().size(); ++i)
                 h += (i ? ", " : "") + std::to_string(i) + " " + FiducialTypeIds()[i];
              return h;
           }()) },
         { "fid-density", FiducialDefaults().densityPerUm2, "Fiducials.DensityPerUm2: beads per um^2 of coverslip, placed at random (Poisson; 0 = no beads)" },
         { "fid-diameter-nm", -1, "Fiducials.DiameterNm: mean bead diameter, nm (brightness ~ d^3; the image is the PSF of the solid sphere); -1 = the type's product size (FluoSpheres orange / red / dark red 200 nm, the others 100 nm)" },
         { "fid-diameter-cv-pct", FiducialDefaults().diameterCvPct, "Fiducials.DiameterCvPct: bead-to-bead diameter spread, % (normal; estimate)" },
         { "fid-height", static_cast<double>(IndexOf(ScopeFiducialHeightNames(), FiducialDefaults().height)), "Fiducials.Height: 0 Coverslip (on the glass), 1 OnTop (on the sample's surface), 2 Layer (anywhere in a gel layer of fid-layer-um, outside the cells; names accepted)" },
         { "fid-layer-um", FiducialDefaults().layerUm, "Fiducials.LayerUm: the gel layer's thickness, um (Layer)" },
         { "fid-bleach-half-sec", FiducialDefaults().bleachHalfSec, "Fiducials.BleachHalfTimeSec: a bead's fluorescence half-time at fid-bleach-ref-kwcm2 at its dye's excitation peak, s (estimate: 288 s; ~ 1 / the excitation; gold and nanodiamonds do not bleach; 0 = no bleaching)" },
         { "fid-bleach-ref-kwcm2", FiducialDefaults().bleachRefKWcm2, "Fiducials.BleachRefKWcm2: the intensity the half-time is given at, kW/cm^2" },
         { "fid-dyes", -1, "Fiducials.DyeCount: equivalent dyes (eps 1e5, QY 0.5) per bead of the type's reference diameter, every component (-1 = the type's, fitted to balinovic2019)" },
         { "fid-blocking-od", 8, "Fiducials.BlockingOD: the emission path (dichroic ~OD 2 x emission filter ~OD 6 at the laser line; estimate) blocks a laser line by at least this optical density: gold's scattered laser light (ideal filters would block it fully)" },
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
         { "blink-binned-density-per-um2", 12.5, "Renderer.BlinkBinnedDensityPerUm2: a frame's blinks render binned (approximate SMLM: the same events on a grid of blink-binned-upscale cells per pixel, FFT-convolved per PSF plane; the cost is per FOV area instead of per blink) above this many ON emitters per um^2 of the FOV, splatted one by one below" },
         { "blink-binned-max-emitters", 1e9, "Renderer.BlinkBinnedMaxEmitters: and binned above this many ON emitters in the frame" },
         { "blink-mean-field-density-per-um2", 1e9, "Renderer.BlinkMeanFieldDensityPerUm2: a frame's blinks render mean-field (no events drawn: every dye's expected ON time in the frame from its kinetics x the dye density, convolved once; the blinking itself is lost, only shot noise stays) above this many expected ON emitters per um^2 of the focal slab (mean-field-slab-nm); 1e9 = never" },
         { "blink-mean-field-max-emitters", 1e9, "Renderer.BlinkMeanFieldMaxEmitters: and mean-field above this many expected ON emitters in the z range; 1e9 = never" },
         { "blink-binned-upscale", 2, "Renderer.BlinkBinnedUpscale: the binned grid's cells per pixel, per axis (a divisor of the PSF oversampling; the position is snapped to +-half a cell)" },
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
         { "bf-n-medium", 1.337, "CellField.IndexMedium: refractive index of the medium (BrightField; the critical angle of illum-bfp)" },
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
         { "blink-min-photons", 50, "Renderer.BlinkMinPhotons: a blink with fewer detected photons than this in a frame is not splatted in that frame (Quality Fast 100, Realistic 50, Exhaustive 0); binned and mean-field frames and continuous dyes keep every one" },
      };
      v.insert(v.end(), rest.begin(), rest.end());
      for (const ScopeOption& x : v)
         if (!PartOf(x.name))
            throw std::runtime_error(std::string("ScopeMovie.cpp: option '") + x.name + "' has no part (PartOf)");
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
      for (int s = 0; s < TargetCount(); ++s)
      {
         const std::string P = TargetAt(s).prefix;
         m[P + "-dye"] = DyeChoices();
         m[P + "-mode"] = DyeModeNames();
         m[P + "-orient"] = { "Free", "Fixed", "Random" };
         m[P + "-motion"] = { "Static" };
         if (TargetAt(s).linkageDefault >= 0)
            m[P + "-binder"] = LinkagePresetIds();
      }
      m["mode"] = DyeModeNames();
      m["specimen"] = SpecimenIds();
      m["origami-design"] = OrigamiDesignIds();
      m["origami-roll"] = { "Uniform", "Fixed" };
      m["fid-type"] = FiducialTypeIds();
      m["fid-height"] = ScopeFiducialHeightNames();
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
      m["illum-profile"] = IlluminationProfileNames();
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

const std::vector<std::string>* ScopeOptionNames(const std::string& name)
{
   return OptionNames(name);
}

const char* ScopeOptionPart(const std::string& name)
{
   if (name.compare(0, 2, "p.") == 0 && name.size() > 2)
      return "sample";
   if (ZernikeKeyIndex(name) >= 0)
      return "instrument";
   OverrideKey ok;
   if (!IsNamedOption(name) && ParseOverrideKey(name, ok) && (ok.slot >= 0 || ok.structure >= 0) &&
       IndexOf(DyeFieldNames(), ok.field) >= 0)
      return "sample";
   return IsNamedOption(name) ? PartOf(name) : nullptr;
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
   if (!std::strcmp(name, "z"))
      return ScopeDefaultZ(spec);
   for (const ScopeOption& o : Options().opts)
      if (!std::strcmp(o.name, name))
         return o.value;
   return 0.0;
}

int ScopeSpecimen(const ScopeSpec& spec)
{
   return static_cast<int>(ScopeSpecGet(spec, "specimen"));
}

std::vector<int> ScopeMountedStructures(const ScopeSpec& spec)
{
   std::vector<int> out;
   const int sp = ScopeSpecimen(spec);
   if (sp < 0 || sp >= static_cast<int>(SpecimenIds().size()))
      return out;
   const SpecimenData& d = SpecimenAt(sp);
   for (int t = d.firstTarget; t < d.firstTarget + d.targetCount; ++t)
      out.push_back(TargetAt(t).structure);
   return out;
}

std::vector<std::string> ScopeOrigamiDesignIds()
{
   return OrigamiDesignIds();
}

std::vector<std::pair<std::string, double>> ScopeSpecimenParams(const ScopeSpec& spec)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   std::vector<std::pair<std::string, double>> v = {
      { "specimen", O("specimen") }, { "origamiDesign", O("origami-design") }, { "origamiSpacingNm", O("origami-spacing-nm") },
      { "origamiDensity", O("origami-density") },
      { "origamiMinSpacingNm", O("origami-min-spacing-nm") }, { "origamiBend", O("origami-bend") },
      { "origamiLpNm", O("origami-lp-nm") }, { "origamiStrandsPerSpot", O("origami-strands-per-spot") },
      { "origamiAttachNm", O("origami-attach-nm") }, { "origamiLinkerNm", O("origami-linker-nm") },
      { "origamiRoll", O("origami-roll") }, { "origamiRollDeg", O("origami-roll-deg") },
      { "origamiHeights", O("origami-heights") },
   };
   for (auto& kv : v)
   {
      auto it = spec.find("p." + kv.first);
      if (it != spec.end())
         kv.second = it->second;
   }
   return v;
}

const std::vector<std::string>& ScopeFiducialHeightNames()
{
   static const std::vector<std::string> v = { "Coverslip", "OnTop", "Layer" };   // = core FiducialHeight
   return v;
}

std::vector<std::pair<std::string, double>> ScopeFiducialParams(const ScopeSpec& spec)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const int t = static_cast<int>(O("fid-type"));
   const bool known = t >= 0 && t < static_cast<int>(FiducialTypeIds().size());
   std::vector<std::pair<std::string, double>> v = {
      { "fidDensity", O("fid-density") },
      { "fidDiameterNm", O("fid-diameter-nm") >= 0 || !known ? O("fid-diameter-nm") : FiducialTypeAt(t).refDiameterNm },
      { "fidDiameterCvPct", O("fid-diameter-cv-pct") }, { "fidHeight", O("fid-height") }, { "fidLayerUm", O("fid-layer-um") },
   };
   for (auto& kv : v)
   {
      auto it = spec.find("p." + kv.first);
      if (it != spec.end())
         kv.second = it->second;
   }
   return v;
}

double ScopeDefaultZ(const ScopeSpec& spec)
{
   double z = 0.5;   // the ZStage's start (the options' default)
   for (const ScopeOption& o : Options().opts)
      if (!std::strcmp(o.name, "z"))
         z = o.value;
   if (ScopeSpecimen(spec) != 1)
      return z;
   IscParams* p = isc_params_new();
   for (const auto& kv : ScopeSpecimenParams(spec))
      isc_params_set(p, kv.first.c_str(), kv.second);
   double h[2] = { z, z };
   isc_origami_heights(p, h);
   isc_params_free(p);
   return h[0];
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
   if (choice == DyeNoneChoice())
      choice = -1;   // no label: resolved as Typical for the callers that ask; ScopeTargetOn is false
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

bool ScopeTargetOn(const ScopeSpec& spec, int s)
{
   const std::vector<int> mounted = ScopeMountedStructures(spec);
   if (std::find(mounted.begin(), mounted.end(), s) == mounted.end())
      return false;
   const char* P = StructurePrefix(s);
   if (static_cast<int>(ScopeSpecGet(spec, Opt(P, "dye").c_str())) == DyeNoneChoice() ||
       ScopeSpecGet(spec, Opt(P, "label-pct").c_str()) == 0)
      return false;
   const std::string density = Opt(P, "density");
   return !IsNamedOption(density) || ScopeSpecGet(spec, density.c_str()) > 0;
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
   int on = 0;
   for (int s : ScopeMountedStructures(spec))
   {
      if (!ScopeTargetOn(spec, s))
         continue;
      EffectiveDye eff;
      int choice = 0;
      if (!ScopeStructureDye(spec, s, eff, choice, err))
         return false;
      out = out && DyeModeNames()[static_cast<size_t>(eff.mode)] == "WideField";
      ++on;
   }
   out = out && on > 0;
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

// A resolved light preset: a library preset, or the auto preset's combination of several.
struct ResolvedLightPreset
{
   std::string id, excitationFilter, dichroic, emissionFilter;
   std::vector<double> lasers;   // per LaserLines(), kW/cm^2
   double bfpPosition;           // the BFP position it sets (NaN: none)
   explicit ResolvedLightPreset(const LightPresetData& q)
      : id(q.id), excitationFilter(q.excitationFilter ? q.excitationFilter : "None"), dichroic(q.dichroic),
        emissionFilter(q.emissionFilter), lasers(q.lasers, q.lasers + LaserLines().size()), bfpPosition(q.bfpPosition)
   {
   }
};

// The light preset of target s's dye in its mode.
static const LightPresetData* TargetLightPreset(const ScopeSpec& spec, int s, std::string& err)
{
   EffectiveDye eff;
   int choice = 0;
   if (!ScopeStructureDye(spec, s, eff, choice, err))
      return nullptr;
   const char* id = eff.dye.modes[eff.mode].lightPreset;
   const LightPresetData* q = FindLightPreset(id ? id : "");
   if (!q)
      err = std::string("no light preset '") + (id ? id : "") + "' for " + eff.dye.id;
   return q;
}

// The auto light preset (JS autoLightPreset): the presets of the labelled targets' dyes in their modes, in target
// order. One distinct preset: it. Several: each laser line at its highest power over them; their excitation filter,
// dichroic and emission filter where all agree, else no clean-up filter and the multi-band set (light_path.json
// multiTarget). No labelled target: the first mounted target's preset (a dark sample, the light as if it were labelled).
static bool AutoLightPreset(const ScopeSpec& spec, std::vector<ResolvedLightPreset>& out, std::string& err)
{
   std::vector<const LightPresetData*> qs;
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
   {
      if (!ScopeTargetOn(spec, s))
         continue;
      const LightPresetData* q = TargetLightPreset(spec, s, err);
      if (!q)
         return false;
      if (std::find(qs.begin(), qs.end(), q) == qs.end())
         qs.push_back(q);
   }
   if (qs.empty())
   {
      const std::vector<int> mounted = ScopeMountedStructures(spec);
      const LightPresetData* q = mounted.empty() ? nullptr : TargetLightPreset(spec, mounted[0], err);
      if (!q)
         return false;
      qs.push_back(q);
   }
   ResolvedLightPreset r(*qs[0]);
   if (qs.size() > 1)
   {
      auto same = [&](const char* LightPresetData::*f) {
         for (const LightPresetData* q : qs)
            if (std::strcmp(q->*f ? q->*f : "None", qs[0]->*f ? qs[0]->*f : "None"))
               return false;
         return true;
      };
      const bool filtersAgree = same(&LightPresetData::dichroic) && same(&LightPresetData::emissionFilter);
      for (size_t k = 1; k < qs.size(); ++k)
      {
         r.id += "+" + std::string(qs[k]->id);
         // The BFP position where all agree (NaN = none; JS autoLightPreset), else none (epi).
         const double b = qs[k]->bfpPosition;
         if (!(b == r.bfpPosition || (std::isnan(b) && std::isnan(r.bfpPosition))))
            r.bfpPosition = std::numeric_limits<double>::quiet_NaN();
         for (size_t l = 0; l < r.lasers.size(); ++l)
            r.lasers[l] = std::max(r.lasers[l], qs[k]->lasers[l]);
      }
      if (!(filtersAgree && same(&LightPresetData::excitationFilter)))
         r.excitationFilter = "None";
      if (!filtersAgree)
      {
         r.dichroic = MultiTargetDichroic();
         r.emissionFilter = MultiTargetEmissionFilter();
      }
   }
   out.assign(1, r);
   return true;
}

bool ScopeAutoLightPreset(const ScopeSpec& spec, ScopeLightPresetValues& out, std::string& err)
{
   std::vector<ResolvedLightPreset> r;
   if (!AutoLightPreset(spec, r, err))
      return false;
   out.id = r[0].id;
   out.excitationFilter = r[0].excitationFilter;
   out.dichroic = r[0].dichroic;
   out.emissionFilter = r[0].emissionFilter;
   out.lasers = r[0].lasers;
   out.bfpPosition = ScopePresetBfpPosition(spec, r[0].bfpPosition);
   return true;
}

const char* ScopeTargetRow(int s) { return TextOf(TargetAt(s)).row; }

// The light preset a spec asks for (JS scopeLightPreset): none (out empty), a library preset, or the auto one.
static bool ScopeLightPreset(const ScopeSpec& spec, std::vector<ResolvedLightPreset>& out, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   out.clear();
   const int i = static_cast<int>(O("light-preset"));
   if (i < 0)
      return true;
   if (i == 0)
      return AutoLightPreset(spec, out, err);
   const LightPresetData* q =
      i - 1 < static_cast<int>(LightPresetIds().size()) ? FindLightPreset(LightPresetIds()[static_cast<size_t>(i - 1)]) : nullptr;
   if (!q)
   {
      err = "light preset " + std::to_string(i) + " out of range";
      return false;
   }
   out.assign(1, ResolvedLightPreset(*q));
   return true;
}

// The light path of a spec (JS scopeLightPath): the light preset's values for
// the options the spec does not give.
static bool MakeScopeLightPath(const ScopeSpec& specIn, const ScopeCamera& camera, LightPath& lp, std::string& err)
{
   std::vector<ResolvedLightPreset> preset;
   if (!ScopeLightPreset(specIn, preset, err))
      return false;
   ScopeSpec spec = specIn;
   const std::vector<int>& lines = LaserLines();
   if (!preset.empty())
   {
      const ResolvedLightPreset& q = preset[0];
      for (size_t l = 0; l < lines.size(); ++l)
      {
         const std::string k = "laser-" + std::to_string(lines[l]);
         if (!spec.count(k))
            spec[k] = q.lasers[l];
      }
      if (!spec.count("ex-filter"))
         spec["ex-filter"] = IndexOf(ExcitationFilterIds(), q.excitationFilter);
      if (!spec.count("dichroic"))
         spec["dichroic"] = IndexOf(DichroicIds(), q.dichroic);
      if (!spec.count("em-filter"))
         spec["em-filter"] = IndexOf(EmissionFilterIds(), q.emissionFilter);
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

double ScopeBfpPosition(const ScopeSpec& spec)
{
   auto it = spec.find("illum-bfp");
   if (it != spec.end())
      return std::max(0.0, it->second);
   std::vector<ResolvedLightPreset> q;
   std::string err;
   if (!ScopeLightPreset(spec, q, err) || q.empty())
      return 0.0;
   return ScopePresetBfpPosition(spec, q[0].bfpPosition);
}

double ScopePresetBfpPosition(const ScopeSpec& spec, double presetBfp)
{
   if (std::isnan(presetBfp))
      return 0.0;
   const int sp = ScopeSpecimen(spec);
   const double own = sp >= 0 && sp < static_cast<int>(SpecimenIds().size()) ? SpecimenAt(sp).bfpPosition
                                                                             : std::numeric_limits<double>::quiet_NaN();
   return std::max(0.0, !std::isnan(own) ? own : presetBfp);
}

ExcitationSpec ScopeExcitationSpec(const ScopeSpec& spec)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   unsigned W = 0, H = 0;
   long N = 0;
   ScopeMovieDims(spec, W, H, N);
   ExcitationSpec e;
   e.profile = static_cast<int>(O("illum-profile"));
   e.fwhmPct = std::min(300.0, std::max(10.0, O("illum-fwhm")));
   e.fovWidthUm = W * O("pixel-nm") / 1000.0;
   e.bfp = ScopeBfpPosition(spec);
   e.na = O("na");
   e.nImm = O("immersion-index");
   e.nMed = O("bf-n-medium");
   e.pivotUm = O("focus-um") + O("z");
   return e;
}

// An off target (ScopeTargetOn false; JS offLabel): no dyes (density 0; the label is dyes.js makeLabel({ density: 0,
// mode })), no emitting state (no PSF group, no population), no imager background.
static LabelPhysics OffLabel(const EffectiveDye& eff)
{
   LabelPhysics L;
   L.eff = eff;
   L.mode = eff.mode;
   L.off = true;
   L.label.assign(ISC_LABEL_COUNT, 0.0);
   L.label[ISC_LABEL_FLUORESCENT_FRACTION] = 1.0;
   L.label[ISC_LABEL_MODE] = eff.mode;
   L.label[ISC_LABEL_ACTIVATION_RATE] = 0.01;   // dyes.js DEFAULT_KINETICS
   L.label[ISC_LABEL_ON_SEC] = 0.05;
   L.label[ISC_LABEL_OFF_SEC] = 1.0;
   L.label[ISC_LABEL_BLEACH_PROB] = 1.0;
   L.label[ISC_LABEL_PHOTON_CV] = 0.5;
   L.label[ISC_LABEL_ORIENT_POLAR_DEG] = 90;
   for (int i = ISC_LABEL_BINDER_NM; i <= ISC_LABEL_LINKER_MAX_NM; ++i)
      L.label[static_cast<size_t>(i)] = std::numeric_limits<double>::quiet_NaN();
   return L;
}

// A target's label linkage (JS scopeLinkage): its <prefix>-binder preset (Legacy: the structure's built-in numbers),
// then each of -binder-nm, -binder-flex-nm, -linker-min-nm, -linker-max-nm that is >= 0; false for a target with no
// linkage model (the docking strands).
static bool ScopeLinkage(const ScopeSpec& spec, int s, double nm[4], std::string& err)
{
   const TargetData& t = TargetAt(s);
   if (t.linkageDefault < 0)
      return false;
   const char* P = t.prefix;
   const int i = static_cast<int>(ScopeSpecGet(spec, Opt(P, "binder").c_str()));
   if (i < 0 || i >= static_cast<int>(LinkagePresetIds().size()))
   {
      err = Opt(P, "binder") + ": no linkage preset " + std::to_string(i);
      return false;
   }
   const LinkagePresetData& q = LinkagePresetAt(i);
   if (q.structureDefault)
      LegacyLinkageNm(t.id, nm);
   else
      std::copy(q.nm, q.nm + 4, nm);
   const char* const opts[4] = { "binder-nm", "binder-flex-nm", "linker-min-nm", "linker-max-nm" };
   for (int k = 0; k < 4; ++k)
   {
      const double v = ScopeSpecGet(spec, Opt(P, opts[k]).c_str());
      if (v >= 0)
         nm[k] = v;
   }
   return true;
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
      if (!ScopeTargetOn(spec, s))
      {
         labels[static_cast<size_t>(s)] = OffLabel(eff);
         continue;
      }
      const double pct = ScopeStructureLabelingPct(spec, s, eff.mode);
      LabelPhysicsOptions o;
      o.density = std::min(1.0, std::max(0.0, pct / 100));
      o.imagerNm = O(Opt(P, "imager-nm"));
      o.paintKonPerMPerSec = O(Opt(P, "paint-kon"));
      o.paintOnSec = O(Opt(P, "paint-on-sec"));
      o.orientationMode = static_cast<int>(O(Opt(P, "orient")));
      o.polarDeg = O(Opt(P, "orient-polar-deg"));
      o.azimuthDeg = O(Opt(P, "orient-azimuth-deg"));
      o.wobbleDeg = O(Opt(P, "wobble-deg"));
      o.motion = static_cast<int>(O(Opt(P, "motion")));
      if (TargetAt(s).linkageDefault >= 0)
      {
         if (!ScopeLinkage(spec, s, o.linkageNm, err))
            return false;
         o.hasLinkage = true;
      }
      if (!MakeLabelPhysics(eff, lp, o, labels[static_cast<size_t>(s)], err))
         return false;
   }
   return true;
}

// The kinetic inputs of a spec (ScopeMovie.h KineticEnv): the light keys (the
// lasers, the light preset, excitation filter and dichroic, the imager
// concentrations and DNA-PAINT binding kinetics: they act from now on) and the
// dye keys (dye choices, modes, overrides).
static bool IsLightKey(const std::string& k)
{
   auto ends = [&](const char* e) {
      const size_t n = std::strlen(e);
      return k.size() >= n && k.compare(k.size() - n, n, e) == 0;
   };
   return k.compare(0, 6, "laser-") == 0 || k == "light-preset" || k == "ex-filter" || k == "ex-lo-nm" ||
          k == "ex-hi-nm" || k == "dichroic" || k == "dichroic-edge-nm" || ends("-imager-nm") || ends("-paint-kon") ||
          ends("-paint-on-sec");
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

bool ScopeSampleLines(const ScopeSpec& spec, std::vector<LaserLine>& lines, std::string& err)
{
   lines.clear();
   ScopeCamera camera;
   LightPath lp;
   if (!MakeScopeCamera(spec, camera, err) || !MakeScopeLightPath(spec, camera, lp, err))
      return false;
   for (const LaserLine& l : lp.lasers)
   {
      const double I = l.kWPerCm2 * (1 - SampleAt(lp.dichroicT, l.nm));
      if (I > 0)
         lines.push_back({ l.nm, I });
   }
   return true;
}

bool ScopeEnvLabels(const ScopeSpec& cur, const KineticEnv& env, std::vector<LabelPhysics>& labels, std::string& err)
{
   const ScopeSpec es = EnvSpec(cur, env);
   ScopeCamera camera;
   LightPath lp;
   return MakeScopeCamera(es, camera, err) && MakeScopeLightPath(es, camera, lp, err) &&
          MakeScopeLabels(es, lp, labels, err);
}

struct ScopeSetup
{
   SimulationParams p;
   SampleSettings cf;
   SampleQuery q;
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
   Excitation excite;   // the excitation geometry (ScopeExcitationSpec)
   // The mounted specimen's structures (ScopeMountedStructures): only their labels render; another specimen's
   // labels exist in the world but have no sites (JS S.mounted).
   std::vector<int> mounted;
   bool Mounted(int s) const { return std::find(mounted.begin(), mounted.end(), s) != mounted.end(); }
   double meanFieldDensityPerUm2 = 20, meanFieldSlabNm = 500, meanFieldMaxEmitters = 5000;
   double blinkBinnedDensityPerUm2 = 12.5, blinkBinnedMaxEmitters = 1e9;
   double blinkMeanFieldDensityPerUm2 = 1e9, blinkMeanFieldMaxEmitters = 1e9;
   int blinkBinnedUpscale = 2;
   double blinkMinPhotons = 50;
   // The fiducial beads (JS S.fid): on at fid-density > 0, the type and its components through the light path.
   bool fidOn = false;
   int fidType = 0;
   std::vector<FiducialComponent> fidComps;
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
   const int specimen = ScopeSpecimen(spec);
   if (specimen < 0 || specimen >= static_cast<int>(SpecimenIds().size()))
   {
      err = "specimen " + std::to_string(specimen) + ": not one of 0.." + std::to_string(SpecimenIds().size() - 1) +
            " (data/specimens.json)";
      return false;
   }
   S.mounted = ScopeMountedStructures(spec);
   if (!MakeScopeCamera(spec, S.camera, err) || !MakeScopeLightPath(spec, S.camera, S.lp, err) ||
       !MakeScopeLabels(spec, S.lp, S.labels, err))
      return false;
   S.excite.Make(ScopeExcitationSpec(spec));
   {
      // The fiducial beads' light (JS scopeFiducials).
      const int t = static_cast<int>(O("fid-type"));
      if (!(t >= 0 && t < static_cast<int>(FiducialTypeIds().size())))
      {
         err = "fid-type " + std::to_string(t) + ": not one of 0.." + std::to_string(FiducialTypeIds().size() - 1);
         return false;
      }
      S.fidType = t;
      S.fidOn = ScopeFiducialParams(spec)[0].second > 0;
      FiducialLightOptions fo;
      fo.dyes = O("fid-dyes");
      fo.halfSec = std::max(0.0, O("fid-bleach-half-sec"));
      fo.refKWcm2 = std::max(1e-9, O("fid-bleach-ref-kwcm2"));
      fo.blockingOD = O("fid-blocking-od");
      if (S.fidOn && !FiducialPhotophysics(t, S.lp, fo, S.fidComps, err))
         return false;
   }
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

   SampleSettings cf;
   const double worldSeed = O("world-seed");
   cf.seed = worldSeed >= 0 ? static_cast<uint32_t>(static_cast<uint64_t>(worldSeed))
                            : static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x43454C4CULL);
   cf.cacheDir = O("disk-cache") >= 1 ? DefaultCacheDir() : std::string();
   SetPsfKernelDiskCacheDir(O("disk-cache") >= 2 ? DefaultCacheDir() : std::string());   // "" under Emscripten
   std::map<std::string, double> world = {
      { "chunkSize", O("chunk-um") }, { "density", O("occupancy") },
      { "cellDiamMin", O("cell-diam-min-um") }, { "cellDiamMax", O("cell-diam-max-um") },
      { "mtDensity", O("mt-density") }, { "npcDensity", O("npc-density") }, { "laminDensity", O("lamin-density") },
      { "enablePacking", O("packing") != 0 ? 1.0 : 0.0 },
   };
   // The specimen and its params (the origami-* options; p.<name> on top, below).
   for (const auto& kv : ScopeSpecimenParams(spec))
      world[kv.first] = kv.second;
   for (const auto& kv : ScopeFiducialParams(spec))
      world[kv.first] = kv.second;
   // p.* pass-through (known core names only), overriding the named ones.
   IscParams* probe = isc_params_new();
   for (const auto& kv : spec)
      if (kv.first.compare(0, 2, "p.") == 0 && isc_params_set(probe, kv.first.c_str() + 2, kv.second) == 0)
         world[kv.first.substr(2)] = kv.second;
   isc_params_free(probe);
   cf.params.assign(world.begin(), world.end());
   for (const LabelPhysics& l : S.labels)
      cf.labels.push_back(l.label);

   // The camera's SampleQueryFor (drift: grown below).
   const double um = p.pixelSizeNm / 1000.0, margin = 2.0;
   SampleQuery q;
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
   S.blinkBinnedDensityPerUm2 = std::max(0.0, O("blink-binned-density-per-um2"));
   S.blinkBinnedMaxEmitters = std::max(0.0, O("blink-binned-max-emitters"));
   S.blinkBinnedUpscale = std::max(1, std::min(8, static_cast<int>(std::lround(O("blink-binned-upscale")))));
   S.blinkMinPhotons = std::max(0.0, O("blink-min-photons"));
   S.blinkMeanFieldDensityPerUm2 = std::max(0.0, O("blink-mean-field-density-per-um2"));
   S.blinkMeanFieldMaxEmitters = std::max(0.0, O("blink-mean-field-max-emitters"));
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

bool ScopeExcitation(const ScopeSpec& spec, Excitation& out, std::vector<ExcitationLine>& shares, std::string& err,
                     bool* lightKinetics)
{
   ScopeSetup S;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   out = S.excite;
   if (lightKinetics)
   {
      *lightKinetics = false;
      for (size_t s = 0; s < S.labels.size(); ++s)
         *lightKinetics = *lightKinetics || (S.Mounted(static_cast<int>(s)) && S.labels[s].mode != MODE_DNA_PAINT);
   }
   shares.clear();
   for (size_t s = 0; s < S.labels.size(); ++s)
      if (S.Mounted(static_cast<int>(s)) && S.labels[s].main.emits)
      {
         for (const StatePhysics::LineShare& l : S.labels[s].main.lineShares)
            shares.push_back({ l.nm, l.share });
         break;
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
// v sorted ascending, the same vector as std::sort's: bucketed by height (a bucket's index never falls as z grows), the
// buckets sorted on all cores. The ~7 M heights of a 48 um WideField column took 0.65 s in one std::sort.
void SortHeights(std::vector<double>& v)
{
   if (v.size() < (1u << 16))
   {
      std::sort(v.begin(), v.end());
      return;
   }
   const auto mm = std::minmax_element(v.begin(), v.end());
   const double lo = *mm.first, hi = *mm.second;
   if (!(hi > lo))
      return;
   const size_t B = std::min<size_t>(v.size() / 256, size_t(1) << 16);
   const double scale = static_cast<double>(B) / (hi - lo);
   auto bucket = [&](double z) { return std::min(B - 1, static_cast<size_t>((z - lo) * scale)); };
   std::vector<size_t> start(B + 1, 0);
   for (double z : v)
      start[bucket(z) + 1]++;
   for (size_t b = 0; b < B; ++b)
      start[b + 1] += start[b];
   std::vector<size_t> pos(start.begin(), start.end() - 1);
   std::vector<double> out(v.size());
   for (double z : v)
      out[pos[bucket(z)]++] = z;
   const unsigned chunks = 64;
   ParallelFor(chunks, [&](unsigned k) {
      for (size_t b = B * k / chunks; b < B * (k + 1) / chunks; ++b)
         std::sort(out.begin() + static_cast<std::ptrdiff_t>(start[b]), out.begin() + static_cast<std::ptrdiff_t>(start[b + 1]));
   });
   v.swap(out);
}

struct MapEntries;
struct MeanFieldSlot
{
   WidefieldScene scene;
   std::unique_ptr<WidefieldPsf> psf;
   PsfKernelCache psfCache;
   int upscale = 0;
   double gauss[4] = { 0, 0, 0, 0 };
   bool gpuMode = false, has = false;
   long psfVersion = 0;
   // AnchorMeanField: the pose (FOV origin, world um) the scene is kept at.
   bool anchored = false;
   double anchorX = 0, anchorY = 0;
   // A host's map (MapEntriesFor): the grid (geometry) and lattice its entries were made for, each grid cell's place
   // between them (the scene's weight map), and the population's weight entries by (map, grid, population).
   std::vector<double> tileGeom;
   std::shared_ptr<const WidefieldWeightMap> cellMap;
   std::vector<double> entKey;
   std::shared_ptr<const MapEntries> ent;
};

struct MovieCache
{
   std::mutex mutex;
   SampleSource source;
   SampleSettings world;
   bool haveWorld = false;
   uint64_t version = 0;      // the cells (seed, params): the BrightField scene
   uint64_t dyeVersion = 0;   // the dyes too (labels): the mean-field dye tiles
   BrightfieldScene brightfield;
   // Mean-field scenes by population (structure x 4 + state).
   std::map<int, std::unique_ptr<MeanFieldSlot>> meanField;
   // Dye counts (the mean-field switch's nZ, nSlab, nZLit) from a column's dye heights, sorted per structure (one
   // sites query per column rect and dyeVersion): z in [zLo, zHi) and the rect as the core's ForEachDye filters, so a
   // count = Density3d's with one bin. The boxes' heights follow the focus, so a count per box (as until 2026-10-14)
   // missed at every focus step: 37-65 ms of each z step of a live WideField scrub. -1 on a core failure.
   struct SlabColumn
   {
      double rect[4];
      uint64_t version = 0;
      std::vector<std::vector<double>> z;   // per structure, sorted
   };
   std::vector<SlabColumn> slabColumns;
   static constexpr size_t kSlabColumns = 4;
   static constexpr double kSlabStripUm = 4;
   SlabColumn* Column(double x0, double y0, double x1, double y1)
   {
      const double rect[4] = { x0, y0, x1, y1 };
      for (SlabColumn& c : slabColumns)
         if (c.version == dyeVersion && std::equal(rect, rect + 4, c.rect))
            return &c;
      const double inf = std::numeric_limits<double>::infinity();
      SlabColumn c;
      std::copy(rect, rect + 4, c.rect);
      c.version = dyeVersion;
      // In strips of kSlabStripUm (half-open in x as the core's filter, the boundaries shared, so each dye is in one
      // strip), keeping the heights only: a whole column's sites at once (5 doubles per dye, and the core's own copy)
      // took > 1 GB for the ~7 M dyes of a 48 um WideField field, past the viewer's 2 GB WASM heap (2026-10-16).
      const int strips = std::max(1, static_cast<int>(std::ceil((x1 - x0) / kSlabStripUm)));
      auto edge = [&](int k) { return k == 0 ? x0 : k == strips ? x1 : x0 + (x1 - x0) * k / strips; };
      std::vector<double> rows;
      for (int k = 0; k < strips; ++k)
      {
         if (!source.Sites(edge(k), y0, edge(k + 1), y1, -inf, inf, rows))
            return nullptr;
         for (size_t i = 0; i + ISC_SITE_STRIDE <= rows.size(); i += ISC_SITE_STRIDE)
         {
            const size_t s = static_cast<size_t>(rows[i + 4]);
            if (s >= c.z.size())
               c.z.resize(s + 1);
            c.z[s].push_back(rows[i + 2]);
         }
      }
      for (std::vector<double>& v : c.z)
         SortHeights(v);
      if (slabColumns.size() >= kSlabColumns)
         slabColumns.erase(slabColumns.begin());
      slabColumns.push_back(std::move(c));
      return &slabColumns.back();
   }
   // A rect without a column is counted by the core (Density3d, one bin: the same count), the box's count kept; asked
   // again at other heights (a focus scrub) it gets its column. So a single movie builds no column for a count it
   // reads once (the FOV's nSlab: 0.4 s of a 48 um WideField field in the viewer's WASM).
   struct SlabCount
   {
      double box[6];
      int mask;
      uint64_t version;
      long n;
   };
   std::vector<SlabCount> slabCounts;
   long CountSlab(double x0, double y0, double x1, double y1, double zLo, double zHi, int mask)
   {
      const double rect[4] = { x0, y0, x1, y1 }, box[6] = { x0, y0, x1, y1, zLo, zHi };
      bool hasColumn = false, seen = false;
      for (const SlabColumn& c : slabColumns)
         hasColumn = hasColumn || (c.version == dyeVersion && std::equal(rect, rect + 4, c.rect));
      if (!hasColumn)
      {
         for (const SlabCount& c : slabCounts)
            if (c.version == dyeVersion && std::equal(rect, rect + 4, c.box))
            {
               if (c.box[4] == zLo && c.box[5] == zHi)
               {
                  if (c.mask == mask)
                     return c.n;
               }
               else
                  seen = true;
            }
         if (!seen)
         {
            float one = 0;
            const long n = source.Density3d(x0, y0, x1, y1, zLo, zHi, 1, 1, 1, mask, &one);
            if (n >= 0)
            {
               if (slabCounts.size() >= 4 * kSlabColumns)
                  slabCounts.erase(slabCounts.begin());
               SlabCount c;
               std::copy(box, box + 6, c.box);
               c.mask = mask;
               c.version = dyeVersion;
               c.n = n;
               slabCounts.push_back(c);
            }
            return n;
         }
      }
      const SlabColumn* col = Column(x0, y0, x1, y1);
      if (!col)
         return -1;
      long n = 0;
      for (size_t s = 0; s < col->z.size(); ++s)
         if ((mask >> s) & 1)
         {
            const std::vector<double>& v = col->z[s];
            n += static_cast<long>(std::lower_bound(v.begin(), v.end(), zHi) - std::lower_bound(v.begin(), v.end(), zLo));
         }
      return n;
   }
   // The heights of structure s's dyes in [zLo, zHi) of the column at K quantiles ((k + 1/2) / K; none without dyes).
   bool SlabQuantiles(double x0, double y0, double x1, double y1, double zLo, double zHi, int s, int K,
                      std::vector<double>& out)
   {
      out.clear();
      const SlabColumn* col = Column(x0, y0, x1, y1);
      if (!col)
         return false;
      if (s < 0 || static_cast<size_t>(s) >= col->z.size())
         return true;
      const std::vector<double>& v = col->z[static_cast<size_t>(s)];
      const size_t lo = static_cast<size_t>(std::lower_bound(v.begin(), v.end(), zLo) - v.begin());
      const size_t hi = static_cast<size_t>(std::lower_bound(v.begin(), v.end(), zHi) - v.begin());
      for (int k = 0; hi > lo && k < K; ++k)
         out.push_back(v[lo + static_cast<size_t>((k + 0.5) / K * static_cast<double>(hi - lo))]);
      return true;
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
   // A host's map: each population's survey (FluorescenceMovie::Impl::Survey) by population (structure x 4 + state):
   // the clock pairs of its places the light reaches (DOSE: their epochs' bounds, nBounds each) and the count of all,
   // made per (map, query, heights, light).
   struct SurveyPoints
   {
      std::vector<double> key;
      std::vector<double> a, b, bounds;
      long n = 0;
   };
   std::map<int, SurveyPoints> surveys;
};

MovieCache& SharedMovieCache()
{
   static MovieCache c;
   return c;
}

// Configures the shared source; bumps the versions when the world (and for
// dyeVersion the labels) change.
bool ConfigureShared(MovieCache& c, const SampleSettings& cf, std::string& err)
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

bool ScopePreviewState(const ScopeSpec& spec, ScopeStateReadout& out, std::string& err)
{
   ScopeSetup S;
   out = ScopeStateReadout();
   if (!MakeScopeSetup(spec, S, err))
      return false;
   for (int s = 0; s < static_cast<int>(S.labels.size()); ++s)
   {
      const LabelPhysics& L = S.labels[static_cast<size_t>(s)];
      const StatePhysics* st = L.main.emits ? &L.main : L.pre.emits ? &L.pre : nullptr;
      if (L.off || !st)
         continue;
      out.emits = true;
      out.detectedFraction = st->detectedFraction;
      out.lambdaNm = st->lambdaNm;
      out.detectedPerSec = st->detectedPerSec;
      out.onSecNow = L.onSecNow;
      return true;
   }
   return true;
}

bool MakeScopePsfPreview(const ScopeSpec& spec, ScopePsfPreview& out, std::string& err)
{
   ScopeStateReadout st;
   if (!ScopePreviewState(spec, st, err))
      return false;
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
   ExcitationShares shares;   // its state's excitation per laser line (Excitation.h)
   // The binned blink regime's grid and kernel spectra (main groups).
   BinnedBlinkRenderer binned;
   // The blink mean-field regime (main groups of a blinking label): the label
   // mode, its dyes in the z range / focal slab, the kinetics on the reference
   // clock (start-sec, or a host's map at the FOV centre: the clock at frame 0
   // and its rate, clock seconds per second) and which frames render
   // mean-field (no events drawn for them).
   int mode = 0;
   long nZ = 0, nSlab = 0;
   std::vector<BlinkKineticsSegment> refSegs;
   double refClock = 0, refRate = 1;
   std::vector<char> mfFrame;
   bool anyMf = false, allMf = false;
};

// The mean-field image of a population: its scene, the scene's spec at the spec's focus and its images.
struct MeanFieldNode
{
   MeanFieldSlot* slot = nullptr;
   WidefieldSceneSpec ws;
   std::vector<float> image;                       // W x H photons per (photon per dye), at the spec's z
   std::map<double, std::vector<float>> imageAt;   // ... at other z (a host's z sequence)
   // Drift: the mean-field images (with spectra) at the focus-grid foci, by world focus; a weighted scene's basis at
   // each anchored at driftAnchor (the weights of the frame that set it).
   std::map<double, WidefieldImages> driftImages;
   std::vector<float> driftAnchor;
   bool driftAnchored = false;
   bool deferred = false;   // a weighted scene's images left to the GPU host (BuildMeanField)
   // AnchorMeanField: the scene's images go to the FOV shifted by these (grid cells).
   bool shifted = false;
   double shiftX = 0, shiftY = 0;
};

// A population's weight entries on its mean-field scene under a host's clock map (MapEntriesFor, 2026-10-15): on the
// lattice of the structure's map (its finest grid's nodes, ((tx + 1/2) h, (ty + 1/2) h), h <= kLightTileUm) over the
// scene's grid and one node beyond (the slot's cellMap: each grid cell reads them log-bilinearly, as a dye reads the
// map), per height band when the map changes with height (edges: world um between the bands, SetWeightBands), each
// entry's clock pair (ClockPairAt) at its node and its band's middle dye plane, band-major ((band x nty + ty) x ntx +
// tx). A rate population's decay per entry (rates; null when it is the same everywhere: decay0) is fixed while the map
// is.
struct MapEntries
{
   size_t ntx = 0, nty = 0, nb = 1;
   std::vector<double> edges;
   std::vector<double> a, b;
   size_t nBounds = 0;   // DOSE: its epochs' bounds per entry (epochs - 1)
   std::vector<double> bounds;
   std::shared_ptr<const std::vector<float>> rates;
   double decay0 = 0;
   size_t Count() const { return a.size(); }
   const double* Bounds(size_t i) const { return nBounds ? &bounds[i * nBounds] : nullptr; }
};

// A continuous population (WideField dyes, a PALM pre state, the dSTORM
// initial ON): mean-field or per dye per frame.
struct Population
{
   int structure = 0, state = 0;
   size_t group = 0;
   double lambda = 0, budget = 0, emissionPerSec = 0, rate = 0;
   long nZ = 0, nSlab = 0;
   long nZLit = 0;   // nZ at the places the light reaches (a host's map: Survey; the mean-field switch)
   bool meanFieldAt0 = false, needWindows = false;
   // A host's map (Survey): the survival at frame 0 averaged over the places the light reaches, and the decay rate
   // averaged with the survival as its weight (the mean-field switch: meanSurvival exp(-surveyRate t)).
   bool surveyed = false;
   double meanSurvival = 1.0, surveyRate = 0.0;
   // A map: the mean-field image is frame 0's photons with each weight entry at its own clock (ent; wb0: mean photons
   // per dye); frame f is it x exp(-decay f exposure), or with rates each entry at its own (RateWeightedFrame). lam:
   // each epoch's decay rate per clock second (PopulationLambda under the epoch's labels).
   bool weighted = false;
   std::shared_ptr<const MapEntries> ent;
   std::vector<float> wb0;
   std::shared_ptr<const std::vector<float>> rates;
   double decay = 0;
   // wb0 in its scene's Chebyshev basis (AnchorDecay): frames f > 0 render from the basis' decay (RenderDecayed).
   WidefieldScene::DecayAnchor anchor;
   bool anchored = false;
   std::vector<double> lam;
   std::vector<BlinkEvent> wins;
   // Its mean-field image (meanFieldAt0): one node.
   std::vector<MeanFieldNode> mf;
   bool mfBuilt = false;
   std::vector<double> acc;         // the running image of the per-dye path
   long accFrame = -1;
   double accZ = 0;                 // the z the running image was made at
   double accDx = 0, accDy = 0;     // ... and the sample drift (px)
   long meanFieldFrames = 0, perDyeFrames = 0;
   // A blink population (state ISC_STATE_BLINK, the blink mean-field regime):
   // the label mode, its kinetics on its clock (unweighted: the clock at the
   // FOV centre at frame 0 and its rate), and the focus the weighted scene is
   // at.
   int mode = 0;
   std::vector<BlinkKineticsSegment> blinkSegs;
   double blinkClock = 0, blinkRate = 1;
   double sceneZ = 0;
   // A weighted blink population: the weight entries its scene's dye cells read (WidefieldScene::UsedWeights; the
   // others stay 0: each costs a Markov chain per frame).
   std::vector<uint8_t> used;
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

// ---- a host's clock map (LightHistory.h; spec/ALGORITHM.md "Exact light history") ----
// A dye's clock there is a pair (a, b) at its place (ClockPairAt): its clock, or hazard, at T = tOpen and its rate
// per second after. Per mode:
//   GLOBAL (DNA-PAINT; a structure no light excites): the sample's time T itself;
//   DOSE (dSTORM, WideField): the dose a + b (T - tOpen), epoch e > 0 from the place's dose bound[e - 1] on; b = 0
//     (no light now): frozen, no photons;
//   ACTIVATION (PALM): the activation hazard a + b (T - tOpen) (a continuous population: its pre state, with the
//     photon budget's bleaching); the blinks run in T after their activation.

// Whether a structure's clock changes with the height (TIRF's depth tables, HILO's shear).
bool MapZDependent(const ClockMapView& m, int st)
{
   const MapClock& mc = m.s[static_cast<size_t>(st)];
   auto dep = [](const MapField& f) {
      for (const MapTerm& t : f)
         if (t.depth >= 0 || t.sx != 0.0 || t.sy != 0.0)
            return true;
      return false;
   };
   bool z = dep(mc.total) || dep(mc.rate) || dep(mc.bleachTotal) || dep(mc.bleachRate);
   for (const MapField& f : mc.bound)
      z = z || dep(f);
   for (const MapStep& p : mc.steps)
      z = z || dep(p.field);
   return z;
}

// A place's clock pair (world um) and, DOSE, its epochs' bounds (mc.bound.size() of them). blink: the blinks' (PALM:
// the activation alone), else a continuous population's (PALM's pre state: with its bleaching).
void ClockPairAt(const ClockMapView& m, int st, bool blink, double x, double y, double z, double& a, double& b,
                 double* bounds)
{
   const MapClock& mc = m.s[static_cast<size_t>(st)];
   if (mc.mode == ISC_CLOCK_GLOBAL)
   {
      a = mc.tOpen;
      b = 1.0;
      return;
   }
   if (mc.mode == ISC_CLOCK_DOSE)
   {
      a = m.Clock(st, x, y, z);
      b = m.Rate(st, x, y, z);
      for (size_t e = 0; e < mc.bound.size(); ++e)
         bounds[e] = m.Eval(mc.bound[e], x, y, z);
      return;
   }
   a = m.Hazard(st, x, y, z, mc.tOpen);
   b = m.Rate(st, x, y, z);
   if (!blink)
   {
      a += m.Eval(mc.bleachTotal, x, y, z);
      b += m.Eval(mc.bleachRate, x, y, z);
   }
}

// Each epoch's decay rate per clock second of a continuous population (PopulationLambda under the epoch's labels;
// none: the current spec's).
std::vector<double> EpochLambdas(const MapClock& mc, const std::vector<LabelPhysics>& cur, int st, int state)
{
   std::vector<double> lam;
   for (const MapEpoch& e : mc.epochs)
      lam.push_back(PopulationLambda(state, e.labels ? (*e.labels)[static_cast<size_t>(st)] : cur[static_cast<size_t>(st)]));
   return lam;
}

// A continuous population's decay rate per second at a place's rate b (an infinite one: 0, it has no dye left).
double PopulationDecay(const MapClock& mc, const std::vector<double>& lam, double b)
{
   if (mc.mode == ISC_CLOCK_ACTIVATION)
      return b;
   const double r = mc.mode == ISC_CLOCK_GLOBAL ? 1.0 : b, l = lam.empty() ? 0.0 : lam.back();
   return r > 0 && std::isfinite(l) ? l * r : 0.0;
}

// ... its survival at the sample's time T at a place's clock pair (bounds: DOSE's) and its decay rate per second.
void PopulationAt(const MapClock& mc, const std::vector<double>& lam, double a, double b, const double* bounds,
                  double T, double& S, double& h)
{
   if (mc.mode == ISC_CLOCK_ACTIVATION)
   {
      S = std::exp(-(a + b * (T - mc.tOpen)));
      h = b;
      return;
   }
   const bool global = mc.mode == ISC_CLOCK_GLOBAL;
   const double tau = global ? T : a + b * (T - mc.tOpen);
   const size_t n = std::min(mc.epochs.size(), lam.size());
   auto start = [&](size_t e) { return e == 0 ? 0.0 : global ? mc.epochs[e].tStart : bounds[e - 1]; };
   double H = 0.0;
   for (size_t e = 0; e < n; ++e)
   {
      const double d = (e + 1 < n ? std::min(tau, start(e + 1)) : tau) - start(e);
      if (d > 0)
         H += lam[e] * d;
   }
   S = std::exp(-H);
   h = S > 0 ? PopulationDecay(mc, lam, b) : 0.0;
}

// The mean of exp(-h t) over [0, w) times w (exact for any h >= 0: expm1, no cancellation at a small h w).
double DecayWindow(double h, double w)
{
   return h > 0 ? -std::expm1(-h * w) / h : w;
}

// ... its mean photons per dye in [T, T + expSec) (rate: detected per second at the movie's light; the excitation's
// factor is the scene's plane factor): its survival at T x the window of its decay rate per second (a place the light
// barely reaches has a clock rate near 0: no difference of exponentials, which there is rounding noise).
double PopulationPhotons(const MapClock& mc, const std::vector<double>& lam, double rate, double a, double b,
                         const double* bounds, double T, double expSec)
{
   if (mc.mode == ISC_CLOCK_ACTIVATION)
      return rate * std::exp(-(a + std::max(0.0, b) * (T - mc.tOpen))) * DecayWindow(std::max(0.0, b), expSec);
   const bool global = mc.mode == ISC_CLOCK_GLOBAL;
   if (!global && !(b > 0))
      return 0.0;   // frozen: no light
   if (lam.size() == 1)
   {
      if (!std::isfinite(lam[0]))
         return 0.0;
      const double r = global ? 1.0 : b, tau = global ? T : a + b * (T - mc.tOpen);
      return rate * std::exp(-lam[0] * tau) * DecayWindow(lam[0] * r, expSec);
   }
   double S = 0, h = 0;
   PopulationAt(mc, lam, a, b, bounds, T, S, h);
   if (!(S > 0))
      return 0.0;
   return rate * S * DecayWindow(h, expSec);
}

// A kinetics row (ISC_KIN_*) as a blink segment from t0 on.
BlinkKineticsSegment KinSegment(const double* k, double t0)
{
   BlinkKineticsSegment g;
   g.tStart = t0;
   g.activationRatePerSec = k[ISC_KIN_ACTIVATION_RATE];
   g.onSec = k[ISC_KIN_ON_SEC];
   g.offSec = k[ISC_KIN_OFF_SEC];
   g.bleachProb = k[ISC_KIN_BLEACH_PROB];
   g.initialOnSec = k[ISC_KIN_INITIAL_ON_SEC];
   return g;
}

// A blink population at a place's clock pair: its clock at the sample's time T (t), the clock's rate (clock seconds
// per second; 0: frozen, no light) and its kinetics segments on that clock (ExpectedBlinkOnSeconds). PALM: the
// activation hazard a + b (T - tOpen) as the constant rate b since a / b before T, its blinks the last epoch's (without
// activation now: the hazard a spread over [0, tOpen]).
void BlinkClockAt(const MapClock& mc, double a, double b, const double* bounds, double T, double& t, double& rate,
                  std::vector<BlinkKineticsSegment>& segs)
{
   segs.clear();
   if (mc.mode == ISC_CLOCK_GLOBAL)
   {
      t = T;
      rate = 1.0;
      for (const MapEpoch& e : mc.epochs)
         segs.push_back(KinSegment(e.kin, e.tStart));
      return;
   }
   if (mc.mode == ISC_CLOCK_DOSE)
   {
      t = a + b * (T - mc.tOpen);
      rate = b;
      for (size_t e = 0; e < mc.epochs.size(); ++e)
         segs.push_back(KinSegment(mc.epochs[e].kin, e == 0 ? 0.0 : bounds[e - 1]));
      return;
   }
   rate = 1.0;
   segs.push_back(KinSegment(mc.epochs.back().kin, 0.0));
   if (b > 0)
   {
      t = a / b + (T - mc.tOpen);
      segs[0].activationRatePerSec = b;
      return;
   }
   t = T;
   segs[0].activationRatePerSec = mc.tOpen > 0 ? a / mc.tOpen : 0.0;
   if (mc.tOpen > 0)
   {
      segs.push_back(segs[0]);
      segs[1].tStart = mc.tOpen;
      segs[1].activationRatePerSec = 0.0;
   }
}

// ... its photons per dye in the exposure from clock t (rate: detected per second ON; clock rate r).
double BlinkPhotons(int mode, double rate, const std::vector<BlinkKineticsSegment>& segs, double t, double r,
                    double expSec)
{
   return r > 0 ? rate * ExpectedBlinkOnSeconds(mode, segs, t, t + r * expSec) / r : 0.0;
}

const char* PopulationName(int state)
{
   return state == ISC_STATE_ALWAYS_ON    ? "WideField dyes"
          : state == ISC_STATE_INITIAL_ON ? "initial ON"
          : state == ISC_STATE_BLINK      ? "blinks"
                                          : "pre state";
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
   if (e.excite != 1.0)
   {
      sign *= e.excite;
      if (sign == 0.0)
         return;
   }
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

// The mean-field scenes' plane factor of a shaped excitation (JS widefield.js
// planeFactor): the factor a at the cell centre = min(F, F x the footprint at
// the plane centre (at least 0: a plane below the coverslip, dyes of a label
// offset, at z = 0's) x the plane's mean depth factor); the weights carry the
// clocks (photons per dye at a factor of 1).
struct ExcitePlaneFactor : WidefieldPlaneFactor
{
   Excitation ex;
   ExcitationShares shares;
   double offX = 0, offY = 0;   // the beam's axis from the scene's (AnchorMeanField), um
   float At(long, double z0, double z1, double x, double y) const override
   {
      if (ex.Clipped())
         return 0.0f;
      x += offX;
      y += offY;
      const double dm = ex.DepthMean(z0, z1, shares), zc = std::max(0.0, (z0 + z1) / 2);
      return static_cast<float>(std::min(ex.F(), ex.F() * ex.Lateral(x, y, zc) * dm));
   }
};

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
   std::unique_ptr<FiducialMovie> fid;   // the beads (none: nullptr)
   // A shaped excitation (not Uniform): the factor of a dye state at (xUm,
   // yUm, zNm) in the FOV frame, the per-pixel background (empty: m.bg) and
   // its imager part's mean.
   bool shaped = false;
   double axisX = 0, axisY = 0;
   std::vector<float> bgMap;
   double imagerMean = 0;
   std::vector<double> exciteKey;
   double ExciteAt(const ExcitationShares& sh, double xUm, double yUm, double zNm) const
   {
      return S.excite.At(xUm - axisX, yUm - axisY, zNm / 1000.0 + S.q.zRefUm, sh);
   }
   std::chrono::steady_clock::time_point t0;
   double setupSec = 0;
   bool gpuMode = false;
   WidefieldAccelerator* accel = nullptr;
   // The clock map (Begin): a host's (its light history and this movie's light) or, under a shaped beam without one,
   // the movie's own (the past start-sec under the movie's light, then the movie); every blink and per-dye window on
   // its own dye's clock (isc_events_in_map, isc_continuous_in_map), the mean-field populations per weight entry
   // (MapEntries; mapped). None: start-sec everywhere.
   std::shared_ptr<const ClockMapView> map;
   bool mapped = false;
   // A structure whose clock is the same everywhere (no map, or the map's GLOBAL: DNA-PAINT, a target no light excites).
   bool GlobalClock(int s) const { return !mapped || map->s[static_cast<size_t>(s)].mode == ISC_CLOCK_GLOBAL; }
   bool carry = false;                // CarryRunningImages
   bool anchor = false;               // AnchorMeanField
   // The mean-field nodes (population, node) whose scene images are pending (GPU mode).
   std::vector<std::pair<Population*, size_t>> sceneQueue;
   Impl() : cache(SharedMovieCache()), lock(cache.mutex, std::defer_lock) {}

   bool IsMeanField(const Population& p, long f) const
   {
      const double P = p.surveyed ? p.meanSurvival * std::exp(-p.surveyRate * ((f + 0.5) * S.expSec))
                                  : std::exp(-p.lambda * (S.t0Sec + (f + 0.5) * S.expSec));
      return p.nSlab * P / fovArea > S.meanFieldDensityPerUm2 || p.nZLit * P > S.meanFieldMaxEmitters;
   }
   bool BuildMeanField(Population& p, std::string& err);
   // The binned blink regime (spec/ALGORITHM.md "Blink render regimes"): main
   // group gi's frame f renders binned when its ON emitters (overlap-weighted:
   // what the splat would draw) per um^2 of the FOV, or in all, pass the
   // thresholds -- the splat costs per emitter, the binned FFT per FOV area.
   bool BinnedFrame(size_t gi, long f) const
   {
      if (!groups[gi].binned.Valid())
         return false;
      double nOn = 0.0;
      for (uint32_t i : buckets[gi][static_cast<size_t>(f)])
      {
         const BlinkEvent& e = byGroup[gi][i];
         const double ov = std::min(static_cast<double>(f + 1), e.tEnd) - std::max(static_cast<double>(f), e.tStart);
         if (ov > 0.0)
            nOn += std::min(ov, 1.0);
      }
      return nOn / fovArea > S.blinkBinnedDensityPerUm2 || nOn > S.blinkBinnedMaxEmitters;
   }
   // Main group gi's blinks of frame f are splatted one by one (neither
   // binned nor mean-field).
   bool SplatFrame(size_t gi, long f) const
   {
      const Group& g = groups[gi];
      return !(!g.mfFrame.empty() && g.mfFrame[static_cast<size_t>(f)]) && !BinnedFrame(gi, f);
   }
   // A structure's blink kinetics on start-sec's clock (no map): the label's, from 0.
   void LabelSegments(int structure, std::vector<BlinkKineticsSegment>& out) const
   {
      out.assign(1, KinSegment(&S.labels[static_cast<size_t>(structure)].label[ISC_LABEL_ACTIVATION_RATE], 0.0));
   }
   // A host's map over a population: the share of its dyes the light reaches, their survival and decay (Survey).
   void Survey(Population& p, double zMin, double zMax);
   // A weighted blink population's photons per dye in frame f, per weight entry its dye cells read (p.used; the
   // others 0), in chunks on the pool (consecutive entries with the same clock share the value).
   void BlinkWeights(const Population& p, long f, std::vector<float>& wb) const
   {
      const MapEntries& E = *p.ent;
      const MapClock& mc = map->s[static_cast<size_t>(p.structure)];
      const size_t n = E.Count();
      wb.assign(n, 0.0f);
      constexpr size_t kChunk = 2048;
      const bool all = p.used.size() != n;
      ParallelFor(static_cast<unsigned>((n + kChunk - 1) / kChunk), [&](unsigned c) {
         std::vector<BlinkKineticsSegment> segs;
         double la = std::numeric_limits<double>::quiet_NaN(), lb = la;
         const double* lbd = nullptr;
         float lv = 0.0f;
         for (size_t i = c * kChunk, e = std::min(n, i + kChunk); i < e; ++i)
         {
            if (!all && !p.used[i])
               continue;
            const double* bd = E.Bounds(i);
            if (!(E.a[i] == la && E.b[i] == lb && (!bd || std::equal(bd, bd + E.nBounds, lbd))))
            {
               double t = 0, r = 0;
               BlinkClockAt(mc, E.a[i], E.b[i], bd, S.t0Sec, t, r, segs);
               lv = static_cast<float>(BlinkPhotons(p.mode, p.rate, segs, t + r * (f * S.expSec), r, S.expSec));
               la = E.a[i];
               lb = E.b[i];
               lbd = bd;
            }
            wb[i] = lv;
         }
      });
   }
   // ... and its frame f at stage z into img (+=): the scene refocused if z
   // moved, rendered with the frame's weights.
   void BlinkWeightedFrame(Population& p, long f, double z, std::vector<float>& img)
   {
      if (p.mf.empty())
         return;
      MeanFieldNode& nd = p.mf[0];
      if (z != p.sceneZ)
      {
         WidefieldSceneSpec ws = nd.ws;
         ws.focusWorldUm = ws.slabCentreUm = S.q.zRefUm + z;
         const double um = S.p.pixelSizeNm / 1000.0;
         const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
         std::string err;
         nd.slot->scene.SetDeferImages(false);
         if (!nd.slot->scene.Update(cache.source, ill, ws, *nd.slot->psf, err))
            return;
         p.sceneZ = z;
         nd.slot->scene.UsedWeights(p.used);   // (the slab moved with the focus)
      }
      std::vector<float> wb, frame(static_cast<size_t>(S.W) * S.H, 0.0f);
      BlinkWeights(p, f, wb);
      if (nd.shifted)
         nd.slot->scene.RenderFrameShifted(wb, nd.shiftX, nd.shiftY, frame);
      else
         nd.slot->scene.RenderFrame(wb, frame);
      for (size_t i = 0; i < img.size(); ++i)
         img[i] += frame[i];
   }
   // A weighted rate population with rates (its decay not the same everywhere): its frame f at stage z into img (+=),
   // each entry f exposures later at its own decay rate (wb0 exp(-rate f exposure)), from the scene's basis (refocused
   // if z moved).
   void RateWeightedFrame(Population& p, long f, double z, std::vector<float>& img)
   {
      if (p.mf.empty() || !p.rates || p.rates->size() != p.wb0.size())
         return;
      MeanFieldNode& nd = p.mf[0];
      if (f == 0 && z == p.sceneZ)
      {
         for (size_t i = 0; i < img.size(); ++i)
            img[i] += nd.image[i];
         return;
      }
      const auto tR = TimingClock::now();
      if (z != p.sceneZ)
      {
         WidefieldSceneSpec ws = nd.ws;
         ws.focusWorldUm = ws.slabCentreUm = S.q.zRefUm + z;
         const double um = S.p.pixelSizeNm / 1000.0;
         const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
         std::string err;
         nd.slot->scene.SetDeferImages(false);
         if (!nd.slot->scene.Update(cache.source, ill, ws, *nd.slot->psf, err))
            return;
         p.sceneZ = z;
      }
      const std::vector<float>& rate = *p.rates;
      const double k = f * S.expSec;
      std::vector<float> frame(static_cast<size_t>(S.W) * S.H, 0.0f);
      // (The basis' coefficients at the decay, without a pass over the weights; else the weights.)
      if (!(p.anchored && nd.slot->scene.RenderDecayed(p.anchor, k, nd.shifted, nd.shiftX, nd.shiftY, frame)))
      {
         std::vector<float> wb(p.wb0.size());
         for (size_t i = 0; i < wb.size(); ++i)
            wb[i] = p.wb0[i] == 0.0f || k == 0.0 ? p.wb0[i] : static_cast<float>(p.wb0[i] * std::exp(-k * rate[i]));
         if (nd.shifted)
            nd.slot->scene.RenderFrameShifted(wb, nd.shiftX, nd.shiftY, frame);
         else
            nd.slot->scene.RenderFrame(wb, frame);
      }
      for (size_t i = 0; i < img.size(); ++i)
         img[i] += frame[i];
      TimingLog("fl.mean-field-rate-frame", TimingSince(tR));
   }
   void TakeImage(Population& p, size_t j);
   // Weighted (clock) mean-field: frame 0's photons of the scene's grid.
   void WeightedImage(Population& p, std::vector<float>& img);
   // The mean-field image at another stage z (refocused scene; cached).
   const std::vector<float>& ImageAt(Population& p, size_t j, double z);
   // An unweighted population's photons per dye in frame f (a blink population: on its clock at the FOV centre).
   double FramePhotons(const Population& p, long f) const;
   void AdvanceAcc(Population& p, long f, double z, double dxPx = 0.0, double dyPx = 0.0);
   // Drift: the mean-field image of frame f (stage z, drift d) into img (+=):
   // the scene's images at the two focus-grid foci around z - dz, their
   // spectra interpolated and shifted by d (photons per (photon per dye); a
   // weighted population: frame 0's photons, with rates each entry decayed to
   // frame f on the bleach basis -- every focus's images anchored alike).
   void DriftMeanField(Population& p, size_t j, long f, double z, const DriftNm& d, std::vector<float>& img);
   DriftFocusGrid driftGrid;
};

FluorescenceMovie::FluorescenceMovie() : impl_(new Impl) {}
void FluorescenceMovie::CarryRunningImages(bool on)
{
   impl_->carry = on;
}
void FluorescenceMovie::AnchorMeanField(bool on)
{
   impl_->anchor = on;
}
FluorescenceMovie::~FluorescenceMovie() = default;

// A host's map spans the query and this margin (um; the mean-field scenes' tiles, an anchored scene up to
// kMeanFieldAnchorUm off).
constexpr double kMapMarginUm = 2.0;
// A population whose expected dyes still in their state (Survey: initial ON, pre state, unbleached) are fewer is not
// queried for windows: about one frame in 10^4 at most would miss one such dye.
constexpr double kWindowAliveMin = 1e-4;
// A rate population's bleach basis holds this long (s) before it is rebuilt (its Chebyshev terms,
// WidefieldScene::SetWeightRates: few under a slow bleach; every term is an image to make at each refocus).
constexpr double kBasisSpanSec = 4.0;
// The pivot's index in Impl::exciteKey (the plane factor's key zeroes it for a beam that is not tilted).
constexpr size_t kExciteKeyPivot = 7;
// MapEntries' height bands: adjacent dye planes share a band while their photons per dye (the weights) differ by at
// most kMapBandTol of the largest, in the movie's first, middle and last frames of the span the bands hold for, judged
// on a raster of about kMapRasterUm; at most kMapBands bands (the tolerance doubled until they fit).
constexpr double kMapBandTol = 0.01;
constexpr size_t kMapBands = 64;
constexpr double kMapRasterUm = 1.0;
// Survey: a raster of this pitch (um) over the query at this many heights.
constexpr double kSurveyStepUm = 1.0;
constexpr int kSurveyHeights = 4;

// A movie's own light (no host): a Flat beam's field stop grown to its map's box and 1 um more (no dye or mean-field
// tile of the movie is beyond it).
static void GrowFieldStop(LightState& light, double x0, double y0, double x1, double y1)
{
   for (LightPose& p : light.poses)
   {
      p.x0 = std::min(p.x0, x0 - 1.0);
      p.y0 = std::min(p.y0, y0 - 1.0);
      p.x1 = std::max(p.x1, x1 + 1.0);
      p.y1 = std::max(p.y1, y1 + 1.0);
   }
}

// The node spacing of structure st's map: its finest grid's (kLightTileUm without grids).
static double MapLatticeUm(const ClockMapView& m, int st)
{
   const MapClock& mc = m.s[static_cast<size_t>(st)];
   double h = kLightTileUm;
   auto scan = [&](const MapField& f) {
      for (const MapTerm& t : f)
         if (t.grid >= 0)
            h = std::min(h, m.grids[static_cast<size_t>(t.grid)].h);
   };
   scan(mc.total);
   scan(mc.rate);
   scan(mc.bleachTotal);
   scan(mc.bleachRate);
   for (const MapField& f : mc.bound)
      scan(f);
   for (const MapStep& q : mc.steps)
      scan(q.field);
   return h;
}

// A population's weight entries under map m on its scene's grid gr (MapEntries; memoized in the slot per map, grid and
// population): on the structure's map lattice over the grid and one node beyond (slot.cellMap, per grid and lattice:
// each grid cell's stencil between them) and, when the map changes with height, the dye planes' heights in bands.
static std::shared_ptr<const MapEntries> MapEntriesFor(const ClockMapView& m, const Population& p,
                                                       const WidefieldGridSpec& gr, MeanFieldSlot& slot, double t0Sec,
                                                       double spanSec, double expSec)
{
   const double T = MapLatticeUm(m, p.structure);
   auto X = [&](unsigned i) { return gr.x0Um + (i + 0.5) * gr.pitchUm; };
   auto Y = [&](unsigned jy) { return gr.y0Um + (jy + 0.5) * gr.pitchUm; };
   long tx0 = 0, ty0 = 0;
   size_t ntx = 0, nty = 0;
   if (gr.nx && gr.ny)
   {
      tx0 = static_cast<long>(std::floor(X(0) / T)) - 1;
      ty0 = static_cast<long>(std::floor(Y(0) / T)) - 1;
      ntx = static_cast<size_t>(static_cast<long>(std::floor(X(gr.nx - 1) / T)) - tx0 + 2);
      nty = static_cast<size_t>(static_cast<long>(std::floor(Y(gr.ny - 1) / T)) - ty0 + 2);
   }
   const std::vector<double> geom = { gr.x0Um, gr.y0Um, gr.pitchUm, static_cast<double>(gr.nx),
                                      static_cast<double>(gr.ny), T, static_cast<double>(tx0), static_cast<double>(ty0) };
   if (geom != slot.tileGeom || !slot.cellMap)
   {
      // A cell's tile (the one it lies in) and its stencil: the nodes below and left of it.
      std::vector<uint32_t> txOf(gr.nx), tyOf(gr.ny), sxOf(gr.nx), syOf(gr.ny);
      std::vector<float> fxOf(gr.nx), fyOf(gr.ny);
      auto axis = [&](double v, long t0, uint32_t& tile, uint32_t& lo, float& f) {
         tile = static_cast<uint32_t>(static_cast<long>(std::floor(v / T)) - t0);
         const double u = v / T - 0.5 - static_cast<double>(t0);
         const double fl = std::floor(u);
         lo = static_cast<uint32_t>(static_cast<long>(fl));
         f = static_cast<float>(u - fl);
      };
      for (unsigned i = 0; i < gr.nx; ++i)
         axis(X(i), tx0, txOf[i], sxOf[i], fxOf[i]);
      for (unsigned jy = 0; jy < gr.ny; ++jy)
         axis(Y(jy), ty0, tyOf[jy], syOf[jy], fyOf[jy]);
      auto wm = std::make_shared<WidefieldWeightMap>();
      const size_t nc = static_cast<size_t>(gr.nx) * gr.ny;
      wm->count = ntx * nty;
      wm->nx = ntx;
      wm->tile.resize(nc);
      wm->corner.resize(nc);
      wm->fx.resize(nc);
      wm->fy.resize(nc);
      for (unsigned jy = 0; jy < gr.ny; ++jy)
         for (unsigned i = 0; i < gr.nx; ++i)
         {
            const size_t c = static_cast<size_t>(gr.nx) * jy + i;
            wm->tile[c] = static_cast<uint32_t>(tyOf[jy] * ntx + txOf[i]);
            const bool in = sxOf[i] + 1 < ntx && syOf[jy] + 1 < nty;
            wm->corner[c] = in ? static_cast<uint32_t>(syOf[jy] * ntx + sxOf[i]) : WidefieldWeightMap::kNone;
            wm->fx[c] = fxOf[i];
            wm->fy[c] = fyOf[jy];
         }
      slot.cellMap = wm;
      slot.tileGeom = geom;
      slot.ent.reset();
   }
   const int st = p.structure;
   const bool blink = p.state == ISC_STATE_BLINK;
   const double dz = gr.zPlaneUm;
   const bool zDep = MapZDependent(m, st) && dz > 0 && ntx && nty;
   // The bands hold for [Tq, Tq + 2 span], Tq the movie's start on a grid of the span (a host's next movies share them
   // while their start is below Tq + span).
   const double Tq = zDep && spanSec > 0 ? std::floor(t0Sec / spanSec) * spanSec : 0.0;
   std::vector<double> key = geom;
   key.insert(key.end(), { static_cast<double>(m.serial), gr.zMinUm, gr.zMaxUm, gr.zPlaneUm, static_cast<double>(st),
                           static_cast<double>(p.state), Tq, zDep ? spanSec : 0.0, zDep ? expSec : 0.0 });
   key.insert(key.end(), p.lam.begin(), p.lam.end());
   if (slot.ent && key == slot.entKey)
   {
      TimingLog("fl.mean-field-entries-reused", 0.0);
      return slot.ent;
   }
   const auto t0 = TimingClock::now();
   auto E = std::make_shared<MapEntries>();
   E->ntx = ntx;
   E->nty = nty;
   const MapClock& mc = m.s[static_cast<size_t>(st)];
   E->nBounds = mc.mode == ISC_CLOCK_DOSE ? mc.bound.size() : 0;
   auto tileX = [&](size_t tx) { return (static_cast<double>(tx0 + static_cast<long>(tx)) + 0.5) * T; };
   auto tileY = [&](size_t ty) { return (static_cast<double>(ty0 + static_cast<long>(ty)) + 0.5) * T; };
   // The bands: the grid's dye planes (centres (k + 1/2) dz, within the map's heights), merged.
   std::vector<double> bandZ;
   if (zDep)
   {
      const long k0 = static_cast<long>(std::floor(std::max(gr.zMinUm, -2.0) / dz));
      const long k1 = static_cast<long>(std::floor(std::min(gr.zMaxUm, 20.0) / dz));
      std::vector<std::pair<double, double>> pts;
      const size_t step = std::max<size_t>(1, static_cast<size_t>(std::lround(kMapRasterUm / T)));
      for (size_t ty = 0; ty < nty; ty += step)
         for (size_t tx = 0; tx < ntx; tx += step)
            pts.push_back({ tileX(tx), tileY(ty) });
      const size_t np = pts.size(), nk = k1 >= k0 ? static_cast<size_t>(k1 - k0 + 1) : 0;
      constexpr size_t kTimes = 3;
      const double Ts[kTimes] = { Tq, Tq + spanSec, Tq + 2 * spanSec };
      std::vector<double> V(nk * np * kTimes);
      ParallelFor(static_cast<unsigned>(nk), [&](unsigned kk) {
         std::vector<double> bd(mc.bound.size());
         std::vector<BlinkKineticsSegment> segs;
         const double z = (static_cast<double>(k0 + static_cast<long>(kk)) + 0.5) * dz;
         for (size_t i = 0; i < np; ++i)
         {
            double A = 0, B = 0;
            ClockPairAt(m, st, blink, pts[i].first, pts[i].second, z, A, B, bd.data());
            for (size_t t = 0; t < kTimes; ++t)
            {
               double v = 0;
               if (blink)
               {
                  double tc = 0, r = 0;
                  BlinkClockAt(mc, A, B, bd.data(), Ts[t], tc, r, segs);
                  v = BlinkPhotons(p.mode, p.rate, segs, tc, r, expSec);
               }
               else
                  v = PopulationPhotons(mc, p.lam, p.rate, A, B, bd.data(), Ts[t], expSec);
               V[(kk * np + i) * kTimes + t] = v;
            }
         }
      });
      double vMax = 0.0;
      for (double v : V)
         vMax = std::max(vMax, std::fabs(v));
      for (double tol = kMapBandTol; nk; tol *= 2)
      {
         std::vector<size_t> starts(1, 0);
         size_t s0 = 0;
         for (size_t k = 1; k < nk; ++k)
         {
            bool fits = true;
            for (size_t i = 0; i < np * kTimes && fits; ++i)
               fits = std::fabs(V[k * np * kTimes + i] - V[s0 * np * kTimes + i]) <= tol * vMax;
            if (!fits)
            {
               starts.push_back(k);
               s0 = k;
            }
         }
         if (starts.size() > kMapBands)
            continue;
         for (size_t j = 0; j < starts.size(); ++j)
         {
            const size_t ks = starts[j], ke = j + 1 < starts.size() ? starts[j + 1] - 1 : nk - 1;
            if (j > 0)
               E->edges.push_back(static_cast<double>(k0 + static_cast<long>(ks)) * dz);
            bandZ.push_back((static_cast<double>(k0 + static_cast<long>((ks + ke) / 2)) + 0.5) * dz);
         }
         break;
      }
   }
   if (bandZ.empty())
      bandZ.push_back(0.0);   // (any height: the same everywhere)
   E->nb = bandZ.size();
   const size_t nt = ntx * nty, n = nt * E->nb;
   E->a.resize(n);
   E->b.resize(n);
   E->bounds.resize(n * E->nBounds);
   ParallelFor(static_cast<unsigned>(E->nb * nty), [&](unsigned row) {
      const size_t band = row / nty, ty = row % nty;
      std::vector<double> bd(mc.bound.size());
      for (size_t tx = 0; tx < ntx; ++tx)
      {
         const size_t i = band * nt + ty * ntx + tx;
         ClockPairAt(m, st, blink, tileX(tx), tileY(ty), bandZ[band], E->a[i], E->b[i], bd.data());
         std::copy(bd.begin(), bd.begin() + static_cast<long>(E->nBounds), E->bounds.begin() + static_cast<long>(i * E->nBounds));
      }
   });
   // A rate population's decay per entry: one number, or the rates (the scene's basis by rate).
   if (!blink && n)
   {
      auto r = std::make_shared<std::vector<float>>(n);
      bool equal = true;
      const double h0 = PopulationDecay(mc, p.lam, E->b[0]);
      for (size_t i = 0; i < n; ++i)
      {
         const double h = PopulationDecay(mc, p.lam, E->b[i]);
         (*r)[i] = static_cast<float>(h);
         equal = equal && h == h0;
      }
      E->decay0 = h0;
      if (!equal)
         E->rates = r;
   }
   char b[96];
   std::snprintf(b, sizeof b, "%zu entries, %zu bands", n, E->nb);
   TimingLog("fl.mean-field-entries", TimingSince(t0), b);
   slot.entKey.swap(key);
   slot.ent = E;
   return slot.ent;
}

// A host's map over a population (2026-10-15; the mean-field switch and the windows query): on a kSurveyStepUm raster
// of the query at its dyes' heights (kSurveyHeights quantiles of the structure's dye heights there; one height when the
// map does not change with it), the share of the places the light reaches now (a shaped beam's factor above 0, and a
// DOSE clock's rate), their mean survival at frame 0, and the decay rate averaged with the survival as its weight. The
// places' clock pairs are kept per map and query (MovieCache::surveys): a live frame computes the survivals only.
void FluorescenceMovie::Impl::Survey(Population& p, double zMin, double zMax)
{
   const auto tSurvey = TimingClock::now();
   const ClockMapView& mp = *map;
   const int st = p.structure;
   const MapClock& mc = mp.s[static_cast<size_t>(st)];
   const Group& g = groups[p.group];
   std::vector<double> zs;
   if (MapZDependent(mp, st))
      cache.SlabQuantiles(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, st, kSurveyHeights, zs);
   if (zs.empty())
      zs.push_back(S.q.zCullCentreUm);
   std::vector<double> key = { static_cast<double>(mp.serial), S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, S.q.originXUm,
                               S.q.originYUm, static_cast<double>(st), static_cast<double>(p.state), shaped ? 1.0 : 0.0 };
   key.insert(key.end(), zs.begin(), zs.end());
   if (shaped)
   {
      // (Not the focus, nor the pivot of a beam that is not tilted: the plane factor's key.)
      std::vector<double> ek(exciteKey.begin(), exciteKey.end() - 1);
      if (!S.excite.Tilted())
         ek[kExciteKeyPivot] = 0.0;
      key.insert(key.end(), ek.begin(), ek.end());
      for (const ExcitationLine& l : g.shares)
      {
         key.push_back(l.nm);
         key.push_back(l.share);
      }
   }
   MovieCache::SurveyPoints& sp = cache.surveys[st * 4 + p.state];
   const size_t nb = mc.mode == ISC_CLOCK_DOSE ? mc.bound.size() : 0;
   if (sp.key != key)
   {
      sp = MovieCache::SurveyPoints();
      std::vector<double> bd(mc.bound.size());
      for (double y = S.q.y0Um + kSurveyStepUm / 2; y < S.q.y1Um; y += kSurveyStepUm)
         for (double x = S.q.x0Um + kSurveyStepUm / 2; x < S.q.x1Um; x += kSurveyStepUm)
            for (double z : zs)
            {
               ++sp.n;
               if (shaped && !(ExciteAt(g.shares, x - S.q.originXUm, y - S.q.originYUm, (z - S.q.zRefUm) * 1000.0) > 0))
                  continue;
               double a = 0, b = 0;
               ClockPairAt(mp, st, false, x, y, z, a, b, bd.data());
               if (mc.mode == ISC_CLOCK_DOSE && !(b > 0))
                  continue;
               sp.a.push_back(a);
               sp.b.push_back(b);
               sp.bounds.insert(sp.bounds.end(), bd.begin(), bd.begin() + static_cast<long>(nb));
            }
      sp.key.swap(key);
   }
   double sumS = 0, sumSH = 0;
   const size_t lit = sp.a.size();
   for (size_t i = 0; i < lit; ++i)
   {
      double Sv = 0, h = 0;
      PopulationAt(mc, p.lam, sp.a[i], sp.b[i], nb ? &sp.bounds[i * nb] : nullptr, S.t0Sec, Sv, h);
      sumS += Sv;
      sumSH += Sv * h;
   }
   p.surveyed = true;
   p.nZLit = sp.n ? static_cast<long>(std::llround(static_cast<double>(p.nZ) * static_cast<double>(lit) /
                                                   static_cast<double>(sp.n)))
                  : 0;
   p.meanSurvival = lit ? sumS / static_cast<double>(lit) : 0.0;
   p.surveyRate = sumS > 0 ? sumSH / sumS : 0.0;
   TimingLog("fl.survey", TimingSince(tSurvey));
}

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
   p.mfBuilt = true;
   p.mf.assign(1, MeanFieldNode());
   MeanFieldNode& nd = p.mf[0];
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
   // With a map the dyes go to the scene's weighted (bleaching) channel, one
   // weight per entry (MapEntries); else unit weights. A blink population on a
   // clock the same everywhere (GlobalClock) renders unweighted: the image x
   // its photons per dye in the frame (FramePhotons; it follows a drift).
   p.weighted = mapped && !(p.state == ISC_STATE_BLINK && GlobalClock(p.structure));
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
      slot.anchored = false;
   }
   // AnchorMeanField: the scene at its anchor while the FOV is within kMeanFieldAnchorUm of it (else a new anchor
   // here), the images shifted by anchor - origin (the FOV over a sample moved by d: the image moves by d).
   nd.shifted = false;
   if (anchor)
   {
      const double ox = S.q.originXUm, oy = S.q.originYUm;
      if (!(slot.anchored && std::fabs(ox - slot.anchorX) <= kMeanFieldAnchorUm &&
            std::fabs(oy - slot.anchorY) <= kMeanFieldAnchorUm))
      {
         slot.anchored = true;
         slot.anchorX = ox;
         slot.anchorY = oy;
      }
      ws.originXUm = slot.anchorX;
      ws.originYUm = slot.anchorY;
      const double pitchUm = um / ws.grid.upscale;
      nd.shiftX = (slot.anchorX - ox) / pitchUm;
      nd.shiftY = (slot.anchorY - oy) / pitchUm;
      nd.shifted = nd.shiftX != 0.0 || nd.shiftY != 0.0;
   }
   // Drift (and the anchor) keep the spectra (focus work on the CPU: the accelerator returns images only).
   const bool shifts = S.driftOn || anchor;
   slot.scene.SetKeepSpectra(shifts);
   slot.scene.SetAccelerator(shifts ? nullptr : accel);
   // GPU mode: the images wait for the GPU host (SetMeanFieldImages), a weighted scene's those of its basis (below; not
   // a blink population's: its weights change every frame).
   nd.deferred = gpuMode && !shifts && !(p.weighted && p.state == ISC_STATE_BLINK);
   slot.scene.SetDeferImages(nd.deferred);
   // A shaped excitation: each dye plane's factor per cell.
   if (shaped)
   {
      auto pf = std::make_shared<ExcitePlaneFactor>();
      pf->ex = S.excite;
      pf->shares = g.shares;
      if (anchor)
      {
         // The beam stays with the FOV: its axis is the scene's + (anchor - origin), in 0.25 um steps (a step re-weights
         // the scene; exact, every frame would refocus it every frame).
         pf->offX = std::round((slot.anchorX - S.q.originXUm) / 0.25) * 0.25;
         pf->offY = std::round((slot.anchorY - S.q.originYUm) / 0.25) * 0.25;
      }
      // Not the focus (exciteKey's last entry: the factor reads world heights), nor the pivot (it follows the focus)
      // unless the beam is tilted: with them every focus step re-made the scene's weights and its bleach basis, 170 ms
      // per z step of a live WideField scrub (2026-10-14). A HILO sheet does move with the focus.
      pf->key.assign(exciteKey.begin(), exciteKey.end() - 1);
      if (!S.excite.Tilted())
         pf->key[kExciteKeyPivot] = 0.0;
      pf->key.push_back(pf->offX);
      pf->key.push_back(pf->offY);
      for (const ExcitationLine& l : g.shares)
      {
         pf->key.push_back(l.nm);
         pf->key.push_back(l.share);
      }
      slot.scene.SetPlaneFactor(pf);
   }
   else
      slot.scene.SetPlaneFactor(nullptr);
   const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
   const auto tPhase = TimingClock::now();
   if (!slot.scene.Update(cache.source, ill, ws, *slot.psf, err))
      return false;
   TimingLog("fl.mean-field-scene", TimingSince(tPhase));
   nd.slot = &slot;
   nd.ws = ws;
   if (p.weighted)
   {
      // A map: each weight entry's clock (MapEntries: per world tile and height band), frame 0's mean photons per dye
      // at it (wb0), a rate population's decay per entry (rates: frame f is wb0 exp(-rate f exposure),
      // RateWeightedFrame; the same everywhere: one decay).
      auto tW = TimingClock::now();
      const WidefieldGridSpec& gr = slot.scene.Grid();
      p.ent = MapEntriesFor(*map, p, gr, slot, S.t0Sec, std::max(kBasisSpanSec, S.N * S.expSec), S.expSec);
      const MapEntries& E = *p.ent;
      const MapClock& mc = map->s[static_cast<size_t>(p.structure)];
      slot.scene.SetWeightBands(E.edges);
      slot.scene.SetWeightMap(slot.cellMap);
      p.sceneZ = zStage;
      if (p.state == ISC_STATE_BLINK)
      {
         slot.scene.UsedWeights(p.used);
         BlinkWeights(p, 0, p.wb0);
         slot.scene.SetWeightRates(nullptr, 0.0);
      }
      else
      {
         const size_t n = E.Count();
         p.wb0.assign(n, 0.0f);
         double la = std::numeric_limits<double>::quiet_NaN(), lb = la;
         float lv = 0.0f;
         for (size_t i = 0; i < n; ++i)
         {
            const double* bd = E.Bounds(i);
            if (!(E.a[i] == la && E.b[i] == lb && (!bd || std::equal(bd, bd + E.nBounds, E.Bounds(i - 1)))))
            {
               lv = static_cast<float>(PopulationPhotons(mc, p.lam, p.rate, E.a[i], E.b[i], bd, S.t0Sec, S.expSec));
               la = E.a[i];
               lb = E.b[i];
            }
            p.wb0[i] = lv;
         }
         p.rates = E.rates;
         p.decay = E.decay0;
         // The basis holds kBasisSpanSec, or the movie: its Chebyshev terms in the rates.
         slot.scene.SetWeightRates(p.rates, std::max(kBasisSpanSec, S.N * S.expSec));
         // Several frames (or the GPU's images): the whole basis at once, anchored at frame 0 -- a frame it holds
         // renders from its images, one it does not anchors a new one --, unless the scene's holds frame 0 already (a
         // host's next movie at the same pose).
         p.anchored = false;
         if (S.N > 1 || nd.deferred)
         {
            p.anchored = slot.scene.AnchorDecay(p.wb0, p.anchor);
            std::vector<double> a;
            if (!p.anchored && !slot.scene.BleachCoefficients(p.wb0, a))
            {
               slot.scene.SetBleachWeights(p.wb0, false);
               p.anchored = slot.scene.AnchorDecay(p.wb0, p.anchor);
            }
         }
      }
      TimingLog("fl.mean-field-weights", TimingSince(tW));
      if (!nd.deferred)
      {
         tW = TimingClock::now();
         WeightedImage(p, nd.image);
         TimingLog("fl.mean-field-render", TimingSince(tW));
      }
   }
   else if (!gpuMode || anchor)
      TakeImage(p, 0);
   return true;
}

void FluorescenceMovie::Impl::WeightedImage(Population& p, std::vector<float>& img)
{
   img.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   const MeanFieldNode& nd = p.mf[0];
   if (nd.shifted)
      nd.slot->scene.RenderFrameShifted(p.wb0, nd.shiftX, nd.shiftY, img);
   else
      nd.slot->scene.RenderFrame(p.wb0, img);
}

double FluorescenceMovie::Impl::FramePhotons(const Population& p, long f) const
{
   if (p.state == ISC_STATE_BLINK)
      return BlinkPhotons(p.mode, p.rate, p.blinkSegs, p.blinkClock + p.blinkRate * (f * S.expSec), p.blinkRate,
                          S.expSec);
   const double tf0 = S.t0Sec + f * S.expSec;
   return MeanPhotons(p.rate, p.lambda, tf0, tf0 + S.expSec);
}

const std::vector<float>& FluorescenceMovie::Impl::ImageAt(Population& p, size_t j, double z)
{
   MeanFieldNode& nd = p.mf[j];
   if (z == zStage || !nd.slot)
      return nd.image;
   auto it = nd.imageAt.find(z);
   if (it != nd.imageAt.end())
      return it->second;
   WidefieldSceneSpec ws = nd.ws;
   ws.focusWorldUm = ws.slabCentreUm = S.q.zRefUm + z;
   const double um = S.p.pixelSizeNm / 1000.0;
   const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
   std::vector<float>& img = nd.imageAt[z];
   img.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   std::string err;
   nd.slot->scene.SetDeferImages(false);
   if (nd.slot->scene.Update(cache.source, ill, ws, *nd.slot->psf, err))
   {
      if (p.weighted)
         WeightedImage(p, img);
      else if (nd.shifted && nd.slot->scene.Images().HasSpectra())
         RenderShiftedImages(nd.slot->scene.Images(), nullptr, 0.0, {}, nd.shiftX, nd.shiftY, img);
      else
         nd.slot->scene.Images().Render({}, img);
   }
   return img;
}

void FluorescenceMovie::Impl::DriftMeanField(Population& p, size_t j, long f, double z, const DriftNm& d,
                                             std::vector<float>& img)
{
   MeanFieldNode& nd = p.mf[j];
   if (!nd.slot)
      return;
   WidefieldScene& scene = nd.slot->scene;
   // A weighted population's weights in frame f: frame 0's, with rates each entry f exposures on (without, the caller
   // scales the image by the common decay); the foci's images hold the basis anchored at driftAnchor: a frame it does
   // not hold anchors a new one (and new images).
   std::vector<float> wb;
   std::vector<double> coef;
   if (p.weighted)
   {
      wb = p.wb0;
      if (p.rates && p.rates->size() == wb.size() && f > 0)
      {
         const std::vector<float>& rate = *p.rates;
         const double k = f * S.expSec;
         for (size_t i = 0; i < wb.size(); ++i)
            if (wb[i] != 0.0f)
               wb[i] = static_cast<float>(p.wb0[i] * std::exp(-k * rate[i]));
      }
      if (!nd.driftAnchored || !scene.BleachCoefficients(wb, coef))
      {
         nd.driftAnchor = wb;
         nd.driftAnchored = true;
         nd.driftImages.clear();
      }
   }
   int k = 0;
   double w = 0.0;
   driftGrid.Weights(d.z, k, w);
   const double um = S.p.pixelSizeNm / 1000.0;
   auto imagesAt = [&](int kk) -> const WidefieldImages* {
      // The sample dz higher: the focal plane dz lower in it.
      const double focus = S.q.zRefUm + z - driftGrid.DzNm(kk) / 1000.0;
      auto it = nd.driftImages.find(focus);
      if (it != nd.driftImages.end())
         return &it->second;
      WidefieldSceneSpec ws = nd.ws;
      ws.focusWorldUm = ws.slabCentreUm = focus;
      const FlatIllumination ill(S.W * um + 2 * ws.marginUm, S.H * um + 2 * ws.marginUm);
      std::string err;
      scene.SetDeferImages(false);
      if (!scene.Update(cache.source, ill, ws, *nd.slot->psf, err))
         return nullptr;
      if (p.weighted)
         scene.SetBleachWeights(nd.driftAnchor, !p.rates);   // (every focus's basis alike: the anchor's)
      else if (!scene.Images().HasSpectra())
         scene.ComputeCpuImages();
      return &(nd.driftImages[focus] = scene.Images());
   };
   const WidefieldImages* i0 = imagesAt(k);
   const WidefieldImages* i1 = (w != 0.0 && k + 1 < driftGrid.n) ? imagesAt(k + 1) : nullptr;
   if (!i0)
      return;
   if (p.weighted && !scene.BleachCoefficients(wb, coef))
      coef = scene.AnchorCoefficients();
   const double pitchNm = scene.Grid().pitchUm * 1000.0;
   RenderShiftedImages(*i0, i1, i1 ? w : 0.0, coef, d.x / pitchNm, d.y / pitchNm, img);
}

void FluorescenceMovie::Impl::TakeImage(Population& p, size_t j)
{
   MeanFieldNode& nd = p.mf[j];
   nd.slot->scene.SetDeferImages(false);   // (its images are in: what follows is the CPU's)
   if (p.weighted)
   {
      WeightedImage(p, nd.image);
      return;
   }
   nd.image.assign(static_cast<size_t>(S.W) * S.H, 0.0f);
   if (nd.shifted && !nd.slot->scene.Images().HasSpectra())
      nd.slot->scene.ComputeCpuImages();   // (images made without spectra: again, with them)
   if (nd.shifted && nd.slot->scene.Images().HasSpectra())
      RenderShiftedImages(nd.slot->scene.Images(), nullptr, 0.0, {}, nd.shiftX, nd.shiftY, nd.image);
   else
      nd.slot->scene.Images().Render({}, nd.image);
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
         if (ok[i] && dyes[i]->excite * signs[i] != 0.0)
            SplatUnitInto(img, W, H, g.kernel, plans[i], dyes[i]->excite != 1.0 ? signs[i] * dyes[i]->excite : signs[i],
                          y0, y1);
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
         for (double v : exciteKey)
            bits(v);
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

namespace {
// One grid focus's background map, in two parts (photons per pixel per frame): lat = the flat background x the beam's
// footprint on the coverslip, im = the free imagers' collected columns.
struct BgSlice
{
   std::vector<double> lat, im;
};
// What a slice is made from (the focus is the grid's: pivotUm = k x kBackgroundFocusStepUm).
struct BgJob
{
   std::vector<double> key;   // everything but the focus, and then k
   ExcitationSpec es;
   unsigned W = 0, H = 0;
   double um = 0, axisX = 0, axisY = 0, H0 = 0, expSec = 0, backgroundPhotons = 0;
   std::vector<ImagerSource> sources;
};

BgSlice MakeBgSlice(const BgJob& j)
{
   const Excitation ex(j.es);
   const std::vector<double> imMap = ex.ImagerMap(j.W, j.H, j.um, j.axisX, j.axisY, j.H0, j.sources);
   BgSlice s;
   const size_t n = static_cast<size_t>(j.W) * j.H;
   s.lat.resize(n);
   s.im.resize(n);
   ParallelFor(j.H, [&](unsigned y) {
      for (unsigned x = 0; x < j.W; x++)
      {
         const double dx = x * j.um - j.axisX, dy = y * j.um - j.axisY;
         const size_t k = static_cast<size_t>(y) * j.W + x;
         double im = imMap[k];
         im *= j.expSec;
         s.im[k] = im;
         s.lat[k] = j.backgroundPhotons * ex.Lateral(dx, dy, 0.0);
      }
   });
   return s;
}

// The grid's slices, most recently used last, at most kSlices and kBytes; the slices in the making (the prefetch
// worker's or another movie's) are waited for, not made twice.
class BgMapCache
{
public:
   static BgMapCache& Get()
   {
      static BgMapCache c;
      return c;
   }
   ~BgMapCache() { Shutdown(); }

   std::shared_ptr<const BgSlice> Slice(const BgJob& j)
   {
      std::unique_lock<std::mutex> lk(m_);
      for (;;)
      {
         for (auto it = slices_.begin(); it != slices_.end(); ++it)
            if (it->first == j.key)
            {
               auto s = it->second;
               slices_.splice(slices_.end(), slices_, it);
               return s;
            }
         if (!InFlight(j.key))
            break;
         cv_.wait(lk);
      }
      // Not queued for the worker any more: this thread makes it.
      for (auto it = queue_.begin(); it != queue_.end(); ++it)
         if (it->key == j.key)
         {
            queue_.erase(it);
            break;
         }
      making_.push_back(j.key);
      lk.unlock();
      const auto t = TimingClock::now();
      auto s = std::make_shared<const BgSlice>(MakeBgSlice(j));
      TimingLog("fl.bg-map", TimingSince(t));
      lk.lock();
      Done(j.key, s);
      return s;
   }

   // Queue slices for the worker (the ones not made, queued or in the making); the queue keeps the latest few.
   void Prefetch(std::vector<BgJob> jobs)
   {
#ifndef __EMSCRIPTEN__
      std::lock_guard<std::mutex> lk(m_);
      if (!prefetch_ || stop_)
         return;
      for (BgJob& j : jobs)
      {
         bool have = InFlight(j.key);
         for (const auto& e : slices_)
            have = have || e.first == j.key;
         for (const BgJob& q : queue_)
            have = have || q.key == j.key;
         if (!have)
            queue_.push_back(std::move(j));
      }
      while (queue_.size() > kQueue)
         queue_.pop_front();
      if (!worker_.joinable() && !queue_.empty())
         worker_ = std::thread([this] { Work(); });
      cv_.notify_all();
#else
      (void)jobs;
#endif
   }

   void SetPrefetch(bool on)
   {
      {
         std::lock_guard<std::mutex> lk(m_);
         prefetch_ = on;
         if (on)
            stop_ = false;
         else
            queue_.clear();
      }
      cv_.notify_all();
   }

   void Shutdown()
   {
      {
         std::lock_guard<std::mutex> lk(m_);
         stop_ = true;
         prefetch_ = false;
         queue_.clear();
      }
      cv_.notify_all();
#ifndef __EMSCRIPTEN__
      if (worker_.joinable())
         worker_.join();
#endif
   }

private:
   static constexpr size_t kSlices = 12, kQueue = 4;
   static constexpr size_t kBytes = 192u << 20;

   bool InFlight(const std::vector<double>& key) const
   {
      for (const auto& k : making_)
         if (k == key)
            return true;
      return false;
   }
   // (under m_)
   void Done(const std::vector<double>& key, const std::shared_ptr<const BgSlice>& s)
   {
      for (auto it = making_.begin(); it != making_.end(); ++it)
         if (*it == key)
         {
            making_.erase(it);
            break;
         }
      slices_.emplace_back(key, s);
      size_t bytes = 0;
      for (const auto& e : slices_)
         bytes += (e.second->lat.size() + e.second->im.size()) * sizeof(double);
      while (slices_.size() > 1 && (slices_.size() > kSlices || bytes > kBytes))
      {
         bytes -= (slices_.front().second->lat.size() + slices_.front().second->im.size()) * sizeof(double);
         slices_.pop_front();
      }
      cv_.notify_all();
   }
#ifndef __EMSCRIPTEN__
   void Work()
   {
      std::unique_lock<std::mutex> lk(m_);
      for (;;)
      {
         cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
         if (stop_)
            return;
         BgJob j = std::move(queue_.back());   // the latest request first
         queue_.pop_back();
         making_.push_back(j.key);
         lk.unlock();
         const auto t = TimingClock::now();
         auto s = std::make_shared<const BgSlice>(MakeBgSlice(j));
         TimingLog("fl.bg-map-prefetch", TimingSince(t));
         lk.lock();
         Done(j.key, s);
      }
   }
#endif

   std::mutex m_;
   std::condition_variable cv_;
   std::list<std::pair<std::vector<double>, std::shared_ptr<const BgSlice>>> slices_;
   std::vector<std::vector<double>> making_;
   std::deque<BgJob> queue_;
#ifndef __EMSCRIPTEN__
   std::thread worker_;   // the prefetch (the WASM viewer has no threads)
#endif
   bool prefetch_ = false, stop_ = false;
};
} // namespace

void SetBackgroundMapPrefetch(bool on)
{
   BgMapCache::Get().SetPrefetch(on);
}

void BackgroundMapPrefetchShutdown()
{
   BgMapCache::Get().Shutdown();
}

bool FluorescenceMovie::Begin(const ScopeSpec& spec, bool gpuMode, std::string& err, WidefieldAccelerator* accel,
                              LightHistory* history, const LightState* light)
{
   Impl& m = *impl_;
   m.t0 = std::chrono::steady_clock::now();
   m.spec = spec;
   m.gpuMode = gpuMode;
   m.accel = accel;
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
   SampleSource& source = m.cache.source;
   TimingLog("fl.configure", TimingSince(tPhase));

   // ---- groups: (structure, state) with their PSF and detected photons per frame ----
   struct Want { int s; bool pre; const StatePhysics* st; };
   std::vector<Want> wanted;
   for (int s = 0; s < static_cast<int>(S.labels.size()); ++s)
      for (bool pre : { false, true })
      {
         if (!S.Mounted(s))
            continue;
         const StatePhysics& st = pre ? S.labels[static_cast<size_t>(s)].pre : S.labels[static_cast<size_t>(s)].main;
         if (st.emits && st.detectedFraction >= kMinDetectedFraction && st.detectedPerSec > 0)
            wanted.push_back({ s, pre, &st });
      }
   // The beads' components whose PSF no group's kernel serves need one of their own (at most).
   size_t ownKernels = 0;
   if (S.fidOn)
      for (const FiducialComponent& c : S.fidComps)
      {
         bool shared = false;
         for (const Want& w : wanted)
            shared = shared || std::fabs(KernelWavelengthNm(w.st->lambdaNm) - KernelWavelengthNm(c.lambdaNm)) <= kFiducialKernelReuseNm;
         ownKernels += shared ? 0 : 1;
      }
   SetPsfKernelMemoEntries(wanted.size() + 1 + ownKernels);
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
      for (const StatePhysics::LineShare& l : wanted[i].st->lineShares)
         g.shares.push_back({ l.nm, l.share });
   }
   auto groupOf = [&](int s, bool pre) -> int {
      for (size_t i = 0; i < m.groups.size(); ++i)
         if (m.groups[i].structure == s && m.groups[i].pre == pre)
            return static_cast<int>(i);
      return -1;
   };
   m.anyBlinks = false;
   for (int s : S.mounted)
      m.anyBlinks = m.anyBlinks || S.labels[static_cast<size_t>(s)].mode != MODE_WIDEFIELD;
   // The excitation geometry (Excitation.h; JS fluorescence.js): a dye
   // state's factor at its site in the FOV frame (the axis at the FOV centre),
   // none when it is 1 everywhere. The light-driven rates follow it too:
   //   dSTORM: its kinetic clock is its dose (the 405 nm return taken to fall
   //     off as the main line);
   //   PALM: its activation at spont + lin a + quad a^2, its blinks after it on
   //     the sample's time;
   //   DNA-PAINT: binding needs no light; bleaching by a photon budget
   //     (WideField, the PALM pre state): at a x the rate.
   // Every dye reads its clock from a clock map (spec/ALGORITHM.md "Exact light
   // history"): a host's (its light history and this movie's light) or, under a
   // shaped beam without one, the movie's own (the past start-sec under the
   // movie's light, then the movie: one record); the mean field per weight
   // entry of either (MapEntries). Neither: start-sec everywhere.
   m.shaped = !S.excite.Uniform();
   const bool hostMap = history != nullptr && light != nullptr;
   m.axisX = (S.W - 1) / 2.0 * um;
   m.axisY = (S.H - 1) / 2.0 * um;
   {
      const ExcitationSpec es = ScopeExcitationSpec(spec);
      // (The pivot at kExciteKeyPivot, the focus last: the plane factor's key leaves the focus out.)
      m.exciteKey = { static_cast<double>(es.profile), es.fwhmPct, es.fovWidthUm, es.bfp, es.na, es.nImm, es.nMed,
                      es.pivotUm, m.axisX, m.axisY, S.q.zRefUm };
   }
   m.map.reset();
   if (hostMap || m.shaped)
   {
      tPhase = TimingClock::now();
      const double zLo = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm - S.q.zHalfRangeUm : -1e3;
      const double zHi = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm + S.q.zHalfRangeUm : 1e3;
      const double g = kMapMarginUm, x0 = S.q.x0Um - g, y0 = S.q.y0Um - g, x1 = S.q.x1Um + g, y1 = S.q.y1Um + g;
      if (hostMap)
         m.map = history->BuildMap(spec, *light, x0, y0, x1, y1, zLo, zHi, err);
      else
      {
         LightState own;
         if (!MakeLightState(spec, true, own, err))
            return false;
         GrowFieldStop(own, x0, y0, x1, y1);
         LightHistory past;
         past.Advance(S.t0Sec, own);
         m.map = past.BuildMap(spec, own, x0, y0, x1, y1, zLo, zHi, err);
      }
      if (!m.map)
         return false;
      TimingLog("fl.clock-map", TimingSince(tPhase), std::to_string(m.map->buffer.size()).c_str());
   }
   m.mapped = m.map != nullptr;

   // ---- blink mean-field frames (spec/ALGORITHM.md "Blink render regimes") ----
   // Decided before the events query from the expected ON emitters on the
   // reference clock: a frame that renders mean-field draws no events, and a
   // movie mean-field throughout queries none. Not for a map's light-driven
   // clock under drift (its weighted scene does not follow the drift; a GLOBAL
   // clock, the same everywhere, renders unweighted).
   m.fovArea = S.W * S.H * um * um;
   bool queryBlinks = false;
   const bool blinkMfPossible = S.blinkMeanFieldDensityPerUm2 < 1e9 || S.blinkMeanFieldMaxEmitters < 1e9;
   for (Group& g : m.groups)
   {
      const LabelPhysics& L = S.labels[static_cast<size_t>(g.structure)];
      if (g.pre || L.mode == MODE_WIDEFIELD)
         continue;
      g.mode = L.mode;
      if (blinkMfPossible && !(S.driftOn && !m.GlobalClock(g.structure)))
      {
         const double inf = std::numeric_limits<double>::infinity();
         const double focus = S.q.zCullCentreUm, slabHalf = S.meanFieldSlabNm / 2000;
         const double zMin = S.q.zHalfRangeUm > 0 ? focus - S.q.zHalfRangeUm : -inf;
         const double zMax = S.q.zHalfRangeUm > 0 ? focus + S.q.zHalfRangeUm : inf;
         const double ox = S.q.originXUm, oy = S.q.originYUm;
         const double sLo = std::max(zMin, focus - slabHalf), sHi = std::min(zMax, focus + slabHalf);
         g.nZ = m.cache.CountSlab(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, 1 << g.structure);
         g.nSlab = sHi > sLo ? m.cache.CountSlab(ox, oy, ox + S.W * um, oy + S.H * um, sLo, sHi, 1 << g.structure) : 0;
         if (g.nZ < 0 || g.nSlab < 0)
         {
            err = "cell-field dye count failed";
            return false;
         }
         g.refClock = S.t0Sec;
         g.refRate = 1.0;
         if (m.mapped)
         {
            // At the FOV centre and the focal plane (the slab the switch counts).
            const MapClock& mc = m.map->s[static_cast<size_t>(g.structure)];
            std::vector<double> bd(mc.bound.size());
            double a = 0, b = 0;
            ClockPairAt(*m.map, g.structure, true, S.q.originXUm + (S.W - 1) * um / 2,
                        S.q.originYUm + (S.H - 1) * um / 2, focus, a, b, bd.data());
            BlinkClockAt(mc, a, b, bd.data(), S.t0Sec, g.refClock, g.refRate, g.refSegs);
         }
         else
            m.LabelSegments(g.structure, g.refSegs);
         g.mfFrame.assign(static_cast<size_t>(S.N), 0);
         g.allMf = g.nZ > 0;
         for (long f = 0; f < S.N; ++f)
         {
            const double t = g.refClock + g.refRate * (f * S.expSec);
            const double on = BlinkPhotons(L.mode, 1.0, g.refSegs, t, g.refRate, S.expSec) / S.expSec;
            const bool mf = g.nZ > 0 && (g.nSlab * on / m.fovArea > S.blinkMeanFieldDensityPerUm2 ||
                                         g.nZ * on > S.blinkMeanFieldMaxEmitters);
            g.mfFrame[static_cast<size_t>(f)] = mf;
            g.anyMf = g.anyMf || mf;
            g.allMf = g.allMf && mf;
         }
      }
      queryBlinks = queryBlinks || !g.allMf;
   }

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
   // The blinks under a clock map (JS mapEvents): every dye on its own clock (isc_events_in_map; a dye the light does
   // not reach emits nothing), each event's photons x its factor under a shaped beam. Without one: at start-sec.
   auto mapEvents = [&]() -> bool {
      std::vector<BlinkEvent> got;
      const bool ok = source.SetClockMap(&m.map->buffer) && source.EventsInMap(S.q, got);
      source.SetClockMap(nullptr);
      if (!ok)
         return false;
      for (BlinkEvent& e : got)
      {
         const int gi = groupOf(e.structure, false);
         if (gi < 0)
            continue;
         if (m.shaped)
            e.excite = m.ExciteAt(m.groups[static_cast<size_t>(gi)].shares, e.xUm, e.yUm, e.zNm);
         all.push_back(e);
      }
      return true;
   };
   auto queryEvents = [&]() { return m.map ? mapEvents() : source.Events(S.q, all); };
   tPhase = TimingClock::now();
#if defined(__EMSCRIPTEN__)
   kernels();
   const auto tEv = TimingClock::now();
   if (psfOk && queryBlinks)
      eventsOk = queryEvents();
   eventsSec = TimingSince(tEv);
#else
   std::thread psfThread(kernels);
   if (queryBlinks)
      eventsOk = queryEvents();
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
   // The fiducial beads (JS fiducialBeads): after the dye groups' kernels, so a bead component can share them.
   m.fid.reset();
   if (S.fidOn)
   {
      const auto tFid = TimingClock::now();
      std::vector<double> rows;
      if (!source.Fiducials(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, rows))
      {
         err = "fiducial query failed";
         return false;
      }
      const double inf = std::numeric_limits<double>::infinity();
      const double zMin = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm - S.q.zHalfRangeUm : -inf;
      const double zMax = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm + S.q.zHalfRangeUm : inf;
      std::unique_ptr<FiducialMovie> FM(new FiducialMovie());
      FM->type = S.fidType;
      FM->expSec = S.expSec;
      FM->N = S.N;
      std::vector<const double*> sites;
      for (size_t i = 0; i + ISC_FIDUCIAL_STRIDE <= rows.size(); i += ISC_FIDUCIAL_STRIDE)
         if (rows[i + 5] >= zMin && rows[i + 5] < zMax)
            sites.push_back(&rows[i]);
      FM->sites = sites.size();
      // When each bead's decay starts: the movie's start-sec, or under a host's light history the bead's past light
      // as seconds of the light now (its dose there / the dose rate the light gives there; exact while the light's
      // spectrum stays the same, ample for hours of half-time), plus the frames between the history's now and the
      // movie's start-sec (a render-ahead batch after queued frames: the map's clocks run on to it as well).
      std::vector<double> t0s(sites.size(), S.t0Sec);
      if (history && light)
      {
         const double ahead = std::max(0.0, S.t0Sec - history->T());
         for (size_t i = 0; i < sites.size(); ++i)
         {
            const double rate = history->DoseRate(*light, sites[i][3], sites[i][4]);
            t0s[i] = rate > 0 ? history->DoseAt(sites[i][3], sites[i][4]) / rate + ahead : 0.0;
         }
      }
      for (const FiducialComponent& c : S.fidComps)
      {
         FiducialMovieComponent F;
         double maxRate = 0.0;
         for (size_t i = 0; i < sites.size(); ++i)
         {
            const double* b = sites[i];
            const double xUm = b[3] - S.q.originXUm, yUm = b[4] - S.q.originYUm, zNm = (b[5] - S.q.zRefUm) * 1000.0;
            const double a = m.shaped ? m.ExciteAt(c.lineShares, xUm, yUm, zNm) : 1.0, rate = BeadRatePerSec(c, b[6]) * a;
            if (!(rate > 0))
               continue;
            F.beads.push_back({ xUm, yUm, zNm, b[6], rate, c.bleachPerSec * a, t0s[i] });
            maxRate = std::max(maxRate, rate);
         }
         if (F.beads.empty() || maxRate * S.expSec < kFiducialMinPhotons)
            continue;
         F.c = c;
         F.lambdaNm = KernelWavelengthNm(c.lambdaNm);
         double best = inf;
         for (const Group& g : m.groups)
         {
            const double dl = std::fabs(g.lambdaNm - F.lambdaNm);
            if (g.kernel.valid && dl <= kFiducialKernelReuseNm && dl < best)
            {
               best = dl;
               F.kernel = g.kernel;
            }
         }
         if (!F.kernel.valid && !ScopePsfKernel(spec, F.lambdaNm, F.kernel, psfErr))
         {
            err = psfErr;
            return false;
         }
         F.sigmaPx = GaussianSigmaPx(F.lambdaNm, ScopeSpecGet(spec, "na"), pixelNm);
         if (F.kernel.valid)
         {
            F.B = BeadKernelFor(F.kernel, pixelNm);
            // The halo: the point PSF at the component's wavelength over a window twice the FOV, one sample per pixel.
            PsfGeneratorRequest hr;
            if (!ScopePsfRequest(spec, F.lambdaNm, hr, psfErr))
            {
               err = psfErr;
               return false;
            }
            hr.oversampling = 1;
            hr.kernelHalfWidthPx = BeadHaloHalfWidthPx(S.W, S.H, pixelNm,
                                                       F.kernel.halfWidthOversampled / std::max(1, F.kernel.oversampling));
            const auto tHalo = TimingClock::now();
            if (!BeadHaloFor(hr, F.halo, psfErr))
            {
               err = psfErr;
               return false;
            }
            TimingLog("fl.fiducials-halo", TimingSince(tHalo));
         }
         FM->comps.push_back(std::move(F));
      }
      FM->sec = TimingSince(tFid);
      TimingLog("fl.fiducials-setup", FM->sec);
      m.fid = std::move(FM);
   }
   // The binned blink regime (its kernel spectra are made on first use).
   const bool binnedPossible = S.blinkBinnedDensityPerUm2 < 1e9 || S.blinkBinnedMaxEmitters < 1e9;
   for (Group& g : m.groups)
      if (!g.pre && binnedPossible)
         g.binned.Setup(g.blinkKernel.valid ? &g.blinkKernel : nullptr, g.sigmaPx, S.W, S.H, S.blinkBinnedUpscale);
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
      if (!S.Mounted(s))
         continue;
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
      const long nZ = m.cache.CountSlab(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, 1 << s);
      const double sLo = std::max(zMin, focus - slabHalf), sHi = std::min(zMax, focus + slabHalf);
      const long nSlab = sHi > sLo ? m.cache.CountSlab(ox, oy, ox + S.W * um, oy + S.H * um, sLo, sHi, 1 << s) : 0;
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
         // A map: each epoch's decay rate, and the share of the dyes the light reaches, their survival.
         p.nZLit = nZ;
         if (m.mapped)
         {
            p.lam = EpochLambdas(m.map->s[static_cast<size_t>(s)], S.labels, s, state);
            m.Survey(p, zMin, zMax);
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
   std::vector<double> windowClock;   // (a map) each window's dye's clock and rate
   bool haveWindows = false;
   for (Population& p : m.pops)
   {
      p.meanFieldAt0 = m.IsMeanField(p, 0);
      p.needWindows = !m.IsMeanField(p, S.N - 1);
      // A host's map where no dye is likely still in the state (Survey: the initial ON's ends, long gone after a few
      // seconds of light): no windows, no query.
      if (p.needWindows && p.surveyed && static_cast<double>(p.nZLit) * p.meanSurvival < kWindowAliveMin)
         p.needWindows = false;
   }
   for (Population& p : m.pops)
   {
      if (!p.needWindows)
         continue;   // mean-field to the end (or no dye left): no windows needed
      if (m.map)
      {
         // The map's windows in T (the initial ON ends on its dye's dose, the pre state at its activation; a DOSE dye
         // the light does not reach is frozen: dark), a photon budget's end where the dye's bleach hazard reaches its
         // draw (the core's, from the map's bleach fields).
         if (!haveWindows)
         {
            const auto tq = TimingClock::now();
            const bool ok = source.SetClockMap(&m.map->buffer) && source.ContinuousInMap(S.q, windows, &windowClock);
            source.SetClockMap(nullptr);
            TimingLog("fl.windows-query", TimingSince(tq), std::to_string(windows.size()).c_str());
            if (!ok)
            {
               err = "cell-field window query failed";
               return false;
            }
            haveWindows = true;
         }
         const Group& g = m.groups[p.group];
         const bool dose = m.map->s[static_cast<size_t>(p.structure)].mode == ISC_CLOCK_DOSE;
         for (size_t wi = 0; wi < windows.size(); ++wi)
         {
            const BlinkEvent& e = windows[wi];
            if (e.structure != p.structure || e.state != p.state)
               continue;
            if (dose && !(windowClock[2 * wi + 1] > 0))
               continue;   // frozen: no light
            const double a = m.shaped ? m.ExciteAt(g.shares, e.xUm, e.yUm, e.zNm) : 1.0;
            if (!(a > 0))
               continue;
            if (e.tEnd > 0)
            {
               BlinkEvent w = e;
               w.brightness = 1;
               w.excite = a;
               p.wins.push_back(w);
            }
         }
         continue;
      }
      if (!haveWindows)
      {
         if (!source.Continuous(S.q, windows))
         {
            err = "cell-field window query failed";
            return false;
         }
         haveWindows = true;
      }
      for (const BlinkEvent& e : windows)
      {
         if (e.structure != p.structure || e.state != p.state)
            continue;
         const double tq = S.q.tSec;
         double tEndSec = (e.tEnd - S.q.frameIndex) * S.expSec + tq;   // back to seconds (Continuous mapped it)
         if (p.budget > 0)
            tEndSec = std::min(tEndSec, e.aux * p.budget / p.emissionPerSec);   // the photon budget used up
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
   // The blink populations of the groups with mean-field frames.
   for (size_t gi = 0; gi < m.groups.size(); ++gi)
   {
      const Group& g = m.groups[gi];
      if (!g.anyMf)
         continue;
      Population p;
      p.structure = g.structure;
      p.state = ISC_STATE_BLINK;
      p.group = gi;
      p.rate = g.detectedPerSec;
      p.nZ = g.nZ;
      p.nZLit = g.nZ;
      p.nSlab = g.nSlab;
      p.mode = g.mode;
      p.blinkSegs = g.refSegs;
      p.blinkClock = g.refClock;
      p.blinkRate = g.refRate;
      p.meanFieldAt0 = true;
      m.pops.push_back(std::move(p));
   }
   // The mean-field images of the populations that start mean-field.
   m.sceneQueue.clear();
   for (Population& p : m.pops)
      if (p.meanFieldAt0)
      {
         if (!m.BuildMeanField(p, err))
            return false;
         // (A weighted scene's images wait for the GPU only when its basis was made for it: deferred.)
         if (gpuMode)
            for (size_t j = 0; j < p.mf.size(); ++j)
               if (!p.weighted || p.mf[j].deferred)
                  m.sceneQueue.push_back({ &p, j });
      }
   m.imagerPerFrame = 0;
   for (size_t s = 0; s < S.labels.size(); ++s)
      if (S.Mounted(static_cast<int>(s)))
         m.imagerPerFrame = m.imagerPerFrame + S.labels[s].ImagerBackgroundPerPxPerSec(um);
   m.imagerPerFrame *= S.expSec;
   m.bg = S.p.backgroundPhotons + m.imagerPerFrame;
   // A shaped excitation: the background per pixel -- the flat background x
   // the beam's footprint on the coverslip (no field factor; it includes
   // stray light), plus each DNA-PAINT label's free imager x the excitation's
   // column integral over the chamber (epi: its height; HILO: the sheet;
   // TIRF: ~ the decay depth).
   m.bgMap.clear();
   m.imagerMean = m.imagerPerFrame;
   if (m.shaped)
   {
      m.bgMap.resize(static_cast<size_t>(S.W) * S.H);
      const double H0 = S.lp.chamberHeightUm;
      std::vector<const LabelPhysics*> imagers;
      for (size_t s = 0; s < S.labels.size(); ++s)
         if (S.Mounted(static_cast<int>(s)) && S.labels[s].ImagerPerPxPerUmPerSec(um) > 0)
            imagers.push_back(&S.labels[s]);
      std::vector<ExcitationShares> imagerShares;
      for (const LabelPhysics* L : imagers)
      {
         ExcitationShares sh;
         for (const StatePhysics::LineShare& l : L->main.lineShares)
            sh.push_back({ l.nm, l.share });
         imagerShares.push_back(sh);
      }
      // The map at the grid's foci around this one (kBackgroundFocusStepUm apart; on a grid focus: that one alone),
      // blended linearly: a focus move reuses the grid's maps (and the prefetched neighbours) instead of making a
      // new map (~0.2 s at 256 px). The blend is exact on the grid; in between it is the linear interpolation of
      // smooth maps (the defocused imager; HILO's sheet moving tan theta_s um per um of focus).
      BgJob job;
      job.es = ScopeExcitationSpec(spec);
      job.W = S.W;
      job.H = S.H;
      job.um = um;
      job.axisX = m.axisX;
      job.axisY = m.axisY;
      job.H0 = H0;
      job.expSec = S.expSec;
      job.backgroundPhotons = S.p.backgroundPhotons;
      for (size_t i = 0; i < imagers.size(); ++i)
         job.sources.push_back({ imagers[i]->ImagerPerPxPerUmPerSec(um), imagerShares[i] });
      const ExcitationSpec& es = job.es;
      job.key = { static_cast<double>(es.profile), es.fwhmPct, es.fovWidthUm, es.bfp, es.na, es.nImm, es.nMed,
                  m.axisX, m.axisY, static_cast<double>(S.W), static_cast<double>(S.H), um, H0, S.expSec,
                  S.p.backgroundPhotons };
      for (const ImagerSource& src : job.sources)
      {
         job.key.push_back(src.rate);
         for (const ExcitationLine& l : src.shares)
         {
            job.key.push_back(l.nm);
            job.key.push_back(l.share);
         }
      }
      const double step = kBackgroundFocusStepUm, pivot = es.pivotUm;
      const double k0 = std::floor(pivot / step), t = (pivot - k0 * step) / step;
      auto at = [&](double k) {
         BgJob jk = job;
         jk.es.pivotUm = k * step;
         jk.key.push_back(k);
         return jk;
      };
      const std::shared_ptr<const BgSlice> A = BgMapCache::Get().Slice(at(k0));
      const std::shared_ptr<const BgSlice> B = t > 0.0 ? BgMapCache::Get().Slice(at(k0 + 1)) : nullptr;
      BgMapCache::Get().Prefetch({ at(k0 + 1), at(k0 - 1), at(k0 + 2) });
      const size_t n = static_cast<size_t>(S.W) * S.H;
      double sum = 0.0;
      for (size_t k = 0; k < n; ++k)
      {
         const double im = B ? (1.0 - t) * A->im[k] + t * B->im[k] : A->im[k];
         const double lat = B ? (1.0 - t) * A->lat[k] + t * B->lat[k] : A->lat[k];
         sum += im;
         m.bgMap[k] = static_cast<float>(lat + im);
      }
      m.imagerMean = sum / (S.W * S.H);
   }
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
   const auto& q = impl_->sceneQueue[static_cast<size_t>(i)];
   return q.first->mf[q.second].slot->scene;
}

bool FluorescenceMovie::SetMeanFieldImages(int i, std::vector<std::vector<float>>& images)
{
   Impl& m = *impl_;
   if (i < 0 || i >= static_cast<int>(m.sceneQueue.size()))
      return false;
   Population& p = *m.sceneQueue[static_cast<size_t>(i)].first;
   const size_t j = m.sceneQueue[static_cast<size_t>(i)].second;
   if (!p.mf[j].slot->scene.SetImages(images))
      return false;
   m.TakeImage(p, j);
   return true;
}

void FluorescenceMovie::ComputeCpuImages()
{
   Impl& m = *impl_;
   for (const auto& q : m.sceneQueue)
   {
      MeanFieldNode& nd = q.first->mf[q.second];
      if (!nd.image.empty())
         continue;
      nd.slot->scene.ComputeCpuImages();
      m.TakeImage(*q.first, q.second);
   }
}

FluorescenceSimplePlan FluorescenceMovie::SimplePlan() const
{
   const Impl& m = *impl_;
   FluorescenceSimplePlan sp;
   // The blink groups: the main groups of the blinking labels (a WideField
   // label's main group only lights its population), in Render's order.
   size_t blinks = 0;
   sp.populations = !m.pops.empty() || (m.fid && !m.fid->comps.empty());   // the beads come in that image too
   for (size_t gi = 0; gi < m.groups.size(); ++gi)
   {
      const Group& g = m.groups[gi];
      if (g.pre || m.S.labels[static_cast<size_t>(g.structure)].mode == MODE_WIDEFIELD)
         continue;
      FluorescenceSimpleGroup sg;
      sg.kernel = g.blinkKernel.valid ? &g.blinkKernel : nullptr;
      sg.photonsPerBlink = g.perFrame;
      sg.minPhotons = m.S.blinkMinPhotons;
      sg.sigmaPx = g.sigmaPx;
      sg.events = &m.byGroup[gi];
      // Frames whose blinks are binned or mean-field come from Render too.
      sg.splatFrame.assign(static_cast<size_t>(std::max(0L, m.S.N)), 1);
      for (long f = 0; f < m.S.N; ++f)
         if (!m.SplatFrame(gi, f))
         {
            sg.splatFrame[static_cast<size_t>(f)] = 0;
            sp.populations = true;
         }
      blinks += m.byGroup[gi].size();
      sp.groups.push_back(std::move(sg));
   }
   if (sp.groups.empty() || blinks != m.events.size())
      return FluorescenceSimplePlan();   // no blinks, blinks of another group (none should be)
   sp.ok = true;
   sp.backgroundPhotons = m.bg;
   sp.backgroundMap = m.bgMap.empty() ? nullptr : &m.bgMap;
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
      if (p.meanFieldAt0)
         for (size_t j = 0; j < p.mf.size(); ++j)
            if (p.mf[j].image.empty())
            {
               p.mf[j].slot->scene.ComputeCpuImages();   // GPU mode with no images handed over
               m.TakeImage(p, j);
            }
   (void)err;
   info.width = W;
   info.height = H;
   info.frames = N;
   info.blinks = m.events.size();
   info.dyes = 0;
   for (const Population& p : m.pops)
      if (p.state != ISC_STATE_BLINK)
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
         if (!S.Mounted(static_cast<int>(s)))
            continue;
         const LabelPhysics& L = S.labels[s];
         std::snprintf(b, sizeof b, " label%zu=%s:%s:%.4g%%:kact=%.4g", s, L.off ? "None" : L.eff.dye.id, DyeModeNames()[static_cast<size_t>(L.mode)].c_str(),
                       100 * L.label[ISC_LABEL_DENSITY], L.kActPerSec);
         d += b;
      }
      for (const Group& g : m.groups)
      {
         std::snprintf(b, sizeof b, " group=%d:%s:%gnm:%.4g_photons_per_frame:%.4g_detected", g.structure,
                       g.pre ? "pre" : "main", g.lambdaNm, g.perFrame, g.detectedFraction);
         d += b;
      }
      std::snprintf(b, sizeof b, " illum=%s:bfp=%.4g:F=%.4g:depth_nm=%.4g",
                    IlluminationRegimeNames()[static_cast<size_t>(S.excite.Regime())].c_str(), S.excite.U(),
                    S.excite.F(), S.excite.DepthNm(!m.groups.empty() && !m.groups[0].shares.empty() ? m.groups[0].shares[0].nm : 640));
      d += b;
      std::snprintf(b, sizeof b, " imager_per_px_per_frame=%.4g psf=%s", m.imagerMean,
                    !m.groups.empty() && m.groups[0].kernel.valid ? "GibsonLanniZernike" : "Gaussian");
      d += b;
      if (m.fid)
      {
         std::snprintf(b, sizeof b, " fiducials=%s:%zu_beads", FiducialTypeIds()[static_cast<size_t>(m.fid->type)].c_str(),
                       m.fid->sites);
         d += b;
         for (const FiducialMovieComponent& F : m.fid->comps)
         {
            double mean = 0;
            for (const FiducialBeadSite& e : F.beads)
               mean += e.rate / F.beads.size();
            std::snprintf(b, sizeof b, ":%s:%gnm:%.4g_photons_per_sec", F.c.id.c_str(), F.lambdaNm, mean);
            d += b;
         }
      }
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
   double tFid = 0;   // the beads' part of tPops
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
   std::vector<char> frameBinned(slots), frameMf(slots);
   long blinks = 0, binnedFrames = 0;
   RenderExtras into;
   into.accumulate = true;
   // The continuous populations' per-dye splats run frame by frame: in bands
   // of rows on all cores (the same pixels as serial, RenderPhotonImage).
   into.parallel = true;
   RenderExtras blinkInto = into;   // the blinks also see the host's illumination field
   blinkInto.minPhotons = m.S.blinkMinPhotons;   // and skip the dim ones (the continuous dyes keep every one)
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
         frameBinned[k] = 0;
         frameMf[k] = 0;
         const double bgScale = opt.backgroundScale ? opt.backgroundScale(f) : 1.0;
         if (popsOnly)
            photons[k].assign(n, 0.0f);   // the host splats the background and the SMLM frames' blinks
         else if (!m.bgMap.empty() && !shaped && bgScale == 1.0)
            photons[k] = m.bgMap;   // the excitation's background map
         else if (!shaped && bgScale == 1.0)
            photons[k].assign(n, static_cast<float>(m.bg));
         else
         {
            // The adapter's shaped background: x the illumination field, x the fade.
            photons[k].resize(n);
            for (size_t i = 0; i < n; ++i)
            {
               double b = m.bgMap.empty() ? m.bg : m.bgMap[i];
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
            if (!g.mfFrame.empty() && g.mfFrame[static_cast<size_t>(f)])
            {
               frameMf[k] = 1;   // the blink population draws it
               continue;
            }
            const bool binnedFrame = m.BinnedFrame(gi, f);
            if (popsOnly && !binnedFrame)
               continue;   // the host splats them (FluorescenceSimpleGroup::splatFrame)
            fe[k].clear();
            for (uint32_t i : m.buckets[gi][static_cast<size_t>(f)])
               fe[k].push_back(m.byGroup[gi][i]);
            frameBlinks[k] += static_cast<long>(fe[k].size());
            if (binnedFrame)
            {
               std::vector<FrameEmitter> ems;
               CollectFrameEmitters(fe[k], f, W, H, pixelNm, g.perFrame, dx, dy,
                                    g.blinkKernel.valid ? &g.blinkKernel : nullptr, zf, blinkInto.illumField, ems);
               g.binned.Render(ems, photons[k]);
               frameBinned[k] = 1;
               continue;
            }
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
            // The unweighted image (a drifting sample: its images at the frame's drift), or nullptr.
            std::vector<float> driftImage;
            auto unweighted = [&]() -> const std::vector<float>* {
               if (p.mf.empty())
                  return nullptr;
               if (!drift)
                  return &m.ImageAt(p, 0, zf);
               driftImage.assign(n, 0.0f);
               m.DriftMeanField(p, 0, f, zBaseAt(f), S.drift[static_cast<size_t>(f)], driftImage);
               return &driftImage;
            };
            if (p.state == ISC_STATE_BLINK)
            {
               if (!g.mfFrame[static_cast<size_t>(f)])
                  continue;   // drawn from its events (splat or binned)
               if (p.weighted)
                  m.BlinkWeightedFrame(p, f, zf, img);
               else if (const std::vector<float>* image = unweighted())
               {
                  const float mp = static_cast<float>(m.FramePhotons(p, f));
                  for (size_t i = 0; i < n; ++i)
                     img[i] += static_cast<float>(static_cast<double>(mp) * (*image)[i]);
               }
               p.meanFieldFrames++;
               paths[static_cast<size_t>(k)].push_back("blinks: mean-field (FFT)");
               continue;
            }
            if (m.IsMeanField(p, f))
            {
               if (!p.mfBuilt)
               {
                  // A population that turns mean-field later: never (they only
                  // decay), but keep the frame valid.
                  continue;
               }
               // (A weighted population with rates: each tile at its light, RateWeightedFrame; under drift in the
               // drift's weights.)
               if (p.weighted && p.rates && !drift)
                  m.RateWeightedFrame(p, f, zf, img);
               else if (const std::vector<float>* image = unweighted())
               {
                  if (p.weighted)
                  {
                     // Frame 0 at each entry's clock, every one f exposures later (its decay the same everywhere;
                     // with rates already in the image's weights).
                     const double sc = !p.rates && p.decay > 0 ? std::exp(-p.decay * f * S.expSec) : 1.0;
                     for (size_t i = 0; i < n; ++i)
                        img[i] += static_cast<float>(sc * (*image)[i]);
                  }
                  else
                  {
                     const float mp = static_cast<float>(m.FramePhotons(p, f));
                     for (size_t i = 0; i < n; ++i)
                        img[i] += static_cast<float>(static_cast<double>(mp) * (*image)[i]);
                  }
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
         // The fiducial beads, last (a host splatting the blinks itself still gets them in this image).
         if (m.fid && !m.fid->comps.empty())
         {
            const auto tb = TimingClock::now();
            double ddx = 0.0, ddy = 0.0;
            if (opt.driftPx)
               opt.driftPx(f, ddx, ddy);
            else if (drift)
            {
               ddx = S.drift[static_cast<size_t>(f)].x / pixelNm;
               ddy = S.drift[static_cast<size_t>(f)].y / pixelNm;
            }
            RenderFiducials(img, W, H, *m.fid, f, zf, pixelNm, !drift && !opt.driftPx, ddx, ddy);
            const double sec = TimingSince(tb);
            m.fid->sec += sec;
            tFid += sec;
            std::string path = "fiducials: ";
            for (size_t c = 0; c < m.fid->comps.size(); ++c)
               path += (c ? ", " : "") + std::to_string(m.fid->comps[c].beads.size()) + " " + m.fid->comps[c].c.id;
            paths[static_cast<size_t>(k)].push_back(path);
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
         binnedFrames += frameBinned[static_cast<size_t>(k)];
         more = opt.onPhotons ? opt.onPhotons(f0 + k, photons[static_cast<size_t>(k)])
                              : onFrame(f0 + k, adu[static_cast<size_t>(k)]);
         if (progress && *progress)
         {
            // Which backend drew this frame: the SMLM splat for blinks, mean-field or per dye per population.
            std::string j = "{\"frame\":" + std::to_string(f0 + k) + ",\"frames\":" + std::to_string(N) +
                            ",\"blinks\":" + std::to_string(frameBlinks[static_cast<size_t>(k)]) + ",\"backends\":[";
            bool first = true;
            if (m.anyBlinks && !(frameMf[static_cast<size_t>(k)] && !frameBlinks[static_cast<size_t>(k)]))
            {
               j += "\"SMLM: " + std::to_string(frameBlinks[static_cast<size_t>(k)]) +
                    (frameBinned[static_cast<size_t>(k)] ? " blinks (binned FFT)\"" : " blinks (splat)\"");
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
   if (m.fid)
      TimingLog("fl.fiducials", tFid, (std::to_string(m.fid->sites) + " beads").c_str());
   if (TimingEnabled())
   {
      char b[160];
      std::snprintf(b, sizeof b, "blinks %ld, binned frames %ld, groups %zu, populations %zu, ParallelFor spawns %lu",
                    blinks, binnedFrames, m.groups.size(), m.pops.size(), ParallelForSpawns().load() - spawns0);
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
                 "absorption_per_um=%g photons_per_px=%.4g drift_xy=%g drift_z=%g",   // (no times: the same file every run)
                 S.seed, S.cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, S.expSec * 1000, N, focusUm, bs.quality,
                 scene.Sources(), q.upscale, q.sub, scene.Slices(), scene.GridNx(), scene.GridNy(), bs.na,
                 bs.condenserNa, bs.wavelengthNm, bs.nMedium, bs.nCytoplasm, bs.nNucleus, bs.nMicrotubule,
                 bs.absorptionPerUm, flux, p.drift.xyNmPerSqrtSec, p.drift.zNmPerSqrtSec);
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
   SampleSource source;
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
   json += "{\"specimen\":" + std::to_string(ScopeSpecimen(spec)) + ",\"x0\":";
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
         // Nuclear pores (isc_cell_npcs): centre x, y, z, the outward normal and e1, in world um / unit vectors.
         std::vector<double> pores(static_cast<size_t>(ISC_NPC_STRIDE) * 1024);
         int32_t np = isc_cell_npcs(w, ccx, ccy, pores.data(), 1024);
         if (np > 1024)
         {
            pores.resize(static_cast<size_t>(ISC_NPC_STRIDE) * np);
            np = isc_cell_npcs(w, ccx, ccy, pores.data(), np);
         }
         json += ",\"npcs\":[";
         for (int32_t k = 0; k < np; ++k)
         {
            const double* q = pores.data() + static_cast<size_t>(k) * ISC_NPC_STRIDE;
            double v[ISC_NPC_STRIDE];
            toWorld(q[0], q[1], v[0], v[1]);
            v[2] = q[2];
            for (int a = 3; a < ISC_NPC_STRIDE; a += 3)   // directions: the rotation only
            {
               v[a] = q[a] * cr - q[a + 1] * sr;
               v[a + 1] = q[a] * sr + q[a + 1] * cr;
               v[a + 2] = q[a + 2];
            }
            json += k ? ",[" : "[";
            for (int a = 0; a < ISC_NPC_STRIDE; ++a)
            {
               if (a)
                  json += ',';
               AppendNum(json, v[a]);
            }
            json += ']';
         }
         json += ']';
      }
      json += '}';
   }
   json += ']';
   // The DNA-origami molecules (a nanoruler world; none in a cell field): isc_origami_in_window's row, the bent
   // outline, and with detail every designed strand: k, n, x, y, z (world um), the straight-frame x, y, h and the bent
   // X, Y (nm), fluorescent (0/1 under the label).
   std::vector<double> mols(static_cast<size_t>(ISC_ORIGAMI_STRIDE) * 256);
   int32_t nm = isc_origami_in_window(w, x0, y0, x1, y1, mols.data(), 256);
   if (nm > 256)
   {
      mols.resize(static_cast<size_t>(ISC_ORIGAMI_STRIDE) * nm);
      nm = isc_origami_in_window(w, x0, y0, x1, y1, mols.data(), nm);
   }
   if (nm < 0)
   {
      err = "origami query failed";
      return false;
   }
   json += ",\"origami\":[";
   for (int32_t i = 0; i < nm; ++i)
   {
      const double* m = mols.data() + static_cast<size_t>(i) * ISC_ORIGAMI_STRIDE;
      const int32_t tx = static_cast<int32_t>(m[0]), ty = static_cast<int32_t>(m[1]), mi = static_cast<int32_t>(m[2]);
      static const char* const kKeys[ISC_ORIGAMI_STRIDE] = { "tx", "ty", "i", "design", "x", "y", "yaw", "roll",
                                                              "endAngle", "fold" };
      json += i ? ",{" : "{";
      for (int k = 0; k < ISC_ORIGAMI_STRIDE; ++k)
      {
         json += k ? ",\"" : "\"";
         json += kKeys[k];
         json += "\":";
         AppendNum(json, m[k]);
      }
      constexpr int32_t kPerSide = 48;
      std::vector<double> o(static_cast<size_t>(4 * (kPerSide + kPerSide / 4)));
      const int32_t no = isc_origami_outline(w, tx, ty, mi, kPerSide, o.data(), static_cast<int32_t>(o.size() / 2));
      json += ",\"outline\":[";
      for (int32_t k = 0; k < no; ++k)
      {
         json += k ? ",[" : "[";
         AppendNum(json, o[2 * static_cast<size_t>(k)]);
         json += ',';
         AppendNum(json, o[2 * static_cast<size_t>(k) + 1]);
         json += ']';
      }
      json += ']';
      if (detail)
      {
         const int32_t ns = isc_origami_sites(w, tx, ty, mi, nullptr, 0);
         buf.assign(static_cast<size_t>(std::max(0, ns)) * ISC_ORIGAMI_SITE_STRIDE, 0.0);
         if (ns > 0)
            isc_origami_sites(w, tx, ty, mi, buf.data(), ns);
         json += ",\"sites\":[";
         for (int32_t k = 0; k < ns; ++k)
         {
            json += k ? ",[" : "[";
            for (int f = 0; f < ISC_ORIGAMI_SITE_STRIDE; ++f)
            {
               if (f)
                  json += ',';
               AppendNum(json, buf[static_cast<size_t>(k) * ISC_ORIGAMI_SITE_STRIDE + f]);
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
      for (int s : S.mounted)
         for (const StatePhysics* st : { &S.labels[static_cast<size_t>(s)].main, &S.labels[static_cast<size_t>(s)].pre })
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

bool PrefetchScope(const ScopeSpec& spec, double marginUm, double budgetMs, LightHistory* history,
                   const LightState* light)
{
   ScopeSetup S;
   std::string err;
   if (!MakeScopeSetup(spec, S, err))
      return false;
   MovieCache& cache = SharedMovieCache();
   std::unique_lock<std::mutex> lock(cache.mutex, std::try_to_lock);
   if (!lock.owns_lock() || !cache.haveWorld || !cache.world.SameWorld(S.cf))
      return false;
   if (!history || !light)
      return cache.source.Prefetch(S.q, marginUm, budgetMs);
   // The history's map with the light there (the memo's other entry), each dye on its clock there.
   const double zLo = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm - S.q.zHalfRangeUm : -1e3;
   const double zHi = S.q.zHalfRangeUm > 0 ? S.q.zCullCentreUm + S.q.zHalfRangeUm : 1e3;
   const double g = marginUm + kMapMarginUm;
   const std::shared_ptr<const ClockMapView> map =
      history->BuildMap(spec, *light, S.q.x0Um - g, S.q.y0Um - g, S.q.x1Um + g, S.q.y1Um + g, zLo, zHi, err);
   if (!map)
      return false;
   const bool ok = cache.source.SetClockMap(&map->buffer) && cache.source.Prefetch(S.q, marginUm, budgetMs, true);
   cache.source.SetClockMap(nullptr);
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
                        std::string& err, const ScopeSpec* past, bool carryRunningImages,
                        const ScopeProgress* progress, LightHistory* hostHistory, const LightState* hostLight)
{
   // A past: the sample lit by its light from 0 to start-sec, then by the movie's (a light history of two records
   // over the query; the field stops grown to it), as the adapter's movies read theirs. A host's history instead:
   // as given.
   LightHistory history;
   LightState now;
   if (past && !hostHistory)
   {
      ScopeSetup S;
      LightState before;
      if (!MakeScopeSetup(spec, S, err) || !MakeLightState(*past, true, before, err) ||
          !MakeLightState(spec, true, now, err))
         return false;
      const double g = kMapMarginUm;   // (the box the movie's map spans)
      GrowFieldStop(before, S.q.x0Um - g, S.q.y0Um - g, S.q.x1Um + g, S.q.y1Um + g);
      GrowFieldStop(now, S.q.x0Um - g, S.q.y0Um - g, S.q.x1Um + g, S.q.y1Um + g);
      history.Advance(S.t0Sec, before);
   }
   LightHistory* const hist = hostHistory && hostLight ? hostHistory : past ? &history : nullptr;
   const LightState* const light = hostHistory && hostLight ? hostLight : past ? &now : nullptr;
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
      fm.CarryRunningImages(carryRunningImages);
      ScopeSetup S;
      BrightfieldSpec bs;
      if (!fm.Begin(spec, false, err, nullptr, hist, light) || !MakeScopeSetup(spec, S, err) ||
          !ScopeBrightfieldSpec(spec, bs, err))
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
      return fm.Render(noFrames, info, err, progress, &opt);
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
   fm.CarryRunningImages(carryRunningImages);
   FluorescenceFrameOptions opt;
   opt.onPhotons = onPhotons;
   return fm.Begin(spec, false, err, nullptr, hist, light) && fm.Render(noFrames, info, err, progress, &opt);
}

} // namespace sim
