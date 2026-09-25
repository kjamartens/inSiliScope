// World-level checks (spec/PORT.md section 10, items 2, 3, 6): determinism
// under any query history, tiling, packing off, dye lattice statistics.
// Built natively and with Emscripten (run under Node), like isc_parity.
//
//   isc_world_tests          exit code 0 = all checks passed
#include "dyes.h"
#include "jsmath.h"
#include "microtubules.h"
#include "params.h"
#include "world.h"

#include "insilicell/insilicell.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <tuple>
#include <vector>

using namespace isc;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok) g_failures++;
}

const double INF = std::numeric_limits<double>::infinity();

bool SameDyes(const std::vector<WorldDye>& a, const std::vector<WorldDye>& b)
{
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++)
      if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z || a[i].id != b[i].id) return false;
   return true;
}

std::vector<WorldDye> Sorted(std::vector<WorldDye> v)
{
   std::sort(v.begin(), v.end(), [](const WorldDye& a, const WorldDye& b) {
      return std::tie(a.cx, a.cy, a.mtIndex, a.id, a.x) < std::tie(b.cx, b.cy, b.mtIndex, b.id, b.x);
   });
   return v;
}

double Ms(std::chrono::steady_clock::time_point t0)
{
   return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Determinism()
{
   Params p;
   const uint32_t seed = 1249;
   World a(seed, p);
   // A 12.8 um FOV (128 px at 100 nm) beside the nucleus of the first cell
   // near the origin, reaching out towards its edge.
   std::vector<Cell> near;
   a.CellsInRect(-30, -30, 30, 30, near);
   Check(!near.empty(), "cells near the origin");
   if (near.empty()) return;
   const double W = 12.8, x0 = near[0].x + 6, y0 = near[0].y - W / 2;

   std::vector<WorldDye> first, again, back, fresh;
   auto t0 = std::chrono::steady_clock::now();
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, first);
   const double firstMs = Ms(t0);
   t0 = std::chrono::steady_clock::now();
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, again);
   const double warmMs = Ms(t0);
   std::printf("      FOV %.1f um: %zu dyes, %ld cells built, first visit %.0f ms, warm %.0f ms\n",
               W, first.size(), a.Stats().cellsBuilt, firstMs, warmMs);
   Check(first.size() > 100, "default FOV holds dyes");
   Check(SameDyes(first, again), "repeat query identical");

   // Stage 1 mm away and back.
   std::vector<WorldDye> far;
   a.SitesInWindow(x0 + 1000, y0 + 1000, x0 + 1000 + W, y0 + 1000 + W, -INF, INF, far);
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, back);
   Check(SameDyes(first, back), "stage 1 mm away and back: identical");

   a.DropCaches();
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, fresh);
   Check(SameDyes(first, fresh), "after dropping every cache: identical");

   // Different history in a fresh world: visit a neighbouring window first.
   World b(seed, p, 2);
   std::vector<WorldDye> other, viaB;
   b.SitesInWindow(x0 - 30, y0 + 7, x0 - 30 + W, y0 + 7 + W, -INF, INF, other);
   b.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, viaB);
   Check(SameDyes(first, viaB), "independent of query history (tiny asset cache)");

   // Tiling: four quadrants = the whole window.
   std::vector<WorldDye> tiles;
   const double h = W / 2;
   for (int i = 0; i < 2; i++)
      for (int j = 0; j < 2; j++)
         a.SitesInWindow(x0 + i * h, y0 + j * h, x0 + (i + 1) * h, y0 + (j + 1) * h, -INF, INF, tiles);
   Check(SameDyes(Sorted(first), Sorted(tiles)), "four quadrant windows = the whole window");

   // z slab: [1, 2) um subset.
   std::vector<WorldDye> slab;
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, 1, 2, slab);
   size_t inSlab = 0;
   for (const WorldDye& d : first) inSlab += d.z >= 1 && d.z < 2;
   Check(slab.size() == inSlab && !slab.empty(), "z-limited query = z filter of the full query");

   // Density grid sums to the site count.
   std::vector<float> grid(64 * 64);
   const long n = a.DensityInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, 64, 64, grid.data());
   double sum = 0;
   for (float v : grid) sum += v;
   Check(n == (long)first.size() && sum == (double)n, "density grid sums to the site count");

   // Efficiency scales the count (hash decided per site: a strict subset).
   Params p2 = p;
   p2.labelEfficiency = 0.05;
   World c(seed, p2);
   std::vector<WorldDye> sparse;
   c.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, sparse);
   size_t subset = 0;
   std::vector<WorldDye> sf = Sorted(first);
   for (const WorldDye& d : sparse)
      subset += std::binary_search(sf.begin(), sf.end(), d, [](const WorldDye& u, const WorldDye& v) {
         return std::tie(u.cx, u.cy, u.mtIndex, u.id, u.x) < std::tie(v.cx, v.cy, v.mtIndex, v.id, v.x);
      });
   const double ratio = (double)sparse.size() / first.size();
   std::printf("      efficiency 0.05 / 0.1: %zu / %zu dyes (ratio %.3f)\n", sparse.size(), first.size(), ratio);
   Check(subset == sparse.size() && ratio > 0.4 && ratio < 0.6, "lower efficiency = a subset, about half");
}

void PackingOff()
{
   Params p;
   p.enablePacking = false;
   World a(7, p), b(7, p);
   std::vector<Cell> ca, cb;
   a.CellsInRect(-100, -100, 100, 100, ca);
   b.CellsInRect(500, 500, 600, 600, cb);
   cb.clear();
   b.CellsInRect(-100, -100, 100, 100, cb);
   bool same = ca.size() == cb.size() && !ca.empty();
   for (size_t i = 0; same && i < ca.size(); i++) same = ca[i].x == cb[i].x && ca[i].y == cb[i].y && ca[i].packRot == 0;
   Check(same, "packing off: raw candidates, deterministic");
   bool raw = true;
   for (const Cell& c : ca) {
      const Cell r = RawCandidate(7, c.cx, c.cy, p);
      raw = raw && r.x == c.x && r.y == c.y;
   }
   Check(raw, "packing off: positions are the raw jittered ones");

   Params q;
   World pk(7, q);
   std::vector<Cell> packed;
   pk.CellsInRect(-100, -100, 100, 100, packed);
   std::printf("      cells in 200x200 um: %zu packed, %zu unpacked\n", packed.size(), ca.size());
   Check(!packed.empty() && packed.size() <= ca.size(), "packing never adds cells");
}

// PORT.md 10.3 on a straight synthetic microtubule along x at z = 1 um.
void DyeStatistics()
{
   const double L = 5.0;
   std::vector<Pt3> pts;
   for (int i = 0; i <= 100; i++) pts.push_back({ L * i / 100, 0, 1 });
   const MtFrames fr = BuildMtFrames(pts);

   // Lattice geometry with hashed linker draws.
   double maxAttErr = 0, maxTipErr = 0, maxLink = 0, maxAngErr = 0;
   int nLinks = 0, below = 0;
   const double rMin = MT_LINKER_MIN_NM, rMax = MT_LINKER_MAX_NM;
   const double rMid = std::cbrt((rMin * rMin * rMin + rMax * rMax * rMax) / 2);   // volume median
   for (int k = 0; k < MT_N_PROTOFILAMENTS; k++) {
      const double th = MtProtofilamentTheta(0.7, k);
      for (int n = 0; n < 50; n++) {
         const double S = (MtProtofilamentOffsetNm(k) + 8 * n) * 1e-3;
         const double r1 = HashUnit(3, k, n, 1), r2 = HashUnit(3, k, n, 2), r3 = HashUnit(3, k, n, 3);
         const SiteGeom g = MtSiteGeometry(pts, fr, MtSegmentAt(fr, S), S, th, r1, r2, r3);
         const double ra = jsm::hypot(g.att.y, g.att.z - 1) * 1e3, rt = jsm::hypot(g.tip.y, g.tip.z - 1) * 1e3;
         maxAttErr = std::max(maxAttErr, std::fabs(ra - 12.5) + std::fabs(g.att.x - S) * 1e3);
         maxTipErr = std::max(maxTipErr, std::fabs(rt - 24.5));
         // azimuth in the U/V frame (U = +z, V = T x U = -y for T = +x)
         double ang = jsm::atan2(-g.att.y, g.att.z - 1) - th;
         ang = std::remainder(ang, 2 * jsm::PI);
         maxAngErr = std::max(maxAngErr, std::fabs(ang));
         const double dl = jsm::hypot(g.dye.x - g.tip.x, g.dye.y - g.tip.y, g.dye.z - g.tip.z) * 1e3;
         maxLink = std::max(maxLink, dl);
         nLinks++;
         below += dl < rMid;
      }
   }
   Check(maxAttErr < 1e-9, "attachment on the 12.5 nm cylinder, at its arc position");
   Check(maxTipErr < 1e-9, "binder tip at 24.5 nm");
   Check(maxAngErr < 1e-9, "protofilament azimuths = phase + k 2pi/13");
   Check(maxLink <= rMax + 1e-9, "linker within 5 nm of the tip");
   const double fracBelow = (double)below / nLinks;
   std::printf("      linker: %d samples, %.3f below the volume median (expect 0.5)\n", nLinks, fracBelow);
   Check(std::fabs(fracBelow - 0.5) < 0.06, "linker radius uniform in volume (r^3 CDF)");

   // Axial stagger 3*8/13 nm between neighbouring protofilaments.
   bool stagger = true;
   for (int k = 1; k < MT_N_PROTOFILAMENTS; k++)
      stagger = stagger && std::fabs(std::fmod(MtProtofilamentOffsetNm(k) - MtProtofilamentOffsetNm(k - 1) + 8, 8) - 24.0 / 13) < 1e-12;
   Check(stagger, "13_3 lattice: axial stagger 24/13 nm");

   // Blocks partition the lattice; counts ~ 1625 * L * efficiency.
   std::vector<Dye> all;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 1.0, all);
   long expected = 0;
   for (int k = 0; k < MT_N_PROTOFILAMENTS; k++)
      for (int n = 0;; n++) {
         if ((MtProtofilamentOffsetNm(k) + 8 * n) * 1e-3 >= L) break;
         expected++;
      }
   std::vector<std::pair<int, int>> kn;
   for (const Dye& d : all) kn.push_back({ d.k, d.n });
   std::sort(kn.begin(), kn.end());
   const bool unique = std::adjacent_find(kn.begin(), kn.end()) == kn.end();
   std::printf("      %zu sites on %.0f um at 100%% (%.0f per um)\n", all.size(), L, all.size() / L);
   Check((long)all.size() == expected && unique, "blocks partition the lattice (each site exactly once)");
   Check(std::fabs(all.size() / L - 1625) < 5, "about 1625 sites per um");
   std::vector<Dye> some;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, some);
   const double mean = 0.1 * expected, sd = std::sqrt(expected * 0.1 * 0.9);
   std::printf("      efficiency 0.1: %zu labelled (expect %.0f +- %.0f)\n", some.size(), mean, sd);
   Check(std::fabs(some.size() - mean) < 4 * sd, "labelled count ~ efficiency x sites");
}

void CApi()
{
   IscParams* p = isc_params_new();
   isc_params_set(p, "labelEfficiency", 0.05);
   IscWorld* w = isc_world_new(1249, p);
   // 4 x 4 um beside the nucleus of cell (-1,-1) at (-10.1, -5.0), see Determinism().
   const double x0 = -4, y0 = -7, x1 = 0, y1 = -3;
   const int32_t n = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, nullptr, 0);
   std::vector<double> buf((size_t)std::max(0, n) * ISC_SITE_STRIDE);
   const int32_t n2 = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, buf.data(), n);
   std::vector<float> grid(16 * 16);
   const int32_t n3 = isc_density_in_window(w, x0, y0, x1, y1, -INF, INF, 16, 16, grid.data());
   const int32_t nc = isc_cells_in_window(w, x0, y0, x1, y1, nullptr, 0);
   Check(n > 0 && n2 == n && n3 == n && nc > 0, "C ABI: sites/density/cells agree");
   Check(isc_sites_in_window(w, 1, 0, 0, 1, -INF, INF, nullptr, 0) == -1, "C ABI: empty rect rejected");
   isc_world_free(w);
   isc_params_free(p);
}

} // namespace

int main()
{
   Determinism();
   PackingOff();
   DyeStatistics();
   CApi();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall world checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
