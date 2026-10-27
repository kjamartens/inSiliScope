#include "dyes.h"

#include "jsmath.h"
#include "npcs.h"

#include <algorithm>
#include <cmath>

namespace isc {

namespace {
constexpr double NM = 1e-3;

inline double Unit(uint32_t a) { return ((double)(a >> 9) + 0.5) * 1.1920928955078125e-7; }
} // namespace

size_t MtSegmentAt(const MtFrames& fr, double S)
{
   const size_t nseg = fr.T.size();
   // first seg in [0, nseg-1] with cum[seg+1] >= S, else nseg-1
   size_t lo = 0, hi = nseg - 1;
   while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (fr.cum[mid + 1] < S) lo = mid + 1; else hi = mid;
   }
   return lo;
}

double MtProtofilamentTheta(double phase, int k) { return phase + k * 2 * jsm::PI / MT_N_PROTOFILAMENTS; }

double MtProtofilamentOffsetNm(int k)
{
   return std::fmod(k * MT_LATTICE_START * MT_DIMER_NM / MT_N_PROTOFILAMENTS, MT_DIMER_NM);
}

double MtSeamPhase(uint32_t seed, int32_t cx, int32_t cy, int mtIndex)
{
   return 2 * jsm::PI * HashUnit(seed, cx, cy, MT_CH_SEAM_PHASE + (uint32_t)mtIndex);
}

namespace {
// MtSiteGeometry with the per-protofilament (cos, sin theta) and the linker
// shell's cubed radii passed in: DyesInBlock evaluates those once, not per
// site. Same operands in the same order, so the same bits.
SiteGeom SiteGeometryWith(const std::vector<Pt3>& pts, const MtFrames& fr, size_t seg, double S, double ct,
                          double st, double minL3, double maxL3, double r1, double r2, double r3)
{
   const Pt3& t = fr.T[seg]; const Pt3& u = fr.U[seg]; const Pt3& v = fr.V[seg];
   const double f = S - fr.cum[seg];
   const double cx = pts[seg].x + t.x * f, cy = pts[seg].y + t.y * f, cz = pts[seg].z + t.z * f;
   const double rx = ct * u.x + st * v.x, ry = ct * u.y + st * v.y, rz = ct * u.z + st * v.z;
   const double R = MT_RADIUS_NM * NM, B = (MT_RADIUS_NM + MT_BINDER_NM) * NM;
   SiteGeom g;
   g.att = { cx + rx * R, cy + ry * R, cz + rz * R };
   g.tip = { cx + rx * B, cy + ry * B, cz + rz * B };
   // mtDisplaceByLinker: uniform direction, radius uniform in volume.
   const double lu = r1 * 2 - 1, phi = r2 * 2 * jsm::PI, sn = jsm::sqrt(1 - lu * lu);
   const double r = jsm::cbrt(minL3 + (maxL3 - minL3) * r3);
   g.dye = { g.tip.x + r * sn * jsm::cos(phi), g.tip.y + r * sn * jsm::sin(phi), g.tip.z + r * lu };
   return g;
}

constexpr double LINK_MIN_UM = MT_LINKER_MIN_NM * NM, LINK_MAX_UM = MT_LINKER_MAX_NM * NM;
} // namespace

Linkage LegacyLinkage(int structure)
{
   Linkage l;
   if (structure == 0) { l.binderNm = MT_BINDER_NM; l.linkerMinNm = MT_LINKER_MIN_NM; l.linkerMaxNm = MT_LINKER_MAX_NM; }
   else if (structure == 1) { l.linkerMinNm = NPC_LINKER_MIN_NM; l.linkerMaxNm = NPC_LINKER_MAX_NM; }
   return l;
}

Linkage NormLinkage(const Linkage& lk)
{
   auto nz = [](double v) { return v > 0 ? v : 0.0; };
   Linkage o;
   o.binderNm = nz(lk.binderNm);
   o.binderFlexNm = nz(lk.binderFlexNm);
   o.linkerMinNm = nz(lk.linkerMinNm);
   o.linkerMaxNm = std::max(o.linkerMinNm, nz(lk.linkerMaxNm));
   return o;
}

LinkageSetup::LinkageSetup(const Linkage& l) : lk(l)
{
   const double lMin = l.linkerMinNm * NM, lMax = l.linkerMaxNm * NM;
   minL3 = lMin * lMin * lMin;
   maxL3 = lMax * lMax * lMax;
   flexUm = l.binderFlexNm * NM;
}

Pt3 LinkedDye(const Pt3& b, const Pt3& a, double attachNm, const LinkageSetup& L, uint32_t h1, int32_t k, int32_t n,
              double r1, double r2, double r3, LinkageParts* parts)
{
   const Linkage& lk = L.lk;
   const double B = (attachNm + lk.binderNm) * NM;
   double tx = b.x + a.x * B, ty = b.y + a.y * B, tz = b.z + a.z * B;
   if (parts) {
      const double A = attachNm * NM;
      parts->site = { b.x + a.x * A, b.y + a.y * A, b.z + a.z * A };
      parts->rigid = { tx, ty, tz };
   }
   if (lk.binderFlexNm > 0) {
      const double fu = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::BIND_U).a) * 2 - 1;
      const double fphi = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::BIND_PHI).a) * 2 * jsm::PI;
      const double fr = L.flexUm * jsm::cbrt(Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::BIND_R).a));
      const double fs = jsm::sqrt(1 - fu * fu);
      tx += fr * fs * jsm::cos(fphi);
      ty += fr * fs * jsm::sin(fphi);
      tz += fr * fu;
   }
   if (parts) parts->tip = { tx, ty, tz };
   // The linker: uniform direction, radius uniform in volume (mtDisplaceByLinker, webSMLM's displaceByLinker).
   const double lu = r1 * 2 - 1, phi = r2 * 2 * jsm::PI, sn = jsm::sqrt(1 - lu * lu);
   const double r = jsm::cbrt(L.minL3 + (L.maxL3 - L.minL3) * r3);
   return { tx + r * sn * jsm::cos(phi), ty + r * sn * jsm::sin(phi), tz + r * lu };
}

SiteGeom MtSiteGeometry(const std::vector<Pt3>& pts, const MtFrames& fr, size_t seg, double S, double theta,
                        double r1, double r2, double r3)
{
   return SiteGeometryWith(pts, fr, seg, S, jsm::cos(theta), jsm::sin(theta), jsm::pow(LINK_MIN_UM, 3),
                           jsm::pow(LINK_MAX_UM, 3), r1, r2, r3);
}

void DyesInBlock(uint32_t seed, int32_t cx, int32_t cy, int mtIndex, const std::vector<Pt3>& pts, const MtFrames& fr,
                 int blockIndex, double density, double fluorescentFraction, std::vector<Dye>& out,
                 const Linkage& linkage, std::vector<LinkageParts>* parts)
{
   if (pts.size() < 2 || blockIndex < 0) return;
   const double total = fr.Length();
   const double blockNm0 = blockIndex * DYE_BLOCK_UM * 1000;
   const double labelledBelow = std::min(1.0, std::max(0.0, density));
   const double ff = std::min(1.0, std::max(0.0, fluorescentFraction));
   if (blockNm0 * NM >= total || !(labelledBelow > 0) || !(ff > 0)) return;
   const uint32_t h1 = DyeH1(seed, cx, cy, mtIndex);
   const double phase = MtSeamPhase(seed, cx, cy, mtIndex);
   const LinkageSetup L(linkage);
   for (int k = 0; k < MT_N_PROTOFILAMENTS; k++) {
      const double off = MtProtofilamentOffsetNm(k);
      const double theta = MtProtofilamentTheta(phase, k);
      const double ct = jsm::cos(theta), st = jsm::sin(theta);
      // Block membership is decided on sNm = off + 8n alone, so every site
      // lands in exactly one block.
      long n = std::max(0L, (long)std::floor((blockNm0 - off) / MT_DIMER_NM) - 1);
      for (;; n++) {
         const double sNm = off + n * MT_DIMER_NM;
         const double blk = std::floor(sNm / (DYE_BLOCK_UM * 1000));
         if (blk < blockIndex) continue;
         if (blk > blockIndex) break;
         const double S = sNm * NM;
         if (S >= total) break;
         const uint32_t label = Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LABEL).a;
         const double u = Unit(label);
         if (u >= labelledBelow) continue;
         if (ff < 1.0 && Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::FLUOR).a) >= ff) continue;
         const double r1 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U).a);
         const double r2 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI).a);
         const double r3 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R).a);
         // The site: the axis point at arc S, the protofilament's radial direction in the segment's frame.
         const size_t seg = MtSegmentAt(fr, S);
         const Pt3& ft = fr.T[seg]; const Pt3& fu = fr.U[seg]; const Pt3& fv = fr.V[seg];
         const double f = S - fr.cum[seg];
         const Pt3 c = { pts[seg].x + ft.x * f, pts[seg].y + ft.y * f, pts[seg].z + ft.z * f };
         const Pt3 a = { ct * fu.x + st * fv.x, ct * fu.y + st * fv.y, ct * fu.z + st * fv.z };
         LinkageParts lp;
         const Pt3 pos = LinkedDye(c, a, MT_RADIUS_NM, L, h1, k, (int32_t)n, r1, r2, r3, parts ? &lp : nullptr);
         out.push_back({ pos, mtIndex, k, (int32_t)n, label, theta });
         if (parts) parts->push_back(lp);
      }
   }
}

uint32_t DyeH1(uint32_t seed, int32_t cx, int32_t cy, int mtIndex)
{
   return Pcg4d(seed ^ DYE_SALT, (uint32_t)cx, (uint32_t)cy, (uint32_t)mtIndex).a;
}

uint32_t NpcH1(uint32_t seed, int32_t cx, int32_t cy)
{
   return Pcg4d(seed ^ NPC_DYE_SALT, (uint32_t)cx, (uint32_t)cy, 0u).a;
}

void NpcDyesInBlock(uint32_t seed, int32_t cx, int32_t cy, const std::vector<Npc>& pores, const std::vector<int32_t>& ids,
                    const double* sites, double density, double fluorescentFraction, std::vector<NpcDye>& out,
                    const Linkage& linkage, std::vector<LinkageParts>* parts)
{
   const double labelledBelow = std::min(1.0, std::max(0.0, density));
   const double ff = std::min(1.0, std::max(0.0, fluorescentFraction));
   if (!(labelledBelow > 0) || !(ff > 0)) return;
   const uint32_t h1 = NpcH1(seed, cx, cy);
   const LinkageSetup L(linkage);
   double corner[8][2];
   for (int c = 0; c < 8; c++) { corner[c][0] = jsm::cos(c * jsm::PI / 4); corner[c][1] = jsm::sin(c * jsm::PI / 4); }
   for (int32_t pi : ids) {
      const Npc& q = pores[(size_t)pi];
      const int32_t n = q.cand;
      for (int32_t k = 0; k < NPC_SITES; k++) {
         const uint32_t label = Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LABEL).a;
         if (Unit(label) >= labelledBelow) continue;
         if (ff < 1.0 && Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::FLUOR).a) >= ff) continue;
         const double r1 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U).a);
         const double r2 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI).a);
         const double r3 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R).a);
         const double sa = sites[3 * k] * NM, sb = sites[3 * k + 1] * NM, sc = sites[3 * k + 2] * NM;
         const Pt3 at = { q.c.x + sa * q.e1.x + sb * q.e2.x + sc * q.n.x, q.c.y + sa * q.e1.y + sb * q.e2.y + sc * q.n.y,
                          q.c.z + sa * q.e1.z + sb * q.e2.z + sc * q.n.z };
         const double ct = corner[(k & 31) >> 2][0], st = corner[(k & 31) >> 2][1];
         const Pt3 a = { ct * q.e1.x + st * q.e2.x, ct * q.e1.y + st * q.e2.y, ct * q.e1.z + st * q.e2.z };
         LinkageParts lp;
         const Pt3 pos = LinkedDye(at, a, 0, L, h1, k, n, r1, r2, r3, parts ? &lp : nullptr);
         out.push_back({ pos, pi, k, n, label });
         if (parts) parts->push_back(lp);
      }
   }
}

uint32_t LaminH1(uint32_t seed, int32_t cx, int32_t cy)
{
   return Pcg4d(seed ^ LAMIN_DYE_SALT, (uint32_t)cx, (uint32_t)cy, 0u).a;
}

void LaminDyesInBlock(uint32_t seed, int32_t cx, int32_t cy, const std::vector<LaminPoint>& pts, const std::vector<int32_t>& ids,
                      double density, double fluorescentFraction, std::vector<NpcDye>& out, const Linkage& linkage,
                      std::vector<LinkageParts>* parts)
{
   const double labelledBelow = std::min(1.0, std::max(0.0, density));
   const double ff = std::min(1.0, std::max(0.0, fluorescentFraction));
   if (!(labelledBelow > 0) || !(ff > 0)) return;
   const uint32_t h1 = LaminH1(seed, cx, cy);
   const LinkageSetup L(linkage);
   const int32_t k = 0;
   for (int32_t pi : ids) {
      const LaminPoint& q = pts[(size_t)pi];
      const int32_t n = q.cand;
      const uint32_t label = Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LABEL).a;
      if (Unit(label) >= labelledBelow) continue;
      if (ff < 1.0 && Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::FLUOR).a) >= ff) continue;
      const double r1 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U).a);
      const double r2 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI).a);
      const double r3 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R).a);
      const Pt3 a = { -q.n.x, -q.n.y, -q.n.z };
      LinkageParts lp;
      const Pt3 pos = LinkedDye(q.c, a, 0, L, h1, k, n, r1, r2, r3, parts ? &lp : nullptr);
      out.push_back({ pos, pi, k, n, label });
      if (parts) parts->push_back(lp);
   }
}

namespace {
// Log-normal factor with mean 1 and CV cv from two uniforms (Box-Muller);
// sigma and mu (LogNormalParams) are the CV's, computed once per schedule.
struct LogNormalParams { double sigma, mu; };
LogNormalParams LogNormalFor(double cv)
{
   const double s2 = jsm::log(1 + cv * cv);
   return { jsm::sqrt(s2), -s2 / 2 };
}
double LogNormalMean1(const LogNormalParams& ln, double u1, double u2)
{
   const double z = jsm::sqrt(-2 * jsm::log(u1)) * jsm::cos(2 * jsm::PI * u2);
   return jsm::exp(ln.mu + ln.sigma * z);
}

// Inverse-CDF Poisson count of mean m (<= 30) from one uniform, expM =
// exp(-m) passed in so a caller with a fixed m evaluates it once.
long PoissonInverse(double m, double expM, double u)
{
   double p = expM, F = p;
   long c = 0;
   while (u > F && c < 1000) { c++; p *= m / c; F += p; }
   return c;
}

// Poisson count of mean m from one uniform (inverse CDF; a normal
// approximation above m = 30, which a per-second bin rarely reaches).
long PoissonFromUniform(double m, double u, double u2)
{
   if (!(m > 0)) return 0;
   if (m > 30) {
      const double z = jsm::sqrt(-2 * jsm::log(u)) * jsm::cos(2 * jsm::PI * u2);
      return std::max(0L, (long)std::floor(m + jsm::sqrt(m) * z + 0.5));
   }
   return PoissonInverse(m, jsm::exp(-m), u);
}
} // namespace

void DyeSchedule(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, std::vector<Blink>& out, double tMax)
{
   if (!(kin.activationRatePerSec > 0)) return;
   const double t = -jsm::log(Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::ACT).a)) / kin.activationRatePerSec;
   DyeScheduleFrom(h1, k, n, kin, t, out, tMax);
}

void DyeScheduleFrom(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t, std::vector<Blink>& out,
                     double tMax)
{
   auto U = [&](uint32_t ch) { return Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, ch).a); };
   const double pBleach = std::min(1.0, std::max(0.01, kin.bleachProb));
   const double cv = std::max(0.0, kin.photonCV);
   const LogNormalParams ln = LogNormalFor(cv);
   for (int j = 0; j < DYE_MAX_BLINKS; j++) {
      if (t >= tMax) break;
      const uint32_t base = DYE_CH::SCHED0 + (uint32_t)j * DYE_CH::SCHED_STRIDE;
      const double on = -jsm::log(U(base + DYE_CH::ON)) * kin.onSec;
      double b = 1;
      if (cv > 0) {
         const double u1 = U(base + DYE_CH::BRIGHT1);
         const double u2 = U(base + DYE_CH::BRIGHT2);
         b = LogNormalMean1(ln, u1, u2);
      }
      out.push_back({ t, t + on, b });
      if (U(base + DYE_CH::BLEACH) < pBleach) break;
      t += on - jsm::log(U(base + DYE_CH::OFF)) * kin.offSec;
   }
}

size_t KineticsHistory::SegmentAt(double t) const
{
   size_t lo = 0, hi = tStart.size();
   while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if (tStart[mid] <= t) lo = mid; else hi = mid;
   }
   return lo;
}

namespace {
// Spends unit-exponential draws over a history's segments in a dye's local time (absolute time - t0). Within one
// segment the arithmetic is the single-segment schedule's (tau + E / rate, tau + E x scale).
struct HazardWalk {
   const KineticsHistory& h;
   double t0;
   size_t i = 0;   // the segment of the last time synced to (times only increase)
   HazardWalk(const KineticsHistory& hh, double t0In) : h(hh), t0(t0In) {}
   double End(size_t s) const { return s + 1 < h.Size() ? h.tStart[s + 1] - t0 : INFINITY; }
   size_t Sync(double tau)
   {
      while (i + 1 < h.Size() && h.tStart[i + 1] - t0 <= tau) i++;
      return i;
   }
   // When rateOf(segment) integrated from tau reaches E (INFINITY if never).
   template <class Rate> double AfterRate(double tau, double E, Rate rateOf)
   {
      for (size_t s = Sync(tau);; s++) {
         const double a = rateOf(h.kin[s]), end = End(s);
         if (a > 0) {
            const double t = tau + E / a;
            if (!(t >= end)) return t;
            E = std::max(0.0, E - a * (end - tau));
         } else if (end == INFINITY) {
            return INFINITY;
         }
         tau = end;
      }
   }
   // The same for a state of mean duration scaleOf(segment) (rate 1 / scale); a scale <= 0 leaves at once.
   template <class Scale> double AfterScale(double tau, double E, Scale scaleOf)
   {
      for (size_t s = Sync(tau);; s++) {
         const double sc = scaleOf(h.kin[s]), end = End(s);
         if (!(sc > 0)) return tau;
         const double t = tau + E * sc;
         if (!(t >= end)) return t;
         E = std::max(0.0, E - (end - tau) / sc);
         tau = end;
      }
   }
};
double RateAct(const Kinetics& q) { return q.activationRatePerSec; }
double ScaleOn(const Kinetics& q) { return q.onSec; }
double ScaleOff(const Kinetics& q) { return q.offSec; }
double ScaleInitialOn(const Kinetics& q) { return q.initialOnSec; }
} // namespace

void DyeScheduleHistory(uint32_t h1, int32_t k, int32_t n, const KineticsHistory& h, double t0, std::vector<Blink>& out,
                        double tMax)
{
   const double t = HazardWalk(h, t0).AfterRate(0.0, -jsm::log(Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::ACT).a)),
                                                RateAct);
   DyeScheduleHistoryFrom(h1, k, n, h, t0, t, out, tMax);
}

void DyeScheduleHistoryFrom(uint32_t h1, int32_t k, int32_t n, const KineticsHistory& h, double t0, double t,
                            std::vector<Blink>& out, double tMax)
{
   auto U = [&](uint32_t ch) { return Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, ch).a); };
   HazardWalk w(h, t0);
   double lnCv = -1;
   LogNormalParams ln = { 0, 0 };
   for (int j = 0; j < DYE_MAX_BLINKS; j++) {
      if (t >= tMax) break;
      const uint32_t base = DYE_CH::SCHED0 + (uint32_t)j * DYE_CH::SCHED_STRIDE;
      const size_t seg = w.Sync(t);
      const Kinetics& q = h.kin[seg];
      const double end = w.End(seg);
      const double eOn = -jsm::log(U(base + DYE_CH::ON));
      const double on = eOn * q.onSec;
      double b = 1;
      const double cv = std::max(0.0, q.photonCV);
      if (cv > 0) {
         if (cv != lnCv) { ln = LogNormalFor(cv); lnCv = cv; }
         const double u1 = U(base + DYE_CH::BRIGHT1);
         const double u2 = U(base + DYE_CH::BRIGHT2);
         b = LogNormalMean1(ln, u1, u2);
      }
      const bool onInSeg = !(t + on >= end);
      const double tOff = onInSeg ? t + on : w.AfterScale(t, eOn, ScaleOn);
      out.push_back({ t, tOff, b });
      // Bleach after the ON with the probability of the segment it ended in.
      const double pBleach = std::min(1.0, std::max(0.01, h.kin[w.Sync(tOff)].bleachProb));
      if (U(base + DYE_CH::BLEACH) < pBleach) break;
      const double eOff = -jsm::log(U(base + DYE_CH::OFF));
      const double off = eOff * q.offSec;
      if (onInSeg && !(t + (on + off) >= end))
         t += on + off;   // the cycle stays in its segment: DyeSchedule's sum
      else
         t = w.AfterScale(tOff, eOff, ScaleOff);
   }
}

const char* ValidateLabel(const Label& l)
{
   const int mode = (int)l.mode, orient = (int)l.orientation.mode;
   if (mode < 0 || mode > 3) return "label mode is not one of dSTORM, PALM, DNA-PAINT, WideField";
   if (orient < 0 || orient > 2) return "orientation is not one of Free, Fixed, Random";
   if (l.motion != 0) return "motion is not implemented yet (only Static)";
   if (l.offTargetCount != 0) return "off-target binding is not implemented yet (offTarget must be empty)";
   if (l.preState && l.mode != LabelMode::PALM) return "a pre state needs mode PALM";
   return nullptr;
}

bool LabelNotImplemented(const Label& l) { return l.motion != 0 || l.offTargetCount != 0; }

void LabelSchedule(uint32_t h1, int32_t k, int32_t n, const Label& label, std::vector<Blink>* blinks,
                   std::vector<ContWindow>* cont, double tMax, const KineticsHistory* hist)
{
   // Two or more segments: the walk over them; else one set of kinetics from t = 0.
   const KineticsHistory* multi = hist && hist->Size() >= 2 ? hist : nullptr;
   const Kinetics& kin = hist && hist->Size() == 1 ? hist->kin[0] : label.kin;
   auto U = [&](uint32_t ch) { return Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, ch).a); };
   if (label.mode == LabelMode::WideField) {
      if (cont) cont->push_back({ 0.0, INFINITY, STATE_ALWAYS_ON, -jsm::log(U(DYE_CH::AUX)) });
      return;
   }
   if (label.mode == LabelMode::DnaPaint) return;
   double shift = 0.0;
   if (label.mode == LabelMode::dSTORM) {
      if (!multi && kin.initialOnSec > 0)
         shift = -jsm::log(U(DYE_CH::INIT_ON)) * kin.initialOnSec;
      else if (multi && multi->kin[0].initialOnSec > 0)
         shift = HazardWalk(*multi, 0.0).AfterScale(0.0, -jsm::log(U(DYE_CH::INIT_ON)), ScaleInitialOn);
   }
   if (cont && shift != 0) cont->push_back({ 0.0, shift, STATE_INITIAL_ON, 0.0 });
   if (blinks) {
      const size_t first = blinks->size();
      if (multi)
         DyeScheduleHistory(h1, k, n, *multi, shift, *blinks, tMax - shift);
      else
         DyeSchedule(h1, k, n, kin, *blinks, tMax - shift);
      if (shift != 0)
         for (size_t i = first; i < blinks->size(); i++) { (*blinks)[i].tOn += shift; (*blinks)[i].tOff += shift; }
   }
   // The pre state lasts until the first activation: DyeSchedule's first blink time, the ACT draw.
   if (cont && label.mode == LabelMode::PALM && label.preState) {
      double tOff;
      if (multi)
         tOff = HazardWalk(*multi, 0.0).AfterRate(0.0, -jsm::log(U(DYE_CH::ACT)), RateAct);
      else
         tOff = kin.activationRatePerSec > 0 ? -jsm::log(U(DYE_CH::ACT)) / kin.activationRatePerSec : INFINITY;
      cont->push_back({ 0.0, tOff, STATE_PRE, -jsm::log(U(DYE_CH::AUX)) });
   }
}

void LabelScheduleActivated(uint32_t h1, int32_t k, int32_t n, const Label& label, const KineticsHistory* hist,
                            double tAct, std::vector<Blink>* blinks, std::vector<ContWindow>* cont, double tMax)
{
   if (label.mode != LabelMode::PALM) return;
   const KineticsHistory* multi = hist && hist->Size() >= 2 ? hist : nullptr;
   const Kinetics& kin = hist && hist->Size() == 1 ? hist->kin[0] : label.kin;
   if (blinks && std::isfinite(tAct)) {
      if (multi)
         DyeScheduleHistoryFrom(h1, k, n, *multi, 0.0, tAct, *blinks, tMax);
      else
         DyeScheduleFrom(h1, k, n, kin, tAct, *blinks, tMax);
   }
   if (cont && label.preState)
      cont->push_back({ 0.0, tAct, STATE_PRE, -jsm::log(Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::AUX).a)) });
}

double ActivationDraw(uint32_t h1, int32_t k, int32_t n)
{
   return -jsm::log(Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::ACT).a));
}

bool DyeOrientation(uint32_t h1, int32_t k, int32_t n, const Label& label, const MtFrames& fr, size_t seg, double theta,
                    Pt3& dir)
{
   const Orientation& o = label.orientation;
   if (o.mode == OrientationMode::Free) return false;
   if (DyeOrientationRandom(h1, k, n, o, dir)) return true;
   const Pt3& t = fr.T[seg]; const Pt3& u = fr.U[seg]; const Pt3& v = fr.V[seg];
   const double ct = jsm::cos(theta), st = jsm::sin(theta);
   const Pt3 r = { ct * u.x + st * v.x, ct * u.y + st * v.y, ct * u.z + st * v.z };            // radial
   dir = DyeOrientationFixed(o, t, r);
   return true;
}

bool DyeOrientationRandom(uint32_t h1, int32_t k, int32_t n, const Orientation& o, Pt3& dir)
{
   if (o.mode != OrientationMode::Random) return false;
   const double cz = 2 * Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::ORIENT_U).a) - 1;
   const double phi = 2 * jsm::PI * Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::ORIENT_PHI).a);
   const double sz = jsm::sqrt(1 - cz * cz);
   dir = { sz * jsm::cos(phi), sz * jsm::sin(phi), cz };
   return true;
}

Pt3 DyeOrientationFixed(const Orientation& o, const Pt3& t, const Pt3& r)
{
   const Pt3 q = { t.y * r.z - t.z * r.y, t.z * r.x - t.x * r.z, t.x * r.y - t.y * r.x };    // T x r
   const double pol = o.polarDeg * jsm::PI / 180, az = o.azimuthDeg * jsm::PI / 180;
   const double sp = jsm::sin(pol), cp = jsm::cos(pol), ca = jsm::cos(az), sa = jsm::sin(az);
   return { cp * t.x + sp * (ca * r.x + sa * q.x), cp * t.y + sp * (ca * r.y + sa * q.y),
            cp * t.z + sp * (ca * r.z + sa * q.z) };
}

namespace {
// The one generator of persistent blinks: calls emit(bin, j, tOn, on, br)
// for every blink starting in bins [b0, b1]. keep(tOn, on) decides whether a
// blink is wanted before its brightness is drawn (the draws are addressed,
// so skipping them changes no other value).
// A bin's kinetics, derived once per set of kinetics (per site, or per segment under a history).
struct BinKinetics {
   double rate, onSec, cv, maxOn, m, expM;
   bool small;
   LogNormalParams ln;
};
BinKinetics BinKineticsOf(const Kinetics& kin)
{
   BinKinetics p;
   p.rate = kin.activationRatePerSec;
   p.onSec = kin.onSec;
   p.cv = std::max(0.0, kin.photonCV);
   p.ln = LogNormalFor(p.cv);
   p.maxOn = PERSIST_ON_CAP * kin.onSec;
   p.m = p.rate * PERSIST_BIN_SEC;
   // PoissonFromUniform, with exp(-m) once per site and the second uniform
   // (addressed, so skipping it changes nothing) only where it is used.
   p.small = p.m <= 30;
   p.expM = p.small ? jsm::exp(-p.m) : 0.0;
   return p;
}

// hist with two or more segments: bin b's own draws (piece 0) take the kinetics of the segment holding its start
// b x PERSIST_BIN_SEC and keep the blinks starting before the next segment's start; each later segment starting
// inside the bin (piece i) adds its own Poisson draws, at its rate over its part of the bin, on channels
// PIECE_CH x i + purpose, ids j | i << PIECE_SHIFT. So a change acts at once, and what came before it (earlier bins,
// piece 0 up to the change) never changes. One segment in a bin = the single-segment arithmetic.
template <class Keep, class Emit>
void PersistentGen(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, const KineticsHistory* hist, long b0, long b1,
                   Keep keep, Emit emit)
{
   const KineticsHistory* multi = hist && hist->Size() >= 2 ? hist : nullptr;
   const Kinetics& one = hist && hist->Size() == 1 ? hist->kin[0] : kin;
   if (!multi && !(one.activationRatePerSec > 0)) return;
   // Per-site stream key, then per (bin, j, purpose).
   const uint32_t key = Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::PERSIST).a;
   auto U = [&](uint32_t bin, uint32_t j, uint32_t ch) { return Unit(Pcg4d(key, bin, j, ch).a); };
   enum : uint32_t { COUNT = 0, COUNT2 = 1, START = 2, ON = 3, BRIGHT1 = 4, BRIGHT2 = 5, PIECE_CH = 8, PIECE_SHIFT = 24 };
   // Blinks of one piece: c draws starting in [lo, hi) (piece 0: (b + u) x PERSIST_BIN_SEC, the single-segment
   // arithmetic; later ones lo + u (hi - lo)), its kinetics q, channels o + purpose, ids j | id.
   auto piece = [&](uint32_t bin, long c, const BinKinetics& q, uint32_t o, uint32_t id, double lo, double hi) {
      for (long j = 0; j < c; j++) {
         const uint32_t jj = (uint32_t)j;
         const double u = U(bin, jj, o + START);
         const double tOn = o == 0 ? (bin + u) * PERSIST_BIN_SEC : lo + u * (hi - lo);
         if (!(tOn < hi)) continue;
         const double on = std::min(q.maxOn, -jsm::log(U(bin, jj, o + ON)) * q.onSec);
         if (!keep(tOn, on)) continue;
         double br = 1;
         if (q.cv > 0) {
            const double u1 = U(bin, jj, o + BRIGHT1);
            const double u2 = U(bin, jj, o + BRIGHT2);
            br = LogNormalMean1(q.ln, u1, u2);
         }
         emit(bin, jj | id, tOn, on, br);
      }
   };
   size_t seg = multi ? multi->SegmentAt(std::max(0L, b0) * PERSIST_BIN_SEC) : 0;
   BinKinetics p = BinKineticsOf(multi ? multi->kin[seg] : one);
   for (long b = std::max(0L, b0); b <= b1; b++) {
      const uint32_t bin = (uint32_t)b;
      const double binEnd = (b + 1) * PERSIST_BIN_SEC;
      double pEnd = INFINITY;   // piece 0 ends where the next segment starts inside the bin
      if (multi) {
         const size_t s = multi->SegmentAt(b * PERSIST_BIN_SEC);
         if (s != seg) { seg = s; p = BinKineticsOf(multi->kin[s]); }
         if (s + 1 < multi->Size() && multi->tStart[s + 1] < binEnd) pEnd = multi->tStart[s + 1];
      }
      if (p.rate > 0) {
         const long c = p.small ? PoissonInverse(p.m, p.expM, U(bin, 0, COUNT))
                                : PoissonFromUniform(p.m, U(bin, 0, COUNT), U(bin, 0, COUNT2));
         piece(bin, c, p, 0, 0, b * PERSIST_BIN_SEC, pEnd);
      }
      if (!(pEnd < binEnd)) continue;
      uint32_t i = 1;
      for (size_t s = seg + 1; s < multi->Size() && multi->tStart[s] < binEnd; s++, i++) {
         const double a = multi->tStart[s];
         const double e = s + 1 < multi->Size() && multi->tStart[s + 1] < binEnd ? multi->tStart[s + 1] : binEnd;
         const BinKinetics q = BinKineticsOf(multi->kin[s]);
         if (!(e > a) || !(q.rate > 0)) continue;
         const double m = q.rate * (e - a);
         const uint32_t o = PIECE_CH * i;
         const long c = m <= 30 ? PoissonInverse(m, jsm::exp(-m), U(bin, 0, o + COUNT))
                                : PoissonFromUniform(m, U(bin, 0, o + COUNT), U(bin, 0, o + COUNT2));
         piece(bin, c, q, o, i << PIECE_SHIFT, a, e);
      }
   }
}
} // namespace

double PersistentMaxOn(const Kinetics& kin, const KineticsHistory* hist)
{
   if (!hist || !hist->Size()) return PERSIST_ON_CAP * kin.onSec;
   double m = 0;
   for (const Kinetics& q : hist->kin) m = std::max(m, PERSIST_ON_CAP * q.onSec);
   return m;
}

void PersistentBlinks(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t0, double t1,
                      std::vector<Blink>& out, const KineticsHistory* hist)
{
   if (!(t1 > t0)) return;
   const double maxOn = PersistentMaxOn(kin, hist);
   const long b0 = std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC));
   const long b1 = (long)std::floor(t1 / PERSIST_BIN_SEC);
   PersistentGen(h1, k, n, kin, hist, b0, b1,
                 [&](double tOn, double on) { return tOn < t1 && tOn + on > t0; },
                 [&](uint32_t, uint32_t, double tOn, double on, double br) { out.push_back({ tOn, tOn + on, br }); });
}

void PersistentBlinksInBins(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, long binLo, long binHi,
                            std::vector<BinBlink>& out, const KineticsHistory* hist)
{
   PersistentGen(h1, k, n, kin, hist, binLo, binHi, [](double, double) { return true; },
                 [&](uint32_t bin, uint32_t j, double tOn, double on, double br) {
                    out.push_back({ tOn, tOn + on, br, bin, j });
                 });
}

} // namespace isc
