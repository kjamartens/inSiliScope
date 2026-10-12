// DNA-origami nanorulers (specimen 1, data/specimens.json): rods lying on a
// coverslip, each a design of data/origami/designs.json (the 80 nm nanoruler:
// a 200 x 13 nm 12-helix bundle with three spots of docking strands), bent
// in-plane as a 2D surface-equilibrated worm-like chain. The port of the JS
// reference web/prototype/scope/origami.js, line for line (bit-exact: jsm::
// math, JS operand order); the model is tools/origami_reference/
// origami_bend.py with its sequential stream replaced by address-based pcg4d
// draws. spec/ALGORITHM.md "DNA-origami nanorulers". BSD-3-Clause.
//
// Per molecule: rest sites -> incorporation (the label's LABEL/FLUOR draws,
// as on microtubules) -> deformation (wlc2d or rigid) -> landing (roll about
// the rod's axis, attach height) -> placement (position, yaw) -> sites in
// world um. Every step is a pure function of (seed, tile, molecule index):
// placement draws never depend on the bending parameters.
#pragma once

#include "params.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace isc {

constexpr int SPECIMEN_CELLFIELD = 0;
constexpr int SPECIMEN_NANORULERS = 1;
constexpr int SPECIMEN_COUNT = 2;
// The field is tiled in ORIGAMI_TILE_UM squares; a tile holds Poisson(density
// x area) candidates (capped), thinned against the 3x3 tiles around it
// (Matern type II): a candidate is dropped when one of higher priority lies
// closer than origamiMinSpacingNm (capped at the tile size).
constexpr double ORIGAMI_TILE_UM = 2.0;
constexpr int ORIGAMI_MAX_PER_TILE = 4096;
constexpr uint32_t ORIGAMI_SALT = 0x4F524947;       // 'ORIG': placement (seed ^ salt, tx, ty, channel)
constexpr uint32_t ORIGAMI_DYE_SALT = 0x52554C52;   // 'RULR': h1 = pcg4d(seed ^ salt, tx, ty, i).a
// Placement channels (4th word): the tile's count, then candidate i at CAND0 + i.
constexpr uint32_t ORIGAMI_CH_COUNT = 0, ORIGAMI_CH_CAND0 = 16;
// Molecule draws on h1 (2nd word above any spot index): bending normals
// pcg4d(h1, K_BEND, pair, 0) (Box-Muller), the roll pcg4d(h1, K_LAND, 0, 0).
constexpr uint32_t ORIGAMI_K_BEND = 0x80000000u, ORIGAMI_K_LAND = 0x80000001u;
constexpr int ORIGAMI_MIN_SEGMENTS = 8;

enum OrigamiDeformation { ORIGAMI_RIGID = 0, ORIGAMI_WLC2D = 1 };
enum OrigamiRollMode { ORIGAMI_ROLL_UNIFORM = 0, ORIGAMI_ROLL_FIXED = 1 };

// A spot: a ring of ringN docking-strand positions around the body at atNm
// along it (tools/gen_origami_designs.mjs -> origami_designs.inc).
struct OrigamiSpot {
   const char* id;
   double atNm;
   int ringN;
   double radiusNm, phaseDeg;
};
struct OrigamiDesign {
   const char* id;
   const char* name;
   double lengthNm, diameterNm;
   int deformation;                  // OrigamiDeformation
   double persistenceLengthNm, segmentNm;
   int roll;                         // OrigamiRollMode (the design's suggestion)
   double rollDeg, attachNm;
   const OrigamiSpot* spots;
   int nSpots;
   int maxRingN;
};
int OrigamiDesignCount();
const OrigamiDesign& OrigamiDesignAt(int i);   // clamped

// normalizeOrigamiParams: specimen, origami* clamped, integers truncated.
void NormalizeOrigamiParams(Params& p);

// One bent realisation: the centreline as n circular arcs of length ds, in
// the "aligned" frame (chord along +x, chord midpoint at the origin).
struct OrigamiShape {
   double L = 0, W = 0;
   int n = 0;
   double ds = 0;
   std::vector<double> theta, kappa, nx, ny;   // n + 1, n, n + 1, n + 1
   double phi = 0, cphi = 1, sphi = 0, ax = 0, ay = 0;
   double endAngle = 0;
   bool fold = false;                          // max |kappa| W/2 >= 1 (a cross-section folds over)
};
double OrigamiBendNormal(uint32_t h1, int i);
// normals: n standard normals instead of the address-based ones (nullptr: those).
OrigamiShape OrigamiBentShape(const OrigamiDesign& d, const Params& p, uint32_t h1, const double* normals = nullptr);
// Bent centreline at straight coordinate x (nm, origin at the body's centre).
void OrigamiCenterline(const OrigamiShape& S, double x, double& X, double& Y, double& th);
// Straight in-plane (x, y) -> bent (X, Y), nm.
void OrigamiTransform(const OrigamiShape& S, double x, double y, double& X, double& Y);
// Closed top-view outline, x0, y0, x1, y1, ... nm (2 (nPerSide + max(2, nPerSide >> 2)) points).
std::vector<double> OrigamiShapeOutline(const OrigamiShape& S, int nPerSide = 64);

uint32_t OrigamiMoleculeH1(uint32_t seed, int32_t tx, int32_t ty, int32_t i);
double OrigamiMoleculeRoll(const Params& p, uint32_t h1);

struct OrigamiMolecule {
   int32_t tx, ty, i;
   double x, y;        // world um (the chord midpoint)
   uint32_t prio;
   double yaw;         // rad
   uint32_t h1;
};
struct OrigamiSite {
   int32_t k, n;       // spot, ring position
   uint32_t id;        // the LABEL draw (the dye's id)
   double rest[3];     // straight frame x, y, h (nm; h from the axis)
   double bent[2];     // molecule frame X, Y (nm)
   double x, y, z;     // world um
};
// The docking strands of a placed molecule, fluorescent under (density,
// fluorescentFraction); withAll: every designed strand (label draws ignored).
void OrigamiMoleculeSites(const OrigamiDesign& d, const Params& p, const OrigamiMolecule& m, const OrigamiShape& S,
                          double density, double fluorescentFraction, std::vector<OrigamiSite>& out, bool withAll = false);

// The field: candidates and thinned molecules per tile (cached; a pure
// function of seed and params). Thread-safe.
class OrigamiField {
public:
   OrigamiField(uint32_t seed, const Params& p);
   const Params& P() const { return p_; }
   const OrigamiDesign& Design() const { return *d_; }
   // How far a molecule's sites reach from its centre, um.
   double ReachUm() const;
   double MaxHeightUm() const;
   // Molecules whose reach (+ extraUm) meets [x0,x1) x [y0,y1): tx outer, ty inner, molecule order.
   void MoleculesInRect(double x0, double y0, double x1, double y1, std::vector<OrigamiMolecule>& out,
                        double extraUm = 0.0);
   // Molecule i of tile (tx, ty); false if thinned away or absent.
   bool Find(int32_t tx, int32_t ty, int32_t i, OrigamiMolecule& out);
   OrigamiShape Shape(const OrigamiMolecule& m) const { return OrigamiBentShape(*d_, p_, m.h1); }
   // Bent outline in world um, x, y, ...
   std::vector<double> OutlineWorld(const OrigamiMolecule& m, int nPerSide = 64) const;

private:
   struct Candidate { int32_t tx, ty, i; double x, y; uint32_t prio; double yaw; };
   const std::vector<Candidate>& Candidates(int32_t tx, int32_t ty);
   const std::vector<OrigamiMolecule>& TileMolecules(int32_t tx, int32_t ty);
   uint32_t seed_;
   Params p_;
   const OrigamiDesign* d_;
   std::mutex mu_;
   std::map<std::pair<int32_t, int32_t>, std::vector<Candidate>> cands_;
   std::map<std::pair<int32_t, int32_t>, std::vector<OrigamiMolecule>> tiles_;
};

} // namespace isc
