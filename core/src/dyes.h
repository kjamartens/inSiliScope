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

#include <cmath>
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
// Nuclear pores (issue 15): one dye hash per cell (H1 = Pcg4d(seed ^ NPC_DYE_SALT, cx, cy, 0)), site k = the pore's
// attachment point (0-63), n = the pore's candidate index: the DYE_CH channels as on a microtubule.
constexpr uint32_t NPC_DYE_SALT = 0x4E504344u;   // "NPCD"
constexpr double NPC_LINKER_MIN_NM = 2;
constexpr double NPC_LINKER_MAX_NM = 5;
// Nuclear lamina (2026-10-27): one dye hash per cell (H1 = Pcg4d(seed ^ LAMIN_DYE_SALT, cx, cy, 0)), one site per point
// (k = 0), n = the point's candidate index (BuildLaminForCell).
constexpr uint32_t LAMIN_DYE_SALT = 0x4C414D44u;   // "LAMD"
constexpr double DYE_BLOCK_UM = 1.0;             // generation/caching unit along a microtubule

// Per-site purpose channels (the 4th Pcg4d word of H2).
namespace DYE_CH {
constexpr uint32_t LABEL = 0, LINK_U = 1, LINK_PHI = 2, LINK_R = 3, ACT = 4, PERSIST = 5;
// Issue 16 (ABI 10): FLUOR (fluorescent fraction), INIT_ON (dSTORM initial ON
// time), AUX (unit-exponential bleach draw of a continuously emitting dye),
// ORIENT_U/ORIENT_PHI (Random orientation); MOTION (SPT) and OFFTARGET
// (off-target binding) are reserved. Issue 15: BIND_U/BIND_PHI/BIND_R (the
// binder's flexibility: LinkedDye, drawn only when binderFlexNm > 0). All 16
// are taken.
constexpr uint32_t FLUOR = 6, INIT_ON = 7, AUX = 8, ORIENT_U = 9, ORIENT_PHI = 10, MOTION = 11, OFFTARGET = 12;
constexpr uint32_t BIND_U = 13, BIND_PHI = 14, BIND_R = 15;
// Blink j of the schedule draws on SCHED0 + j*SCHED_STRIDE + {ON, BRIGHT1, BRIGHT2, BLEACH, OFF}.
constexpr uint32_t SCHED0 = 16, SCHED_STRIDE = 8;
constexpr uint32_t ON = 0, BRIGHT1 = 1, BRIGHT2 = 2, BLEACH = 3, OFF = 4;
}

struct SiteGeom { Pt3 att, tip, dye; };

// ---- label linkage (issue 15; JS scope/dyes.js): where a label's dye sits relative to its binding site ----
// Three parts (data/dyes/library.json linkagePresets):
//   binder  the probe (antibodies, a nanobody, a tag, a fluorescent protein): binderNm from the binding site along the
//           structure's axis (a microtubule: radially out of the lattice; a pore: its corner's radial direction in the
//           ring plane), then its flexibility: a uniform point in a ball of radius binderFlexNm about that;
//   linker  the chemical linker from the probe to the dye: a uniform direction, length uniform in volume in
//           [linkerMinNm, linkerMaxNm];
//   dye     the end point (the emitter).
// LegacyLinkage: each structure's numbers before the linkage model (the dyes are the same bit for bit).
struct Linkage {
   double binderNm = 0, binderFlexNm = 0, linkerMinNm = 0, linkerMaxNm = 0;
   bool operator==(const Linkage& o) const
   {
      return binderNm == o.binderNm && binderFlexNm == o.binderFlexNm && linkerMinNm == o.linkerMinNm &&
             linkerMaxNm == o.linkerMaxNm;
   }
   bool operator!=(const Linkage& o) const { return !(*this == o); }
};
// Microtubules {12, 0, 2, 5}, pores {0, 0, 2, 5}; any other structure {0, 0, 0, 0}.
Linkage LegacyLinkage(int structure);
// Clamped: lengths >= 0 (NaN -> 0), linkerMaxNm >= linkerMinNm (JS normLinkage).
Linkage NormLinkage(const Linkage& lk);
// The farthest a dye can be from its binding site, nm.
inline double LinkageReachNm(const Linkage& lk) { return lk.binderNm + lk.binderFlexNm + lk.linkerMaxNm; }
// Per block: the linker's cubed bounds (um^3; l*l*l, not pow) and the flexibility radius (um).
struct LinkageSetup {
   Linkage lk;
   double minL3, maxL3, flexUm;
   explicit LinkageSetup(const Linkage& l);
};
// The dye of one site: base point b (um), the unit axis a, the binding site's distance along a from b (attachNm: the
// microtubule radius; 0 at a pore site, which is the base), the linkage, the site's address (h1, k, n) and its three
// linker draws. The binder's rigid end is b + a (attachNm + binderNm) (one product, as before the linkage model); its
// flexibility adds a uniform point in a ball (BIND_U: cos of the polar angle, BIND_PHI: azimuth, BIND_R: radius
// uniform in volume). parts (optional): the binding site, the binder's rigid end and its end (um), for drawing.
struct LinkageParts { Pt3 site, rigid, tip; };
Pt3 LinkedDye(const Pt3& b, const Pt3& a, double attachNm, const LinkageSetup& L, uint32_t h1, int32_t k, int32_t n,
              double r1, double r2, double r3, LinkageParts* parts = nullptr);

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
// initialOnSec: dSTORM only, the dye is ON from t = 0 for Exp(initialOnSec)
// (0 = no initial ON phase) before its blinks (LabelSchedule).
struct Kinetics {
   double activationRatePerSec = 0.01;
   double onSec = 0.05;
   double offSec = 1.0;
   double bleachProb = 1.0;
   double photonCV = 0.5;
   double initialOnSec = 0.0;
};

// A dye clock split into segments of constant kinetics (ABI 11, spec/ALGORITHM.md "Rate history"): segment i holds
// from tStart[i] to tStart[i+1] (tStart[0] = 0, the last for ever). A schedule under a history spends each state's
// unit-exponential draw as the integral of that state's rate over the segments, so the same draws give the same
// past and a change of rates acts from its segment on (a change at T leaves every event before T as it was). One
// segment is the label's kinetics from t = 0 (the old schedules, bit for bit).
struct KineticsHistory {
   std::vector<double> tStart;
   std::vector<Kinetics> kin;
   size_t Size() const { return tStart.size(); }
   // The segment holding t (the last with tStart <= t; 0 before the first).
   size_t SegmentAt(double t) const;
};

// ---- labels (issue 16; JS scope/dyes.js makeLabel) ----
// A structure's label: which binding sites carry a dye, and how that dye
// emits. The world model knows no spectra or light: the imaging side turns a
// dye + light path into these rates.
enum class LabelMode : int { dSTORM = 0, PALM = 1, DnaPaint = 2, WideField = 3 };   // = JS LABEL_MODES
enum class OrientationMode : int { Free = 0, Fixed = 1, Random = 2 };              // = JS ORIENTATION_MODES
// Event states (JS EVENT_STATE): a blink of the main state; the continuous
// windows of a PALM pre state, the dSTORM initial ON, a WideField dye.
enum EventState : uint8_t { STATE_BLINK = 0, STATE_PRE = 1, STATE_INITIAL_ON = 2, STATE_ALWAYS_ON = 3 };

struct Orientation {
   OrientationMode mode = OrientationMode::Free;
   double polarDeg = 90, azimuthDeg = 0, wobbleDeg = 0;
};

struct Label {
   double density = 0.7;               // fraction of the structure's binding sites with a label (LABEL draw)
   double fluorescentFraction = 1.0;   // fraction of those whose dye is fluorescent (FLUOR draw; 1 = no draw)
   LabelMode mode = LabelMode::DnaPaint;
   Kinetics kin;
   bool preState = false;              // PALM: emits in a pre state until its first activation
   Orientation orientation;
   int motion = 0;                     // 0 = Static (SPT: future; MOTION reserved)
   int offTargetCount = 0;             // off-target entries (future; must be 0)
   // Issue 15: where the dye sits (LinkedDye). hasLinkage false = the structure's LegacyLinkage (World::SetLabel
   // fills it in, normalised).
   bool hasLinkage = false;
   Linkage linkage;
};

// nullptr if the label is valid, else why not (JS validateLabel; a non-Static
// motion and off-target binding are "not implemented yet").
const char* ValidateLabel(const Label& l);
// True for the two "not implemented yet" refusals (ValidateLabel's last ones).
bool LabelNotImplemented(const Label& l);

// A continuous emission window from t = 0 (LabelSchedule): state PRE,
// INITIAL_ON or ALWAYS_ON (tOff infinite); aux a unit-exponential draw (AUX)
// the imaging side scales into the dye's own bleach time (0 for INITIAL_ON).
struct ContWindow { double tOn, tOff; uint8_t state; double aux; };
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
// tMax: stop at the first blink starting at or after it (the blinks before it
// are the same; a movie needs only its own time span).
void DyeSchedule(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, std::vector<Blink>& out,
                 double tMax = INFINITY);
// The same under a history of two or more segments, in the dye's local time (absolute time - t0: the segments start
// at tStart - t0): first activation, ON, dark and bleach per segment (KineticsHistory). A blink cycle that ends in
// the segment it started in uses DyeSchedule's arithmetic, so the blinks before a later segment's start are
// DyeSchedule's (or this function's without that segment) bit for bit.
void DyeScheduleHistory(uint32_t h1, int32_t k, int32_t n, const KineticsHistory& h, double t0, std::vector<Blink>& out,
                        double tMax = INFINITY);
// The two from a given first activation t (ABI 14, an activation clock: the blinks after it, the same arithmetic).
void DyeScheduleFrom(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t, std::vector<Blink>& out,
                     double tMax = INFINITY);
void DyeScheduleHistoryFrom(uint32_t h1, int32_t k, int32_t n, const KineticsHistory& h, double t0, double t,
                            std::vector<Blink>& out, double tMax = INFINITY);

// The emission of label dye (k, n) (JS labelSchedule): blinks of the main
// state (dSTORM, PALM; DNA-PAINT blinks come from PersistentBlinks) and the
// continuous windows. dSTORM: ON from 0 for Exp(initialOnSec) (none at 0),
// then DyeSchedule shifted to start there. PALM: DyeSchedule; with a pre
// state, a PRE window until the first blink (never at activation rate 0).
// WideField: one ALWAYS_ON window. blinks / cont: nullptr to skip. tMax: as
// DyeSchedule's (in absolute time).
// hist (ABI 11): the label's kinetics per segment of the clock (nullptr: label.kin from t = 0; one segment: its
// kinetics from t = 0, the same code). dSTORM: the initial ON spends its draw over 1 / initialOnSec per segment
// (there is one iff segment 0 has initialOnSec > 0), the blinks run in local time from its end; PALM: the pre
// window ends at the first activation of DyeScheduleHistory.
void LabelSchedule(uint32_t h1, int32_t k, int32_t n, const Label& label, std::vector<Blink>* blinks,
                   std::vector<ContWindow>* cont, double tMax = INFINITY, const KineticsHistory* hist = nullptr);
// A PALM dye under an activation clock (ABI 14): activated at tAct (infinity: not yet), its blinks from then on under
// hist (as LabelSchedule's; nullptr: label.kin), a pre state [0, tAct). Nothing for other modes.
void LabelScheduleActivated(uint32_t h1, int32_t k, int32_t n, const Label& label, const KineticsHistory* hist,
                            double tAct, std::vector<Blink>* blinks, std::vector<ContWindow>* cont,
                            double tMax = INFINITY);
// The unit-exponential draw a dye's first activation spends (DYE_CH::ACT).
double ActivationDraw(uint32_t h1, int32_t k, int32_t n);

// Blinks of persistent site (k, n) overlapping [t0, t1) (t >= 0), a pure
// function of (address, time bin). Appends, by bin. hist (ABI 11): a bin's
// part in each segment has that segment's kinetics (nullptr or one segment:
// kin / that segment's for every bin): a change acts at once, and the blinks
// before it never change (PersistentGen in dyes.cpp).
void PersistentBlinks(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t0, double t1,
                      std::vector<Blink>& out, const KineticsHistory* hist = nullptr);

// Every blink of persistent site (k, n) starting in time bins [binLo, binHi]
// (bins of PERSIST_BIN_SEC), unfiltered. Same values as PersistentBlinks,
// which is this plus the overlap filter. Appends, by bin then j.
void PersistentBlinksInBins(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, long binLo, long binHi,
                            std::vector<BinBlink>& out, const KineticsHistory* hist = nullptr);

// The longest ON a persistent blink can have under kin / hist (PERSIST_ON_CAP x the largest onSec): the lookback of
// a window query.
double PersistentMaxOn(const Kinetics& kin, const KineticsHistory* hist);

struct Dye {
   Pt3 pos;          // cell-local, um
   int32_t mtIndex;
   int32_t k;        // protofilament
   int32_t n;        // dimer index along the microtubule
   uint32_t id;      // H2.a of the site: stable per-dye hash
   double theta;     // the protofilament's azimuth (the dye's local frame)
};

// Fluorescent dyes of one block (arc length [block, block+1) um) of one
// microtubule, with the label's linkage (default the microtubules' legacy). A site is labelled when its LABEL draw u < density, and its
// dye is fluorescent when fluorescentFraction >= 1 or its FLUOR draw <
// fluorescentFraction (no draw at 1): both nested, so a lower density or
// fraction keeps a subset of the same dyes. Only those sites get geometry.
// Appends.
void DyesInBlock(uint32_t seed, int32_t cx, int32_t cy, int mtIndex, const std::vector<Pt3>& pts, const MtFrames& fr,
                 int blockIndex, double density, double fluorescentFraction, std::vector<Dye>& out,
                 const Linkage& linkage = LegacyLinkage(0), std::vector<LinkageParts>* parts = nullptr);

// H1 of a cell's pores: Pcg4d(seed ^ NPC_DYE_SALT, cx, cy, 0).a.
uint32_t NpcH1(uint32_t seed, int32_t cx, int32_t cy);

// A pore dye: cell-local position, the pore (index into the cell's pores), site k (0-63), n = the pore's candidate
// index, id.
struct NpcDye {
   Pt3 pos;
   int32_t pore, k, n;
   uint32_t id;
};
struct Npc;
// Fluorescent dyes of the pores pores[ids[..]] of a cell (BuildNpcsForCell); sites: NpcAttachmentPoints (64 x 3, nm,
// in the pore frame). Labelled / fluorescent / linker draws as on a microtubule (DyesInBlock): nested in density and
// fluorescent fraction. The binder along the corner's radial direction in the ring plane. Appends (parts: one per
// dye, if given).
void NpcDyesInBlock(uint32_t seed, int32_t cx, int32_t cy, const std::vector<Npc>& pores, const std::vector<int32_t>& ids,
                    const double* sites, double density, double fluorescentFraction, std::vector<NpcDye>& out,
                    const Linkage& linkage, std::vector<LinkageParts>* parts = nullptr);

// H1 of a cell's lamina: Pcg4d(seed ^ LAMIN_DYE_SALT, cx, cy, 0).a.
uint32_t LaminH1(uint32_t seed, int32_t cx, int32_t cy);

struct LaminPoint;
// Fluorescent dyes of the lamina points pts[ids[..]] of a cell (BuildLaminForCell): one site per point (k 0), the
// draws as on a pore (NpcDyesInBlock), the binder along the inward normal (the lamina lines the nucleoplasmic face).
// NpcDye rows: pore = the point's index into pts. Appends (parts: one per dye, if given).
void LaminDyesInBlock(uint32_t seed, int32_t cx, int32_t cy, const std::vector<LaminPoint>& pts, const std::vector<int32_t>& ids,
                      double density, double fluorescentFraction, std::vector<NpcDye>& out, const Linkage& linkage,
                      std::vector<LinkageParts>* parts = nullptr);

// Mean emission dipole of dye (k, n) (JS dyeOrientation): false for Free
// (isotropic, no draws). Fixed: polar angle from the microtubule axis T and
// azimuth about it from the dye's radial direction (theta in the U/V frame of
// segment seg). Random: uniform on the sphere (ORIENT_U, ORIENT_PHI). dir is
// a cell-local unit vector; the dye wobbles within a cone of
// label.orientation.wobbleDeg. Not used by the renderer yet.
bool DyeOrientation(uint32_t h1, int32_t k, int32_t n, const Label& label, const MtFrames& fr, size_t seg, double theta,
                    Pt3& dir);
// Fixed orientation about a structure axis t and a radial direction r (unit, r perpendicular to t): polar angle from
// t, azimuth about it from r (the second half of DyeOrientation). Microtubule: t = the axis, r = the protofilament's;
// pore: t = the pore axis (outward normal), r = the corner's direction in the ring plane.
Pt3 DyeOrientationFixed(const Orientation& o, const Pt3& t, const Pt3& r);
// Random orientation of dye (k, n) (uniform on the sphere); false unless the mode is Random.
bool DyeOrientationRandom(uint32_t h1, int32_t k, int32_t n, const Orientation& o, Pt3& dir);

} // namespace isc
