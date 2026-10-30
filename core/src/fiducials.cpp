// Fiducial beads: the field (see fiducials.h). BSD-3-Clause.
#include "fiducials.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>

namespace isc {

namespace {
// Tiles kept before the cache starts afresh.
constexpr size_t kTileCacheMax = 65536;

inline double Unit(uint32_t a) { return ((double)(a >> 9) + 0.5) * 1.1920928955078125e-7; }
inline uint32_t PcgA(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return Pcg4d(a, b, c, d).a; }
// JS Math.min(hi, Math.max(lo, v)) for a finite v (NaN: lo).
inline double Clamp(double v, double lo, double hi) { return std::isnan(v) ? lo : std::min(hi, std::max(lo, v)); }

int PoissonInverse(double m, double u)
{
   double p = jsm::exp(-m), F = p;
   int c = 0;
   while (u > F && c < FID_MAX_PER_TILE) {
      c++;
      p *= m / c;
      F += p;
   }
   return c;
}
} // namespace

void NormalizeFiducialParams(Params& p)
{
   p.fidDensity = Clamp(p.fidDensity, 0, 100);
   p.fidDiameterNm = Clamp(p.fidDiameterNm, 10, 2000);
   p.fidDiameterCvPct = Clamp(p.fidDiameterCvPct, 0, 50);
   p.fidHeight = std::trunc(Clamp(p.fidHeight, 0, FID_HEIGHT_COUNT - 1));
   p.fidLayerUm = Clamp(p.fidLayerUm, 0, 1000);
   p.fidAggregationPct = Clamp(p.fidAggregationPct, 0, FID_MAX_AGGREGATION_PCT);
}

FiducialField::FiducialField(uint32_t seed, const Params& p, std::function<double(double, double)> surfaceAt)
   : seed_(seed), p_(p), surfaceAt_(std::move(surfaceAt))
{
   NormalizeFiducialParams(p_);
}

const std::vector<FiducialBead>& FiducialField::Tile(int32_t tx, int32_t ty)
{
   const auto key = std::make_pair(tx, ty);
   auto it = tiles_.find(key);
   if (it != tiles_.end()) return it->second;
   if (tiles_.size() > kTileCacheMax) tiles_.clear();
   std::vector<FiducialBead> out;
   const uint32_t s = seed_ ^ FID_SALT, sc = seed_ ^ FID_CLUMP_SALT;
   const double p = p_.fidAggregationPct / 100.0;
   const double T = FID_TILE_UM, lambda = p_.fidDensity * (1.0 - p) * T * T;
   const int count = lambda > 0 ? PoissonInverse(lambda, Unit(PcgA(s, (uint32_t)tx, (uint32_t)ty, FID_CH_COUNT))) : 0;
   const double cv = p_.fidDiameterCvPct / 100.0;
   const int mode = (int)p_.fidHeight;
   auto diameter = [&](uint32_t u1, uint32_t u2) {
      const double g = std::sqrt(-2.0 * jsm::log(Unit(u1))) * jsm::cos(2.0 * jsm::PI * Unit(u2));
      return p_.fidDiameterNm * std::max(0.1, 1.0 + cv * g);
   };
   for (int i = 0; i < count; i++) {
      const Pcg4dOut w = Pcg4d(s, (uint32_t)tx, (uint32_t)ty, FID_CH_CAND0 + 2u * (uint32_t)i);
      const double x = (tx + Unit(w.a)) * T, y = (ty + Unit(w.b)) * T;
      const double d = diameter(w.c, w.d), r = d / 2000.0;
      double z = r;
      if (mode == FID_HEIGHT_ON_TOP) z = surfaceAt_(x, y) + r;
      else if (mode == FID_HEIGHT_LAYER) {
         const double u = Unit(PcgA(s, (uint32_t)tx, (uint32_t)ty, FID_CH_CAND0 + 2u * (uint32_t)i + 1u));
         z = p_.fidLayerUm > 2.0 * r ? r + u * (p_.fidLayerUm - 2.0 * r) : r;
         if (z < surfaceAt_(x, y) + r) continue;   // inside a cell
      }
      const size_t first = out.size();
      out.push_back({ tx, ty, i, x, y, z, d });
      if (p <= 0) continue;
      // The clump: n - 1 more beads, each touching one placed before (fiducials.h).
      const uint32_t base = (uint32_t)FID_MAX_CLUMP * (uint32_t)i;
      const int n = std::min(FID_MAX_CLUMP, 1 + (int)std::floor(jsm::log(Unit(PcgA(sc, (uint32_t)tx, (uint32_t)ty, base))) /
                                                                 jsm::log(p)));
      for (int j = 1; j < n; j++) {
         for (int t = 0; t < FID_TRIES; t++) {
            const uint32_t ch = 2u * ((uint32_t)FID_TRIES * (base + (uint32_t)j) + (uint32_t)t);
            const Pcg4dOut a = Pcg4d(sc, (uint32_t)tx, (uint32_t)ty, ch), b = Pcg4d(sc, (uint32_t)tx, (uint32_t)ty, ch + 1u);
            const FiducialBead& k = out[first + std::min(out.size() - first - 1, (size_t)(Unit(a.a) * (double)(out.size() - first)))];
            const double dj = diameter(b.a, b.b), rk = k.d / 2000.0, rj = dj / 2000.0, dist = rk + rj;
            const double phi = 2.0 * jsm::PI * Unit(a.b);
            // In the plane (Coverslip, OnTop) the two sit at their radii, rk - rj apart in height: the centres touch
            // at sqrt(dist^2 - (rk - rj)^2) = 2 sqrt(rk rj) apart in x, y. Layer: on the sphere of radius dist.
            double cz = 0.0, rho = 2.0 * std::sqrt(rk * rj);
            if (mode == FID_HEIGHT_LAYER) {
               cz = 2.0 * Unit(a.c) - 1.0;
               rho = dist * std::sqrt(std::max(0.0, 1.0 - cz * cz));
            }
            const double bx = k.x + rho * jsm::cos(phi), by = k.y + rho * jsm::sin(phi);
            if (std::fabs(bx - x) > T || std::fabs(by - y) > T) continue;
            double bz = rj;
            if (mode == FID_HEIGHT_ON_TOP) bz = surfaceAt_(bx, by) + rj;
            else if (mode == FID_HEIGHT_LAYER) {
               const double lo = surfaceAt_(bx, by) + rj, hi = std::max(lo, p_.fidLayerUm - rj);
               bz = k.z + dist * cz;
               if (bz < lo || bz > hi) bz = k.z - dist * cz;   // the mirror direction
               if (bz < lo || bz > hi) continue;
            }
            bool clear = true;
            for (size_t q = first; q < out.size() && clear; q++) {
               const double ex = bx - out[q].x, ey = by - out[q].y, ez = bz - out[q].z, m = 0.999 * (out[q].d / 2000.0 + rj);
               clear = ex * ex + ey * ey + ez * ez >= m * m;
            }
            if (!clear) continue;
            out.push_back({ tx, ty, i, bx, by, bz, dj });
            break;
         }
      }
   }
   return tiles_.emplace(key, std::move(out)).first->second;
}

void FiducialField::InRect(double x0, double y0, double x1, double y1, std::vector<FiducialBead>& out)
{
   if (!On()) return;
   const double T = FID_TILE_UM;
   const int32_t pad = p_.fidAggregationPct > 0 ? 1 : 0;   // a clump's beads lie within one tile of its first
   const int32_t tx0 = (int32_t)std::floor(x0 / T) - pad, tx1 = (int32_t)std::floor(x1 / T) + pad;
   const int32_t ty0 = (int32_t)std::floor(y0 / T) - pad, ty1 = (int32_t)std::floor(y1 / T) + pad;
   std::lock_guard<std::mutex> lock(mu_);
   for (int32_t tx = tx0; tx <= tx1; tx++)
      for (int32_t ty = ty0; ty <= ty1; ty++)
         for (const FiducialBead& b : Tile(tx, ty))
            if (b.x >= x0 && b.x < x1 && b.y >= y0 && b.y < y1) out.push_back(b);
}

} // namespace isc
