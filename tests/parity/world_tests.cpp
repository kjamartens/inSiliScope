// World-level checks (spec/PORT.md section 10, items 2, 3, 4, 6): determinism
// under any query history, tiling, packing off, dye lattice statistics,
// blink kinetics and the event query.
// Built natively and with Emscripten (run under Node), like isc_parity.
//
//   isc_world_tests          exit code 0 = all checks passed
#include "dyes.h"
#include "jsmath.h"
#include "microtubules.h"
#include "parallel.h"
#include "params.h"
#include "world.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 1.0, 0.0, all);
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
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, 0.0, some);
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

   // Events: the same through the ABI as through World, after a kinetics change.
   Check(isc_world_set_kinetics(w, 0.5, 0.05, 0.5, 0.3, 0.4) == 0, "C ABI: kinetics accepted");
   Check(isc_world_set_kinetics(w, -1, 0.05, 0.5, 0.3, 0.4) == -1, "C ABI: bad kinetics rejected");
   const int32_t ne = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, nullptr, 0);
   std::vector<double> ev((size_t)std::max(0, ne) * ISC_EVENT_STRIDE);
   const int32_t ne2 = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, ev.data(), ne);
   bool evOk = ne > 0 && ne2 == ne;
   for (int32_t i = 0; evOk && i < ne; i++) {
      const double* e = &ev[(size_t)i * ISC_EVENT_STRIDE];
      evOk = e[3] < 1.1 && e[4] > 1.0 && e[0] >= x0 && e[0] < x1 && e[1] >= y0 && e[1] < y1 && e[5] > 0;
   }
   Check(evOk, "C ABI: events overlap the frame and lie in the window");

   // Viewer geometry of one cell.
   std::vector<double> cb((size_t)std::max(0, nc) * ISC_CELL_STRIDE);
   isc_cells_in_window(w, x0, y0, x1, y1, cb.data(), nc);
   const int32_t ccx = (int32_t)cb[0], ccy = (int32_t)cb[1];
   const int32_t no = isc_cell_outline(w, ccx, ccy, nullptr, 0);
   int32_t dims[2] = { 0, 0 }, tot = 0;
   const int32_t nv = isc_cell_mesh(w, ccx, ccy, dims, nullptr, 0);
   const int32_t nm = isc_cell_microtubules(w, ccx, ccy, nullptr, 0, nullptr, 0, &tot);
   std::vector<double> xyz((size_t)std::max(0, tot) * 3);
   std::vector<int32_t> lens((size_t)std::max(0, nm));
   int32_t tot2 = 0;
   isc_cell_microtubules(w, ccx, ccy, xyz.data(), tot, lens.data(), nm, &tot2);
   long lensSum = 0;
   for (int32_t l : lens) lensSum += l;
   Check(no >= 8 && nv == (dims[0] + 1) * dims[1] && nm > 0 && tot2 == tot && lensSum == tot && cb[6] > 0,
         "C ABI: cell outline / mesh / microtubules consistent");
   isc_world_free(w);
   isc_params_free(p);
}

// ABI 5: the z-resolved, population-selectable density query (WideField).
void Density3d()
{
   IscParams* p = isc_params_new();
   isc_params_set(p, "labelEfficiency", 0.2);
   isc_params_set(p, "labelNonBleaching", 0.3);
   IscWorld* w = isc_world_new(1249, p);
   const double x0 = -4, y0 = -7, x1 = 0, y1 = -3, zLo = -2, zHi = 14;
   const int nx = 16, ny = 12, nz = 32;
   const size_t n2 = (size_t)nx * ny, n3 = n2 * nz;
   std::vector<float> all(n3), bl(n3), pe(n3), none(n3, 7.0f), flat(n2), slab(n2);
   const int32_t na = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, ISC_POP_BLEACHING | ISC_POP_PERSISTENT, all.data());
   const int32_t nb = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, ISC_POP_BLEACHING, bl.data());
   const int32_t np = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, ISC_POP_PERSISTENT, pe.data());
   const int32_t n0 = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 0, none.data());
   const int32_t nf = isc_density_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, flat.data());
   Check(na > 0 && nb > 0 && np > 0 && na == nb + np && nf == na, "density3d: totals (all = bleaching + persistent = 2D)");
   bool sumZ = true, split = true;
   for (size_t i = 0; i < n2; i++) {
      double s = 0;
      for (int k = 0; k < nz; k++) s += all[k * n2 + i];
      sumZ = sumZ && s == flat[i];
   }
   for (size_t i = 0; i < n3; i++) split = split && all[i] == bl[i] + pe[i];
   Check(sumZ, "density3d: summed over z equals isc_density_in_window");
   Check(split, "density3d: bleaching + persistent = all, voxel by voxel");
   Check(n0 == 0 && std::all_of(none.begin(), none.end(), [](float v) { return v == 0.0f; }), "density3d: no population = zeros");

   // Hand-binning the sites by the persistent flag.
   World ref(1249, [] { Params q; q.labelEfficiency = 0.2; q.labelNonBleaching = 0.3; return q; }());
   std::vector<WorldDye> d;
   ref.SitesInWindow(x0, y0, x1, y1, zLo, zHi, d);
   std::vector<float> hb(n3, 0.0f), hp(n3, 0.0f);
   for (const WorldDye& e : d) {
      const int ix = std::min(nx - 1, (int)std::floor((e.x - x0) * (nx / (x1 - x0))));
      const int iy = std::min(ny - 1, (int)std::floor((e.y - y0) * (ny / (y1 - y0))));
      const int iz = std::min(nz - 1, (int)std::floor((e.z - zLo) * (nz / (zHi - zLo))));
      (e.persistent ? hp : hb)[((size_t)iz * ny + iy) * nx + ix] += 1;
   }
   Check(hb == bl && hp == pe, "density3d: matches hand-binned SitesInWindow per population");

   // nz = 1 over a slab = the 2D query with the same z limits; infinite z too.
   const int32_t ns = isc_density3d_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, 1, 3, slab.data());
   isc_density_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, flat.data());
   const int32_t ni = isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 1, 3, all.data());
   Check(slab == flat && ns > 0 && ni >= na, "density3d: nz = 1 slab equals the 2D query");
   Check(isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 2, 3, all.data()) == -1 &&
            isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 4, all.data()) == -1 &&
            isc_density3d_in_window(w, x0, y0, x1, y1, zHi, zLo, nx, ny, nz, 3, all.data()) == -1,
         "density3d: bad arguments rejected");
   isc_world_free(w);
   isc_params_free(p);
}

// Blink kinetics of the per-dye schedule (spec/PORT.md 10.4).
void KineticsStats()
{
   Kinetics k;
   k.activationRatePerSec = 0.1; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.2; k.photonCV = 0.5;
   const int N = 40000;
   std::vector<Blink> b;
   double sumAct = 0, sumOn = 0, sumB = 0, sumB2 = 0, sumBlinks = 0, sumOff = 0;
   long ones = 0, nBlinks = 0, nOff = 0;
   bool ordered = true;
   for (int i = 0; i < N; i++) {
      b.clear();
      DyeSchedule(DyeH1(7, i % 97, -(i / 97), i % 5), i % 13, i, k, b);
      sumAct += b[0].tOn;
      sumBlinks += (double)b.size();
      ones += b.size() == 1;
      for (size_t j = 0; j < b.size(); j++) {
         sumOn += b[j].tOff - b[j].tOn;
         sumB += b[j].brightness; sumB2 += b[j].brightness * b[j].brightness;
         nBlinks++;
         if (j) { sumOff += b[j].tOn - b[j - 1].tOff; nOff++; ordered &= b[j].tOn >= b[j - 1].tOff; }
      }
   }
   const double meanB = sumB / nBlinks, cv = jsm::sqrt(sumB2 / nBlinks - meanB * meanB) / meanB;
   std::printf("      kinetics: act %.3f s, on %.4f s, off %.3f s, blinks %.3f (P1 %.3f), brightness %.3f CV %.3f\n",
               sumAct / N, sumOn / nBlinks, sumOff / nOff, sumBlinks / N, (double)ones / N, meanB, cv);
   Check(std::fabs(sumAct / N / 10 - 1) < 0.03, "first activation ~ Exp(1 / activationRatePerSec)");
   Check(std::fabs(sumOn / nBlinks / 0.05 - 1) < 0.03, "ON time ~ Exp(onSec)");
   Check(std::fabs(sumOff / nOff / 0.5 - 1) < 0.03, "dark time ~ Exp(offSec)");
   Check(std::fabs(sumBlinks / N / 5 - 1) < 0.03 && std::fabs((double)ones / N - 0.2) < 0.01,
         "blink count geometric, mean 1/bleachProb");
   Check(std::fabs(meanB - 1) < 0.02 && std::fabs(cv - 0.5) < 0.03, "brightness log-normal, mean 1, CV photonCV");
   Check(ordered, "blinks in time order, never overlapping");

   k.bleachProb = 1; k.photonCV = 0;
   bool single = true;
   for (int i = 0; i < 1000; i++) {
      b.clear();
      DyeSchedule(DyeH1(7, i, 0, 0), 0, i, k, b);
      single &= b.size() == 1 && b[0].brightness == 1;
   }
   Check(single, "bleachProb 1, CV 0: one blink, brightness exactly 1");
   k.bleachProb = 0;   // clamped to 0.01, and capped at DYE_MAX_BLINKS
   b.clear();
   DyeSchedule(DyeH1(7, 1, 2, 3), 4, 5, k, b);
   Check(b.size() <= (size_t)DYE_MAX_BLINKS, "blink count capped");
}

bool SameEvents(std::vector<WorldEvent> a, std::vector<WorldEvent> b)
{
   auto key = [](const WorldEvent& e) { return std::make_tuple(e.id, e.tOn, e.x, e.y, e.z, e.tOff, e.brightness); };
   auto lt = [&](const WorldEvent& x, const WorldEvent& y) { return key(x) < key(y); };
   std::sort(a.begin(), a.end(), lt);
   std::sort(b.begin(), b.end(), lt);
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++)
      if (key(a[i]) != key(b[i])) return false;
   return true;
}

// Caches under load: a window holding more dyes than the dye cache is not
// regenerated on every query, and the persistent-blink cache (built for a
// range of time bins) gives PersistentBlinks' answer, also when time jumps
// back, across range ends and after a kinetics change.
void CacheUnderLoad()
{
   Params p;
   p.labelEfficiency = 0.05;
   p.labelNonBleaching = 0.05;
   const uint32_t seed = 1249;
   Kinetics k;
   k.activationRatePerSec = 0.2; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   World w(seed, p, 48, 1000);   // a dye cache far smaller than the window
   w.SetKinetics(k);
   std::vector<Cell> near;
   w.CellsInRect(-30, -30, 30, 30, near);
   if (near.empty()) { Check(false, "cells near the origin"); return; }
   const double x0 = near[0].x - 6.4, y0 = near[0].y - 6.4, x1 = x0 + 12.8, y1 = y0 + 12.8, fd = 0.05;
   std::vector<WorldEvent> ev;
   w.EventsInWindow(x0, y0, x1, y1, -INF, INF, 0, fd, ev);
   const long built = w.Stats().dyeBlocks;
   for (int f = 1; f < 40; f++) { ev.clear(); w.EventsInWindow(x0, y0, x1, y1, -INF, INF, f * fd, (f + 1) * fd, ev); }
   Check(built > 0 && w.Stats().dyeBlocks == built, "window larger than the dye cache: blocks built once, not per query");
   // A shifted window (a focus move) builds only the blocks it did not have:
   // eviction waits for the end of the query, so the blocks the old and new
   // windows share survive it.
   {
      World a(seed, p, 48, 1000), b(seed, p, 48, 1000);
      a.SetKinetics(k); b.SetKinetics(k);
      std::vector<WorldEvent> e;
      a.EventsInWindow(x0, y0, x1, y1, -INF, 1.0, 0, fd, e);
      const long before = a.Stats().dyeBlocks;
      a.EventsInWindow(x0, y0, x1, y1, -INF, 2.0, fd, 2 * fd, e);
      b.EventsInWindow(x0, y0, x1, y1, -INF, 2.0, fd, 2 * fd, e);
      Check(a.Stats().dyeBlocks - before < b.Stats().dyeBlocks,
            "window shifted in z over a full dye cache: only its new blocks are built");
   }
   // Prefetch only fills caches: a world pre-loading around a moving window
   // (budget-limited or not) answers exactly like one that does not, and
   // the window it completed is then served without building blocks.
   {
      World a(seed, p, 48, 1000), b(seed, p, 48, 1000);
      a.SetKinetics(k); b.SetKinetics(k);
      bool same = true, partial = false;
      for (int f = 0; f < 12; f++) {
         const double s = 0.4 * f, t0 = 2 + f * fd;
         std::vector<WorldEvent> ea, eb;
         a.EventsInWindow(x0 + s, y0, x1 + s, y1, 0, 2, t0, t0 + fd, ea);
         b.EventsInWindow(x0 + s, y0, x1 + s, y1, 0, 2, t0, t0 + fd, eb);
         same = same && SameEvents(ea, eb) && ea.size() == eb.size();
         partial = partial || !a.Prefetch(x0 + s - 2, y0 - 2, x1 + s + 2, y1 + 2, -INF, INF, t0 + fd, t0 + 2 * fd,
                                          f % 3 == 0 ? 0.0 : 1e9);
      }
      const bool done = a.Prefetch(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, 1e9);
      const long built = a.Stats().dyeBlocks;
      std::vector<WorldEvent> ea, eb;
      a.EventsInWindow(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, ea);
      b.EventsInWindow(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, eb);
      Check(same && SameEvents(ea, eb) && partial && done && a.Stats().dyeBlocks == built,
            "prefetch changes no event; a prefetched window is served from the cache");
   }
   // Frame after frame, the persistent blinks come from blocks extending
   // their bins ahead of time (staggered per block): still PersistentBlinks'.
   bool steady = true;
   for (int f = 0; f < 70; f++) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, 40 + f * fd, 40 + (f + 1) * fd, ev);
      if (f % 7 == 0 || f == 19 || f == 20) {
         std::vector<WorldDye> ds;
         w.SitesInWindow(x0, y0, x1, y1, -INF, INF, ds);
         std::vector<WorldEvent> ref;
         std::vector<Blink> bl;
         const double t0 = 40 + f * fd, t1 = t0 + fd;
         for (const WorldDye& d : ds) {
            bl.clear();
            if (d.persistent) PersistentBlinks(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, w.GetKinetics(), t0, t1, bl);
            else DyeSchedule(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, w.GetKinetics(), bl);
            for (const Blink& x : bl)
               if (x.tOn < t1 && x.tOff > t0) ref.push_back({ d.x, d.y, d.z, x.tOn, x.tOff, x.brightness, d.id });
         }
         steady = steady && SameEvents(ev, ref);
      }
   }
   Check(steady, "persistent blinks frame by frame across bin extensions = PersistentBlinks");

   std::vector<WorldDye> dyes;
   w.SitesInWindow(x0, y0, x1, y1, -INF, INF, dyes);
   auto brute = [&](double t0, double t1) {
      std::vector<WorldEvent> out;
      std::vector<Blink> b;
      for (const WorldDye& d : dyes) {
         b.clear();
         if (d.persistent) PersistentBlinks(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, w.GetKinetics(), t0, t1, b);
         else DyeSchedule(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, w.GetKinetics(), b);
         for (const Blink& bl : b)
            if (bl.tOn < t1 && bl.tOff > t0) out.push_back({ d.x, d.y, d.z, bl.tOn, bl.tOff, bl.brightness, d.id });
      }
      return out;
   };
   bool same = true;
   long nPersist = 0;
   for (double t : { 3.0, 15.97, 16.0, 31.99, 47.5, 0.02, 200.0, 199.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      same = same && SameEvents(ev, brute(t, t + fd));
      nPersist += (long)ev.size();
   }
   Kinetics k2 = k;
   k2.activationRatePerSec = 3;   // more blinks per bin: shorter bin ranges
   w.SetKinetics(k2);
   for (double t : { 7.3, 7.35, 9.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      same = same && SameEvents(ev, brute(t, t + fd));
   }
   Check(same && nPersist > 0, "cached persistent blinks = PersistentBlinks (time jumps, range ends, new kinetics)");
}

// The event query (spec/PORT.md 6.2): brute force equality, determinism,
// time slicing, and the per-frame cost at the default FOV.
void EventQuery()
{
   Params p;
   const uint32_t seed = 1249;
   Kinetics k;
   k.activationRatePerSec = 1.0 / 30; k.onSec = 0.03; k.offSec = 0.3; k.bleachProb = 0.25; k.photonCV = 0.3;
   p.labelNonBleaching = 0.02;   // some persistent sites too
   World w(seed, p);
   w.SetKinetics(k);
   std::vector<Cell> near;
   w.CellsInRect(-30, -30, 30, 30, near);
   if (near.empty()) { Check(false, "cells near the origin"); return; }
   const double W = 12.8, x0 = near[0].x + 6, y0 = near[0].y - W / 2, x1 = x0 + W, y1 = y0 + W;
   const double zLo = 0.5, zHi = 3.0, fd = 0.03;

   // Brute force: every dye in the window, its whole schedule.
   std::vector<WorldDye> dyes;
   w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, dyes);
   const double t0 = 2.0, t1 = 2.0 + fd;
   std::vector<WorldEvent> brute;
   std::vector<Blink> b;
   for (const WorldDye& d : dyes) {
      b.clear();
      if (d.persistent) PersistentBlinks(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, k, t0, t1, b);
      else DyeSchedule(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, k, b);
      for (const Blink& bl : b)
         if (bl.tOn < t1 && bl.tOff > t0) brute.push_back({ d.x, d.y, d.z, bl.tOn, bl.tOff, bl.brightness, d.id });
   }
   std::vector<WorldEvent> got;
   auto tq = std::chrono::steady_clock::now();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, got);
   const double firstMs = Ms(tq);
   Check(!got.empty() && SameEvents(got, brute), "events = brute force over the dyes' schedules");

   // Steady state: consecutive frames of a static FOV.
   tq = std::chrono::steady_clock::now();
   const int F = 200;
   size_t nEv = 0;
   std::vector<WorldEvent> fe;
   for (int f = 0; f < F; f++) {
      fe.clear();
      w.EventsInWindow(x0 - 2, y0 - 2, x1 + 2, y1 + 2, zLo, zHi, 1.0 + f * fd, 1.0 + (f + 1) * fd, fe);
      nEv += fe.size();
   }
   const double frameMs = Ms(tq) / F;
   const long built = w.Stats().dyeBlocks;
   std::printf("      events: %zu dyes in FOV, first query %.0f ms (schedules built), steady %.2f ms/frame, "
               "%.0f events/frame, %ld dye blocks\n", dyes.size(), firstMs, frameMs, (double)nEv / F, built);

   // Frame slices: each blink shows up in exactly the frames it overlaps.
   std::vector<WorldEvent> whole, sliced;
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 5.0, 5.0 + 10 * fd, whole);
   for (int f = 0; f < 10; f++) {
      fe.clear();
      w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 5.0 + f * fd, 5.0 + (f + 1) * fd, fe);
      for (const WorldEvent& e : fe) {
         bool seen = false;
         for (const WorldEvent& s : sliced) seen |= s.id == e.id && s.tOn == e.tOn;
         if (!seen) sliced.push_back(e);
      }
   }
   Check(SameEvents(whole, sliced), "union of per-frame queries = the multi-frame query");

   // Determinism: away and back, dropped caches, kinetics round trip, other world.
   std::vector<WorldEvent> again;
   std::vector<WorldDye> far;
   w.SitesInWindow(x0 + 1000, y0, x1 + 1000, y1, -INF, INF, far);
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: stage 1 mm away and back, identical");
   Kinetics k2 = k;
   k2.onSec = 0.2;
   w.SetKinetics(k2);
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   const bool changed = !SameEvents(got, again);
   w.SetKinetics(k);
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(changed && SameEvents(got, again), "events: kinetics change and back, identical");
   w.DropCaches();
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: after dropping every cache, identical");
   World tiny(seed, p, 1, 1000);   // caches smaller than one FOV
   tiny.SetKinetics(k);
   again.clear();
   tiny.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: independent of cache sizes");
}

// Persistent (non-bleaching, DNA-PAINT-like) sites: a constant blink rate
// per site at any time, windows answered consistently, and the bleaching
// dyes unchanged by adding them.
void PersistentSites()
{
   Kinetics k;
   k.activationRatePerSec = 0.2; k.onSec = 0.1; k.photonCV = 0.3;
   const int N = 4000;
   std::vector<Blink> b;
   double early = 0, late = 0, onSum = 0, brSum = 0;
   long nOn = 0;
   for (int i = 0; i < N; i++) {
      const uint32_t h1 = DyeH1(9, i % 31, i / 31, 0);
      b.clear();
      PersistentBlinks(h1, i % 13, i, k, 0, 100, b);
      for (const Blink& x : b)
         if (x.tOn >= 0 && x.tOn < 100) { early++; onSum += x.tOff - x.tOn; brSum += x.brightness; nOn++; }
      b.clear();
      PersistentBlinks(h1, i % 13, i, k, 10000, 10100, b);
      for (const Blink& x : b)
         if (x.tOn >= 10000 && x.tOn < 10100) late++;
   }
   const double rEarly = early / N / 100, rLate = late / N / 100;
   std::printf("      persistent: %.4f blinks/site/s at 0-100 s, %.4f at 10000-10100 s (rate 0.2), ON %.4f s, brightness %.3f\n",
               rEarly, rLate, onSum / nOn, brSum / nOn);
   Check(std::fabs(rEarly / 0.2 - 1) < 0.03 && std::fabs(rLate / 0.2 - 1) < 0.03, "persistent: constant rate, no depletion");
   Check(std::fabs(onSum / nOn / 0.1 - 1) < 0.03 && std::fabs(brSum / nOn - 1) < 0.02, "persistent: ON ~ Exp(onSec), brightness mean 1");

   // A long window = the union of its slices (each blink once, by tOn).
   bool same = true;
   for (int i = 0; i < 300 && same; i++) {
      const uint32_t h1 = DyeH1(9, i, 0, 1);
      std::vector<Blink> whole, part;
      PersistentBlinks(h1, 1, i, k, 50, 60, whole);
      std::vector<double> starts;
      for (int s = 0; s < 40; s++) {
         part.clear();
         PersistentBlinks(h1, 1, i, k, 50 + s * 0.25, 50 + (s + 1) * 0.25, part);
         for (const Blink& x : part)
            if (std::find(starts.begin(), starts.end(), x.tOn) == starts.end()) starts.push_back(x.tOn);
      }
      same = starts.size() == whole.size();
      for (const Blink& x : whole) same = same && std::find(starts.begin(), starts.end(), x.tOn) != starts.end();
   }
   Check(same, "persistent: a window = the union of its slices");

   // Site fractions: bleaching set unchanged by persistent sites.
   std::vector<Pt3> pts;
   for (int i = 0; i <= 100; i++) pts.push_back({ i * 0.05, 0.2 * std::sin(i * 0.1), 1.0 });
   const MtFrames fr = BuildMtFrames(pts);
   std::vector<Dye> a, c;
   for (int blk = 0; blk < 5; blk++) {
      DyesInBlock(11, 2, 3, 0, pts, fr, blk, 0.1, 0.0, a);
      DyesInBlock(11, 2, 3, 0, pts, fr, blk, 0.1, 0.3, c);
   }
   size_t nb = 0, np = 0;
   bool subset = true;
   for (const Dye& d : c) {
      if (d.persistent) { np++; continue; }
      nb++;
      bool found = false;
      for (const Dye& e : a) found = found || (e.id == d.id && e.k == d.k && e.n == d.n);
      subset = subset && found;
   }
   std::printf("      site fractions 0.1 / 0.3 on 5 um: %zu bleaching, %zu persistent (expect ~812 / ~2437)\n", nb, np);
   Check(subset && nb == a.size() && std::fabs(np / (3.0 * nb) - 1) < 0.15,
         "labelNonBleaching adds persistent sites, bleaching dyes unchanged");
}

} // namespace

// The parallel block work (packing blocks, cell assets, dye blocks, blink
// schedules and persistent covers built on several threads) changes nothing:
// the same events in the same order -- the order the renderer sums them in --
// and the same build counts as one thread, for stack-sized and frame-sized
// queries, jumps, a kinetics change and prefetches.
bool IdenticalEvents(const std::vector<WorldEvent>& a, const std::vector<WorldEvent>& b)
{
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++) {
      const WorldEvent &x = a[i], &y = b[i];
      if (x.id != y.id || std::memcmp(&x.x, &y.x, 6 * sizeof(double)) != 0) return false;
   }
   return true;
}

void Threads()
{
   Params p;
   p.labelEfficiency = 0.1;
   p.labelNonBleaching = 0.3;
   Kinetics k;
   k.activationRatePerSec = 0.01; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   World a(77, p), b(77, p);
   a.SetKinetics(k); b.SetKinetics(k);
   size_t total = 0;
   auto both = [&](const std::function<void(World&, std::vector<WorldEvent>&)>& q) {
      std::vector<WorldEvent> ea, eb;
      SetWorldThreads(1);
      q(a, ea);
      SetWorldThreads(8);
      q(b, eb);
      SetWorldThreads(0);
      total += ea.size();
      return IdenticalEvents(ea, eb);
   };
   bool same = both([](World& w, std::vector<WorldEvent>& e) { w.EventsInWindow(-20, -20, 20, 20, 0, 3, 0, 20, e); });
   for (int f = 0; f < 30 && same; f++) {
      const double s = f < 10 ? 0.3 * f : f < 20 ? 60 + 0.3 * f : 250;   // a move, a jump, another jump
      same = both([&](World& w, std::vector<WorldEvent>& e) {
         if (f == 25) w.SetKinetics(Kinetics{ 0.02, 0.05, 0.5, 0.5, 0.2 });
         w.EventsInWindow(s - 8, -8, s + 8, 8, 0, 4, 21 + f * 0.05, 21 + (f + 1) * 0.05, e);
         if (f % 4 == 0) w.Prefetch(s - 11, -11, s + 11, 11, -INF, INF, 21 + (f + 1) * 0.05, 21 + (f + 2) * 0.05, 1e9);
      });
   }
   const WorldStats &sa = a.Stats(), &sb = b.Stats();
   Check(same && total > 10000, "8 threads = 1 thread: the same events in the same order");
   Check(sa.blocksPacked == sb.blocksPacked && sa.cellsBuilt == sb.cellsBuilt && sa.dyeBlocks == sb.dyeBlocks &&
            sa.dyeBlockHits == sb.dyeBlockHits && sa.schedulesBuilt == sb.schedulesBuilt &&
            sa.persistentBuilt == sb.persistentBuilt,
         "8 threads = 1 thread: the same caches built");
}

int main()
{
   Determinism();
   PackingOff();
   DyeStatistics();
   KineticsStats();
   PersistentSites();
   EventQuery();
   CacheUnderLoad();
   Threads();
   CApi();
   Density3d();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall world checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
