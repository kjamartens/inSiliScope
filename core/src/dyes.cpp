#include "dyes.h"

#include "jsmath.h"

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

SiteGeom MtSiteGeometry(const std::vector<Pt3>& pts, const MtFrames& fr, size_t seg, double S, double theta,
                        double r1, double r2, double r3)
{
   const double ct = jsm::cos(theta), st = jsm::sin(theta);
   const Pt3& t = fr.T[seg]; const Pt3& u = fr.U[seg]; const Pt3& v = fr.V[seg];
   const double f = S - fr.cum[seg];
   const double cx = pts[seg].x + t.x * f, cy = pts[seg].y + t.y * f, cz = pts[seg].z + t.z * f;
   const double rx = ct * u.x + st * v.x, ry = ct * u.y + st * v.y, rz = ct * u.z + st * v.z;
   const double R = MT_RADIUS_NM * NM, B = (MT_RADIUS_NM + MT_BINDER_NM) * NM;
   SiteGeom g;
   g.att = { cx + rx * R, cy + ry * R, cz + rz * R };
   g.tip = { cx + rx * B, cy + ry * B, cz + rz * B };
   // mtDisplaceByLinker: uniform direction, radius uniform in volume.
   const double minL = MT_LINKER_MIN_NM * NM, maxL = MT_LINKER_MAX_NM * NM;
   const double lu = r1 * 2 - 1, phi = r2 * 2 * jsm::PI, sn = jsm::sqrt(1 - lu * lu);
   const double r = jsm::cbrt(jsm::pow(minL, 3) + (jsm::pow(maxL, 3) - jsm::pow(minL, 3)) * r3);
   g.dye = { g.tip.x + r * sn * jsm::cos(phi), g.tip.y + r * sn * jsm::sin(phi), g.tip.z + r * lu };
   return g;
}

void DyesInBlock(uint32_t seed, int32_t cx, int32_t cy, int mtIndex, const std::vector<Pt3>& pts, const MtFrames& fr,
                 int blockIndex, double efficiency, double persistentEfficiency, std::vector<Dye>& out)
{
   if (pts.size() < 2 || blockIndex < 0) return;
   const double total = fr.Length();
   const double blockNm0 = blockIndex * DYE_BLOCK_UM * 1000;
   efficiency = std::min(1.0, std::max(0.0, efficiency));
   const double labelledBelow = std::min(1.0, efficiency + std::max(0.0, persistentEfficiency));
   if (blockNm0 * NM >= total || !(labelledBelow > 0)) return;
   const uint32_t h1 = DyeH1(seed, cx, cy, mtIndex);
   const double phase = MtSeamPhase(seed, cx, cy, mtIndex);
   for (int k = 0; k < MT_N_PROTOFILAMENTS; k++) {
      const double off = MtProtofilamentOffsetNm(k);
      const double theta = MtProtofilamentTheta(phase, k);
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
         const double r1 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U).a);
         const double r2 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI).a);
         const double r3 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R).a);
         const SiteGeom g = MtSiteGeometry(pts, fr, MtSegmentAt(fr, S), S, theta, r1, r2, r3);
         out.push_back({ g.dye, mtIndex, k, (int32_t)n, label, u >= efficiency });
      }
   }
}

uint32_t DyeH1(uint32_t seed, int32_t cx, int32_t cy, int mtIndex)
{
   return Pcg4d(seed ^ DYE_SALT, (uint32_t)cx, (uint32_t)cy, (uint32_t)mtIndex).a;
}

namespace {
// Log-normal factor with mean 1 and CV cv from two uniforms (Box-Muller).
double LogNormalMean1(double cv, double u1, double u2)
{
   const double s2 = jsm::log(1 + cv * cv), sigma = jsm::sqrt(s2), mu = -s2 / 2;
   const double z = jsm::sqrt(-2 * jsm::log(u1)) * jsm::cos(2 * jsm::PI * u2);
   return jsm::exp(mu + sigma * z);
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
   double p = jsm::exp(-m), F = p;
   long c = 0;
   while (u > F && c < 1000) { c++; p *= m / c; F += p; }
   return c;
}
} // namespace

void DyeSchedule(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, std::vector<Blink>& out)
{
   if (!(kin.activationRatePerSec > 0)) return;
   auto U = [&](uint32_t ch) { return Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, ch).a); };
   const double pBleach = std::min(1.0, std::max(0.01, kin.bleachProb));
   const double cv = std::max(0.0, kin.photonCV);
   double t = -jsm::log(U(DYE_CH::ACT)) / kin.activationRatePerSec;
   for (int j = 0; j < DYE_MAX_BLINKS; j++) {
      const uint32_t base = DYE_CH::SCHED0 + (uint32_t)j * DYE_CH::SCHED_STRIDE;
      const double on = -jsm::log(U(base + DYE_CH::ON)) * kin.onSec;
      double b = 1;
      if (cv > 0) {
         const double u1 = U(base + DYE_CH::BRIGHT1);
         const double u2 = U(base + DYE_CH::BRIGHT2);
         b = LogNormalMean1(cv, u1, u2);
      }
      out.push_back({ t, t + on, b });
      if (U(base + DYE_CH::BLEACH) < pBleach) break;
      t += on - jsm::log(U(base + DYE_CH::OFF)) * kin.offSec;
   }
}

void PersistentBlinks(uint32_t h1, int32_t k, int32_t n, const Kinetics& kin, double t0, double t1,
                      std::vector<Blink>& out)
{
   const double rate = kin.activationRatePerSec;
   if (!(rate > 0) || !(t1 > t0)) return;
   // Per-site stream key, then per (bin, j, purpose).
   const uint32_t key = Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::PERSIST).a;
   auto U = [&](uint32_t bin, uint32_t j, uint32_t ch) { return Unit(Pcg4d(key, bin, j, ch).a); };
   enum : uint32_t { COUNT = 0, COUNT2 = 1, START = 2, ON = 3, BRIGHT1 = 4, BRIGHT2 = 5 };
   const double cv = std::max(0.0, kin.photonCV);
   const double maxOn = PERSIST_ON_CAP * kin.onSec;
   const double m = rate * PERSIST_BIN_SEC;
   const long b0 = std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC));
   const long b1 = (long)std::floor(t1 / PERSIST_BIN_SEC);
   for (long b = b0; b <= b1; b++) {
      const uint32_t bin = (uint32_t)b;
      const long c = PoissonFromUniform(m, U(bin, 0, COUNT), U(bin, 0, COUNT2));
      for (long j = 0; j < c; j++) {
         const uint32_t jj = (uint32_t)j;
         const double tOn = (b + U(bin, jj, START)) * PERSIST_BIN_SEC;
         const double on = std::min(maxOn, -jsm::log(U(bin, jj, ON)) * kin.onSec);
         if (!(tOn < t1) || !(tOn + on > t0)) continue;
         double br = 1;
         if (cv > 0) {
            const double u1 = U(bin, jj, BRIGHT1);
            const double u2 = U(bin, jj, BRIGHT2);
            br = LogNormalMean1(cv, u1, u2);
         }
         out.push_back({ tOn, tOn + on, br });
      }
   }
}

} // namespace isc
