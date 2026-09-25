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
constexpr uint32_t LABEL = 0, LINK_U = 1, LINK_PHI = 2, LINK_R = 3;
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

struct Dye {
   Pt3 pos;          // cell-local, um
   int32_t mtIndex;
   int32_t k;        // protofilament
   int32_t n;        // dimer index along the microtubule
   uint32_t id;      // H2.a of the site: stable per-dye hash (seed of its schedule)
};

// Labelled dyes of one block (arc length [block, block+1) um) of one
// microtubule. Only labelled sites get geometry. Appends to `out`.
void DyesInBlock(uint32_t seed, int32_t cx, int32_t cy, int mtIndex, const std::vector<Pt3>& pts, const MtFrames& fr,
                 int blockIndex, double efficiency, std::vector<Dye>& out);

} // namespace isc
