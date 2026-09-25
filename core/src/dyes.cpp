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
                 int blockIndex, double efficiency, std::vector<Dye>& out)
{
   if (pts.size() < 2 || blockIndex < 0) return;
   const double total = fr.Length();
   const double blockNm0 = blockIndex * DYE_BLOCK_UM * 1000;
   if (blockNm0 * NM >= total || !(efficiency > 0)) return;
   const uint32_t h1 = Pcg4d(seed ^ DYE_SALT, (uint32_t)cx, (uint32_t)cy, (uint32_t)mtIndex).a;
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
         if (Unit(label) >= efficiency) continue;
         const double r1 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U).a);
         const double r2 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI).a);
         const double r3 = Unit(Pcg4d(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R).a);
         const SiteGeom g = MtSiteGeometry(pts, fr, MtSegmentAt(fr, S), S, theta, r1, r2, r3);
         out.push_back({ g.dye, mtIndex, k, (int32_t)n, label });
      }
   }
}

} // namespace isc
