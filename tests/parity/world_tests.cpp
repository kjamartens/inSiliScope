// World-level checks (spec/PORT.md section 10, items 2, 3, 4, 6): determinism
// under any query history, tiling, packing off, dye lattice statistics,
// blink kinetics and the event query.
// Built natively and with Emscripten (run under Node), like isc_parity.
//
//   isc_world_tests          exit code 0 = all checks passed
#include "cells.h"
#include "cytomesh.h"
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
#include <filesystem>
#include <fstream>
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

// A label of the given mode and density (the other fields the defaults).
Label MakeLabel(LabelMode mode, double density, const Kinetics& kin = Kinetics(), double fluorescentFraction = 1.0)
{
   Label l;
   l.mode = mode;
   l.density = density;
   l.fluorescentFraction = fluorescentFraction;
   l.kin = kin;
   return l;
}

World LabelledWorld(uint32_t seed, const Params& p, const Label& l, size_t assetCells = 48, size_t dyeCap = 2000000)
{
   World w(seed, p, assetCells, dyeCap);
   w.SetLabel(STRUCTURE_MT, l);
   return w;
}

// The blinks of dye d overlapping [t0, t1) by brute force: its whole schedule
// (LabelSchedule; DNA-PAINT: PersistentBlinks), unwindowed.
void BruteBlinks(uint32_t seed, const WorldDye& d, const Label& l, double t0, double t1, std::vector<WorldEvent>& out)
{
   std::vector<Blink> b;
   const uint32_t h1 = DyeH1(seed, d.cx, d.cy, d.mtIndex);
   if (l.mode == LabelMode::DnaPaint) PersistentBlinks(h1, d.k, d.n, l.kin, t0, t1, b);
   else LabelSchedule(h1, d.k, d.n, l, &b, nullptr);
   for (const Blink& x : b)
      if (x.tOn < t1 && x.tOff > t0)
         out.push_back({ d.x, d.y, d.z, x.tOn, x.tOff, x.brightness, 0.0, d.id, (uint8_t)d.structure, STATE_BLINK });
}

void Determinism()
{
   Params p;
   const uint32_t seed = 1249;
   const Label l10 = MakeLabel(LabelMode::PALM, 0.1);
   World a = LabelledWorld(seed, p, l10);
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
   World b = LabelledWorld(seed, p, l10, 2);
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

   // Density and fluorescent fraction scale the count (hash decided per
   // site: a strict subset), and nest.
   std::vector<WorldDye> sf = Sorted(first);
   auto subsetOf = [&](const std::vector<WorldDye>& v) {
      size_t subset = 0;
      for (const WorldDye& d : v)
         subset += std::binary_search(sf.begin(), sf.end(), d, [](const WorldDye& u, const WorldDye& w) {
            return std::tie(u.cx, u.cy, u.mtIndex, u.id, u.x) < std::tie(w.cx, w.cy, w.mtIndex, w.id, w.x);
         });
      return subset == v.size();
   };
   World c = LabelledWorld(seed, p, MakeLabel(LabelMode::PALM, 0.05));
   std::vector<WorldDye> sparse;
   c.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, sparse);
   const double ratio = (double)sparse.size() / first.size();
   std::printf("      density 0.05 / 0.1: %zu / %zu dyes (ratio %.3f)\n", sparse.size(), first.size(), ratio);
   Check(subsetOf(sparse) && ratio > 0.4 && ratio < 0.6, "lower density = a subset, about half");
   // FLUOR nesting: a fluorescent fraction keeps a subset of the same dyes;
   // set on the same world (redraws the dyes, the geometry stays).
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1, Kinetics(), 0.5));
   std::vector<WorldDye> half, quarter;
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, half);
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1, Kinetics(), 0.25));
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, quarter);
   const double rh = (double)half.size() / first.size(), rq = (double)quarter.size() / half.size();
   bool nested = true;
   {
      std::vector<WorldDye> hs = Sorted(half);
      for (const WorldDye& d : quarter)
         nested = nested && std::binary_search(hs.begin(), hs.end(), d, [](const WorldDye& u, const WorldDye& w) {
            return std::tie(u.cx, u.cy, u.mtIndex, u.id, u.x) < std::tie(w.cx, w.cy, w.mtIndex, w.id, w.x);
         });
   }
   std::printf("      fluorescent fraction 0.5 / 0.25: %zu / %zu dyes (ratios %.3f, %.3f)\n", half.size(), quarter.size(), rh, rq);
   Check(subsetOf(half) && nested && std::fabs(rh - 0.5) < 0.05 && std::fabs(rq - 0.5) < 0.07,
         "fluorescent fraction: nested subsets of the same dyes (FLUOR draw)");
   a.SetLabel(STRUCTURE_MT, l10);
   std::vector<WorldDye> restored;
   a.SitesInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, restored);
   Check(SameDyes(first, restored), "label back to fraction 1: the original dyes");
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

   // Blocks partition the lattice; counts ~ 1625 * L * density.
   std::vector<Dye> all;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 1.0, 1.0, all);
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
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, 1.0, some);
   const double mean = 0.1 * expected, sd = std::sqrt(expected * 0.1 * 0.9);
   std::printf("      density 0.1: %zu labelled (expect %.0f +- %.0f)\n", some.size(), mean, sd);
   Check(std::fabs(some.size() - mean) < 4 * sd, "labelled count ~ density x sites");
   // The fluorescent fraction thins the labelled sites (FLUOR), same positions.
   std::vector<Dye> fl;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, 0.4, fl);
   bool sub = true;
   for (const Dye& d : fl) {
      bool found = false;
      for (const Dye& e : some) found = found || (e.k == d.k && e.n == d.n && e.pos.x == d.pos.x && e.id == d.id);
      sub = sub && found;
   }
   const double m2 = 0.4 * some.size(), sd2 = std::sqrt(some.size() * 0.4 * 0.6);
   Check(sub && std::fabs(fl.size() - m2) < 4 * sd2, "fluorescent fraction 0.4: a subset of the labelled dyes, ~0.4 of them");
}

void CApi()
{
   IscParams* p = isc_params_new();
   Check(isc_params_set(p, "labelEfficiency", 0.05) == -1, "C ABI: labelEfficiency is not a param any more (ABI 10)");
   IscWorld* w = isc_world_new(1249, p);
   double lab[ISC_LABEL_COUNT];
   lab[ISC_LABEL_DENSITY] = 0.05; lab[ISC_LABEL_FLUORESCENT_FRACTION] = 1; lab[ISC_LABEL_MODE] = ISC_MODE_PALM;
   Check(isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, lab, 3) == 0, "C ABI: a short label (the rest defaults)");
   // 4 x 4 um beside the nucleus of cell (-1,-1) at (-10.1, -5.0), see Determinism().
   const double x0 = -4, y0 = -7, x1 = 0, y1 = -3;
   const int32_t n = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, nullptr, 0);
   std::vector<double> buf((size_t)std::max(0, n) * ISC_SITE_STRIDE);
   const int32_t n2 = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, buf.data(), n);
   std::vector<float> grid(16 * 16);
   const int32_t n3 = isc_density_in_window(w, x0, y0, x1, y1, -INF, INF, 16, 16, grid.data());
   const int32_t nc = isc_cells_in_window(w, x0, y0, x1, y1, nullptr, 0);
   bool st = true;
   for (int32_t i = 0; i < n2; i++) st = st && buf[(size_t)i * ISC_SITE_STRIDE + 4] == ISC_STRUCT_MICROTUBULE;
   Check(n > 0 && n2 == n && n3 == n && nc > 0 && st, "C ABI: sites/density/cells agree, sites carry their structure");
   Check(isc_sites_in_window(w, 1, 0, 0, 1, -INF, INF, nullptr, 0) == -1, "C ABI: empty rect rejected");

   // Labels: accepted, rejected, refused as not implemented.
   double v[ISC_LABEL_COUNT] = { 0.05, 1, ISC_MODE_PALM, 0.5, 0.05, 0.5, 0.3, 0.4, 0, 0, ISC_ORIENT_FREE, 90, 0, 0, 0, 0 };
   Check(isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, v, ISC_LABEL_COUNT) == 0, "C ABI: label accepted");
   double bad[ISC_LABEL_COUNT];
   auto tryBad = [&](int idx, double val) {
      std::memcpy(bad, v, sizeof v);
      bad[idx] = val;
      return isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, bad, ISC_LABEL_COUNT);
   };
   Check(tryBad(ISC_LABEL_ACTIVATION_RATE, -1) == -1 && tryBad(ISC_LABEL_ON_SEC, 0) == -1 && tryBad(ISC_LABEL_MODE, 4) == -1 &&
            tryBad(ISC_LABEL_MODE, 1.5) == -1 && tryBad(ISC_LABEL_ORIENT_MODE, 3) == -1 &&
            isc_world_set_label(w, 1, v, ISC_LABEL_COUNT) == -1 && isc_world_set_label(w, 0, v, ISC_LABEL_COUNT + 1) == -1,
         "C ABI: bad labels rejected (-1)");
   std::memcpy(bad, v, sizeof v);
   bad[ISC_LABEL_MODE] = ISC_MODE_DSTORM; bad[ISC_LABEL_PRE_STATE] = 1;
   Check(isc_world_set_label(w, 0, bad, ISC_LABEL_COUNT) == -1, "C ABI: a pre state without PALM rejected");
   Check(tryBad(ISC_LABEL_MOTION, 1) == -2 && tryBad(ISC_LABEL_OFFTARGET_COUNT, 1) == -2,
         "C ABI: motion and off-target refused as not implemented yet (-2)");
   const int32_t ne = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, nullptr, 0);
   std::vector<double> ev((size_t)std::max(0, ne) * ISC_EVENT_STRIDE);
   const int32_t ne2 = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, ev.data(), ne);
   bool evOk = ne > 0 && ne2 == ne;
   for (int32_t i = 0; evOk && i < ne; i++) {
      const double* e = &ev[(size_t)i * ISC_EVENT_STRIDE];
      evOk = e[3] < 1.1 && e[4] > 1.0 && e[0] >= x0 && e[0] < x1 && e[1] >= y0 && e[1] < y1 && e[5] > 0 &&
             e[7] == ISC_STRUCT_MICROTUBULE && e[8] == ISC_STATE_BLINK && e[9] == 0;
   }
   Check(evOk, "C ABI: events overlap the frame, lie in the window, structure 0, state BLINK, aux 0");
   // Continuous windows: WideField, every dye one always-on window.
   v[ISC_LABEL_MODE] = ISC_MODE_WIDEFIELD;
   Check(isc_world_set_label(w, 0, v, ISC_LABEL_COUNT) == 0, "C ABI: WideField label");
   const int32_t nw = isc_continuous_in_window(w, x0, y0, x1, y1, -INF, INF, 0, nullptr, 0);
   std::vector<double> cw((size_t)std::max(0, nw) * ISC_EVENT_STRIDE);
   const int32_t nw2 = isc_continuous_in_window(w, x0, y0, x1, y1, -INF, INF, 0, cw.data(), nw);
   bool cwOk = nw == n && nw2 == nw;
   for (int32_t i = 0; cwOk && i < nw; i++) {
      const double* e = &cw[(size_t)i * ISC_EVENT_STRIDE];
      cwOk = e[3] == 0 && std::isinf(e[4]) && e[5] == 1 && e[8] == ISC_STATE_ALWAYS_ON && e[9] > 0;
   }
   Check(cwOk && isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, nullptr, 0) == 0,
         "C ABI: WideField = one always-on window per dye, no blinks");

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

// ABI 5/10: the z-resolved density query by structure (WideField).
void Density3d()
{
   IscParams* p = isc_params_new();
   IscWorld* w = isc_world_new(1249, p);
   double lab[2] = { 0.5, 1 };
   isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, lab, 2);
   const double x0 = -4, y0 = -7, x1 = 0, y1 = -3, zLo = -2, zHi = 14;
   const int nx = 16, ny = 12, nz = 32;
   const size_t n2 = (size_t)nx * ny, n3 = n2 * nz;
   std::vector<float> all(n3), none(n3, 7.0f), flat(n2), slab(n2);
   const int32_t na = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 1 << ISC_STRUCT_MICROTUBULE, all.data());
   const int32_t n0 = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 0, none.data());
   const int32_t nf = isc_density_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, flat.data());
   Check(na > 0 && nf == na, "density3d: totals (microtubules = 2D)");
   bool sumZ = true;
   for (size_t i = 0; i < n2; i++) {
      double s = 0;
      for (int k = 0; k < nz; k++) s += all[k * n2 + i];
      sumZ = sumZ && s == flat[i];
   }
   Check(sumZ, "density3d: summed over z equals isc_density_in_window");
   Check(n0 == 0 && std::all_of(none.begin(), none.end(), [](float v) { return v == 0.0f; }), "density3d: no structure = zeros");

   // Hand-binning the sites.
   World ref = LabelledWorld(1249, Params(), MakeLabel(LabelMode::DnaPaint, 0.5));
   std::vector<WorldDye> d;
   ref.SitesInWindow(x0, y0, x1, y1, zLo, zHi, d);
   std::vector<float> hb(n3, 0.0f);
   for (const WorldDye& e : d) {
      const int ix = std::min(nx - 1, (int)std::floor((e.x - x0) * (nx / (x1 - x0))));
      const int iy = std::min(ny - 1, (int)std::floor((e.y - y0) * (ny / (y1 - y0))));
      const int iz = std::min(nz - 1, (int)std::floor((e.z - zLo) * (nz / (zHi - zLo))));
      hb[((size_t)iz * ny + iy) * nx + ix] += 1;
   }
   Check(hb == all, "density3d: matches hand-binned SitesInWindow");

   // nz = 1 over a slab = the 2D query with the same z limits; infinite z too.
   const int32_t ns = isc_density3d_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, 1, 1, slab.data());
   isc_density_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, flat.data());
   const int32_t ni = isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 1, 1, all.data());
   Check(slab == flat && ns > 0 && ni >= na, "density3d: nz = 1 slab equals the 2D query");
   Check(isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 2, 1, all.data()) == -1 &&
            isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 2, all.data()) == -1 &&
            isc_density3d_in_window(w, x0, y0, x1, y1, zHi, zLo, nx, ny, nz, 1, all.data()) == -1,
         "density3d: bad arguments rejected (unknown structure bit too)");
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
   // tMax: the blinks before it are the full schedule's (a prefix).
   k.bleachProb = 0.05; k.photonCV = 0.3;
   bool prefix = true;
   for (int i = 0; i < 2000 && prefix; i++) {
      std::vector<Blink> full, part;
      DyeSchedule(DyeH1(7, i, 1, 0), i % 13, i, k, full);
      const double tMax = 5.0 + (i % 40);
      DyeSchedule(DyeH1(7, i, 1, 0), i % 13, i, k, part, tMax);
      size_t m = 0;
      while (m < full.size() && full[m].tOn < tMax) m++;
      prefix = part.size() == m;
      for (size_t j = 0; prefix && j < m; j++)
         prefix = part[j].tOn == full[j].tOn && part[j].tOff == full[j].tOff && part[j].brightness == full[j].brightness;
   }
   Check(prefix, "DyeSchedule(tMax) = the full schedule's blinks starting before tMax");
}

bool SameEvents(std::vector<WorldEvent> a, std::vector<WorldEvent> b)
{
   auto key = [](const WorldEvent& e) {
      return std::make_tuple(e.id, e.tOn, e.x, e.y, e.z, e.tOff, e.brightness, e.structure, e.state, e.aux);
   };
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
// back, across range ends and after a label change. Run for a bleaching
// (PALM) and a persistent (DNA-PAINT) label.
void CacheUnderLoad(LabelMode mode, const char* name)
{
   std::printf("    %s\n", name);
   Params p;
   const uint32_t seed = 1249;
   Kinetics k;
   k.activationRatePerSec = 0.2; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   const Label lab = MakeLabel(mode, 0.05, k);
   World w = LabelledWorld(seed, p, lab, 48, 1000);   // a dye cache far smaller than the window
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
      World a = LabelledWorld(seed, p, lab, 48, 1000), b = LabelledWorld(seed, p, lab, 48, 1000);
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
      World a = LabelledWorld(seed, p, lab, 48, 1000), b = LabelledWorld(seed, p, lab, 48, 1000);
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
   // Frame after frame (the persistent blinks from blocks extending their bins
   // ahead of time, staggered per block; the bleaching ones from windowed
   // schedules): still the brute force's.
   bool steady = true;
   for (int f = 0; f < 70; f++) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, 40 + f * fd, 40 + (f + 1) * fd, ev);
      if (f % 7 == 0 || f == 19 || f == 20) {
         std::vector<WorldDye> ds;
         w.SitesInWindow(x0, y0, x1, y1, -INF, INF, ds);
         std::vector<WorldEvent> ref;
         const double t0 = 40 + f * fd, t1 = t0 + fd;
         for (const WorldDye& d : ds) BruteBlinks(seed, d, w.GetLabel(STRUCTURE_MT), t0, t1, ref);
         steady = steady && SameEvents(ev, ref);
      }
   }
   Check(steady, "blinks frame by frame across bin extensions / schedule horizons = brute force");

   std::vector<WorldDye> dyes;
   w.SitesInWindow(x0, y0, x1, y1, -INF, INF, dyes);
   auto brute = [&](double t0, double t1) {
      std::vector<WorldEvent> out;
      for (const WorldDye& d : dyes) BruteBlinks(seed, d, w.GetLabel(STRUCTURE_MT), t0, t1, out);
      return out;
   };
   bool same = true;
   long nEv = 0;
   for (double t : { 3.0, 15.97, 16.0, 31.99, 47.5, 0.02, 200.0, 199.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      same = same && SameEvents(ev, brute(t, t + fd));
      nEv += (long)ev.size();
   }
   Kinetics k2 = k;
   k2.activationRatePerSec = 3;   // more blinks per bin: shorter bin ranges
   w.SetLabel(STRUCTURE_MT, MakeLabel(mode, 0.05, k2));
   for (double t : { 7.3, 7.35, 9.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      same = same && SameEvents(ev, brute(t, t + fd));
   }
   Check(same && nEv > 0, "cached blinks = brute force (time jumps, range ends, new kinetics)");
}

// The event query (spec/PORT.md 6.2): brute force equality, determinism,
// time slicing, and the per-frame cost at the default FOV, for one label.
// PALM without a pre state and DNA-PAINT are the old bleaching dyes and
// persistent sites (web/lab/label_regression.mjs on the JS).
void EventQuery(const Label& lab, const char* name)
{
   std::printf("    %s\n", name);
   Params p;
   const uint32_t seed = 1249;
   World w = LabelledWorld(seed, p, lab);
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
   for (const WorldDye& d : dyes) BruteBlinks(seed, d, lab, t0, t1, brute);
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

   // Determinism: away and back, dropped caches, label round trip, other world,
   // and any query history (a late window first, an early one after).
   std::vector<WorldEvent> again;
   std::vector<WorldDye> far;
   w.SitesInWindow(x0 + 1000, y0, x1 + 1000, y1, -INF, INF, far);
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: stage 1 mm away and back, identical");
   Label l2 = lab;
   l2.kin.onSec = 0.2;
   w.SetLabel(STRUCTURE_MT, l2);
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   const bool changed = !SameEvents(got, again);
   w.SetLabel(STRUCTURE_MT, lab);
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(changed && SameEvents(got, again), "events: label change and back, identical");
   w.DropCaches();
   again.clear();
   w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: after dropping every cache, identical");
   World tiny = LabelledWorld(seed, p, lab, 1, 1000);   // caches smaller than one FOV
   again.clear();
   tiny.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   Check(SameEvents(got, again), "events: independent of cache sizes");
   World hist = LabelledWorld(seed, p, lab);
   std::vector<WorldEvent> late, early, lateRef;
   hist.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 300, 300 + fd, late);
   hist.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 0, 400, early);
   again.clear();
   hist.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, again);
   std::vector<WorldEvent> lateAgain;
   hist.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 300, 300 + fd, lateAgain);
   for (const WorldDye& d : dyes) BruteBlinks(seed, d, lab, 300, 300 + fd, lateRef);
   Check(SameEvents(got, again) && SameEvents(late, lateRef) && SameEvents(late, lateAgain),
         "events: windowed schedules independent of the query history (late, long, early, late)");
}

// Persistent (DNA-PAINT) sites: a constant blink rate per site at any time,
// windows answered consistently.
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
}

// The label model (issue 16; web/lab/label_regression.mjs on the JS): the
// continuous windows of each mode, orientation statistics, refusals.
void LabelModel()
{
   Params p;
   const uint32_t seed = 1249;
   Kinetics kin;
   kin.activationRatePerSec = 0.05; kin.onSec = 0.05; kin.offSec = 1.0; kin.bleachProb = 0.5; kin.photonCV = 0.5;
   std::vector<Cell> near;
   World probe(seed, p);
   probe.CellsInRect(-30, -30, 30, 30, near);
   if (near.empty()) { Check(false, "cells near the origin"); return; }
   const double x0 = near[0].x + 6, y0 = near[0].y - 2, x1 = x0 + 4, y1 = y0 + 4, zLo = -5, zHi = 50;

   // dSTORM: an initial ON window Exp(initialOnSec) per dye, the blinks after it.
   {
      Kinetics k = kin;
      k.initialOnSec = 2;
      Label l = MakeLabel(LabelMode::dSTORM, 0.1, k);
      World w = LabelledWorld(seed, p, l);
      std::vector<WorldDye> s;
      std::vector<WorldEvent> c, ev;
      w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, s);
      w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, -INF, c);
      w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 0, 1e9, ev);
      double m = 0;
      bool ok = c.size() == s.size() && !s.empty();
      for (size_t i = 0; ok && i < c.size(); i++) {
         m += c[i].tOff;
         ok = c[i].state == STATE_INITIAL_ON && c[i].tOn == 0 && c[i].id == s[i].id && c[i].x == s[i].x && c[i].aux == 0;
      }
      m /= std::max<size_t>(1, c.size());
      // Every blink starts after its dye's initial ON window ends.
      bool after = true;
      for (const WorldEvent& e : ev)
         for (const WorldEvent& w0 : c)
            if (w0.id == e.id && w0.x == e.x) after = after && e.tOn >= w0.tOff;
      std::vector<WorldEvent> late;
      w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, 3.0, late);
      size_t open = 0;
      for (const WorldEvent& e : c) open += e.tOff > 3.0;
      std::printf("      dSTORM initial ON: %zu windows for %zu dyes, mean %.3f s (2), %zu blinks, %zu open at 3 s\n",
                  c.size(), s.size(), m, ev.size(), late.size());
      Check(ok && std::fabs(m - 2) < 0.2 && after && !ev.empty() && late.size() == open,
            "dSTORM: initial ON windows ~ Exp(initialOnSec), blinks after them, tMin filter");
   }
   // WideField: one always-on window per dye, aux ~ Exp(1), no blinks.
   {
      World w = LabelledWorld(seed, p, MakeLabel(LabelMode::WideField, 0.1, kin));
      std::vector<WorldDye> s;
      std::vector<WorldEvent> c, ev;
      w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, s);
      w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, 1e6, c);
      w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, 0, 100, ev);
      double ma = 0;
      bool ok = c.size() == s.size() && !s.empty();
      for (const WorldEvent& e : c) { ma += e.aux; ok = ok && std::isinf(e.tOff) && e.state == STATE_ALWAYS_ON; }
      ma /= std::max<size_t>(1, c.size());
      std::printf("      WideField: %zu always-on windows, mean aux %.3f (1)\n", c.size(), ma);
      Check(ok && ev.empty() && std::fabs(ma - 1) < 0.08, "WideField: one always-on window per dye, aux ~ Exp(1), no blinks");
   }
   // PALM with a pre state: the pre window ends at the first blink; without: none.
   {
      Label l = MakeLabel(LabelMode::PALM, 0.1, kin);
      l.preState = true;
      World w = LabelledWorld(seed, p, l);
      std::vector<WorldDye> s;
      std::vector<WorldEvent> c;
      w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, s);
      w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, -INF, c);
      bool ok = c.size() == s.size() && !s.empty();
      for (size_t i = 0; ok && i < c.size(); i++) {
         std::vector<Blink> bl;
         DyeSchedule(DyeH1(seed, s[i].cx, s[i].cy, s[i].mtIndex), s[i].k, s[i].n, kin, bl);
         ok = c[i].state == STATE_PRE && c[i].x == s[i].x && c[i].tOff == (bl.empty() ? INF : bl[0].tOn) && c[i].aux > 0;
      }
      World w2 = LabelledWorld(seed, p, MakeLabel(LabelMode::PALM, 0.1, kin));
      std::vector<WorldEvent> c2;
      w2.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, -INF, c2);
      Check(ok && c2.empty(), "PALM: the pre state ends at the first blink; no pre state, no windows");
      // DNA-PAINT has no continuous windows.
      World w3 = LabelledWorld(seed, p, MakeLabel(LabelMode::DnaPaint, 0.1, kin));
      std::vector<WorldEvent> c3;
      w3.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, -INF, c3);
      Check(c3.empty(), "DNA-PAINT: no continuous windows");
   }
   // Orientation: Free = no dipole; Random isotropic (<z^2> = 1/3, <x> = 0);
   // Fixed unit vectors, polar 90 perpendicular to the axis, polar 0 along it.
   {
      Label l = MakeLabel(LabelMode::DnaPaint, 0.7, kin);
      World w = LabelledWorld(seed, p, l);
      std::vector<WorldDye> s;
      w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, s);
      if (s.size() > 4000) s.resize(4000);
      Pt3 dir;
      bool free = !s.empty();
      for (const WorldDye& d : s) free = free && !w.DyeOrientationOf(d, dir);
      l.orientation.mode = OrientationMode::Random;
      l.orientation.wobbleDeg = 20;
      w.SetLabel(STRUCTURE_MT, l);
      double z2 = 0, mx = 0;
      bool unit = true;
      for (const WorldDye& d : s) {
         w.DyeOrientationOf(d, dir);
         z2 += dir.z * dir.z; mx += dir.x;
         unit = unit && std::fabs(jsm::hypot(dir.x, dir.y, dir.z) - 1) < 1e-12;
      }
      z2 /= s.size(); mx /= s.size();
      // Fixed: against the segment tangent at the dye.
      auto axisDot = [&](const WorldDye& d, const Pt3& v) {
         Cell c;
         w.FindCell(d.cx, d.cy, c);
         CellAssets& A = w.Assets(c);
         const MtFrames& fr = A.Frames((size_t)d.mtIndex);
         const Pt3& t = fr.T[MtSegmentAt(fr, (MtProtofilamentOffsetNm(d.k) + d.n * MT_DIMER_NM) * 1e-3)];
         return t.x * v.x + t.y * v.y + t.z * v.z;
      };
      l.orientation.mode = OrientationMode::Fixed;
      l.orientation.polarDeg = 90; l.orientation.azimuthDeg = 30;
      w.SetLabel(STRUCTURE_MT, l);
      bool perp = true;
      for (size_t i = 0; i < s.size(); i += 7) {
         w.DyeOrientationOf(s[i], dir);
         perp = perp && std::fabs(axisDot(s[i], dir)) < 1e-9 && std::fabs(jsm::hypot(dir.x, dir.y, dir.z) - 1) < 1e-12;
      }
      l.orientation.polarDeg = 0;
      w.SetLabel(STRUCTURE_MT, l);
      bool along = true;
      for (size_t i = 0; i < s.size(); i += 7) {
         w.DyeOrientationOf(s[i], dir);
         along = along && std::fabs(axisDot(s[i], dir) - 1) < 1e-9;
      }
      std::printf("      orientation: Random <z^2> %.3f (1/3) <x> %.3f over %zu dyes\n", z2, mx, s.size());
      Check(free && unit && std::fabs(z2 - 1.0 / 3) < 0.02 && std::fabs(mx) < 0.03 && perp && along,
            "orientation: Free none, Random isotropic, Fixed polar 90 / 0 perpendicular to / along the axis");
   }
   // Refusals.
   {
      Label m;
      m.motion = 1;
      Label o;
      o.offTargetCount = 1;
      Label pr = MakeLabel(LabelMode::dSTORM, 0.1);
      pr.preState = true;
      World w(seed, p);
      const double d0 = w.GetLabel(STRUCTURE_MT).density;
      Check(ValidateLabel(m) && LabelNotImplemented(m) && ValidateLabel(o) && LabelNotImplemented(o) && ValidateLabel(pr) &&
               !LabelNotImplemented(pr) && !w.SetLabel(STRUCTURE_MT, m) && !w.SetLabel(STRUCTURE_MT, pr) &&
               !w.SetLabel(1, Label()) && w.GetLabel(STRUCTURE_MT).density == d0 && !ValidateLabel(Label()),
            "labels: SPT motion, off-target and a non-PALM pre state refused (nothing changes)");
   }
}

} // namespace

// The parallel block work (packing blocks, cell assets, dye blocks, blink
// schedules and persistent covers built on several threads) changes nothing:
// the same events in the same order -- the order the renderer sums them in --
// and the same build counts as one thread, for stack-sized and frame-sized
// queries, jumps, a label change and prefetches.
bool IdenticalEvents(const std::vector<WorldEvent>& a, const std::vector<WorldEvent>& b)
{
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++) {
      const WorldEvent &x = a[i], &y = b[i];
      if (x.id != y.id || std::memcmp(&x.x, &y.x, 7 * sizeof(double)) != 0 || x.structure != y.structure ||
          x.state != y.state)
         return false;
   }
   return true;
}

void Threads(LabelMode mode, double density, const char* name)
{
   std::printf("    %s\n", name);
   Params p;
   Kinetics k;
   k.activationRatePerSec = 0.01; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   const Label lab = MakeLabel(mode, density, k);
   World a = LabelledWorld(77, p, lab), b = LabelledWorld(77, p, lab);
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
         if (f == 25) w.SetLabel(STRUCTURE_MT, MakeLabel(mode, density, Kinetics{ 0.02, 0.05, 0.5, 0.5, 0.2, 0.0 }));
         w.EventsInWindow(s - 8, -8, s + 8, 8, 0, 4, 21 + f * 0.05, 21 + (f + 1) * 0.05, e);
         if (f % 4 == 0) w.Prefetch(s - 11, -11, s + 11, 11, -INF, INF, 21 + (f + 1) * 0.05, 21 + (f + 2) * 0.05, 1e9);
         if (f % 5 == 1) w.ContinuousInWindow(s - 8, -8, s + 8, 8, 0, 4, 0, e);
      });
   }
   const WorldStats &sa = a.Stats(), &sb = b.Stats();
   Check(same && total > 10000, "8 threads = 1 thread: the same events in the same order");
   Check(sa.blocksPacked == sb.blocksPacked && sa.cellsBuilt == sb.cellsBuilt && sa.dyeBlocks == sb.dyeBlocks &&
            sa.dyeBlockHits == sb.dyeBlockHits && sa.schedulesBuilt == sb.schedulesBuilt &&
            sa.persistentBuilt == sb.persistentBuilt,
         "8 threads = 1 thread: the same caches built");
}

// ABI 6: the optical volume (BrightField). Tiling, threads, z consistency,
// nucleus volume against the ellipsoid, bad arguments.
void OpticalVolume()
{
   Params p;
   // Cells are ~50 um across: a 140 um window holds several whole ones.
   const double x0 = -70, y0 = -66, x1 = 70, y1 = 74, zLo = -0.5, zHi = 9.5;
   const int nx = 140, ny = 140, nz = 20, sub = 2;
   const size_t plane = (size_t)nx * ny, chan = plane * nz;
   std::vector<float> whole(3 * chan), left(3 * chan / 2), right(3 * chan / 2), t8(3 * chan);
   World a(31, p), b(31, p);
   SetWorldThreads(1);
   const long cells = a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, nz, sub, whole.data());
   SetWorldThreads(8);
   b.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, nz, sub, t8.data());
   SetWorldThreads(0);
   Check(cells > 3 && whole == t8, "optical volume: 8 threads = 1 thread");
   // Left/right halves (same voxel grid) assemble to the whole window.
   const double xm = (x0 + x1) / 2;
   b.OpticalVolumeInWindow(x0, y0, xm, y1, zLo, zHi, nx / 2, ny, nz, sub, left.data());
   b.OpticalVolumeInWindow(xm, y0, x1, y1, zLo, zHi, nx / 2, ny, nz, sub, right.data());
   bool tiled = true;
   for (int ch = 0; ch < 3; ch++)
      for (int k = 0; k < nz; k++)
         for (int iy = 0; iy < ny; iy++)
            for (int ix = 0; ix < nx; ix++) {
               const float wv = whole[((size_t)(ch * nz + k) * ny + iy) * nx + ix];
               const std::vector<float>& h = ix < nx / 2 ? left : right;
               const float hv = h[((size_t)(ch * nz + k) * ny + iy) * (nx / 2) + ix % (nx / 2)];
               tiled = tiled && wv == hv;
            }
   Check(tiled, "optical volume: two half windows = the whole window");
   bool range = true;
   double cyto = 0, nuc = 0, mt = 0;
   for (size_t v = 0; v < chan; v++) {
      const double s = (double)whole[v] + whole[chan + v];
      range = range && whole[v] >= -1e-6 && whole[chan + v] >= 0 && s <= 1 + 1e-5 && whole[2 * chan + v] >= 0;
      cyto += whole[v]; nuc += whole[chan + v]; mt += whole[2 * chan + v];
   }
   Check(range && cyto > 0 && nuc > 0 && mt > 0, "optical volume: fractions in [0, 1], all three present");
   // nz = 1 over the same slab = the z sum (per column).
   std::vector<float> flat(3 * plane);
   a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, 1, sub, flat.data());
   double worst = 0;
   for (int ch = 0; ch < 3; ch++)
      for (size_t i = 0; i < plane; i++) {
         double s = 0;
         for (int k = 0; k < nz; k++) s += whole[(size_t)ch * chan + k * plane + i];
         if (ch < 2) worst = std::max(worst, std::fabs(s / nz - flat[ch * plane + i]));
      }
   Check(worst < 1e-5, "optical volume: nz = 1 equals the mean over z");
   // Nucleus volume of each cell entirely inside the window vs a direct
   // quadrature of its column (NucleusColumnLocal; the shaped nucleus) on a
   // 0.05 um grid. Region: within nucleus reach + 1 um of the nucleus centre.
   std::vector<Cell> cs;
   a.CellsInRect(x0, y0, x1, y1, cs);
   double want = 0;
   auto nucR = [](const Cell& q) { return std::max(q.nucLong / 2, q.nucShaped ? q.nucReach : 0.0) + 1; };
   for (const Cell& q : cs) {
      if (!(q.x - q.rOuter > x0 && q.x + q.rOuter < x1 && q.y - q.rOuter > y0 && q.y + q.rOuter < y1)) continue;
      const double R = nucR(q), hq = 0.05;
      for (double yy = -R + hq / 2; yy < R; yy += hq)
         for (double xx = -R + hq / 2; xx < R; xx += hq) {
            double below, above;
            if (NucleusColumnLocal(q, q.nucOffX + xx, q.nucOffY + yy, below, above)) want += (below + above) * hq * hq;
         }
   }
   std::vector<float> fine(3 * plane);
   double got = 0, gotAll = 0;
   a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, 1, 4, fine.data());
   const double vox = (x1 - x0) / nx * (y1 - y0) / ny * (zHi - zLo);
   for (const Cell& q : cs) {
      if (!(q.x - q.rOuter > x0 && q.x + q.rOuter < x1 && q.y - q.rOuter > y0 && q.y + q.rOuter < y1)) continue;
      for (int iy = 0; iy < ny; iy++)
         for (int ix = 0; ix < nx; ix++) {
            // Voxels whose centre lies near this cell's nucleus footprint.
            const double wx = x0 + (ix + 0.5) * (x1 - x0) / nx - q.x, wy = y0 + (iy + 0.5) * (y1 - y0) / ny - q.y;
            const double cr = std::cos(q.packRot), sr = std::sin(q.packRot);
            const double lx = wx * cr + wy * sr - q.nucOffX, ly = -wx * sr + wy * cr - q.nucOffY;
            if (lx * lx + ly * ly <= nucR(q) * nucR(q)) got += fine[plane + (size_t)iy * nx + ix] * vox;
         }
   }
   for (size_t i = 0; i < plane; i++) gotAll += fine[plane + i] * vox;
   Check(want > 0 && std::fabs(got / want - 1) < 0.02, "optical volume: nucleus volume = its column integral (2%)");
   std::printf("      nucleus volume %.2f um^3 vs column integrals %.2f (window %.2f)\n", got, want, gotAll);
   IscParams* ip = isc_params_new();
   IscWorld* w = isc_world_new(31, ip);
   std::vector<float> o(3 * 16 * 16 * 2);
   Check(isc_optical_volume_in_window(w, x0, y0, x1, y1, 0, 8, 16, 16, 2, 1, o.data()) >= 0 &&
            isc_optical_volume_in_window(w, x0, y0, x1, y1, -INF, 8, 16, 16, 2, 1, o.data()) == -1 &&
            isc_optical_volume_in_window(w, x0, y0, x1, y1, 0, 8, 16, 16, 2, 0, o.data()) == -1 &&
            isc_optical_volume_in_window(w, x0, y0, x1, y1, 8, 0, 16, 16, 2, 1, o.data()) == -1,
         "optical volume: C ABI, bad arguments rejected");
   isc_world_free(w);
   isc_params_free(ip);
}

// Fractal edge tail and relaxed cytoplasm height (spec/ALGORITHM.md "Edge
// roughness", "Height relaxation").
void EdgeAndHeight()
{
   auto eqDiam = [](const Params& p, double& meanTail) {
      double sum = 0, tail = 0;
      int n = 0;
      for (int cx = 0; cx < 12; cx++)
         for (int cy = 0; cy < 12; cy++) {
            const Cell c = RawCandidate(77, cx, cy, p);
            if (!c.present) continue;
            const std::vector<Pt2> o = CellOutlineLocal(c, 4096);
            double a = 0;
            for (size_t i = 0, j = o.size() - 1; i < o.size(); j = i++) a += o[j].x * o[i].y - o[i].x * o[j].y;
            sum += 2 * std::sqrt(std::fabs(a) / 2 / jsm::PI);
            tail += c.tailBound;
            n++;
         }
      meanTail = tail / n;
      return sum / n;
   };
   Params p;
   double t0 = 0, tb = 0;
   p.cellRough = 0;
   const double dOff = eqDiam(p, t0);
   Check(t0 == 0, "cellRough 0: no tail");
   p.cellRough = 0.15;
   p.cellBlob = 0;
   eqDiam(p, tb);
   Check(tb == 0, "cellBlob 0: no tail (roughness scales with blobbiness)");
   double worst = 0;
   for (double blob : { 0.5, 1.75, 3.0, 4.0 }) {
      p.cellBlob = blob;
      p.cellRough = 0;
      double t;
      const double d0 = eqDiam(p, t);
      p.cellRough = 0.15;
      const double d = eqDiam(p, tb);
      std::printf("      (no tail: %.2f um)\n", d0);
      worst = std::max(worst, std::fabs(d / d0 - 1));
      std::printf("      blob %.2f: equivalent diameter %.2f um (mean tail bound %.3f)\n", blob, d, tb);
   }
   std::printf("      blob 1.75 without tail: %.2f um\n", dOff);
   Check(worst < 0.04, "the tail keeps the equivalent diameter (4%) at blob 0.5-4");

   // The tail's power spectrum: |c_k|^2 ~ k^-(2H+1) = k^-(5-2D) over its band
   // (a self-affine r(theta) of Hurst H has box-counting dimension D = 2 - H).
   for (double D : { 1.35, 1.7 }) {
      Params q;
      q.cellFractalDim = D;
      q.cellRough = 0.05;   // small: the soft clamp stays linear
      std::vector<double> power(65, 0.0);
      const int M = 512;
      for (int cx = 0; cx < 10; cx++)
         for (int cy = 0; cy < 10; cy++) {
            const Cell c = RawCandidate(91, cx, cy, q);
            if (!c.present || !(c.tailBound > 0)) continue;
            std::vector<double> f(M);
            for (int i = 0; i < M; i++) f[i] = CellTailAt(c, 2 * jsm::PI * i / M);
            for (int k = TAIL_K0; k < TAIL_K0 + N_TAIL; k++) {
               double re = 0, im = 0;
               for (int i = 0; i < M; i++) { re += f[i] * std::cos(2 * jsm::PI * k * i / M); im += f[i] * std::sin(2 * jsm::PI * k * i / M); }
               power[k] += re * re + im * im;
            }
         }
      double sxx = 0, sxy = 0, sx = 0, sy = 0;
      int m = 0;
      for (int k = TAIL_K0; k < TAIL_K0 + N_TAIL; k++) {
         const double x = std::log((double)k), y = std::log(power[k]);
         sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
      }
      const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
      char msg[160];
      std::snprintf(msg, sizeof msg, "tail D %.2f: spectral slope %.2f vs -(5 - 2D) = %.2f (0.25)", D, slope, -(5 - 2 * D));
      Check(std::fabs(slope + (5 - 2 * D)) < 0.25, msg);
   }

   // Height grid: nucleus covered, and the relaxation removes the kinks.
   Params q;
   World w(1249, q);
   std::vector<Cell> cs;
   w.CellsInRect(-60, -60, 60, 60, cs);
   double worstCover = 1e9;
   std::vector<double> lapRelax, lapRaw;
   Params raw = q;
   raw.cytoRelaxUm = 0;
   for (const Cell& c : cs) {
      const CytoHeightGrid hg = BuildCytoHeightGrid(c, q), hr = BuildCytoHeightGrid(c, raw);
      const double margin = std::max(0.1, q.nucMargin);
      for (int k = 0; k < 2000; k++) {
         // Points of the nucleus footprint, in its own frame.
         const double ang = 2 * jsm::PI * k / 2000, rr = std::sqrt((k % 97) / 97.0) * 0.98;
         const double lx = std::cos(ang) * rr * c.nucLong / 2, ly = std::sin(ang) * rr * c.nucShort / 2;
         const double x = c.nucOffX + lx * std::cos(c.nucRot) - ly * std::sin(c.nucRot);
         const double y = c.nucOffY + lx * std::sin(c.nucRot) + ly * std::cos(c.nucRot);
         const double top = c.nucZ + c.nucHeight / 2 * std::sqrt(1 - rr * rr) + margin;
         worstCover = std::min(worstCover, SampleCytoHeightGrid(hg, x, y) - top);
      }
      // |laplacian| at nodes well inside the cell and off the nucleus.
      const int N = hg.N;
      for (int j = 2; j < N - 2; j++)
         for (int i = 2; i < N - 2; i++) {
            const double x = (i - hg.half) * hg.g, y = (j - hg.half) * hg.g;
            if (std::hypot(x, y) + 1.0 > CellRadiusAt(c, std::atan2(y, x))) continue;
            if (NucleusSignedDistLocal(c, x, y) < 1.0) continue;
            auto lap = [&](const CytoHeightGrid& G) {
               const size_t v = (size_t)j * N + i;
               return std::fabs((double)G.h[v - 1] + G.h[v + 1] + G.h[v - N] + G.h[v + N] - 4.0 * G.h[v]) / (G.g * G.g);
            };
            lapRelax.push_back(lap(hg));
            lapRaw.push_back(lap(hr));
         }
   }
   char msg[160];
   std::snprintf(msg, sizeof msg, "relaxed height covers the nucleus + margin (worst %.3f um, tol 0.02)", worstCover);
   Check(!cs.empty() && worstCover > -0.02, msg);
   auto p99 = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[(size_t)(0.99 * (v.size() - 1))]; };
   const double a = lapRelax.empty() ? 0 : p99(lapRelax), b = lapRaw.empty() ? 0 : p99(lapRaw);
   std::snprintf(msg, sizeof msg, "relaxation removes kinks: p99 |laplacian| %.2f vs raw %.2f per um", a, b);
   Check(!lapRaw.empty() && a < 0.5 * b, msg);
}

// ABI 7: a world that takes another world's packed blocks (as the viewer's
// workers take the pack worker's) answers exactly as that world, packs nothing
// for them, and rejects rows that are not its own cells.
void BlockInjection()
{
   Params p;
   World a(1249, p), b(1249, p), c(1249, p), other(7, p);
   const double x0 = -30, y0 = -30, x1 = 30, y1 = 30;
   std::vector<Cell> ca, cb;
   a.CellsInRect(x0, y0, x1, y1, ca);
   // Every block a 60 um rect can touch (reach ~100 um): [-2, 1]^2.
   int installed = 0, skipped = 0, rejected = 0;
   for (int32_t bx = -2; bx <= 1; bx++)
      for (int32_t by = -2; by <= 1; by++) {
         const std::vector<Cell>& cells = a.BlockCells(bx, by);
         std::vector<double> rows(cells.size() * 5);
         for (size_t i = 0; i < cells.size(); i++) {
            rows[5 * i] = cells[i].cx; rows[5 * i + 1] = cells[i].cy;
            rows[5 * i + 2] = cells[i].x; rows[5 * i + 3] = cells[i].y; rows[5 * i + 4] = cells[i].packRot;
         }
         bool sk = false;
         if (b.SetPackedBlock(bx, by, rows.data(), (int32_t)cells.size(), 5, sk)) installed += sk ? 0 : 1, skipped += sk ? 1 : 0;
         else rejected++;
         // The same rows from the other seed's point of view: not its cells.
         const std::vector<Cell>& oc = other.BlockCells(bx, by);
         std::vector<double> orows(oc.size() * 5);
         for (size_t i = 0; i < oc.size(); i++) {
            orows[5 * i] = oc[i].cx; orows[5 * i + 1] = oc[i].cy; orows[5 * i + 2] = oc[i].x;
            orows[5 * i + 3] = oc[i].y; orows[5 * i + 4] = oc[i].packRot;
         }
         if (!oc.empty() && c.SetPackedBlock(bx, by, orows.data(), (int32_t)oc.size(), 5, sk)) installed = -1000;
      }
   Check(installed == 16 && skipped == 0 && rejected == 0, "block injection: 16 blocks installed, none rejected");
   b.CellsInRect(x0, y0, x1, y1, cb);
   bool same = ca.size() == cb.size();
   for (size_t i = 0; same && i < ca.size(); i++) {
      const Cell &u = ca[i], &v = cb[i];
      same = u.cx == v.cx && u.cy == v.cy && u.x == v.x && u.y == v.y && u.packRot == v.packRot && u.rOuter == v.rOuter &&
             u.semiMajor == v.semiMajor && u.semiMinor == v.semiMinor && u.modFloor == v.modFloor && u.height == v.height &&
             u.nucOffX == v.nucOffX && u.nucOffY == v.nucOffY && u.nucRot == v.nucRot && u.nucZ == v.nucZ &&
             u.cytoMidHeight == v.cytoMidHeight && u.priority == v.priority && u.tailBound == v.tailBound;
   }
   Check(same && !ca.empty(), "block injection: the same cells (every field) as the world that packed them");
   Check(b.Stats().blocksPacked == 0 && b.Stats().blocksInjected == 16, "block injection: nothing packed by the taker");
   if (!ca.empty()) {
      CellAssets& A = a.Assets(ca[0]);
      CellAssets& B = b.Assets(cb[0], false);   // mesh only ...
      Check(A.geom.mesh.h == B.geom.mesh.h && !B.mtsBuilt, "block injection: the same cytoplasm mesh; mesh-only assets have no microtubules yet");
      CellAssets& B2 = b.Assets(cb[0]);   // ... then the microtubules on demand
      bool sameMt = B2.mtsBuilt && A.mts.size() == B2.mts.size();
      for (size_t i = 0; sameMt && i < A.mts.size(); i++) {
         sameMt = A.mts[i].pts.size() == B2.mts[i].pts.size();
         for (size_t q = 0; sameMt && q < A.mts[i].pts.size(); q++)
            sameMt = A.mts[i].pts[q].x == B2.mts[i].pts[q].x && A.mts[i].pts[q].z == B2.mts[i].pts[q].z;
      }
      Check(sameMt, "block injection: the same microtubules, built on demand after the mesh");
   }
   std::vector<WorldDye> da, db;
   a.SitesInWindow(x0, y0, x0 + 12.8, y0 + 12.8, -INF, INF, da);
   b.SitesInWindow(x0, y0, x0 + 12.8, y0 + 12.8, -INF, INF, db);
   Check(!da.empty() && SameDyes(da, db), "block injection: the same dyes");
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1));
   b.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1));
   std::vector<WorldEvent> ea, eb;
   a.EventsInWindow(x0, y0, x0 + 12.8, y0 + 12.8, -INF, INF, 0, 3, ea);
   b.EventsInWindow(x0, y0, x0 + 12.8, y0 + 12.8, -INF, INF, 0, 3, eb);
   bool sameEv = ea.size() == eb.size();
   for (size_t i = 0; sameEv && i < ea.size(); i++)
      sameEv = ea[i].x == eb[i].x && ea[i].tOn == eb[i].tOn && ea[i].brightness == eb[i].brightness && ea[i].id == eb[i].id;
   Check(!ea.empty() && sameEv, "block injection: the same events in the same order");
   bool sk = false;
   const std::vector<Cell>& again = a.BlockCells(0, 0);
   std::vector<double> rows(again.size() * 5, 0.0);
   for (size_t i = 0; i < again.size(); i++) { rows[5 * i] = again[i].cx; rows[5 * i + 1] = again[i].cy; }
   Check(b.SetPackedBlock(0, 0, rows.data(), (int32_t)again.size(), 5, sk) && sk, "block injection: a cached block is skipped");
   Check(c.Stats().blocksInjected == 0, "block injection: another seed's rows are rejected, nothing installed");
   std::vector<Cell> cc;
   c.CellsInRect(x0, y0, x1, y1, cc);
   Check(cc.size() == ca.size() && c.Stats().blocksPacked > 0, "block injection: the rejecting world still packs its own");
   b.DropCaches();
   cb.clear();
   b.CellsInRect(x0, y0, x1, y1, cb);
   Check(cb.size() == ca.size() && cb[0].x == ca[0].x && b.Stats().blocksPacked > 0,
         "block injection: after dropping the caches the taker packs the same cells itself");

   // Through the C ABI.
   IscParams* ip = isc_params_new();
   IscWorld* wa = isc_world_new(1249, ip);
   IscWorld* wb = isc_world_new(1249, ip);
   bool abiOk = true;
   for (int32_t bx = -2; bx <= 1 && abiOk; bx++)
      for (int32_t by = -2; by <= 1 && abiOk; by++) {
         const int32_t n = isc_world_pack_block(wa, bx, by, nullptr, 0);
         std::vector<double> buf((size_t)std::max(0, n) * ISC_CELL_STRIDE);
         abiOk = n >= 0 && isc_world_pack_block(wa, bx, by, buf.data(), n) == n && isc_world_set_block(wb, bx, by, buf.data(), n) == 1 &&
                 isc_world_set_block(wb, bx, by, buf.data(), n) == 0;
      }
   const int32_t na = isc_cells_in_window(wa, x0, y0, x1, y1, nullptr, 0), nb = isc_cells_in_window(wb, x0, y0, x1, y1, nullptr, 0);
   std::vector<double> ra((size_t)std::max(0, na) * ISC_CELL_STRIDE), rb((size_t)std::max(0, nb) * ISC_CELL_STRIDE);
   isc_cells_in_window(wa, x0, y0, x1, y1, ra.data(), na);
   isc_cells_in_window(wb, x0, y0, x1, y1, rb.data(), nb);
   Check(abiOk && na > 0 && na == nb && std::memcmp(ra.data(), rb.data(), ra.size() * sizeof(double)) == 0,
         "C ABI: isc_world_pack_block -> isc_world_set_block reproduces isc_cells_in_window");
   Check(isc_world_set_block(wb, 5, 5, ra.data(), 1) == -1, "C ABI: a row outside the block is rejected");
   isc_world_free(wa);
   isc_world_free(wb);
   isc_params_free(ip);
}

// The packed-block disk store (ABI 8): a second world of the same key takes
// every block from the file (same cells, nothing packed); another key or a
// damaged file means a repack and a rewritten file.
void BlockStoreTest()
{
   namespace fs = std::filesystem;
   std::error_code ec;
   const fs::path dir = fs::temp_directory_path(ec) /
      ("isc_blockstore_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
   const std::string d = dir.u8string();
   Params p;
#if defined(__EMSCRIPTEN__)
   // No file system in the viewer's WASM: never a store (the page keeps its own in local storage).
   World w0(1249, p);
   Check(!w0.SetCacheDir(d) && w0.Store() == nullptr, "block store: none under Emscripten (no file system)");
   return;
#endif
   auto sameCells = [](const std::vector<Cell>& a, const std::vector<Cell>& b) {
      bool same = a.size() == b.size() && !a.empty();
      for (size_t i = 0; same && i < a.size(); i++) {
         const Cell &u = a[i], &v = b[i];
         same = u.cx == v.cx && u.cy == v.cy && u.x == v.x && u.y == v.y && u.packRot == v.packRot && u.rOuter == v.rOuter &&
                u.nucOffX == v.nucOffX && u.priority == v.priority && u.tailBound == v.tailBound;
      }
      return same;
   };
   std::vector<Cell> ca, cb, cc, cd;
   long packedA = 0;
   {
      World a(1249, p);
      Check(a.SetCacheDir(d) && a.Store() && !a.Store()->Path().empty(), "block store: cache directory made and accepted");
      a.CellsInRect(-30, -30, 30, 30, ca);
      packedA = a.Stats().blocksPacked;
      const bool flushed = a.FlushCache();
      char m0[200];
      std::snprintf(m0, sizeof m0, "block store: every touched block packed and written (packed %ld, from store %ld, flush %d, pending %zu, stored %zu)",
                    packedA, a.Stats().blocksFromStore, (int)flushed, a.Store()->Pending(), a.Store()->Size());
      Check(packedA == 4 && a.Stats().blocksFromStore == 0 && flushed, m0);   // a 60 um rect reaches 2x2 blocks
   }   // (the destructor flushes too)
   Check(fs::file_size(dir / BLOCK_STORE_FILE, ec) > 20 && fs::file_size(dir / BLOCK_STORE_FILE, ec) < 64 * 1024,
         "block store: a small file (5 doubles per cell)");
   {
      World b(1249, p);
      const bool okB = b.SetCacheDir(d);
      char m1[200];
      std::snprintf(m1, sizeof m1, "block store: a new world of the same key loads the blocks (ok %d, loaded %zu)", (int)okB, b.Store() ? b.Store()->Size() : (size_t)0);
      Check(okB && (long)b.Store()->Size() == packedA, m1);
      b.CellsInRect(-30, -30, 30, 30, cb);
      Check(sameCells(ca, cb) && b.Stats().blocksPacked == 0 && b.Stats().blocksFromStore == packedA,
            "block store: the same cells (every field), nothing packed, every block taken from the file");
      // Microtubule parameters and labels do not touch the pose: same key.
      Params p2 = p;
      p2.mtDensity = 0.4;
      World b2(1249, p2);
      b2.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::dSTORM, 0.5));
      Check(b2.SetCacheDir(d) && (long)b2.Store()->Size() == packedA, "block store: mt* parameters and labels keep the key");
   }
   {
      Params p3 = p;
      p3.packFrac = 0.9;   // a packing parameter: another key
      World c(1249, p3);
      Check(c.SetCacheDir(d) && c.Store()->Size() == 0, "block store: a packing parameter changes the key (file ignored)");
      c.CellsInRect(-30, -30, 30, 30, cc);
      Check(c.Stats().blocksPacked == packedA && c.Stats().blocksFromStore == 0 && !sameCells(ca, cc), "block store: repacked with the other parameters");
   }   // overwrites the file with the p3 key
   {
      World e(1249, p);
      Check(e.SetCacheDir(d) && e.Store()->Size() == 0, "block store: the file now holds the other key");
   }
   {
      std::ofstream f(dir / BLOCK_STORE_FILE, std::ios::binary | std::ios::trunc);
      f << "ISCBLK01 garbage garbage garbage garbage";
   }
   {
      World g(1249, p);
      Check(g.SetCacheDir(d) && g.Store()->Size() == 0, "block store: a damaged file is ignored");
      g.CellsInRect(-30, -30, 30, 30, cd);
      Check(sameCells(ca, cd) && g.Stats().blocksPacked == packedA, "block store: ... and the world packs as usual");
      Check(g.SetCacheDir(std::string()) && g.Store() == nullptr, "block store: \"\" turns the store off");
   }
   {
      World h(1249, p);
      Check(h.SetCacheDir(d) && (long)h.Store()->Size() == packedA, "block store: the damaged file was rewritten with the blocks");
      h.SetCacheDir(std::string());
   }
   fs::remove_all(dir, ec);
   Check(!fs::exists(dir, ec), "block store: test directory removed");
}

// The build must not contract a*b - c into a fused multiply-add anywhere
// (every "same bits as the JS" argument depends on it, link-time optimisation
// included): with x = 1 + 2^-27, x*x rounds to 1 + 2^-26 and x*x - 1 is
// exactly 2^-26; a fused evaluation keeps the 2^-54 term.
void NoContraction()
{
   volatile double x = 1.0 + 7.450580596923828125e-9; // 1 + 2^-27
   const double xv = x;
   volatile double r = xv * xv - 1.0;
   Check(r == 1.490116119384765625e-8, "no floating-point contraction (x*x - 1 with x = 1 + 2^-27 is exactly 2^-26)");
}

int main()
{
   NoContraction();
   BlockStoreTest();
   Determinism();
   PackingOff();
   DyeStatistics();
   KineticsStats();
   PersistentSites();
   {
      Kinetics k;
      k.activationRatePerSec = 1.0 / 30; k.onSec = 0.03; k.offSec = 0.3; k.bleachProb = 0.25; k.photonCV = 0.3;
      EventQuery(MakeLabel(LabelMode::PALM, 0.1, k), "event query: PALM 10 % (the old bleaching dyes)");
      Kinetics kp = k;
      kp.activationRatePerSec = 0.02;
      EventQuery(MakeLabel(LabelMode::DnaPaint, 0.3, kp), "event query: DNA-PAINT 30 % (the old persistent sites)");
      Kinetics kd = k;
      kd.initialOnSec = 1.5;
      EventQuery(MakeLabel(LabelMode::dSTORM, 0.1, kd), "event query: dSTORM 10 % with an initial ON");
   }
   CacheUnderLoad(LabelMode::PALM, "caches under load: PALM");
   CacheUnderLoad(LabelMode::DnaPaint, "caches under load: DNA-PAINT");
   LabelModel();
   Threads(LabelMode::PALM, 0.3, "threads: PALM 30 %");
   Threads(LabelMode::DnaPaint, 0.3, "threads: DNA-PAINT 30 %");
   CApi();
   BlockInjection();
   Density3d();
   OpticalVolume();
   EdgeAndHeight();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall world checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
