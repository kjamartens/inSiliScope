// Fluorophore labels on microtubules (spec/PORT.md section 5).
//
// Geometry per lattice site is the JS buildMicrotubuleLabelPoints: 13
// protofilaments (13_3 lattice), attachment on a 12.5 nm cylinder, binder
// tip 12 nm further out, dye displaced from the tip uniformly in the shell
// [2, 5] nm (mtDisplaceByLinker).
//
// Identity is ADDRESSED, not a stream (normative, differs from the JS
// preview's hashStream on purpose): H1 = Pcg4d(seed ^ DYE_SALT, cx, cy,
// mtIndex), per-site draws off Pcg4d(H1.a, k, n, purpose) with k the
// protofilament and n the dimer index from the microtubule start. So a dye
// is the same dye whatever window, block or order it is generated in.
#pragma once

#include "microtubules.h"
#include "rng.h"

#include <cstdint>
#include <vector>

namespace isc {

constexpr double MT_RADIUS_NM = 12.5;
constexpr int MT_N_PROTOFILAMENTS = 13;
constexpr double MT_DIMER_NM = 8;
constexpr double MT_LATTICE_START = 3;
constexpr double MT_BINDER_NM = 12;
constexpr double MT_LINKER_MIN_NM = 2;
constexpr double MT_LINKER_MAX_NM = 5;
constexpr uint32_t MT_CH_SEAM_PHASE = 8000000;   // hashUnit(seed, cx, cy, 8000000 + mtIndex), as the JS view

constexpr uint32_t DYE_SALT = 0x9E3779B9u;
constexpr double DYE_BLOCK_UM = 1.0;             // generation/caching unit along a microtubule

// Per-site purpose channels (the 4th Pcg4d word of H2).
namespace DYE_CH {
constexpr uint32_t LABEL = 0, LINK_U = 1, LINK_PHI = 2, LINK_R = 3, ACT = 4, PERSIST = 5;
// Blink j of the schedule draws on SCHED0 + j*SCHED_STRIDE + {ON, BRIGHT1, BRIGHT2, BLEACH, OFF}.
constexpr uint32_t SCHED0 = 16, SCHED_STRIDE = 8;
constexpr uint32_t ON = 0, BRIGHT1 = 1, BRIGHT2 = 2, BLEACH = 3, OFF = 4;
}

struct SiteGeom { Pt3 att, tip, dye; };

// First segment whose end reaches arc length S (clamped to the last one):
// the segment the JS scan `while (seg < n-2 && cum[seg+1] < S) seg++` stops at.
size_t MtSegmentAt(const MtFrames& fr, double S);

// Geometry of one site at arc length S (um from the microtubule start) and
// azimuth theta (rad, in the U/V frame). r1..r3 are the three linker draws
// in JS mtDisplaceByLinker order (cos-polar, azimuth, radius).
SiteGeom MtSiteGeometry(const std::vector<Pt3>& pts, const MtFrames& fr, size_t seg, double S, double theta,
                        double r1, double r2, double r3);

// Protofilament k's azimuth and axial offset (nm) on the 13_3 lattice.
double MtProtofilamentTheta(double phase, int k);
double MtProtofilamentOffsetNm(int k);

// Seam phase of one microtubule (same channel the JS view uses).
double MtSeamPhase(uint32_t seed, int32_t cx, int32_t cy, int mtIndex);

// Blink kinetics of every dye (spec/PORT.md 6.1), simulated seconds.
// A dark dye switches on at activationRatePerSec (per dye).
// Bleaching dyes: first activation after Exp(1 / rate); then ON for
// Exp(onSec), bleach with probability bleachProb (clamped to [0.01, 1], as
// the adapter's EmitterModel), else dark for Exp(offSec) and blink again.
// Non-bleaching (persistent, DNA-PAINT-like) sites: blinks start as a
// Poisson process of that rate for ever, each ON for Exp(onSec) (capped at
// PERSIST_ON_CAP x onSec). Per-blink brightness log-normal with mean 1 and
// CV photonCV (exactly 1 at 0).
struct Kinetics {
   double activationRatePerSec = 0.01;
   double onSec = 0.05;
   double offSec = 1.0;
   double bleachProb = 1.0;
   double photonCV = 0.5;
};
constexpr int DYE_MAX_BLINKS = 1000;    // cap, so a tiny bleachProb cannot loop forever
// Persistent blinks are addressed per time bin (dye, bin, j), so any time
// window can be answered without running the dye from t = 0.
constexpr double PERSIST_BIN_SEC = 1.0;
constexpr double PERSIST_ON_CAP = 20.0;  // ON times truncated at 20 x onSec (window lookback)

struct Blink { double tOn, tOff, brightness; };
// A persistent-site blink with its address within the site: time bin and
// index j in that bin (the order PersistentBlinks appends in).
struct BinBlink { double tOn, tOff, brightness; uint32_t bin, j; };

// H1 of a microtubule: Pcg4d(seed ^ DYE_SALT, cx, cy, mtIndex).a.
uint32_t DyeH1(uint32_t seed, int32_t cx, int32_t cy, int mtIndex);

// Full blink lifetime of bleaching dye (k, n) of the microtubule with hash
// h1: a pure function of that address (never of a stream, window or time).
// Appends in time order. Nothing when the activation rate is 0.
void DyeSchedule(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, std::vector<Blink>& out);

// Blinks of persistent site (k, n) overlapping [t0, t1) (t >= 0), a pure
// function of (address, time bin). Appends, by bin.
void PersistentBlinks(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t0, double t1,
                      std::vector<Blink>& out);

// Every blink of persistent site (k, n) starting in time bins [binLo, binHi]
// (bins of PERSIST_BIN_SEC), unfiltered. Same values as PersistentBlinks,
// which is this plus the overlap filter. Appends, by bin then j.
void PersistentBlinksInBins(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, long binLo, long binHi,
                            std::vector<BinBlink>& out);

struct Dye {
   Pt3 pos;          // cell-local, um
   int32_t mtIndex;
   int32_t k;        // protofilament
   int32_t n;        // dimer index along the microtubule
   uint32_t id;      // H2.a of the site: stable per-dye hash
   bool persistent;  // non-bleaching site (DNA-PAINT-like), else a bleaching dye
};

// Labelled sites of one block (arc length [block, block+1) um) of one
// microtubule. One uniform u per site (the LABEL draw): u < efficiency is a
// bleaching dye, efficiency <= u < efficiency + persistentEfficiency a
// persistent one (so the bleaching dyes at a given efficiency never depend
// on persistentEfficiency). Only labelled sites get geometry. Appends.
void DyesInBlock(uint32_t seed, int32_t cx, int32_t cy, int mtIndex, const std::vector<Pt3>& pts, const MtFrames& fr,
                 int blockIndex, double efficiency, double persistentEfficiency, std::vector<Dye>& out);

} // namespace isc
