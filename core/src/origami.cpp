// DNA-origami nanorulers: the port of web/prototype/scope/origami.js (see origami.h). BSD-3-Clause.
#include "origami.h"

#include "dyes.h"
#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>

namespace isc {

namespace {
#include "origami_designs.inc"

constexpr int kDesignCount = (int)(sizeof kOrigamiDesigns / sizeof kOrigamiDesigns[0]);
// Tiles kept before the caches start afresh (a few hundred bytes each).
constexpr size_t kTileCacheMax = 65536;

inline double Unit(uint32_t a) { return ((double)(a >> 9) + 0.5) * 1.1920928955078125e-7; }
inline uint32_t PcgA(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return Pcg4d(a, b, c, d).a; }
// JS Math.min(hi, Math.max(lo, v)) for a finite v (NaN: lo).
inline double Clamp(double v, double lo, double hi) { return std::isnan(v) ? lo : std::min(hi, std::max(lo, v)); }

// Displacement after arc length s from angle theta0 with curvature kappa (an exact circular arc; _arc).
inline void Arc(double theta0, double kappa, double s, double& dx, double& dy)
{
   const double half = kappa * s / 2.0;
   const double chord = half == 0 ? s : s * (jsm::sin(half) / half);
   const double mid = theta0 + half;
   dx = chord * jsm::cos(mid);
   dy = chord * jsm::sin(mid);
}

int PoissonInverse(double m, double u)
{
   double p = jsm::exp(-m), F = p;
   int c = 0;
   while (u > F && c < ORIGAMI_MAX_PER_TILE) {
      c++;
      p *= m / c;
      F += p;
   }
   return c;
}
} // namespace

int OrigamiDesignCount() { return kDesignCount; }
const OrigamiDesign& OrigamiDesignAt(int i) { return kOrigamiDesigns[std::min(kDesignCount - 1, std::max(0, i))]; }

void NormalizeOrigamiParams(Params& p)
{
   p.specimen = std::trunc(Clamp(p.specimen, 0, SPECIMEN_COUNT - 1));
   p.origamiDesign = std::trunc(Clamp(p.origamiDesign, 0, kDesignCount - 1));
   const OrigamiDesign& d = kOrigamiDesigns[(int)p.origamiDesign];
   p.origamiDensity = Clamp(p.origamiDensity, 0, 50);
   p.origamiMinSpacingNm = Clamp(p.origamiMinSpacingNm, 0, ORIGAMI_TILE_UM * 1000);
   p.origamiBend = p.origamiBend != 0 ? 1 : 0;
   p.origamiLpNm = Clamp(p.origamiLpNm, 1, 1e12);
   p.origamiStrandsPerSpot = std::trunc(Clamp(p.origamiStrandsPerSpot, 0, d.maxRingN));
   p.origamiAttachNm = Clamp(p.origamiAttachNm, 0, 1000);
   p.origamiLinkerNm = Clamp(p.origamiLinkerNm, 0, 100);
   p.origamiRoll = p.origamiRoll != 0 ? 1 : 0;
   p.origamiHeights = p.origamiHeights != 0 ? 1 : 0;
}

// ---- molecule shape (origami_bend.py BentShape, frame "aligned") ----

double OrigamiBendNormal(uint32_t h1, int i)
{
   const Pcg4dOut w = Pcg4d(h1, ORIGAMI_K_BEND, (uint32_t)i >> 1, 0);
   const double r = jsm::sqrt(-2.0 * jsm::log(Unit(w.a)));
   const double t = 2.0 * jsm::PI * Unit(w.b);
   return (i & 1) ? r * jsm::sin(t) : r * jsm::cos(t);
}

OrigamiShape OrigamiBentShape(const OrigamiDesign& d, const Params& p, uint32_t h1, const double* normals)
{
   OrigamiShape S;
   const double L = d.lengthNm, W = d.diameterNm;
   const int n = std::max(ORIGAMI_MIN_SEGMENTS, (int)std::ceil(L / d.segmentNm));
   const double ds = L / n;
   const bool bend = p.origamiBend != 0 && d.deformation == ORIGAMI_WLC2D;
   const double sd = bend ? jsm::sqrt(ds / p.origamiLpNm) : 0.0;
   S.theta.assign((size_t)n + 1, 0.0);
   S.kappa.assign((size_t)n, 0.0);
   S.nx.assign((size_t)n + 1, 0.0);
   S.ny.assign((size_t)n + 1, 0.0);
   double maxAbsKappa = 0.0;
   for (int i = 0; i < n; i++) {
      const double z = bend ? (normals ? normals[i] : OrigamiBendNormal(h1, i)) : 0.0;
      const double dtheta = z * sd;
      S.kappa[(size_t)i] = dtheta / ds;
      S.theta[(size_t)i + 1] = S.theta[(size_t)i] + dtheta;
      maxAbsKappa = std::max(maxAbsKappa, std::fabs(S.kappa[(size_t)i]));
   }
   for (int i = 0; i < n; i++) {
      double dx, dy;
      Arc(S.theta[(size_t)i], S.kappa[(size_t)i], ds, dx, dy);
      S.nx[(size_t)i + 1] = S.nx[(size_t)i] + dx;
      S.ny[(size_t)i + 1] = S.ny[(size_t)i] + dy;
   }
   S.L = L;
   S.W = W;
   S.n = n;
   S.ds = ds;
   S.phi = -jsm::atan2(S.ny[(size_t)n] - S.ny[0], S.nx[(size_t)n] - S.nx[0]);
   S.cphi = jsm::cos(S.phi);
   S.sphi = jsm::sin(S.phi);
   S.ax = (S.nx[0] + S.nx[(size_t)n]) / 2;
   S.ay = (S.ny[0] + S.ny[(size_t)n]) / 2;
   S.endAngle = S.theta[(size_t)n] - S.theta[0];
   S.fold = maxAbsKappa * W / 2 >= 1;
   return S;
}

void OrigamiCenterline(const OrigamiShape& S, double x, double& X, double& Y, double& th)
{
   const double u = x + S.L / 2;
   const int i = (int)std::min((double)(S.n - 1), std::max(0.0, std::floor(u / S.ds)));
   const double s = u - i * S.ds;   // may run past an end: extrapolates
   const double th0 = S.theta[(size_t)i], k = S.kappa[(size_t)i];
   double dx, dy;
   Arc(th0, k, s, dx, dy);
   const double qx = S.nx[(size_t)i] + dx - S.ax, qy = S.ny[(size_t)i] + dy - S.ay;
   X = S.cphi * qx - S.sphi * qy;
   Y = S.sphi * qx + S.cphi * qy;
   th = th0 + k * s + S.phi;
}

void OrigamiTransform(const OrigamiShape& S, double x, double y, double& X, double& Y)
{
   double cx, cy, th;
   OrigamiCenterline(S, x, cx, cy, th);
   X = cx - y * jsm::sin(th);
   Y = cy + y * jsm::cos(th);
}

std::vector<double> OrigamiShapeOutline(const OrigamiShape& S, int nPerSide)
{
   const double L = S.L, W = S.W;
   const int nEnd = std::max(2, nPerSide >> 2);
   std::vector<double> out;
   out.reserve((size_t)(2 * (nPerSide + nEnd)) * 2);
   auto push = [&](double x, double y) {
      double X, Y;
      OrigamiTransform(S, x, y, X, Y);
      out.push_back(X);
      out.push_back(Y);
   };
   for (int j = 0; j < nPerSide; j++) push(-L / 2 + L * j / (nPerSide - 1), W / 2);
   for (int j = 0; j < nEnd; j++) push(L / 2, W / 2 - W * j / (nEnd - 1));
   for (int j = 0; j < nPerSide; j++) push(L / 2 - L * j / (nPerSide - 1), -W / 2);
   for (int j = 0; j < nEnd; j++) push(-L / 2, -W / 2 + W * j / (nEnd - 1));
   return out;
}

uint32_t OrigamiMoleculeH1(uint32_t seed, int32_t tx, int32_t ty, int32_t i)
{
   return PcgA(seed ^ ORIGAMI_DYE_SALT, (uint32_t)tx, (uint32_t)ty, (uint32_t)i);
}

double OrigamiMoleculeRoll(const Params& p, uint32_t h1)
{
   if (p.origamiRoll != 0) return p.origamiRollDeg * jsm::PI / 180.0;
   return 2.0 * jsm::PI * Unit(PcgA(h1, ORIGAMI_K_LAND, 0, 0));
}

void OrigamiMoleculeSites(const OrigamiDesign& d, const Params& p, const OrigamiMolecule& m, const OrigamiShape& S,
                          double density, double fluorescentFraction, std::vector<OrigamiSite>& out, bool withAll)
{
   const double labelledBelow = std::min(1.0, std::max(0.0, density));
   const double ff = std::min(1.0, std::max(0.0, fluorescentFraction));
   if (!withAll && (!(labelledBelow > 0) || !(ff > 0))) return;
   const uint32_t h1 = m.h1;
   const double roll = OrigamiMoleculeRoll(p, h1), Lk = p.origamiLinkerNm;
   const double cy = jsm::cos(m.yaw), sy = jsm::sin(m.yaw);
   const double zAxisNm = p.origamiAttachNm + d.diameterNm / 2;
   for (int k = 0; k < d.nSpots; k++) {
      const OrigamiSpot& sp = d.spots[k];
      const int nOn = (int)std::min((double)sp.ringN, p.origamiStrandsPerSpot);
      for (int n = 0; n < nOn; n++) {
         const uint32_t label = PcgA(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LABEL);
         if (!withAll) {
            if (Unit(label) >= labelledBelow) continue;
            if (ff < 1.0 && Unit(PcgA(h1, (uint32_t)k, (uint32_t)n, DYE_CH::FLUOR)) >= ff) continue;
         }
         const double a = roll + sp.phaseDeg * jsm::PI / 180.0 + 2.0 * jsm::PI * n / sp.ringN;
         const double ca = jsm::cos(a), sa = jsm::sin(a);
         // Linker: the dye uniform in the outward half-ball of radius Lk on the surface point.
         const double cu = Unit(PcgA(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_U));
         const double ph = 2.0 * jsm::PI * Unit(PcgA(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_PHI));
         const double rr = Lk * jsm::cbrt(Unit(PcgA(h1, (uint32_t)k, (uint32_t)n, DYE_CH::LINK_R)));
         const double sn = jsm::sqrt(1.0 - cu * cu);
         const double dn = rr * cu, d1 = rr * sn * jsm::cos(ph), d2 = rr * sn * jsm::sin(ph);
         // Frame at the surface point: normal (0, ca, sa), along the axis (1, 0, 0), around it (0, -sa, ca).
         const double x = sp.atNm - d.lengthNm / 2 + d1;
         const double y = sp.radiusNm * ca + dn * ca - d2 * sa;
         const double h = sp.radiusNm * sa + dn * sa + d2 * ca;
         double X, Y;
         OrigamiTransform(S, x, y, X, Y);
         const double zNm = p.origamiHeights != 0 ? zAxisNm + h : zAxisNm;
         OrigamiSite s;
         s.k = k;
         s.n = n;
         s.id = label;
         s.rest[0] = x; s.rest[1] = y; s.rest[2] = h;
         s.bent[0] = X; s.bent[1] = Y;
         s.x = m.x + (X * cy - Y * sy) / 1000.0;
         s.y = m.y + (X * sy + Y * cy) / 1000.0;
         s.z = zNm / 1000.0;
         out.push_back(s);
      }
   }
}

// ---- the field ----

OrigamiField::OrigamiField(uint32_t seed, const Params& p) : seed_(seed), p_(p)
{
   NormalizeOrigamiParams(p_);
   d_ = &kOrigamiDesigns[(int)p_.origamiDesign];
}

double OrigamiField::ReachUm() const { return (d_->lengthNm / 2 + d_->diameterNm / 2 + p_.origamiLinkerNm) / 1000.0; }
double OrigamiField::MaxHeightUm() const { return (p_.origamiAttachNm + d_->diameterNm + p_.origamiLinkerNm) / 1000.0; }

const std::vector<OrigamiField::Candidate>& OrigamiField::Candidates(int32_t tx, int32_t ty)
{
   const auto key = std::make_pair(tx, ty);
   auto it = cands_.find(key);
   if (it != cands_.end()) return it->second;
   std::vector<Candidate> out;
   const uint32_t s = seed_ ^ ORIGAMI_SALT;
   const double lambda = p_.origamiDensity * ORIGAMI_TILE_UM * ORIGAMI_TILE_UM;
   const int count = lambda > 0 ? PoissonInverse(lambda, Unit(PcgA(s, (uint32_t)tx, (uint32_t)ty, ORIGAMI_CH_COUNT))) : 0;
   out.reserve((size_t)count);
   for (int i = 0; i < count; i++) {
      const Pcg4dOut w = Pcg4d(s, (uint32_t)tx, (uint32_t)ty, ORIGAMI_CH_CAND0 + (uint32_t)i);
      out.push_back({ tx, ty, i, (tx + Unit(w.a)) * ORIGAMI_TILE_UM, (ty + Unit(w.b)) * ORIGAMI_TILE_UM, w.c,
                      2.0 * jsm::PI * Unit(w.d) });
   }
   return cands_.emplace(key, std::move(out)).first->second;
}

const std::vector<OrigamiMolecule>& OrigamiField::TileMolecules(int32_t tx, int32_t ty)
{
   const auto key = std::make_pair(tx, ty);
   auto it = tiles_.find(key);
   if (it != tiles_.end()) return it->second;
   // a beats b: a higher priority; ties by address, the later address winning.
   auto beats = [](const Candidate& a, const Candidate& b) {
      return a.prio != b.prio ? a.prio > b.prio : a.ty != b.ty ? a.ty > b.ty : a.tx != b.tx ? a.tx > b.tx : a.i > b.i;
   };
   const double r = p_.origamiMinSpacingNm / 1000.0, r2 = r * r;
   const std::vector<Candidate>& own = Candidates(tx, ty);
   std::vector<const std::vector<Candidate>*> nearLists;
   if (r > 0)
      for (int32_t dx = -1; dx <= 1; dx++)
         for (int32_t dy = -1; dy <= 1; dy++) nearLists.push_back(&Candidates(tx + dx, ty + dy));
   std::vector<OrigamiMolecule> mols;
   for (const Candidate& c : own) {
      bool kept = true;
      for (const std::vector<Candidate>* list : nearLists) {
         for (const Candidate& o : *list) {
            if (o.tx == c.tx && o.ty == c.ty && o.i == c.i) continue;
            const double ex = o.x - c.x, ey = o.y - c.y;
            if (ex * ex + ey * ey < r2 && beats(o, c)) { kept = false; break; }
         }
         if (!kept) break;
      }
      if (kept) mols.push_back({ c.tx, c.ty, c.i, c.x, c.y, c.prio, c.yaw, OrigamiMoleculeH1(seed_, tx, ty, c.i) });
   }
   return tiles_.emplace(key, std::move(mols)).first->second;
}

void OrigamiField::MoleculesInRect(double x0, double y0, double x1, double y1, std::vector<OrigamiMolecule>& out,
                                   double extraUm)
{
   std::lock_guard<std::mutex> lock(mu_);
   if (tiles_.size() > kTileCacheMax) {
      tiles_.clear();
      cands_.clear();
   }
   const double reach = ReachUm() + extraUm, T = ORIGAMI_TILE_UM;
   const int32_t tx0 = (int32_t)std::floor((x0 - reach) / T), tx1 = (int32_t)std::floor((x1 + reach) / T);
   const int32_t ty0 = (int32_t)std::floor((y0 - reach) / T), ty1 = (int32_t)std::floor((y1 + reach) / T);
   for (int32_t tx = tx0; tx <= tx1; tx++)
      for (int32_t ty = ty0; ty <= ty1; ty++)
         for (const OrigamiMolecule& m : TileMolecules(tx, ty)) {
            const double dx = std::max(std::max(x0 - m.x, 0.0), m.x - x1), dy = std::max(std::max(y0 - m.y, 0.0), m.y - y1);
            if (jsm::hypot(dx, dy) <= reach) out.push_back(m);
         }
}

bool OrigamiField::Find(int32_t tx, int32_t ty, int32_t i, OrigamiMolecule& out)
{
   std::lock_guard<std::mutex> lock(mu_);
   for (const OrigamiMolecule& m : TileMolecules(tx, ty))
      if (m.i == i) { out = m; return true; }
   return false;
}

std::vector<double> OrigamiField::OutlineWorld(const OrigamiMolecule& m, int nPerSide) const
{
   std::vector<double> o = OrigamiShapeOutline(Shape(m), nPerSide);
   const double cy = jsm::cos(m.yaw), sy = jsm::sin(m.yaw);
   for (size_t j = 0; j + 1 < o.size(); j += 2) {
      const double X = o[j], Y = o[j + 1];
      o[j] = m.x + (X * cy - Y * sy) / 1000.0;
      o[j + 1] = m.y + (X * sy + Y * cy) / 1000.0;
   }
   return o;
}

} // namespace isc
