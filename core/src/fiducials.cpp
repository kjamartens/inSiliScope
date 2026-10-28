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
   const uint32_t s = seed_ ^ FID_SALT;
   const double T = FID_TILE_UM, lambda = p_.fidDensity * T * T;
   const int count = lambda > 0 ? PoissonInverse(lambda, Unit(PcgA(s, (uint32_t)tx, (uint32_t)ty, FID_CH_COUNT))) : 0;
   const double cv = p_.fidDiameterCvPct / 100.0;
   const int mode = (int)p_.fidHeight;
   for (int i = 0; i < count; i++) {
      const Pcg4dOut w = Pcg4d(s, (uint32_t)tx, (uint32_t)ty, FID_CH_CAND0 + 2u * (uint32_t)i);
      const double x = (tx + Unit(w.a)) * T, y = (ty + Unit(w.b)) * T;
      const double g = std::sqrt(-2.0 * jsm::log(Unit(w.c))) * jsm::cos(2.0 * jsm::PI * Unit(w.d));
      const double d = p_.fidDiameterNm * std::max(0.1, 1.0 + cv * g), r = d / 2000.0;
      double z = r;
      if (mode == FID_HEIGHT_ON_TOP) z = surfaceAt_(x, y) + r;
      else if (mode == FID_HEIGHT_LAYER) {
         const double u = Unit(PcgA(s, (uint32_t)tx, (uint32_t)ty, FID_CH_CAND0 + 2u * (uint32_t)i + 1u));
         z = p_.fidLayerUm > 2.0 * r ? r + u * (p_.fidLayerUm - 2.0 * r) : r;
         if (z < surfaceAt_(x, y) + r) continue;   // inside a cell
      }
      out.push_back({ tx, ty, i, x, y, z, d });
   }
   return tiles_.emplace(key, std::move(out)).first->second;
}

void FiducialField::InRect(double x0, double y0, double x1, double y1, std::vector<FiducialBead>& out)
{
   if (!On()) return;
   const double T = FID_TILE_UM;
   const int32_t tx0 = (int32_t)std::floor(x0 / T), tx1 = (int32_t)std::floor(x1 / T);
   const int32_t ty0 = (int32_t)std::floor(y0 / T), ty1 = (int32_t)std::floor(y1 / T);
   std::lock_guard<std::mutex> lock(mu_);
   for (int32_t tx = tx0; tx <= tx1; tx++)
      for (int32_t ty = ty0; ty <= ty1; ty++)
         for (const FiducialBead& b : Tile(tx, ty))
            if (b.x >= x0 && b.x < x1 && b.y >= y0 && b.y < y1) out.push_back(b);
}

} // namespace isc
