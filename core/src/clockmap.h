// The clock map (ABI 14, spec/ALGORITHM.md "Exact light history"): each dye's kinetic clock read at its own place.
//
// The core knows no light. A host that keeps the sample's light history (the MM adapter; the cli and the viewer for a
// movie's past) gives, per structure, how a dye's clock depends on where it is:
//   CLOCK_GLOBAL      every dye on the sample's time T (DNA-PAINT: binding needs no light), the kinetics history in T;
//   CLOCK_DOSE        a dye's clock is the dose it had (dSTORM, WideField, the dSTORM initial ON: every rate is linear in
//                     the excitation, so the kinetics per unit of dose are constant): total(x) at tOpen, rate(x) per
//                     second after; epoch e >= 1 of the kinetics starts at the dye's dose bound[e - 1](x);
//   CLOCK_ACTIVATION  PALM: the dye activates when its hazard reaches its draw (the past as steps, the open record's
//                     rate after tOpen), and blinks from then on in T with the kinetics history in T.
// A field is a sum of terms: scale x grid(x - ox - (z - zp) sx, y - oy - (z - zp) sy) x depth(z), bilinear on the
// grid (nodes outside it count as 0), linear in the depth table (its end values beyond). Lerp form throughout, so
// equal nodes give exactly that value: a uniform field reads its value bit for bit (the uniform map = the ABI 11
// queries).
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include "dyes.h"

#include <cstdint>
#include <vector>

namespace isc {

enum ClockMode : int { CLOCK_GLOBAL = 0, CLOCK_DOSE = 1, CLOCK_ACTIVATION = 2 };

// Node (i, j) at (x0 + i h, y0 + j h), v[j nx + i].
struct ClockGrid {
   double x0 = 0, y0 = 0, h = 1;
   int32_t nx = 0, ny = 0;
   std::vector<double> v;
   double At(double u, double w) const;
};

// v[k] at height z0 + k dz (um), linear between, the end values beyond; empty: 1.
struct ClockDepth {
   double z0 = 0, dz = 1;
   std::vector<double> v;
   double At(double z) const;
};

struct ClockTerm {
   int32_t grid = -1;   // -1: 1 everywhere
   int32_t depth = -1;  // -1: 1 at every height
   double ox = 0, oy = 0, zp = 0, sx = 0, sy = 0, scale = 1;
};
using ClockField = std::vector<ClockTerm>;

// An interval of an activation clock's past: over [t0, t1) the hazard grows by c + field(x), linearly in time.
struct ClockStep {
   double t0 = 0, t1 = 0, c = 0;
   ClockField field;
};

struct StructClock {
   int mode = CLOCK_GLOBAL;
   // GLOBAL, ACTIVATION: the kinetics history in T (tStart[0] = 0, increasing); ACTIVATION ignores its activation
   // rates. DOSE: the kinetics per epoch (epoch e starts at the dye's bound[e - 1]; tStart, as for the others, says when
   // in T: what a label read on T under this clock -- DNA-PAINT -- takes).
   KineticsHistory hist;
   std::vector<ClockField> bound;   // DOSE: hist.Size() - 1 fields
   ClockField total, rate;          // DOSE, ACTIVATION (with the constants totalC, rateC added)
   double totalC = 0, rateC = 0;
   std::vector<ClockStep> steps;    // ACTIVATION: the past, contiguous from 0 to tOpen
   double tOpen = 0;
   // The photon-budget bleach of the structure's budgeted continuous state (a WideField dye's ALWAYS_ON window, a PALM
   // pre state) as a hazard: bleachTotal(x) by tOpen, then bleachRate(x) per second; such a window ends where it
   // reaches the window's aux (BleachEnd). Both empty: no bleach.
   ClockField bleachTotal, bleachRate;
   // Fingerprints (ClockMap::Finish): the schedules (DOSE: kinetics + bounds; GLOBAL: kinetics), the kinetics as a
   // history in T (= SetKineticsHistory's), the present (total, rate, tOpen, constants, the bleach) and the steps as a
   // chain (stepFp[i]: steps 0..i).
   uint64_t fpSchedule = 0, fpHistT = 0, fpNow = 0;
   std::vector<uint64_t> stepFp;
};

struct ClockMap {
   std::vector<ClockGrid> grids;
   std::vector<ClockDepth> depths;
   std::vector<StructClock> s;   // one per structure
   double Term(const ClockTerm& t, double x, double y, double z) const;
   // The field's value: the terms summed in order from c (exactly c + the first term ...).
   double Eval(const ClockField& f, double x, double y, double z, double c = 0.0) const;
   // A bound of the field over a box (every term's extremes over the box, summed): lo <= value <= hi.
   void Bounds(const ClockField& f, double x0, double x1, double y0, double y1, double z0, double z1, double c,
               double& lo, double& hi) const;
   // Checks the map (nullptr) or says what is wrong, and fills the fingerprints.
   const char* Finish();
};

// The activation time of a dye of hazard draw E under an activation clock at (x, y, z): the steps walked forward (the
// hazard after each, H), the crossing linear within its step; after them tOpen + (E - H) / rate (infinity at a rate
// <= 0). The walk can resume: state (from, H) = the steps already summed (0, 0 at first); it returns the time if the
// crossing lies in the steps, else NaN with (from, H) advanced past them.
double ActivationInSteps(const ClockMap& m, const StructClock& c, double x, double y, double z, double E, size_t& from,
                         double& H);

// When a budgeted window of draw aux bleaches under clock c at (x, y, z), in T: tOpen + (aux - bleachTotal) /
// bleachRate; at a rate <= 0 never (infinity) if aux > bleachTotal, else -infinity (by tOpen). Infinity without bleach.
bool HasBleach(const StructClock& c);
double BleachEnd(const ClockMap& m, const StructClock& c, double x, double y, double z, double aux);

} // namespace isc
