///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpField.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   BFP light maps (2026-11-19, spec/ALGORITHM.md "BFP light
//                maps"): the light in the objective's back focal plane given as
//                maps over the pupil (BfpExpr.h: intensity, phase,
//                polarization, entry tilt, focus, coherence, timing), and the
//                excitation they make in the sample, computed numerically and
//                kept: BfpLight::Make is memoised by the maps' content.
//
//                Per laser line and time slot (t = (s + 0.5) / S of a cycle):
//                every coherent lit pupil point is one field over the pupil --
//                amplitude sqrt(I), phase 2 pi phase minus k times the integral
//                of the ray's landing point s(u) = f tan(tilt + focus towards
//                its region's centroid) (stationary phase: the light of u lands
//                at s(u) in the focal plane), its polarization's lab field per
//                pupil point (Fresnel / Axelrod, BeamFieldOf) -- whose sample
//                field is its angular spectrum, sum_u a(u) exp(i k u.x) x
//                exp(i k kz(u) (z - z_focal)) (propagating) or exp(-k
//                sqrt(u^2 - 1) z) (evanescent), evaluated per z plane by
//                separable chirp-Z transforms (ChirpZ.h); every incoherent lit
//                point is a plane wave of its own over the legacy beam's
//                field-stop footprint (illum-profile, illum-fwhm) centred on
//                s(u) and sheared along its direction with height. Unpolarized
//                points: the mean of their s and p runs. The intensity is kept
//                per frame state as a volume: planes on a lateral grid (bicubic)
//                and a z grid fine enough for the axial beats, interpolated
//                along the light's mean direction between planes. Light that is
//                only propagating is kept relative to the focal plane (a focus
//                change moves it, nothing is recomputed); only evanescent light
//                from the coverslip; both in one coherent field: computed at the
//                given focus. Power: the cycle mean through the pupil = the
//                legacy beam's (its epi envelope's power, Parseval), per line.
//
//                Sampling (2026-11-20): one accuracy, N = samples per the
//                light's finest period (bfp-samples-per-period; Renderer.Quality
//                Fast 6, Realistic 12, Exhaustive 24). The lateral grid takes N
//                samples per the finest fringe (and per the incoherent
//                footprint's scale / 2) on a tile over the field of view plus
//                kTileMarginUm, the rest of the window today's 6 (at most 512 a
//                side); the axial beats and evanescent decays N per period /
//                decay; the pupil the period's spacing, and 512 x N / 6 samples
//                across it where lit light meets a hard edge (the pupil's rim, a
//                step in a map: a Riemann sum over an edge converges only as
//                du). The tile and its field stay within a memory budget: a
//                tile that does not fit is made coarser and the info says so.
//
//                Shortcuts (exact): scanX / scanY / scanRot (functions of t
//                only) shift and rotate one field per slot instead of computing
//                each; slots with the same lit points share a field.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "BfpExpr.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

struct BfpLightParams
{
   std::string text;                // the equations
   std::vector<double> lines;       // the lit laser lines (nm); empty: lambdaRefNm
   double lambdaRefNm = 640;
   double na = 1.4, nS = 1.337, nI = 1.518, fObjUm = 2000;
   double bfp = 0;                  // illum-bfp (the equations' bfp)
   double envFwhmUm = 12.8;         // the legacy beam's envelope FWHM (the equations' fwhm; the incoherent footprint)
   int profile = 1;                 // its profile (ExcitationProfile)
   double fovSpanUm = 12.8;
   double pivotUm = 0;              // the focal plane's height
   bool anchorFocus = true;         // coherent regions in phase at the focal plane (else at the coverslip)
   double sheetTUm = 3, sheetWUm = 12.8;
   int slots = 0;                   // 0: 64 when something depends on t, else 1
   int cycleFrames = 0;             // 0: every slot in every frame; n: the cycle spans n frames
   double zSampleUm = 12;           // the volume's z range (above it the top plane, sheared)
   double samplesPerPeriod = 12;    // N: samples per the light's finest period (laterally, axially, the pupil's edges)
   double tileBudgetMB = 0;         // the fine tile's memory, all lines (0: 1 GB native, 256 MB in WASM)
};

// One part of the light: planes on a lateral grid, z (relative: from the focal plane), the shear between planes; and
// optionally a finer tile of the same planes over the middle (the field of view), read where it reaches.
struct BfpVolume
{
   double x0 = 0, y0 = 0, dx = 1;
   int nx = 0, ny = 0;
   std::vector<double> z;
   bool relative = false;
   bool extendTop = true, extendBottom = true;   // beyond the end planes: the end plane sheared (false: 0)
   double sx = 0, sy = 0;
   std::vector<float> v;   // nz x ny x nx
   double tx0 = 0, tdx = 0;   // the tile: tn x tn from (tx0, tx0) in steps tdx (tn 0: none)
   int tn = 0;
   std::vector<float> t;   // nz x tn x tn
   int Nz() const { return static_cast<int>(z.size()); }
   double Plane(int k, double x, double y) const;   // bicubic (the tile inside its own taps), 0 outside the grid
   double At(double x, double y, double z, double pivot) const;
   double ZAbs(int k, double pivot) const { return z[static_cast<size_t>(k)] + (relative ? pivot : 0.0); }
   // Inside the tile with every bicubic tap on it.
   bool InTile(double x, double y) const
   {
      return tn > 0 && x >= tx0 + tdx && y >= tx0 + tdx && x <= tx0 + (tn - 2) * tdx && y <= tx0 + (tn - 2) * tdx;
   }
   size_t Bytes() const { return (v.size() + t.size()) * sizeof(float); }
};

// The light of a frame state (or the cycle mean) at one line: the sum of its parts.
struct BfpLightField
{
   std::vector<BfpVolume> parts;
   double At(double x, double y, double z, double pivot) const;
};

class BfpLight
{
public:
   // Memoised by the maps' content (the equations, the images' bytes, every parameter; the focus only when the light
   // depends on it). err: a compile error, a missing image, a scan map that depends on the pupil position.
   static std::shared_ptr<const BfpLight> Make(const BfpLightParams& p, std::string& err);
   // The maps alone (no field): their values on an n x n raster over [-umax, umax]^2 at slot s and line nm (for the
   // panels and figures). maps: kBfpMaps planes of n x n (row 0 at +uy).
   static bool Rasters(const BfpLightParams& p, int n, int slot, double nm, std::vector<std::vector<float>>& maps,
                       std::string& err);
   // The BFP drawing: the intensity summed over `slots` (sumI, n x n) and the first slot's maps (first). Maps that do not
   // depend on t (a scan at most) are drawn once and turned per slot.
   static bool RasterSum(const BfpLightParams& p, int n, const std::vector<int>& slots, double nm,
                         std::vector<float>& sumI, std::vector<std::vector<float>>& first, std::string& err);

   uint64_t Hash() const { return hash_; }
   const BfpLightParams& Params() const { return params_; }
   bool PivotDependent() const { return pivotDependent_; }
   int Slots() const { return slots_; }
   int Period() const { return period_; }   // frame states
   int LineCount() const { return static_cast<int>(lines_.size()); }
   double LineNm(int l) const { return lines_[static_cast<size_t>(l)].nm; }
   int LineOf(double nm) const;   // the nearest line
   const BfpLightField& State(int l, int state) const;
   const BfpLightField& Mean(int l) const { return lines_[static_cast<size_t>(l)].mean; }

   // Readouts: the outermost lit u (intensity above 1e-3 of the peak), any evanescent / propagating light, its mean
   // direction's tan (|s|), the shortest 1/e decay per nm of the line (um / nm), the window's half width (um), the
   // largest relative curvature of the mean light (1/um^2), the field's cost and memory.
   double UOuter() const { return uOuter_; }
   bool Evanescent() const { return evanescent_; }
   // The share of the pupil's power that is evanescent (beyond the critical angle), over the lines and the cycle.
   double EvanescentShare() const { return evanescentShare_; }
   bool Propagating() const { return propagating_; }
   double TanS() const { return tanS_; }
   double DecayPerNm() const { return decayPerNm_; }
   double HalfWindowUm() const { return halfWindowUm_; }
   double Curvature() const { return curvature_; }
   // How far from the axis the cycle mean reaches, um (to 1e-4 of its peak, any plane; at most the window's corner).
   double ReachUm() const { return reachUm_; }
   double ComputeSec() const { return computeSec_; }
   size_t Bytes() const;
   // A copy without the volumes (the lines, the readouts and the numbers kept): what a closed light-history chapter
   // keeps (LightHistory; its depth tents and line shares need no volume). State / Mean of a slim light are empty.
   std::shared_ptr<const BfpLight> Slim() const;
   bool IsSlim() const { return slim_; }
   const std::string& Info() const { return info_; }
   // The sampling in words when it fell short of N (a tile made coarser to fit the memory budget, the pupil capped);
   // empty when the light was sampled as asked.
   const std::string& SamplingNote() const { return samplingNote_; }

private:
   struct Line
   {
      double nm = 0;
      std::vector<BfpLightField> states;
      BfpLightField mean;
   };
   friend struct BfpBuilder;
   BfpLightParams params_;
   uint64_t hash_ = 0;
   bool pivotDependent_ = false;
   int slots_ = 1, period_ = 1;
   std::vector<Line> lines_;
   double uOuter_ = 0, tanS_ = 0, decayPerNm_ = 0, halfWindowUm_ = 0, curvature_ = 0, computeSec_ = 0;
   bool evanescent_ = false, propagating_ = false;
   double evanescentShare_ = 0, reachUm_ = 0;
   bool slim_ = false;
   std::string info_, samplingNote_;
};

// The built-in equations of the light-map types (sim-type LightSheet, RingTIRF, Maps), and their slots.
std::string BfpTypeEquations(int simType);
int BfpTypeSlots(int simType);

// Bicubic (Catmull-Rom) on an nx x ny grid of floats (origin x0, y0, step dx), 0 outside.
double BfpBicubic(const float* g, int nx, int ny, double x0, double y0, double dx, double x, double y);

} // namespace sim
