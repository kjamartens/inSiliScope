// World-level checks (spec/PORT.md section 10, items 2, 3, 4, 6): determinism under any query history, tiling,
// packing off, dye lattice statistics, blink kinetics, the event query, caches, threads, the C ABI and the structures.
// Built natively and with Emscripten (run under Node), like isc_parity.
//
//   isc_world_tests [section]   exit code 0 = every check passed. Each line starts with the ms since the previous
//                               check (a slow one shows). A failed check prints why (the measured value and its
//                               bound, or the first difference) and is listed again at the end.
//
// Speed (2026-10-27): the checks run on Cheap() parameters (10 relaxation passes, sparse microtubules) and small
// windows. What they check -- determinism, tiling, caches, threads, the C ABI, the structures' invariants -- holds for
// any parameters; the default parameters are the golden vectors' and label_parity's job. A world's packed blocks are
// packed once per process (Fresh()). The thread checks run natively only (WASM has no threads).
#include "cells.h"
#include "cytomesh.h"
#include "dyes.h"
#include "jsmath.h"
#include "microtubules.h"
#include "npcs.h"
#include "origami.h"
#include "fiducials.h"
#include "parallel.h"
#include "params.h"
#include "world.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace isc;

namespace {

int g_failures = 0;
std::vector<std::string> g_failed;

std::string Fmt(const char* f, ...)
{
   char b[1024];
   va_list a;
   va_start(a, f);
   std::vsnprintf(b, sizeof b, f, a);
   va_end(a);
   return b;
}

// One line per check; a failure adds why (the measured value against its bound, or the first difference).
void Check(bool ok, const std::string& what, const std::string& why = std::string())
{
   static auto last = std::chrono::steady_clock::now();
   const auto now = std::chrono::steady_clock::now();
   std::printf("%6.0f %s  %s\n", std::chrono::duration<double, std::milli>(now - last).count(), ok ? "ok  " : "FAIL", what.c_str());
   last = now;
   if (ok) return;
   std::printf("        -> %s\n", why.empty() ? "(no detail)" : why.c_str());
   g_failures++;
   g_failed.push_back(what);
}
// A comparison: "" = the same, else its first difference.
void Same(const std::string& diff, const std::string& what) { Check(diff.empty(), what, diff); }
void Skip(const std::string& what, const char* why) { std::printf("skip  %s (%s)\n", what.c_str(), why); }

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
constexpr bool kThreads = false;   // WorldThreads() is 1: an "8 threads = 1 thread" check would compare serial with serial
#else
constexpr bool kThreads = true;
#endif

const double INF = std::numeric_limits<double>::infinity();

// The checks' parameters: no pores (the Npcs and Lamin sections add them), 10 relaxation passes instead of 80 and a
// sixth of the microtubules -- a world packs and builds its assets ~10x faster.
Params Cheap()
{
   Params p;
   p.npcDensity = 0;
   p.relaxIters = 10;
   p.mtDensity = 0.15;
   return p;
}
// The same through the C ABI (plus the given parameters).
IscParams* AbiParams(std::initializer_list<std::pair<const char*, double>> extra = {})
{
   IscParams* p = isc_params_new();
   isc_params_set(p, "npcDensity", 0);
   isc_params_set(p, "relaxIters", 10);
   isc_params_set(p, "mtDensity", 0.15);
   for (const auto& e : extra)
      if (isc_params_set(p, e.first, e.second) != 0) std::printf("      (unknown parameter %s)\n", e.first);
   return p;
}
Params CheapWith(std::initializer_list<std::pair<const char*, double>> extra)
{
   Params p = Cheap();
   for (const auto& e : extra)
      if (!SetParam(p, e.first, e.second)) std::printf("      (unknown parameter %s)\n", e.first);
   return p;
}

// A new world whose packed blocks [-1, 0]^2 (every query within ~70 um of the origin) come from the process's first
// world of the same seed and packing parameters, as the viewer's workers take the pack worker's (SetPackedBlock; the
// equality is block_injection's check): the packing is paid once per key, not once per world.
World Fresh(uint32_t seed, const Params& p, size_t assetCells = 48, size_t dyeCap = 2000000)
{
   static std::map<std::pair<uint32_t, uint64_t>, std::vector<std::vector<double>>> bank;
   const auto key = std::make_pair(seed, PackingFingerprint(p));
   auto it = bank.find(key);
   if (it == bank.end()) {
      World donor(seed, p);
      std::vector<std::vector<double>> rows;
      for (int32_t bx = -1; bx <= 0; bx++)
         for (int32_t by = -1; by <= 0; by++) {
            std::vector<double> r;
            for (const Cell& c : donor.BlockCells(bx, by)) r.insert(r.end(), { (double)c.cx, (double)c.cy, c.x, c.y, c.packRot });
            rows.push_back(std::move(r));
         }
      it = bank.emplace(key, std::move(rows)).first;
   }
   World w(seed, p, assetCells, dyeCap);
   size_t i = 0;
   for (int32_t bx = -1; bx <= 0; bx++)
      for (int32_t by = -1; by <= 0; by++, i++) {
         bool skipped = false;
         const std::vector<double>& r = it->second[i];
         if (!r.empty()) w.SetPackedBlock(bx, by, r.data(), (int32_t)(r.size() / 5), 5, skipped);
      }
   return w;
}

// World::SetKineticsHistory with one kinetics per segment, used for every structure (it takes
// STRUCTURE_COUNT per segment, kin[i * STRUCTURE_COUNT + s]).
bool SetHist(World& w, const std::vector<double>& tStart, const std::vector<Kinetics>& perSegment)
{
   std::vector<Kinetics> kin;
   for (const Kinetics& k : perSegment)
      for (int s = 0; s < STRUCTURE_COUNT; s++) kin.push_back(k);
   return w.SetKineticsHistory(tStart, kin);
}

// ---- Comparisons: "" = the same, else the first difference with both values.

std::string DyeStr(const WorldDye& d)
{
   return Fmt("id %u (%.9g, %.9g, %.9g) cell (%d, %d) mt %d k %d n %d structure %d", d.id, d.x, d.y, d.z, d.cx, d.cy,
              d.mtIndex, d.k, d.n, d.structure);
}
std::string DiffDyes(const std::vector<WorldDye>& a, const std::vector<WorldDye>& b)
{
   const size_t n = std::min(a.size(), b.size());
   for (size_t i = 0; i < n; i++)
      if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z || a[i].id != b[i].id)
         return Fmt("%zu vs %zu dyes, first difference at #%zu: ", a.size(), b.size(), i) + DyeStr(a[i]) + " vs " +
                DyeStr(b[i]);
   if (a.size() != b.size()) return Fmt("%zu vs %zu dyes (the first %zu agree)", a.size(), b.size(), n);
   return "";
}
bool DyeLess(const WorldDye& a, const WorldDye& b)
{
   return std::tie(a.cx, a.cy, a.structure, a.mtIndex, a.id, a.x) < std::tie(b.cx, b.cy, b.structure, b.mtIndex, b.id, b.x);
}
std::vector<WorldDye> Sorted(std::vector<WorldDye> v)
{
   std::sort(v.begin(), v.end(), DyeLess);
   return v;
}
// "" if every dye of sub is in all (sorted), else the first that is not.
std::string NotSubset(const std::vector<WorldDye>& sub, const std::vector<WorldDye>& allSorted)
{
   for (size_t i = 0; i < sub.size(); i++)
      if (!std::binary_search(allSorted.begin(), allSorted.end(), sub[i], DyeLess))
         return Fmt("dye #%zu of %zu is not in the larger set: ", i, sub.size()) + DyeStr(sub[i]);
   return "";
}

std::string EventStr(const WorldEvent& e)
{
   return Fmt("id %u structure %d state %d (%.9g, %.9g, %.9g) [%.12g, %.12g) brightness %.9g aux %.9g", e.id,
              e.structure, e.state, e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.aux);
}
// In the same order, every field bit for bit (the order the renderer sums them in).
std::string DiffOrdered(const std::vector<WorldEvent>& a, const std::vector<WorldEvent>& b)
{
   const size_t n = std::min(a.size(), b.size());
   for (size_t i = 0; i < n; i++) {
      const WorldEvent &x = a[i], &y = b[i];
      if (x.id != y.id || std::memcmp(&x.x, &y.x, 7 * sizeof(double)) != 0 || x.structure != y.structure ||
          x.state != y.state)
         return Fmt("%zu vs %zu events, first difference at #%zu: ", a.size(), b.size(), i) + EventStr(x) + " vs " +
                EventStr(y);
   }
   if (a.size() != b.size()) return Fmt("%zu vs %zu events (the first %zu agree)", a.size(), b.size(), n);
   return "";
}
void SortEvents(std::vector<WorldEvent>& v)
{
   auto key = [](const WorldEvent& e) {
      return std::make_tuple(e.id, e.tOn, e.x, e.y, e.z, e.tOff, e.brightness, e.structure, e.state, e.aux);
   };
   std::sort(v.begin(), v.end(), [&](const WorldEvent& x, const WorldEvent& y) { return key(x) < key(y); });
}
// The same set of events (any order).
std::string DiffEvents(std::vector<WorldEvent> a, std::vector<WorldEvent> b)
{
   SortEvents(a);
   SortEvents(b);
   return DiffOrdered(a, b);
}
std::string DiffWindows(const std::vector<MapWindow>& a, const std::vector<MapWindow>& b)
{
   const size_t n = std::min(a.size(), b.size());
   for (size_t i = 0; i < n; i++) {
      const WorldEvent &x = a[i].e, &y = b[i].e;
      if (x.id != y.id || std::memcmp(&x.x, &y.x, 7 * sizeof(double)) != 0 || x.structure != y.structure ||
          x.state != y.state || a[i].clock != b[i].clock || a[i].rate != b[i].rate)
         return Fmt("%zu vs %zu windows, first difference at #%zu: ", a.size(), b.size(), i) + EventStr(x) +
                Fmt(" clock %.12g rate %.12g vs ", a[i].clock, a[i].rate) + EventStr(y) +
                Fmt(" clock %.12g rate %.12g", b[i].clock, b[i].rate);
   }
   if (a.size() != b.size()) return Fmt("%zu vs %zu windows (the first %zu agree)", a.size(), b.size(), n);
   return "";
}
std::vector<WorldEvent> Until(std::vector<WorldEvent> e, double T)   // the events that end before T
{
   e.erase(std::remove_if(e.begin(), e.end(), [&](const WorldEvent& x) { return !(x.tOff < T); }), e.end());
   return e;
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

World Labelled(uint32_t seed, const Params& p, const Label& l, size_t assetCells = 48, size_t dyeCap = 2000000)
{
   World w = Fresh(seed, p, assetCells, dyeCap);
   w.SetLabel(STRUCTURE_MT, l);
   return w;
}

// The first cell of [-30, 30]^2 um (the windows sit beside its nucleus); false (and a failed check) if none.
bool FirstCell(uint32_t seed, const Params& p, Cell& c)
{
   World w = Fresh(seed, p);
   std::vector<Cell> near;
   w.CellsInRect(-30, -30, 30, 30, near);
   Check(!near.empty(), "cells near the origin", "none in [-30, 30]^2 um");
   if (near.empty()) return false;
   c = near[0];
   return true;
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
   const Params p = Cheap();
   const uint32_t seed = 1249;
   const Label l10 = MakeLabel(LabelMode::PALM, 0.1);
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   const double W = 6.4, x0 = c0.x + 3, y0 = c0.y - W / 2;
   auto sites = [&](World& w, double ox, double oy, double zLo, double zHi) {
      std::vector<WorldDye> v;
      w.SitesInWindow(x0 + ox, y0 + oy, x0 + ox + W, y0 + oy + W, zLo, zHi, v);
      return v;
   };
   World a = Labelled(seed, p, l10);
   const std::vector<WorldDye> first = sites(a, 0, 0, -INF, INF);
   Check(first.size() > 500, "the window holds dyes", Fmt("%zu dyes (want > 500)", first.size()));
   Same(DiffDyes(first, sites(a, 0, 0, -INF, INF)), "repeat query identical");
   sites(a, 1000, 1000, -INF, INF);   // the stage 1 mm away: other blocks, evictions
   Same(DiffDyes(first, sites(a, 0, 0, -INF, INF)), "stage 1 mm away and back: identical");
   a.DropCaches();
   Same(DiffDyes(first, sites(a, 0, 0, -INF, INF)), "after dropping every cache: identical");
   {
      World b = Labelled(seed, p, l10, 2);   // a 2-cell asset cache, another window first
      sites(b, -30, 7, -INF, INF);
      Same(DiffDyes(first, sites(b, 0, 0, -INF, INF)), "independent of the query history (tiny asset cache)");
   }
   // Tiling: four quadrants = the whole window.
   std::vector<WorldDye> tiles;
   const double h = W / 2;
   for (int i = 0; i < 2; i++)
      for (int j = 0; j < 2; j++)
         a.SitesInWindow(x0 + i * h, y0 + j * h, x0 + (i + 1) * h, y0 + (j + 1) * h, -INF, INF, tiles);
   const std::vector<WorldDye> sf = Sorted(first);
   Same(DiffDyes(sf, Sorted(tiles)), "four quadrant windows = the whole window");
   // A z slab [1, 2) = the z filter of the full query.
   std::vector<WorldDye> inSlab;
   for (const WorldDye& d : first)
      if (d.z >= 1 && d.z < 2) inSlab.push_back(d);
   Check(!inSlab.empty(), "the z slab [1, 2) um holds dyes", "none");
   Same(DiffDyes(Sorted(inSlab), Sorted(sites(a, 0, 0, 1, 2))), "z-limited query = z filter of the full query");
   // The density grid sums to the site count.
   std::vector<float> grid(32 * 32);
   const long n = a.DensityInWindow(x0, y0, x0 + W, y0 + W, -INF, INF, 32, 32, grid.data());
   double sum = 0;
   for (float v : grid) sum += v;
   Check(n == (long)first.size(), "density query: count = the site count", Fmt("%ld vs %zu", n, first.size()));
   Check(sum == (double)n, "density grid sums to its count", Fmt("%.0f vs %ld", sum, n));

   // Density and fluorescent fraction thin the dyes (a hash per site: strict, nested subsets).
   {
      World c = Labelled(seed, p, MakeLabel(LabelMode::PALM, 0.05));
      const std::vector<WorldDye> sparse = sites(c, 0, 0, -INF, INF);
      const double ratio = (double)sparse.size() / first.size();
      Same(NotSubset(sparse, sf), "density 0.05: a subset of the 0.1 dyes");
      Check(ratio > 0.4 && ratio < 0.6, "density 0.05: about half of 0.1", Fmt("ratio %.3f (want 0.4-0.6)", ratio));
   }
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1, Kinetics(), 0.5));
   const std::vector<WorldDye> half = sites(a, 0, 0, -INF, INF);
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1, Kinetics(), 0.25));
   const std::vector<WorldDye> quarter = sites(a, 0, 0, -INF, INF);
   const double rh = (double)half.size() / first.size(), rq = (double)quarter.size() / half.size();
   Same(NotSubset(half, sf), "fluorescent fraction 0.5: a subset of the dyes");
   Same(NotSubset(quarter, Sorted(half)), "fluorescent fraction 0.25: a subset of the 0.5 dyes");
   Check(std::fabs(rh - 0.5) < 0.05 && std::fabs(rq - 0.5) < 0.07, "fluorescent fraction: about the fraction",
         Fmt("0.5 / 1 = %.3f (0.45-0.55), 0.25 / 0.5 = %.3f (0.43-0.57)", rh, rq));
   a.SetLabel(STRUCTURE_MT, l10);
   Same(DiffDyes(first, sites(a, 0, 0, -INF, INF)), "label back to fraction 1: the original dyes");
}

void PackingOff()
{
   Params p = Cheap();
   p.enablePacking = false;
   World a(7, p), b(7, p);
   std::vector<Cell> ca, cb, far;
   a.CellsInRect(-30, -30, 30, 30, ca);
   b.CellsInRect(500, 500, 520, 520, far);
   b.CellsInRect(-30, -30, 30, 30, cb);
   Check(!ca.empty(), "packing off: cells", "none in [-30, 30]^2 um");
   std::string diff = ca.size() != cb.size() ? Fmt("%zu vs %zu cells", ca.size(), cb.size()) : "";
   for (size_t i = 0; diff.empty() && i < ca.size(); i++)
      if (ca[i].x != cb[i].x || ca[i].y != cb[i].y || ca[i].packRot != 0)
         diff = Fmt("cell #%zu (%d, %d): (%.9g, %.9g) rot %g vs (%.9g, %.9g)", i, ca[i].cx, ca[i].cy, ca[i].x, ca[i].y,
                    ca[i].packRot, cb[i].x, cb[i].y);
   Same(diff, "packing off: deterministic, no rotation");
   diff.clear();
   for (const Cell& c : ca) {
      const Cell r = RawCandidate(7, c.cx, c.cy, p);
      if (diff.empty() && (r.x != c.x || r.y != c.y))
         diff = Fmt("cell (%d, %d): (%.9g, %.9g) vs raw (%.9g, %.9g)", c.cx, c.cy, c.x, c.y, r.x, r.y);
   }
   Same(diff, "packing off: positions are the raw jittered ones");
   World pk(7, Cheap());
   std::vector<Cell> packed;
   pk.CellsInRect(-30, -30, 30, 30, packed);
   Check(!packed.empty() && packed.size() <= ca.size(), "packing never adds cells",
         Fmt("%zu packed vs %zu unpacked in 60 x 60 um", packed.size(), ca.size()));
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
   Check(maxAttErr < 1e-9, "attachment on the 12.5 nm cylinder, at its arc position", Fmt("worst %.3g nm", maxAttErr));
   Check(maxTipErr < 1e-9, "binder tip at 24.5 nm", Fmt("worst %.3g nm", maxTipErr));
   Check(maxAngErr < 1e-9, "protofilament azimuths = phase + k 2pi/13", Fmt("worst %.3g rad", maxAngErr));
   Check(maxLink <= rMax + 1e-9, "linker within 5 nm of the tip", Fmt("longest %.6f nm", maxLink));
   const double fracBelow = (double)below / nLinks;
   Check(std::fabs(fracBelow - 0.5) < 0.06, "linker radius uniform in volume (r^3 CDF)",
         Fmt("%.3f of %d below the volume median (want 0.44-0.56)", fracBelow, nLinks));

   // Axial stagger 3*8/13 nm between neighbouring protofilaments.
   double worstStagger = 0;
   for (int k = 1; k < MT_N_PROTOFILAMENTS; k++)
      worstStagger = std::max(worstStagger, std::fabs(std::fmod(MtProtofilamentOffsetNm(k) - MtProtofilamentOffsetNm(k - 1) + 8, 8) - 24.0 / 13));
   Check(worstStagger < 1e-12, "13_3 lattice: axial stagger 24/13 nm", Fmt("worst %.3g nm", worstStagger));

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
   const auto dup = std::adjacent_find(kn.begin(), kn.end());
   Check((long)all.size() == expected, "blocks cover the lattice", Fmt("%zu sites, %ld on the lattice", all.size(), expected));
   Check(dup == kn.end(), "blocks partition the lattice (each site once)",
         dup == kn.end() ? "" : Fmt("site k %d n %d twice", dup->first, dup->second));
   Check(std::fabs(all.size() / L - 1625) < 5, "about 1625 sites per um", Fmt("%.1f per um", all.size() / L));
   std::vector<Dye> some;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, 1.0, some);
   const double mean = 0.1 * expected, sd = std::sqrt(expected * 0.1 * 0.9);
   Check(std::fabs(some.size() - mean) < 4 * sd, "labelled count ~ density x sites",
         Fmt("%zu labelled, expect %.0f +- 4 x %.1f", some.size(), mean, sd));
   // The fluorescent fraction thins the labelled sites (FLUOR), same positions.
   std::vector<Dye> fl;
   for (int b = 0; b < (int)std::ceil(L); b++) DyesInBlock(11, 2, 3, 0, pts, fr, b, 0.1, 0.4, fl);
   std::string missing;
   for (const Dye& d : fl) {
      bool found = false;
      for (const Dye& e : some) found = found || (e.k == d.k && e.n == d.n && e.pos.x == d.pos.x && e.id == d.id);
      if (!found && missing.empty()) missing = Fmt("dye k %d n %d id %u is not labelled at fraction 1", d.k, d.n, d.id);
   }
   Same(missing, "fluorescent fraction 0.4: a subset of the labelled dyes");
   const double m2 = 0.4 * some.size(), sd2 = std::sqrt(some.size() * 0.4 * 0.6);
   Check(std::fabs(fl.size() - m2) < 4 * sd2, "fluorescent fraction 0.4: ~0.4 of them",
         Fmt("%zu, expect %.0f +- 4 x %.1f", fl.size(), m2, sd2));
}

void CApi()
{
   IscParams* p = AbiParams();
   Check(isc_params_set(p, "labelEfficiency", 0.05) == -1, "C ABI: labelEfficiency is not a param any more (ABI 10)");
   IscWorld* w = isc_world_new(1249, p);
   double lab[ISC_LABEL_COUNT];
   lab[ISC_LABEL_DENSITY] = 0.05; lab[ISC_LABEL_FLUORESCENT_FRACTION] = 1; lab[ISC_LABEL_MODE] = ISC_MODE_PALM;
   Check(isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, lab, 3) == 0, "C ABI: a short label (the rest defaults)");
   Cell c0;
   if (!FirstCell(1249, Cheap(), c0)) return;
   const double x0 = c0.x + 3, y0 = c0.y - 2, x1 = x0 + 4, y1 = y0 + 4;
   const int32_t n = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, nullptr, 0);
   std::vector<double> buf((size_t)std::max(0, n) * ISC_SITE_STRIDE);
   const int32_t n2 = isc_sites_in_window(w, x0, y0, x1, y1, -INF, INF, buf.data(), n);
   std::vector<float> grid(16 * 16);
   const int32_t n3 = isc_density_in_window(w, x0, y0, x1, y1, -INF, INF, 16, 16, grid.data());
   const int32_t nc = isc_cells_in_window(w, x0, y0, x1, y1, nullptr, 0);
   Check(n > 0 && nc > 0, "C ABI: the window holds sites and cells", Fmt("%d sites, %d cells", n, nc));
   Check(n2 == n && n3 == n, "C ABI: sites and density agree on the count", Fmt("count %d, filled %d, density %d", n, n2, n3));
   int32_t badStruct = -1;
   for (int32_t i = 0; i < n2 && badStruct < 0; i++)
      if (buf[(size_t)i * ISC_SITE_STRIDE + 4] != ISC_STRUCT_MICROTUBULE) badStruct = i;
   Check(badStruct < 0, "C ABI: sites carry their structure", Fmt("site #%d", badStruct));
   Check(isc_sites_in_window(w, 1, 0, 0, 1, -INF, INF, nullptr, 0) == -1, "C ABI: empty rect rejected");

   // Labels: accepted, rejected, refused as not implemented.
   double v[ISC_LABEL_COUNT] = { 0.05, 1, ISC_MODE_PALM, 0.5, 0.05, 0.5, 0.3, 0.4, 0, 0, ISC_ORIENT_FREE, 90, 0, 0, 0, 0,
                                 NAN, NAN, NAN, NAN };   // NaN linkage: the structure's own
   Check(isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, v, ISC_LABEL_COUNT) == 0, "C ABI: label accepted");
   double bad[ISC_LABEL_COUNT];
   auto tryBad = [&](int idx, double val) {
      std::memcpy(bad, v, sizeof v);
      bad[idx] = val;
      return isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, bad, ISC_LABEL_COUNT);
   };
   const struct { int idx; double val; const char* what; } rejected[] = {
      { ISC_LABEL_ACTIVATION_RATE, -1, "a negative activation rate" }, { ISC_LABEL_ON_SEC, 0, "onSec 0" },
      { ISC_LABEL_MODE, 4, "mode 4" }, { ISC_LABEL_MODE, 1.5, "mode 1.5" }, { ISC_LABEL_ORIENT_MODE, 3, "orientation 3" },
      { ISC_LABEL_BINDER_FLEX_NM, INFINITY, "an infinite linkage" } };
   for (const auto& r : rejected) Check(tryBad(r.idx, r.val) == -1, std::string("C ABI: rejected (-1): ") + r.what);
   Check(isc_world_set_label(w, ISC_STRUCT_COUNT, v, ISC_LABEL_COUNT) == -1, "C ABI: rejected (-1): structure out of range");
   Check(isc_world_set_label(w, 0, v, ISC_LABEL_COUNT + 1) == -1, "C ABI: rejected (-1): too many values");
   std::memcpy(bad, v, sizeof v);
   bad[ISC_LABEL_MODE] = ISC_MODE_DSTORM; bad[ISC_LABEL_PRE_STATE] = 1;
   Check(isc_world_set_label(w, 0, bad, ISC_LABEL_COUNT) == -1, "C ABI: rejected (-1): a pre state without PALM");
   Check(tryBad(ISC_LABEL_MOTION, 1) == -2, "C ABI: motion refused as not implemented yet (-2)");
   Check(tryBad(ISC_LABEL_OFFTARGET_COUNT, 1) == -2, "C ABI: off-target refused as not implemented yet (-2)");
   const int32_t ne = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, nullptr, 0);
   std::vector<double> ev((size_t)std::max(0, ne) * ISC_EVENT_STRIDE);
   const int32_t ne2 = isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, ev.data(), ne);
   Check(ne > 0 && ne2 == ne, "C ABI: events counted and filled", Fmt("count %d, filled %d", ne, ne2));
   std::string evBad;
   for (int32_t i = 0; evBad.empty() && i < ne2; i++) {
      const double* e = &ev[(size_t)i * ISC_EVENT_STRIDE];
      if (!(e[3] < 1.1 && e[4] > 1.0 && e[0] >= x0 && e[0] < x1 && e[1] >= y0 && e[1] < y1 && e[5] > 0 &&
            e[7] == ISC_STRUCT_MICROTUBULE && e[8] == ISC_STATE_BLINK && e[9] == 0))
         evBad = Fmt("event #%d: (%g, %g) [%g, %g) brightness %g structure %g state %g aux %g", i, e[0], e[1], e[3], e[4],
                     e[5], e[7], e[8], e[9]);
   }
   Same(evBad, "C ABI: events overlap the frame, lie in the window, structure 0, state BLINK, aux 0");
   // Continuous windows: WideField, every dye one always-on window.
   v[ISC_LABEL_MODE] = ISC_MODE_WIDEFIELD;
   Check(isc_world_set_label(w, 0, v, ISC_LABEL_COUNT) == 0, "C ABI: WideField label");
   const int32_t nw = isc_continuous_in_window(w, x0, y0, x1, y1, -INF, INF, 0, nullptr, 0);
   std::vector<double> cw((size_t)std::max(0, nw) * ISC_EVENT_STRIDE);
   const int32_t nw2 = isc_continuous_in_window(w, x0, y0, x1, y1, -INF, INF, 0, cw.data(), nw);
   Check(nw == n && nw2 == nw, "C ABI: WideField = one window per dye", Fmt("%d windows (%d filled) for %d dyes", nw, nw2, n));
   std::string cwBad;
   for (int32_t i = 0; cwBad.empty() && i < nw2; i++) {
      const double* e = &cw[(size_t)i * ISC_EVENT_STRIDE];
      if (!(e[3] == 0 && std::isinf(e[4]) && e[5] == 1 && e[8] == ISC_STATE_ALWAYS_ON && e[9] > 0))
         cwBad = Fmt("window #%d: [%g, %g) brightness %g state %g aux %g", i, e[3], e[4], e[5], e[8], e[9]);
   }
   Same(cwBad, "C ABI: WideField windows always on from 0, aux > 0");
   Check(isc_events_in_window(w, x0, y0, x1, y1, -INF, INF, 1.0, 1.1, nullptr, 0) == 0, "C ABI: WideField has no blinks");

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
   Check(no >= 8, "C ABI: cell outline", Fmt("%d points", no));
   Check(nv == (dims[0] + 1) * dims[1], "C ABI: mesh size = (rings + 1) x theta", Fmt("%d vs (%d + 1) x %d", nv, dims[0], dims[1]));
   Check(nm > 0 && tot2 == tot && lensSum == tot, "C ABI: microtubule lengths sum to the points",
         Fmt("%d microtubules, %d points, filled %d, lengths sum %ld", nm, tot, tot2, lensSum));
   Check(cb[6] > 0, "C ABI: the cell row's radius", Fmt("%g", cb[6]));
   isc_world_free(w);
   isc_params_free(p);
}

// ABI 5/10: the z-resolved density query by structure (WideField).
void Density3d()
{
   IscParams* p = AbiParams();
   IscWorld* w = isc_world_new(1249, p);
   double lab[2] = { 0.5, 1 };
   isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, lab, 2);
   Cell c0;
   if (!FirstCell(1249, Cheap(), c0)) return;
   const double x0 = c0.x + 3, y0 = c0.y - 2, x1 = x0 + 4, y1 = y0 + 4, zLo = -2, zHi = 14;
   const int nx = 16, ny = 12, nz = 32;
   const size_t n2 = (size_t)nx * ny, n3 = n2 * nz;
   std::vector<float> all(n3), none(n3, 7.0f), flat(n2), slab(n2);
   const int32_t na = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 1 << ISC_STRUCT_MICROTUBULE, all.data());
   const int32_t n0 = isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 0, none.data());
   const int32_t nf = isc_density_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, flat.data());
   Check(na > 0 && nf == na, "density3d: the microtubule total = the 2D count", Fmt("%d vs %d", na, nf));
   std::string zd;
   for (size_t i = 0; zd.empty() && i < n2; i++) {
      double s = 0;
      for (int k = 0; k < nz; k++) s += all[k * n2 + i];
      if (s != flat[i]) zd = Fmt("column %zu: %.0f summed over z vs %.0f", i, s, (double)flat[i]);
   }
   Same(zd, "density3d: summed over z equals isc_density_in_window");
   const auto nz0 = std::find_if(none.begin(), none.end(), [](float v) { return v != 0.0f; });
   Check(n0 == 0 && nz0 == none.end(), "density3d: no structure = zeros",
         Fmt("count %d, first non-zero voxel %td", n0, nz0 - none.begin()));

   // Hand-binning the sites of the same world.
   World ref = Labelled(1249, Cheap(), MakeLabel(LabelMode::DnaPaint, 0.5));
   std::vector<WorldDye> d;
   ref.SitesInWindow(x0, y0, x1, y1, zLo, zHi, d);
   std::vector<float> hb(n3, 0.0f);
   for (const WorldDye& e : d) {
      const int ix = std::min(nx - 1, (int)std::floor((e.x - x0) * (nx / (x1 - x0))));
      const int iy = std::min(ny - 1, (int)std::floor((e.y - y0) * (ny / (y1 - y0))));
      const int iz = std::min(nz - 1, (int)std::floor((e.z - zLo) * (nz / (zHi - zLo))));
      hb[((size_t)iz * ny + iy) * nx + ix] += 1;
   }
   std::string hd;
   for (size_t i = 0; hd.empty() && i < n3; i++)
      if (hb[i] != all[i]) hd = Fmt("voxel %zu: %.0f hand-binned vs %.0f", i, (double)hb[i], (double)all[i]);
   Same(hd, "density3d: matches hand-binned SitesInWindow");

   // nz = 1 over a slab = the 2D query with the same z limits; infinite z too.
   const int32_t ns = isc_density3d_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, 1, 1, slab.data());
   isc_density_in_window(w, x0, y0, x1, y1, 1.0, 3.0, nx, ny, flat.data());
   const int32_t ni = isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 1, 1, all.data());
   Check(slab == flat && ns > 0, "density3d: nz = 1 slab equals the 2D query", Fmt("%d dyes in the slab", ns));
   Check(ni >= na, "density3d: infinite z holds at least the slab's", Fmt("%d vs %d", ni, na));
   Check(isc_density3d_in_window(w, x0, y0, x1, y1, -INF, INF, nx, ny, 2, 1, all.data()) == -1,
         "density3d: infinite z with nz > 1 rejected");
   Check(isc_density3d_in_window(w, x0, y0, x1, y1, zLo, zHi, nx, ny, nz, 1 << ISC_STRUCT_COUNT, all.data()) == -1,
         "density3d: an unknown structure bit rejected");
   Check(isc_density3d_in_window(w, x0, y0, x1, y1, zHi, zLo, nx, ny, nz, 1, all.data()) == -1,
         "density3d: reversed z rejected");
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
   long ones = 0, nBlinks = 0, nOff = 0, unordered = -1;
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
         if (j) {
            sumOff += b[j].tOn - b[j - 1].tOff;
            nOff++;
            if (b[j].tOn < b[j - 1].tOff && unordered < 0) unordered = i;
         }
      }
   }
   const double meanB = sumB / nBlinks, cv = jsm::sqrt(sumB2 / nBlinks - meanB * meanB) / meanB;
   Check(std::fabs(sumAct / N / 10 - 1) < 0.03, "first activation ~ Exp(1 / activationRatePerSec)",
         Fmt("mean %.3f s (want 10 +- 3 %%)", sumAct / N));
   Check(std::fabs(sumOn / nBlinks / 0.05 - 1) < 0.03, "ON time ~ Exp(onSec)", Fmt("mean %.4f s (want 0.05 +- 3 %%)", sumOn / nBlinks));
   Check(std::fabs(sumOff / nOff / 0.5 - 1) < 0.03, "dark time ~ Exp(offSec)", Fmt("mean %.3f s (want 0.5 +- 3 %%)", sumOff / nOff));
   Check(std::fabs(sumBlinks / N / 5 - 1) < 0.03 && std::fabs((double)ones / N - 0.2) < 0.01,
         "blink count geometric, mean 1/bleachProb",
         Fmt("mean %.3f (want 5 +- 3 %%), P(1) %.3f (want 0.2 +- 0.01)", sumBlinks / N, (double)ones / N));
   Check(std::fabs(meanB - 1) < 0.02 && std::fabs(cv - 0.5) < 0.03, "brightness log-normal, mean 1, CV photonCV",
         Fmt("mean %.3f (1 +- 0.02), CV %.3f (0.5 +- 0.03)", meanB, cv));
   Check(unordered < 0, "blinks in time order, never overlapping", Fmt("dye %ld", unordered));

   k.bleachProb = 1; k.photonCV = 0;
   int notSingle = -1;
   for (int i = 0; i < 1000 && notSingle < 0; i++) {
      b.clear();
      DyeSchedule(DyeH1(7, i, 0, 0), 0, i, k, b);
      if (!(b.size() == 1 && b[0].brightness == 1)) notSingle = i;
   }
   Check(notSingle < 0, "bleachProb 1, CV 0: one blink, brightness exactly 1", Fmt("dye %d", notSingle));
   k.bleachProb = 0;   // clamped to 0.01, and capped at DYE_MAX_BLINKS
   b.clear();
   DyeSchedule(DyeH1(7, 1, 2, 3), 4, 5, k, b);
   Check(b.size() <= (size_t)DYE_MAX_BLINKS, "blink count capped", Fmt("%zu blinks", b.size()));
   // tMax: the blinks before it are the full schedule's (a prefix).
   k.bleachProb = 0.05; k.photonCV = 0.3;
   int notPrefix = -1;
   for (int i = 0; i < 2000 && notPrefix < 0; i++) {
      std::vector<Blink> full, part;
      DyeSchedule(DyeH1(7, i, 1, 0), i % 13, i, k, full);
      const double tMax = 5.0 + (i % 40);
      DyeSchedule(DyeH1(7, i, 1, 0), i % 13, i, k, part, tMax);
      size_t m = 0;
      while (m < full.size() && full[m].tOn < tMax) m++;
      bool prefix = part.size() == m;
      for (size_t j = 0; prefix && j < m; j++)
         prefix = part[j].tOn == full[j].tOn && part[j].tOff == full[j].tOff && part[j].brightness == full[j].brightness;
      if (!prefix) notPrefix = i;
   }
   Check(notPrefix < 0, "DyeSchedule(tMax) = the full schedule's blinks starting before tMax", Fmt("dye %d", notPrefix));
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
   Check(std::fabs(rEarly / 0.2 - 1) < 0.03 && std::fabs(rLate / 0.2 - 1) < 0.03, "persistent: constant rate, no depletion",
         Fmt("%.4f blinks/site/s at 0-100 s, %.4f at 10000-10100 s (want 0.2 +- 3 %%)", rEarly, rLate));
   Check(std::fabs(onSum / nOn / 0.1 - 1) < 0.03 && std::fabs(brSum / nOn - 1) < 0.02,
         "persistent: ON ~ Exp(onSec), brightness mean 1",
         Fmt("ON %.4f s (0.1 +- 3 %%), brightness %.3f (1 +- 0.02)", onSum / nOn, brSum / nOn));

   // A long window = the union of its slices (each blink once, by tOn).
   int bad = -1;
   for (int i = 0; i < 300 && bad < 0; i++) {
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
      bool same = starts.size() == whole.size();
      for (const Blink& x : whole) same = same && std::find(starts.begin(), starts.end(), x.tOn) != starts.end();
      if (!same) bad = i;
   }
   Check(bad < 0, "persistent: a window = the union of its slices", Fmt("dye %d", bad));
}

// Caches under load: a window holding more dyes than the dye cache is not regenerated on every query, and the
// persistent-blink cache (built for a range of time bins) gives PersistentBlinks' answer, also when time jumps back,
// across range ends and after a label change. Run for a bleaching (PALM) and a persistent (DNA-PAINT) label.
void CacheUnderLoad(LabelMode mode, const char* name)
{
   const std::string N = std::string(name) + ": ";
   const Params p = Cheap();
   const uint32_t seed = 1249;
   Kinetics k;
   k.activationRatePerSec = 0.2; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   const Label lab = MakeLabel(mode, 0.05, k);
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   const double x0 = c0.x - 3.2, y0 = c0.y - 3.2, x1 = x0 + 6.4, y1 = y0 + 6.4, fd = 0.05;
   std::vector<WorldDye> dyes;
   Labelled(seed, p, lab).SitesInWindow(x0, y0, x1, y1, -INF, INF, dyes);
   const size_t cap = std::max<size_t>(1, dyes.size() / 4);   // a dye cache far smaller than the window
   Check(dyes.size() > 400, N + "the window holds dyes", Fmt("%zu (want > 400)", dyes.size()));
   World w = Labelled(seed, p, lab, 48, cap);
   std::vector<WorldEvent> ev;
   w.EventsInWindow(x0, y0, x1, y1, -INF, INF, 0, fd, ev);
   const long built = w.Stats().dyeBlocks;
   for (int f = 1; f < 10; f++) { ev.clear(); w.EventsInWindow(x0, y0, x1, y1, -INF, INF, f * fd, (f + 1) * fd, ev); }
   Check(built > 0 && w.Stats().dyeBlocks == built, N + "window larger than the dye cache: blocks built once, not per query",
         Fmt("%ld blocks after the first frame, %ld after 10", built, w.Stats().dyeBlocks));
   // A shifted window (a focus move) builds only the blocks it did not have: eviction waits for the end of the query,
   // so the blocks the old and new windows share survive it.
   {
      World a = Labelled(seed, p, lab, 48, cap), b = Labelled(seed, p, lab, 48, cap);
      std::vector<WorldEvent> e;
      a.EventsInWindow(x0, y0, x1, y1, -INF, 1.0, 0, fd, e);
      const long before = a.Stats().dyeBlocks;
      a.EventsInWindow(x0, y0, x1, y1, -INF, 2.0, fd, 2 * fd, e);
      b.EventsInWindow(x0, y0, x1, y1, -INF, 2.0, fd, 2 * fd, e);
      Check(a.Stats().dyeBlocks - before < b.Stats().dyeBlocks, N + "window shifted in z over a full dye cache: only its new blocks are built",
            Fmt("%ld new blocks vs %ld for the whole window", a.Stats().dyeBlocks - before, b.Stats().dyeBlocks));
   }
   // Prefetch only fills caches: a world pre-loading around a moving window (budget-limited or not) answers exactly
   // like one that does not, and the window it completed is then served without building blocks.
   {
      World a = Labelled(seed, p, lab, 48, cap), b = Labelled(seed, p, lab, 48, cap);
      std::string diff;
      bool partial = false;
      for (int f = 0; f < 6; f++) {
         const double s = 0.4 * f, t0 = 2 + f * fd;
         std::vector<WorldEvent> ea, eb;
         a.EventsInWindow(x0 + s, y0, x1 + s, y1, 0, 2, t0, t0 + fd, ea);
         b.EventsInWindow(x0 + s, y0, x1 + s, y1, 0, 2, t0, t0 + fd, eb);
         if (diff.empty()) diff = DiffOrdered(ea, eb);
         partial = partial || !a.Prefetch(x0 + s - 2, y0 - 2, x1 + s + 2, y1 + 2, -INF, INF, t0 + fd, t0 + 2 * fd,
                                          f % 3 == 0 ? 0.0 : 1e9);
      }
      Same(diff, N + "prefetch changes no event (a moving window)");
      Check(partial, N + "a zero-budget prefetch stops early");
      const bool done = a.Prefetch(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, 1e9);
      const long builtA = a.Stats().dyeBlocks;
      std::vector<WorldEvent> ea, eb;
      a.EventsInWindow(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, ea);
      b.EventsInWindow(x0 - 1, y0 - 1, x1 + 1, y1 + 1, -INF, INF, 3, 3 + fd, eb);
      Same(DiffOrdered(ea, eb), N + "a prefetched window: the same events");
      Check(done && a.Stats().dyeBlocks == builtA, N + "a prefetched window is served from the cache",
            Fmt("prefetch done %d, %ld blocks built by the query", (int)done, a.Stats().dyeBlocks - builtA));
   }
   // Frame after frame (the persistent blinks from blocks extending their bins ahead of time, staggered per block; the
   // bleaching ones from windowed schedules): still the brute force's.
   std::string steady;
   for (int f = 0; f < 21 && steady.empty(); f++) {
      ev.clear();
      const double t0 = 40 + f * fd, t1 = t0 + fd;
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t0, t1, ev);
      if (f % 7 == 0 || f == 19 || f == 20) {
         std::vector<WorldEvent> ref;
         for (const WorldDye& d : dyes) BruteBlinks(seed, d, w.GetLabel(STRUCTURE_MT), t0, t1, ref);
         const std::string d = DiffEvents(ev, ref);
         if (!d.empty()) steady = Fmt("frame %d [%g, %g): ", f, t0, t1) + d;
      }
   }
   Same(steady, N + "blinks frame by frame across bin extensions / schedule horizons = brute force");
   auto brute = [&](double t0, double t1) {
      std::vector<WorldEvent> out;
      for (const WorldDye& d : dyes) BruteBlinks(seed, d, w.GetLabel(STRUCTURE_MT), t0, t1, out);
      return out;
   };
   std::string jumps;
   long nEv = 0;
   for (double t : { 3.0, 15.97, 16.0, 31.99, 0.02, 200.0, 199.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      const std::string d = DiffEvents(ev, brute(t, t + fd));
      if (jumps.empty() && !d.empty()) jumps = Fmt("t %g: ", t) + d;
      nEv += (long)ev.size();
   }
   Same(jumps, N + "cached blinks = brute force when time jumps (range ends, back in time)");
   Check(nEv > 0, N + "the jumps saw blinks", "none");
   Kinetics k2 = k;
   k2.activationRatePerSec = 3;   // more blinks per bin: shorter bin ranges
   w.SetLabel(STRUCTURE_MT, MakeLabel(mode, 0.05, k2));
   std::string relabel;
   for (double t : { 7.3, 7.35, 9.0 }) {
      ev.clear();
      w.EventsInWindow(x0, y0, x1, y1, -INF, INF, t, t + fd, ev);
      const std::string d = DiffEvents(ev, brute(t, t + fd));
      if (relabel.empty() && !d.empty()) relabel = Fmt("t %g: ", t) + d;
   }
   Same(relabel, N + "cached blinks = brute force after new kinetics");
}

// The event query (spec/PORT.md 6.2): brute force equality, determinism and time slicing, for one label. PALM without
// a pre state and DNA-PAINT are the old bleaching dyes and persistent sites.
void EventQuery(const Label& lab, const char* name)
{
   const std::string N = std::string(name) + ": ";
   const Params p = Cheap();
   const uint32_t seed = 1249;
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   World w = Labelled(seed, p, lab);
   const double W = 6.4, x0 = c0.x + 3, y0 = c0.y - W / 2, x1 = x0 + W, y1 = y0 + W;
   const double zLo = 0.5, zHi = 3.0, fd = 0.03;
   auto events = [&](World& u, double t0, double t1) {
      std::vector<WorldEvent> e;
      u.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, e);
      return e;
   };

   // Brute force: every dye in the window, its whole schedule.
   std::vector<WorldDye> dyes;
   w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, dyes);
   const double t0 = 2.0, t1 = 2.0 + fd;
   std::vector<WorldEvent> brute;
   for (const WorldDye& d : dyes) BruteBlinks(seed, d, lab, t0, t1, brute);
   const std::vector<WorldEvent> got = events(w, t0, t1);
   Check(!got.empty(), N + "the frame has blinks", Fmt("%zu dyes, no blinks", dyes.size()));
   Same(DiffEvents(got, brute), N + "events = brute force over the dyes' schedules");

   // Frame slices: each blink shows up in exactly the frames it overlaps.
   const std::vector<WorldEvent> whole = events(w, 5.0, 5.0 + 10 * fd);
   std::vector<WorldEvent> sliced;
   for (int f = 0; f < 10; f++)
      for (const WorldEvent& e : events(w, 5.0 + f * fd, 5.0 + (f + 1) * fd)) {
         bool seen = false;
         for (const WorldEvent& s : sliced) seen |= s.id == e.id && s.tOn == e.tOn;
         if (!seen) sliced.push_back(e);
      }
   Same(DiffEvents(whole, sliced), N + "union of per-frame queries = the multi-frame query");

   // Determinism: away and back, dropped caches, label round trip, other caches, any query history.
   std::vector<WorldDye> far;
   w.SitesInWindow(x0 + 1000, y0, x1 + 1000, y1, -INF, INF, far);
   Same(DiffEvents(got, events(w, t0, t1)), N + "stage 1 mm away and back, identical");
   Label l2 = lab;
   l2.kin.onSec = 0.2;
   w.SetLabel(STRUCTURE_MT, l2);
   Check(!DiffEvents(got, events(w, t0, t1)).empty(), N + "another label changes the events", "the same events");
   w.SetLabel(STRUCTURE_MT, lab);
   Same(DiffEvents(got, events(w, t0, t1)), N + "label change and back, identical");
   w.DropCaches();
   Same(DiffEvents(got, events(w, t0, t1)), N + "after dropping every cache, identical");
   World tiny = Labelled(seed, p, lab, 1, 100);   // caches smaller than one window
   Same(DiffEvents(got, events(tiny, t0, t1)), N + "independent of cache sizes");
   World hist = Labelled(seed, p, lab);
   const std::vector<WorldEvent> late = events(hist, 300, 300 + fd);
   events(hist, 0, 400);
   Same(DiffEvents(got, events(hist, t0, t1)), N + "an early frame after a late and a long one = brute force");
   std::vector<WorldEvent> lateRef;
   for (const WorldDye& d : dyes) BruteBlinks(seed, d, lab, 300, 300 + fd, lateRef);
   Same(DiffEvents(late, lateRef), N + "a late frame first = brute force");
   Same(DiffEvents(late, events(hist, 300, 300 + fd)), N + "the late frame again, after the others: identical");
}

// The label model (issue 16): the continuous windows of each mode, orientation statistics, refusals.
void LabelModel()
{
   const Params p = Cheap();
   const uint32_t seed = 1249;
   Kinetics kin;
   kin.activationRatePerSec = 0.05; kin.onSec = 0.05; kin.offSec = 1.0; kin.bleachProb = 0.5; kin.photonCV = 0.5;
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   const double x0 = c0.x + 3, y0 = c0.y - 2, x1 = x0 + 4, y1 = y0 + 4, zLo = -5, zHi = 50;
   World w = Fresh(seed, p);
   auto use = [&](const Label& l) { w.SetLabel(STRUCTURE_MT, l); };
   auto sites = [&] { std::vector<WorldDye> s; w.SitesInWindow(x0, y0, x1, y1, zLo, zHi, s); return s; };
   auto cont = [&](double tMin) { std::vector<WorldEvent> c; w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, tMin, c); return c; };
   auto events = [&](double t0, double t1) { std::vector<WorldEvent> e; w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, e); return e; };

   // dSTORM: an initial ON window Exp(initialOnSec) per dye, the blinks after it.
   {
      Kinetics k = kin;
      k.initialOnSec = 2;
      use(MakeLabel(LabelMode::dSTORM, 0.1, k));
      const std::vector<WorldDye> s = sites();
      const std::vector<WorldEvent> c = cont(-INF), ev = events(0, 1e9);
      Check(!s.empty() && c.size() == s.size(), "dSTORM: one initial ON window per dye", Fmt("%zu windows, %zu dyes", c.size(), s.size()));
      double m = 0;
      std::string bad;
      for (size_t i = 0; i < c.size() && i < s.size(); i++) {
         m += c[i].tOff;
         if (bad.empty() && !(c[i].state == STATE_INITIAL_ON && c[i].tOn == 0 && c[i].id == s[i].id && c[i].x == s[i].x && c[i].aux == 0))
            bad = Fmt("window #%zu: ", i) + EventStr(c[i]) + " for " + DyeStr(s[i]);
      }
      m /= std::max<size_t>(1, c.size());
      Same(bad, "dSTORM: the windows are the dyes' (in order), INITIAL_ON from 0, aux 0");
      Check(std::fabs(m - 2) < 0.2, "dSTORM: initial ON ~ Exp(initialOnSec)", Fmt("mean %.3f s (want 2 +- 0.2)", m));
      // Every blink starts after its dye's initial ON window ends.
      std::unordered_map<uint32_t, double> offOf;
      for (const WorldEvent& w0 : c) offOf[w0.id] = w0.tOff;
      std::string early;
      for (const WorldEvent& e : ev) {
         const auto it = offOf.find(e.id);
         if (early.empty() && it != offOf.end() && e.tOn < it->second)
            early = EventStr(e) + Fmt(" starts before its initial ON ends at %.12g", it->second);
      }
      Check(!ev.empty(), "dSTORM: blinks after the initial ON", "no blinks");
      Same(early, "dSTORM: every blink starts after its dye's initial ON window");
      size_t open = 0;
      for (const WorldEvent& e : c) open += e.tOff > 3.0;
      const size_t late = cont(3.0).size();
      Check(late == open, "dSTORM: tMin keeps the windows open after it", Fmt("%zu returned, %zu open at 3 s", late, open));
   }
   // WideField: one always-on window per dye, aux ~ Exp(1), no blinks.
   {
      use(MakeLabel(LabelMode::WideField, 0.1, kin));
      const std::vector<WorldDye> s = sites();
      const std::vector<WorldEvent> c = cont(1e6);
      double ma = 0;
      std::string bad;
      for (const WorldEvent& e : c) {
         ma += e.aux;
         if (bad.empty() && !(std::isinf(e.tOff) && e.state == STATE_ALWAYS_ON)) bad = EventStr(e);
      }
      ma /= std::max<size_t>(1, c.size());
      Check(!s.empty() && c.size() == s.size(), "WideField: one window per dye", Fmt("%zu windows, %zu dyes", c.size(), s.size()));
      Same(bad, "WideField: the windows are always on");
      Check(events(0, 100).empty(), "WideField: no blinks");
      Check(std::fabs(ma - 1) < 0.08, "WideField: aux ~ Exp(1)", Fmt("mean %.3f (want 1 +- 0.08)", ma));
   }
   // PALM with a pre state: the pre window ends at the first blink; without: none. DNA-PAINT: none.
   {
      Label l = MakeLabel(LabelMode::PALM, 0.1, kin);
      l.preState = true;
      use(l);
      const std::vector<WorldDye> s = sites();
      const std::vector<WorldEvent> c = cont(-INF);
      Check(!s.empty() && c.size() == s.size(), "PALM (pre): one window per dye", Fmt("%zu windows, %zu dyes", c.size(), s.size()));
      std::string bad;
      for (size_t i = 0; bad.empty() && i < c.size() && i < s.size(); i++) {
         std::vector<Blink> bl;
         DyeSchedule(DyeH1(seed, s[i].cx, s[i].cy, s[i].mtIndex), s[i].k, s[i].n, kin, bl);
         const double want = bl.empty() ? INF : bl[0].tOn;
         if (!(c[i].state == STATE_PRE && c[i].x == s[i].x && c[i].tOff == want && c[i].aux > 0))
            bad = Fmt("window #%zu: ", i) + EventStr(c[i]) + Fmt(", first blink %.12g", want);
      }
      Same(bad, "PALM: the pre state ends at the first blink");
      use(MakeLabel(LabelMode::PALM, 0.1, kin));
      Check(cont(-INF).empty(), "PALM without a pre state: no windows");
      use(MakeLabel(LabelMode::DnaPaint, 0.1, kin));
      Check(cont(-INF).empty(), "DNA-PAINT: no continuous windows");
   }
   // Orientation: Free = no dipole; Random isotropic (<z^2> = 1/3, <x> = 0); Fixed unit vectors, polar 90
   // perpendicular to the axis, polar 0 along it.
   {
      Label l = MakeLabel(LabelMode::DnaPaint, 0.7, kin);
      use(l);
      std::vector<WorldDye> s = sites();
      if (s.size() > 4000) s.resize(4000);
      Pt3 dir;
      size_t withDipole = 0;
      for (const WorldDye& d : s) withDipole += w.DyeOrientationOf(d, dir);
      Check(!s.empty() && withDipole == 0, "orientation Free: no dipole", Fmt("%zu of %zu dyes have one", withDipole, s.size()));
      l.orientation.mode = OrientationMode::Random;
      l.orientation.wobbleDeg = 20;
      use(l);
      double z2 = 0, mx = 0, worstUnit = 0;
      for (const WorldDye& d : s) {
         w.DyeOrientationOf(d, dir);
         z2 += dir.z * dir.z; mx += dir.x;
         worstUnit = std::max(worstUnit, std::fabs(jsm::hypot(dir.x, dir.y, dir.z) - 1));
      }
      z2 /= s.size(); mx /= s.size();
      Check(worstUnit < 1e-12, "orientation Random: unit vectors", Fmt("worst |len - 1| %.3g", worstUnit));
      Check(std::fabs(z2 - 1.0 / 3) < 0.02 && std::fabs(mx) < 0.03, "orientation Random: isotropic",
            Fmt("<z^2> %.4f (1/3 +- 0.02), <x> %.4f (0 +- 0.03) over %zu dyes", z2, mx, s.size()));
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
      use(l);
      double worstPerp = 0;
      for (size_t i = 0; i < s.size(); i += 7) {
         w.DyeOrientationOf(s[i], dir);
         worstPerp = std::max(worstPerp, std::fabs(axisDot(s[i], dir)));
      }
      Check(worstPerp < 1e-9, "orientation Fixed polar 90: perpendicular to the axis", Fmt("worst |cos| %.3g", worstPerp));
      l.orientation.polarDeg = 0;
      use(l);
      double worstAlong = 0;
      for (size_t i = 0; i < s.size(); i += 7) {
         w.DyeOrientationOf(s[i], dir);
         worstAlong = std::max(worstAlong, std::fabs(axisDot(s[i], dir) - 1));
      }
      Check(worstAlong < 1e-9, "orientation Fixed polar 0: along the axis", Fmt("worst |cos - 1| %.3g", worstAlong));
   }
   // Refusals.
   {
      Label m;
      m.motion = 1;
      Label o;
      o.offTargetCount = 1;
      Label pr = MakeLabel(LabelMode::dSTORM, 0.1);
      pr.preState = true;
      use(MakeLabel(LabelMode::DnaPaint, 0.3, kin));
      const double d0 = w.GetLabel(STRUCTURE_MT).density;
      Check(ValidateLabel(m) && LabelNotImplemented(m) && !w.SetLabel(STRUCTURE_MT, m), "labels: SPT motion refused (not implemented)");
      Check(ValidateLabel(o) && LabelNotImplemented(o), "labels: off-target refused (not implemented)");
      Check(ValidateLabel(pr) && !LabelNotImplemented(pr) && !w.SetLabel(STRUCTURE_MT, pr), "labels: a non-PALM pre state refused");
      Check(!w.SetLabel(STRUCTURE_COUNT, Label()), "labels: an unknown structure refused");
      Check(w.GetLabel(STRUCTURE_MT).density == d0, "labels: a refusal changes nothing", Fmt("density %g vs %g", w.GetLabel(STRUCTURE_MT).density, d0));
      Check(!ValidateLabel(Label()), "labels: the default label is valid");
   }
}

} // namespace


// ABI 11: a kinetics history (piecewise-constant rates in clock time). One segment = no history; a segment appended
// at T leaves everything before T as it was; the new rates act after T (PALM activation off, dSTORM 10x faster,
// DNA-PAINT at once); two histories alternating on one world (the parked schedule slots) answer as fresh worlds.
void KineticsHistoryChecks()
{
   const Params p = Cheap();
   const uint32_t seed = 1249;
   Kinetics kin;
   kin.activationRatePerSec = 0.05; kin.onSec = 0.05; kin.offSec = 1.0; kin.bleachProb = 0.5; kin.photonCV = 0.5;
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   const double x0 = c0.x + 3, y0 = c0.y - 2, x1 = x0 + 4, y1 = y0 + 4, zLo = -5, zHi = 50;
   auto events = [&](World& w, double t0, double t1) {
      std::vector<WorldEvent> e;
      w.EventsInWindow(x0, y0, x1, y1, zLo, zHi, t0, t1, e);
      return e;
   };
   auto cont = [&](World& w) {
      std::vector<WorldEvent> c;
      w.ContinuousInWindow(x0, y0, x1, y1, zLo, zHi, -INF, c);
      return c;
   };
   const double T = 30.3;

   Kinetics pre = kin;
   pre.initialOnSec = 2;
   struct Case { const char* name; Label l; };
   Kinetics dp = kin;
   dp.activationRatePerSec = 0.02;
   Label palm = MakeLabel(LabelMode::PALM, 0.1, kin);
   palm.preState = true;
   const Case cases[] = { { "PALM", palm }, { "dSTORM", MakeLabel(LabelMode::dSTORM, 0.1, pre) },
                          { "DNA-PAINT", MakeLabel(LabelMode::DnaPaint, 0.3, dp) } };
   for (const Case& c : cases) {
      const std::string N = std::string("kinetics history, ") + c.name + ": ";
      // One world: no history, then one segment, then two (a history change on a used world; the alternating
      // check below compares such a world with fresh ones).
      World w = Labelled(seed, p, c.l);
      const auto e0 = events(w, 0, 60), c0 = cont(w);
      Check(SetHist(w, { 0.0 }, { c.l.kin }), N + "one segment accepted");
      const auto e1 = events(w, 0, 60), c1 = cont(w);
      Kinetics k2 = c.l.kin;
      k2.activationRatePerSec *= 3; k2.onSec *= 0.2; k2.offSec *= 0.5; k2.bleachProb = 0.9;
      Check(SetHist(w, { 0.0, T }, { c.l.kin, k2 }), N + "two segments accepted");
      const auto e2 = events(w, 0, 60);
      Check(!e0.empty(), N + "blinks in [0, 60) s", "none");
      Same(DiffOrdered(e0, e1), N + "one segment = no history, bit for bit (blinks)");
      Same(DiffOrdered(c0, c1), N + "one segment = no history, bit for bit (continuous windows)");
      Same(DiffEvents(Until(e0, T), Until(e2, T)), N + "the past before a new segment is unchanged");
      Check(!DiffEvents(e0, e2).empty(), N + "the future follows the new segment", "the same events");
   }
   // Bad histories are refused (and change nothing).
   {
      World w = Labelled(seed, p, palm);
      Kinetics bad = kin;
      bad.onSec = 0;
      Check(!SetHist(w, { 1.0 }, { kin }), "kinetics history: refused when it does not start at 0");
      Check(!SetHist(w, { 0.0, 0.0 }, { kin, kin }), "kinetics history: refused when the starts do not increase");
      Check(!SetHist(w, { 0.0 }, { bad }), "kinetics history: refused with bad kinetics");
      Check(!SetHist(w, { 0.0 }, {}), "kinetics history: refused without kinetics");
      Check(!w.HistoryOf(0), "kinetics history: a refusal sets none");
   }
   // PALM, UV off at T: no activation after T (each pre window ends by T or never), activated dyes still blink.
   {
      World w = Labelled(seed, p, palm);
      Kinetics off = kin;
      off.activationRatePerSec = 0;
      SetHist(w, { 0.0, T }, { kin, off });
      std::string late;
      size_t pending = 0;
      for (const WorldEvent& e : cont(w))
         if (e.state == STATE_PRE) {
            if (late.empty() && !(e.tOff <= T || std::isinf(e.tOff))) late = EventStr(e);
            pending += std::isinf(e.tOff);
         }
      size_t after = 0;
      for (const WorldEvent& e : events(w, T, T + 5)) after += e.tOn >= T;
      Same(late, "kinetics history, PALM: activation off stops new activations (no pre window ends after T)");
      Check(pending > 0, "kinetics history, PALM: some dyes are never activated", "none");
      Check(after > 0, "kinetics history, PALM: the activated ones still blink after T", "no blinks in the 5 s after T");
   }
   // dSTORM, ON and OFF 10x shorter after T: the mean ON of blinks starting after T + 1 is ~onSec/10.
   {
      Kinetics fast = kin;
      fast.onSec = kin.onSec / 10; fast.offSec = kin.offSec / 10;
      World w = Labelled(seed, p, MakeLabel(LabelMode::dSTORM, 0.1, kin));
      SetHist(w, { 0.0, T }, { kin, fast });
      double m = 0, mb = 0;
      size_t n = 0, nb = 0;
      for (const WorldEvent& e : events(w, 0, T + 20)) {
         if (e.tOn > T + 1) { m += e.tOff - e.tOn; n++; }
         else if (e.tOff < T) { mb += e.tOff - e.tOn; nb++; }
      }
      m /= std::max<size_t>(1, n);
      mb /= std::max<size_t>(1, nb);
      Check(n > 50 && nb > 50 && std::fabs(m / fast.onSec - 1) < 0.25 && std::fabs(mb / kin.onSec - 1) < 0.25,
            "kinetics history, dSTORM: ON times follow the segment they start in",
            Fmt("before: %zu blinks, mean ON %.4f s (want %.3f +- 25 %%); after: %zu blinks, %.4f s (want %.4f); need > 50 each",
                nb, mb, kin.onSec, n, m, fast.onSec));
   }
   // DNA-PAINT, imager x10 at 60.4 s: the blinks before it as without the change, busier from 60.4 s on (at once,
   // not from the next bin).
   {
      Label l = MakeLabel(LabelMode::DnaPaint, 0.3, dp);
      Kinetics more = dp;
      more.activationRatePerSec = dp.activationRatePerSec * 10;
      World a = Labelled(seed, p, l), b = Labelled(seed, p, l);
      SetHist(b, { 0.0, 60.4 }, { dp, more });
      auto inBin = [](std::vector<WorldEvent> e, double lo, double hi) {
         e.erase(std::remove_if(e.begin(), e.end(), [&](const WorldEvent& x) { return !(x.tOn >= lo && x.tOn < hi); }),
                 e.end());
         return e;
      };
      const auto ea = events(a, 59, 64), eb = events(b, 59, 64);
      const size_t na = inBin(ea, 61, 63).size(), nb = inBin(eb, 61, 63).size();
      const size_t nPa = inBin(ea, 60.4, 61).size(), nPb = inBin(eb, 60.4, 61).size();
      Same(DiffEvents(inBin(ea, 0, 60.4), inBin(eb, 0, 60.4)), "kinetics history, DNA-PAINT: the blinks before a rate change stay");
      Check(na > 0 && nb > 5 * na && nPb > 3 * std::max<size_t>(nPa, 1), "kinetics history, DNA-PAINT: a rate change acts at once",
            Fmt("blinks starting in [60.4, 61): %zu -> %zu (want > 3x), in [61, 63): %zu -> %zu (want > 5x)", nPa, nPb, na, nb));
   }
   // Two histories alternating on one world = fresh worlds (PALM and DNA-PAINT).
   for (const Case& c : { cases[0], cases[2] }) {
      const std::string N = std::string("kinetics history, ") + c.name + ": ";
      Kinetics k2 = c.l.kin;
      k2.activationRatePerSec *= 4;
      const std::vector<double> tA = { 0.0, 20.0 }, tB = { 0.0, 40.0 };
      const std::vector<Kinetics> kA = { c.l.kin, k2 }, kB = { k2, c.l.kin };
      World w = Labelled(seed, p, c.l), fa = Labelled(seed, p, c.l), fb = Labelled(seed, p, c.l);
      SetHist(fa, tA, kA);
      SetHist(fb, tB, kB);
      const auto ra = events(fa, 50, 50.5), rb = events(fb, 50, 50.5);
      World fresh = Labelled(seed, p, c.l);
      const auto r0 = events(fresh, 50, 50.5);
      std::string diff;
      for (int i = 0; i < 6 && diff.empty(); i++) {
         const bool useA = i % 2 == 0;
         SetHist(w, useA ? tA : tB, useA ? kA : kB);
         const std::string d = DiffOrdered(events(w, 50, 50.5), useA ? ra : rb);
         if (!d.empty()) diff = Fmt("switch %d (history %c): ", i, useA ? 'A' : 'B') + d;
      }
      Same(diff, N + "alternating histories on one world = fresh worlds");
      SetHist(w, {}, {});
      Check(!w.HistoryOf(0), N + "an empty history removes it");
      Same(DiffOrdered(events(w, 50, 50.5), r0), N + "no history again = a fresh world");
      // A label change of the kinetics alone keeps the schedules made under a history (they never read it): the
      // answers stay those of fresh worlds.
      SetHist(w, tA, kA);
      events(w, 50, 50.5);
      Label l2 = c.l;
      l2.kin.activationRatePerSec *= 2;
      l2.kin.onSec *= 0.5;
      w.SetLabel(0, l2);
      World f2 = Labelled(seed, p, l2);
      Same(DiffOrdered(events(w, 50, 50.5), ra), N + "a kinetics-only label change keeps the history's schedules");
      SetHist(w, {}, {});
      Same(DiffOrdered(events(w, 50, 50.5), events(f2, 50, 50.5)), N + "... and renews the rest (= a fresh world)");
   }
}

// The parallel block work (packing blocks, cell assets, dye blocks, blink schedules and persistent covers built on
// several threads) changes nothing: the same events in the same order -- the order the renderer sums them in -- and
// the same build counts as one thread, for stack-sized and frame-sized queries, jumps, a label change and prefetches.
// Natively only (ctest leaves it out under Emscripten).
void Threads(LabelMode mode, double density, const char* name)
{
   const std::string N = std::string(name) + ": ";
   if (!kThreads) { Skip(N + "8 threads = 1 thread", "no threads in WASM"); return; }
   SetWorldThreads(8);   // the process's worker pool is made at the first query with WorldThreads() - 1 workers
   const Params p = Cheap();
   Kinetics k;
   k.activationRatePerSec = 0.01; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5; k.photonCV = 0.2;
   const Label lab = MakeLabel(mode, density, k);
   World a = Labelled(77, p, lab), b = Labelled(77, p, lab);
   size_t total = 0;
   std::string diff;
   auto both = [&](const char* step, const std::function<void(World&, std::vector<WorldEvent>&)>& q) {
      if (!diff.empty()) return;
      std::vector<WorldEvent> ea, eb;
      SetWorldThreads(1);
      q(a, ea);
      SetWorldThreads(8);
      q(b, eb);
      total += ea.size();
      const std::string d = DiffOrdered(ea, eb);
      if (!d.empty()) diff = std::string(step) + ": " + d;
   };
   both("a 5 s stack query", [](World& w, std::vector<WorldEvent>& e) { w.EventsInWindow(-5, -5, 5, 5, 0, 3, 0, 5, e); });
   for (int f = 0; f < 10; f++) {
      const double s = f < 4 ? 0.9 * f : f < 7 ? 30 + 0.9 * f : 60;   // a move, a jump, another jump
      both(Fmt("frame %d", f).c_str(), [&](World& w, std::vector<WorldEvent>& e) {
         if (f == 8) w.SetLabel(STRUCTURE_MT, MakeLabel(mode, density, Kinetics{ 0.02, 0.05, 0.5, 0.5, 0.2, 0.0 }));
         w.EventsInWindow(s - 4, -4, s + 4, 4, 0, 4, 21 + f * 0.05, 21 + (f + 1) * 0.05, e);
         if (f % 3 == 0) w.Prefetch(s - 6, -6, s + 6, 6, -INF, INF, 21 + (f + 1) * 0.05, 21 + (f + 2) * 0.05, 1e9);
         if (f % 3 == 1) w.ContinuousInWindow(s - 4, -4, s + 4, 4, 0, 4, 0, e);
      });
   }
   SetWorldThreads(0);
   Same(diff, N + "8 threads = 1 thread: the same events in the same order");
   Check(total > 300, N + "the queries saw events", Fmt("%zu (want > 300)", total));
   const WorldStats &sa = a.Stats(), &sb = b.Stats();
   const struct { const char* what; long x, y; } st[] = {
      { "blocks packed", sa.blocksPacked, sb.blocksPacked }, { "cells built", sa.cellsBuilt, sb.cellsBuilt },
      { "dye blocks", sa.dyeBlocks, sb.dyeBlocks }, { "dye block hits", sa.dyeBlockHits, sb.dyeBlockHits },
      { "schedules", sa.schedulesBuilt, sb.schedulesBuilt }, { "persistent covers", sa.persistentBuilt, sb.persistentBuilt } };
   std::string sd;
   for (const auto& s : st)
      if (sd.empty() && s.x != s.y) sd = Fmt("%s: %ld (1 thread) vs %ld (8)", s.what, s.x, s.y);
   Same(sd, N + "8 threads = 1 thread: the same caches built");
}

// ABI 6: the optical volume (BrightField). Tiling, threads, z consistency, nucleus volume against a column
// quadrature, bad arguments.
void OpticalVolume()
{
   const Params p = Cheap();
   Cell c0;
   if (!FirstCell(31, p, c0)) return;
   // Tiling, threads, range and z: a 30 um window over the first cell's nucleus, 1 um voxels.
   {
      const double x0 = c0.x - 15, y0 = c0.y - 15, x1 = x0 + 30, y1 = y0 + 30, zLo = -0.5, zHi = 9.5;
      const int nx = 30, ny = 30, nz = 10, sub = 2;
      const size_t plane = (size_t)nx * ny, chan = plane * nz;
      std::vector<float> whole(3 * chan), left(3 * chan / 2), right(3 * chan / 2), t8(3 * chan);
      SetWorldThreads(8);   // (the worlds' pool: 7 workers)
      World a = Fresh(31, p), b = Fresh(31, p);
      SetWorldThreads(1);
      a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, nz, sub, whole.data());
      if (kThreads) {
         SetWorldThreads(8);
         b.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, nz, sub, t8.data());
         SetWorldThreads(0);
         size_t i = 0;
         while (i < whole.size() && whole[i] == t8[i]) i++;
         Check(i == whole.size(), "optical volume: 8 threads = 1 thread",
               i < whole.size() ? Fmt("voxel %zu: %.9g vs %.9g", i, whole[i], t8[i]) : "");
      } else {
         SetWorldThreads(0);
         Skip("optical volume: 8 threads = 1 thread", "no threads in WASM");
      }
      // Left/right halves (same voxel grid) assemble to the whole window.
      const double xm = (x0 + x1) / 2;
      b.OpticalVolumeInWindow(x0, y0, xm, y1, zLo, zHi, nx / 2, ny, nz, sub, left.data());
      b.OpticalVolumeInWindow(xm, y0, x1, y1, zLo, zHi, nx / 2, ny, nz, sub, right.data());
      std::string tiled;
      for (int ch = 0; ch < 3 && tiled.empty(); ch++)
         for (int k = 0; k < nz && tiled.empty(); k++)
            for (int iy = 0; iy < ny && tiled.empty(); iy++)
               for (int ix = 0; ix < nx && tiled.empty(); ix++) {
                  const float wv = whole[((size_t)(ch * nz + k) * ny + iy) * nx + ix];
                  const std::vector<float>& hv = ix < nx / 2 ? left : right;
                  const float v = hv[((size_t)(ch * nz + k) * ny + iy) * (nx / 2) + ix % (nx / 2)];
                  if (wv != v) tiled = Fmt("channel %d voxel (%d, %d, %d): %.9g whole vs %.9g half", ch, ix, iy, k, wv, v);
               }
      Same(tiled, "optical volume: two half windows = the whole window");
      std::string range;
      double cyto = 0, nuc = 0, mt = 0;
      for (size_t v = 0; v < chan; v++) {
         const double s = (double)whole[v] + whole[chan + v];
         if (range.empty() && !(whole[v] >= -1e-6 && whole[chan + v] >= 0 && s <= 1 + 1e-5 && whole[2 * chan + v] >= 0))
            range = Fmt("voxel %zu: cytoplasm %.9g nucleus %.9g microtubules %.9g", v, whole[v], whole[chan + v], whole[2 * chan + v]);
         cyto += whole[v]; nuc += whole[chan + v]; mt += whole[2 * chan + v];
      }
      Same(range, "optical volume: fractions in [0, 1]");
      Check(cyto > 0 && nuc > 0 && mt > 0, "optical volume: all three present", Fmt("sums %.3g %.3g %.3g", cyto, nuc, mt));
      // nz = 1 over the same slab = the z mean (per column).
      std::vector<float> flat(3 * plane);
      a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, 1, sub, flat.data());
      double worst = 0;
      for (int ch = 0; ch < 2; ch++)
         for (size_t i = 0; i < plane; i++) {
            double s = 0;
            for (int k = 0; k < nz; k++) s += whole[(size_t)ch * chan + k * plane + i];
            worst = std::max(worst, std::fabs(s / nz - flat[ch * plane + i]));
         }
      Check(worst < 1e-5, "optical volume: nz = 1 equals the mean over z", Fmt("worst %.3g (tol 1e-5)", worst));
   }
   // The nucleus volume of the first cell vs a direct quadrature of its column (NucleusColumnLocal; the shaped
   // nucleus) on a 0.05 um grid. Region: within nucleus reach + 1 um of the nucleus centre.
   {
      World a = Fresh(31, p);
      const Cell& q = c0;
      const double R = std::max(q.nucLong / 2, q.nucShaped ? q.nucReach : 0.0) + 1, hq = 0.05;
      double want = 0;
      for (double yy = -R + hq / 2; yy < R; yy += hq)
         for (double xx = -R + hq / 2; xx < R; xx += hq) {
            double below, above;
            if (NucleusColumnLocal(q, q.nucOffX + xx, q.nucOffY + yy, below, above)) want += (below + above) * hq * hq;
         }
      double ncx, ncy;
      LocalToWorld(q, q.nucOffX, q.nucOffY, ncx, ncy);
      const double half = std::ceil(R + 1), x0 = ncx - half, y0 = ncy - half, x1 = ncx + half, y1 = ncy + half;
      const double zLo = -0.5, zHi = 9.5;
      const int nx = (int)(2 * half), ny = nx;
      const size_t plane = (size_t)nx * ny;
      std::vector<float> fine(3 * plane);
      a.OpticalVolumeInWindow(x0, y0, x1, y1, zLo, zHi, nx, ny, 1, 4, fine.data());
      const double vox = (x1 - x0) / nx * (y1 - y0) / ny * (zHi - zLo);
      double got = 0;
      const double cr = std::cos(q.packRot), sr = std::sin(q.packRot);
      for (int iy = 0; iy < ny; iy++)
         for (int ix = 0; ix < nx; ix++) {
            const double wx = x0 + (ix + 0.5) * (x1 - x0) / nx - q.x, wy = y0 + (iy + 0.5) * (y1 - y0) / ny - q.y;
            const double lx = wx * cr + wy * sr - q.nucOffX, ly = -wx * sr + wy * cr - q.nucOffY;
            if (lx * lx + ly * ly <= R * R) got += fine[plane + (size_t)iy * nx + ix] * vox;
         }
      Check(want > 0 && std::fabs(got / want - 1) < 0.02, "optical volume: nucleus volume = its column integral (2%)",
            Fmt("%.3f um^3 from the volume vs %.3f by quadrature", got, want));
   }
   IscParams* ip = AbiParams();
   IscWorld* w = isc_world_new(31, ip);
   std::vector<float> o(3 * 16 * 16 * 2);
   const double x0 = c0.x - 8, y0 = c0.y - 8, x1 = x0 + 16, y1 = y0 + 16;
   Check(isc_optical_volume_in_window(w, x0, y0, x1, y1, 0, 8, 16, 16, 2, 1, o.data()) >= 0, "optical volume: C ABI accepts a window");
   Check(isc_optical_volume_in_window(w, x0, y0, x1, y1, -INF, 8, 16, 16, 2, 1, o.data()) == -1, "optical volume: C ABI rejects infinite z");
   Check(isc_optical_volume_in_window(w, x0, y0, x1, y1, 0, 8, 16, 16, 2, 0, o.data()) == -1, "optical volume: C ABI rejects sub 0");
   Check(isc_optical_volume_in_window(w, x0, y0, x1, y1, 8, 0, 16, 16, 2, 1, o.data()) == -1, "optical volume: C ABI rejects reversed z");
   isc_world_free(w);
   isc_params_free(ip);
}

// Fractal edge tail and relaxed cytoplasm height (spec/ALGORITHM.md "Edge roughness", "Height relaxation").
void EdgeAndHeight()
{
   auto eqDiam = [](const Params& p, double& meanTail) {
      double sum = 0, tail = 0;
      int n = 0;
      for (int cx = 0; cx < 8; cx++)
         for (int cy = 0; cy < 8; cy++) {
            const Cell c = RawCandidate(77, cx, cy, p);
            if (!c.present) continue;
            const std::vector<Pt2> o = CellOutlineLocal(c, 1024);
            double a = 0;
            for (size_t i = 0, j = o.size() - 1; i < o.size(); j = i++) a += o[j].x * o[i].y - o[i].x * o[j].y;
            sum += 2 * std::sqrt(std::fabs(a) / 2 / jsm::PI);
            tail += c.tailBound;
            n++;
         }
      meanTail = tail / n;
      return sum / n;
   };
   Params p = Cheap();
   double t0 = 0, tb = 0;
   p.cellRough = 0;
   eqDiam(p, t0);
   Check(t0 == 0, "cellRough 0: no tail", Fmt("mean tail bound %g", t0));
   p.cellRough = 0.15;
   p.cellBlob = 0;
   eqDiam(p, tb);
   Check(tb == 0, "cellBlob 0: no tail (roughness scales with blobbiness)", Fmt("mean tail bound %g", tb));
   std::string worst;
   double worstRel = 0;
   for (double blob : { 0.5, 1.75, 4.0 }) {
      p.cellBlob = blob;
      p.cellRough = 0;
      double t;
      const double d0 = eqDiam(p, t);
      p.cellRough = 0.15;
      const double d = eqDiam(p, tb);
      if (std::fabs(d / d0 - 1) > worstRel) {
         worstRel = std::fabs(d / d0 - 1);
         worst = Fmt("blob %.2f: %.3f um with the tail vs %.3f without (%.1f %%)", blob, d, d0, 100 * worstRel);
      }
   }
   Check(worstRel < 0.04, "the tail keeps the equivalent diameter (4%) at blob 0.5-4", worst);

   // The tail's power spectrum: |c_k|^2 ~ k^-(2H+1) = k^-(5-2D) over its band (a self-affine r(theta) of Hurst H has
   // box-counting dimension D = 2 - H).
   const int M = 512;
   std::vector<double> cs((size_t)M * N_TAIL), sn((size_t)M * N_TAIL);
   for (int k = 0; k < N_TAIL; k++)
      for (int i = 0; i < M; i++) {
         cs[(size_t)k * M + i] = std::cos(2 * jsm::PI * (TAIL_K0 + k) * i / M);
         sn[(size_t)k * M + i] = std::sin(2 * jsm::PI * (TAIL_K0 + k) * i / M);
      }
   for (double D : { 1.35, 1.7 }) {
      Params q = Cheap();
      q.cellFractalDim = D;
      q.cellRough = 0.05;   // small: the soft clamp stays linear
      std::vector<double> power(N_TAIL, 0.0), f(M);
      int cells = 0;
      for (int cx = 0; cx < 6; cx++)
         for (int cy = 0; cy < 6; cy++) {
            const Cell c = RawCandidate(91, cx, cy, q);
            if (!c.present || !(c.tailBound > 0)) continue;
            cells++;
            for (int i = 0; i < M; i++) f[i] = CellTailAt(c, 2 * jsm::PI * i / M);
            for (int k = 0; k < N_TAIL; k++) {
               double re = 0, im = 0;
               for (int i = 0; i < M; i++) { re += f[i] * cs[(size_t)k * M + i]; im += f[i] * sn[(size_t)k * M + i]; }
               power[k] += re * re + im * im;
            }
         }
      double sxx = 0, sxy = 0, sx = 0, sy = 0;
      for (int k = 0; k < N_TAIL; k++) {
         const double x = std::log((double)(TAIL_K0 + k)), y = std::log(power[k]);
         sx += x; sy += y; sxx += x * x; sxy += x * y;
      }
      const double m = N_TAIL, slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
      Check(std::fabs(slope + (5 - 2 * D)) < 0.25, Fmt("tail D %.2f: spectral slope = -(5 - 2D)", D),
            Fmt("%.3f vs %.3f over %d cells (tol 0.25)", slope, -(5 - 2 * D), cells));
   }

   // Height grid: nucleus covered, and the relaxation removes the kinks (the first 3 cells near the origin).
   Params q = Cheap();
   World w = Fresh(1249, q);
   std::vector<Cell> cells;
   w.CellsInRect(-30, -30, 30, 30, cells);
   if (cells.size() > 3) cells.resize(3);
   double worstCover = 1e9;
   std::vector<double> lapRelax, lapRaw;
   Params raw = q;
   raw.cytoRelaxUm = 0;
   for (const Cell& c : cells) {
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
   Check(!cells.empty() && worstCover > -0.02, "relaxed height covers the nucleus + margin",
         Fmt("worst %.3f um below (tol 0.02) over %zu cells", worstCover, cells.size()));
   auto p99 = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[(size_t)(0.99 * (v.size() - 1))]; };
   const double a = lapRelax.empty() ? 0 : p99(lapRelax), b = lapRaw.empty() ? 0 : p99(lapRaw);
   Check(!lapRaw.empty() && a < 0.5 * b, "relaxation removes kinks (p99 |laplacian| below half the raw one)",
         Fmt("%.2f vs raw %.2f per um", a, b));
}

// ABI 7: a world that takes another world's packed blocks (as the viewer's workers take the pack worker's) answers
// exactly as that world, packs nothing for them, and rejects rows that are not its own cells.
std::vector<double> RowsOf(const std::vector<Cell>& cells)
{
   std::vector<double> rows;
   for (const Cell& c : cells) rows.insert(rows.end(), { (double)c.cx, (double)c.cy, c.x, c.y, c.packRot });
   return rows;
}
std::string DiffCells(const std::vector<Cell>& a, const std::vector<Cell>& b)
{
   if (a.size() != b.size()) return Fmt("%zu vs %zu cells", a.size(), b.size());
   for (size_t i = 0; i < a.size(); i++) {
      const Cell &u = a[i], &v = b[i];
      if (!(u.cx == v.cx && u.cy == v.cy && u.x == v.x && u.y == v.y && u.packRot == v.packRot && u.rOuter == v.rOuter &&
            u.semiMajor == v.semiMajor && u.semiMinor == v.semiMinor && u.modFloor == v.modFloor && u.height == v.height &&
            u.nucOffX == v.nucOffX && u.nucOffY == v.nucOffY && u.nucRot == v.nucRot && u.nucZ == v.nucZ &&
            u.cytoMidHeight == v.cytoMidHeight && u.priority == v.priority && u.tailBound == v.tailBound))
         return Fmt("cell #%zu (%d, %d): (%.12g, %.12g) rot %.12g vs (%d, %d): (%.12g, %.12g) rot %.12g (or another field)",
                    i, u.cx, u.cy, u.x, u.y, u.packRot, v.cx, v.cy, v.x, v.y, v.packRot);
   }
   return "";
}

void BlockInjection()
{
   const Params p = Cheap();
   World a(1249, p), b(1249, p), c(1249, p), other(7, p);
   const double x0 = -30, y0 = -30, x1 = 30, y1 = 30;
   std::vector<Cell> ca, cb;
   a.CellsInRect(x0, y0, x1, y1, ca);
   // The blocks a 60 um rect at the origin reaches (reach ~130 um): [-1, 0]^2.
   int installed = 0, skipped = 0, rejected = 0, foreign = 0;
   for (int32_t bx = -1; bx <= 0; bx++)
      for (int32_t by = -1; by <= 0; by++) {
         const std::vector<double> rows = RowsOf(a.BlockCells(bx, by));
         bool sk = false;
         if (b.SetPackedBlock(bx, by, rows.data(), (int32_t)(rows.size() / 5), 5, sk)) (sk ? skipped : installed)++;
         else rejected++;
         // The other seed's rows: not this world's cells.
         const std::vector<double> orows = RowsOf(other.BlockCells(bx, by));
         if (!orows.empty() && c.SetPackedBlock(bx, by, orows.data(), (int32_t)(orows.size() / 5), 5, sk)) foreign++;
      }
   Check(installed == 4 && skipped == 0 && rejected == 0, "block injection: 4 blocks installed, none rejected",
         Fmt("installed %d, skipped %d, rejected %d", installed, skipped, rejected));
   b.CellsInRect(x0, y0, x1, y1, cb);
   Check(!ca.empty(), "block injection: cells in the rect", "none");
   Same(DiffCells(ca, cb), "block injection: the same cells (every field) as the world that packed them");
   Check(b.Stats().blocksPacked == 0 && b.Stats().blocksInjected == 4, "block injection: nothing packed by the taker",
         Fmt("packed %ld, injected %ld", b.Stats().blocksPacked, b.Stats().blocksInjected));
   if (!ca.empty() && !cb.empty()) {
      CellAssets& A = a.Assets(ca[0]);
      CellAssets& B = b.Assets(cb[0], false);   // mesh only ...
      Check(A.geom.mesh.h == B.geom.mesh.h, "block injection: the same cytoplasm mesh");
      Check(!B.mtsBuilt, "block injection: mesh-only assets have no microtubules yet");
      CellAssets& B2 = b.Assets(cb[0]);   // ... then the microtubules on demand
      std::string mt = B2.mtsBuilt && A.mts.size() == B2.mts.size() ? "" : Fmt("%zu vs %zu microtubules (built %d)", A.mts.size(), B2.mts.size(), (int)B2.mtsBuilt);
      for (size_t i = 0; mt.empty() && i < A.mts.size(); i++) {
         if (A.mts[i].pts.size() != B2.mts[i].pts.size()) mt = Fmt("microtubule %zu: %zu vs %zu points", i, A.mts[i].pts.size(), B2.mts[i].pts.size());
         for (size_t q = 0; mt.empty() && q < A.mts[i].pts.size(); q++)
            if (A.mts[i].pts[q].x != B2.mts[i].pts[q].x || A.mts[i].pts[q].z != B2.mts[i].pts[q].z)
               mt = Fmt("microtubule %zu point %zu differs", i, q);
      }
      Same(mt, "block injection: the same microtubules, built on demand after the mesh");
   }
   std::vector<WorldDye> da, db;
   a.SitesInWindow(x0, y0, x0 + 6.4, y0 + 6.4, -INF, INF, da);
   b.SitesInWindow(x0, y0, x0 + 6.4, y0 + 6.4, -INF, INF, db);
   Check(!da.empty(), "block injection: dyes in the window", "none");
   Same(DiffDyes(da, db), "block injection: the same dyes");
   a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1));
   b.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::PALM, 0.1));
   std::vector<WorldEvent> ea, eb;
   a.EventsInWindow(x0, y0, x0 + 6.4, y0 + 6.4, -INF, INF, 0, 3, ea);
   b.EventsInWindow(x0, y0, x0 + 6.4, y0 + 6.4, -INF, INF, 0, 3, eb);
   Check(!ea.empty(), "block injection: events in the window", "none");
   Same(DiffOrdered(ea, eb), "block injection: the same events in the same order");
   bool sk = false;
   const std::vector<Cell>& again = a.BlockCells(0, 0);
   std::vector<double> rows(again.size() * 5, 0.0);
   for (size_t i = 0; i < again.size(); i++) { rows[5 * i] = again[i].cx; rows[5 * i + 1] = again[i].cy; }
   Check(b.SetPackedBlock(0, 0, rows.data(), (int32_t)again.size(), 5, sk) && sk, "block injection: a cached block is skipped");
   Check(foreign == 0 && c.Stats().blocksInjected == 0, "block injection: another seed's rows are rejected, nothing installed",
         Fmt("%d accepted, %ld installed", foreign, c.Stats().blocksInjected));
   std::vector<Cell> cc;
   c.CellsInRect(x0, y0, x1, y1, cc);
   Same(DiffCells(ca, cc), "block injection: the rejecting world packs its own (the same cells)");
   b.DropCaches();
   cb.clear();
   b.CellsInRect(x0, y0, x1, y1, cb);
   Same(DiffCells(ca, cb), "block injection: after dropping the caches the taker packs the same cells itself");
   Check(b.Stats().blocksPacked > 0, "block injection: ... packing them", Fmt("%ld packed", b.Stats().blocksPacked));

   // Through the C ABI.
   IscParams* ip = AbiParams();
   IscWorld* wa = isc_world_new(1249, ip);
   IscWorld* wb = isc_world_new(1249, ip);
   std::string abi;
   for (int32_t bx = -1; bx <= 0 && abi.empty(); bx++)
      for (int32_t by = -1; by <= 0 && abi.empty(); by++) {
         const int32_t n = isc_world_pack_block(wa, bx, by, nullptr, 0);
         std::vector<double> buf((size_t)std::max(0, n) * ISC_CELL_STRIDE);
         const int32_t filled = isc_world_pack_block(wa, bx, by, buf.data(), n);
         const int32_t set1 = isc_world_set_block(wb, bx, by, buf.data(), n), set2 = isc_world_set_block(wb, bx, by, buf.data(), n);
         if (!(n >= 0 && filled == n && set1 == 1 && set2 == 0))
            abi = Fmt("block (%d, %d): %d cells, filled %d, set %d then %d (want 1 then 0)", bx, by, n, filled, set1, set2);
      }
   Same(abi, "C ABI: isc_world_pack_block -> isc_world_set_block installs each block once");
   const int32_t na = isc_cells_in_window(wa, x0, y0, x1, y1, nullptr, 0), nb = isc_cells_in_window(wb, x0, y0, x1, y1, nullptr, 0);
   std::vector<double> ra((size_t)std::max(0, na) * ISC_CELL_STRIDE), rb((size_t)std::max(0, nb) * ISC_CELL_STRIDE);
   isc_cells_in_window(wa, x0, y0, x1, y1, ra.data(), na);
   isc_cells_in_window(wb, x0, y0, x1, y1, rb.data(), nb);
   Check(na > 0 && na == nb && std::memcmp(ra.data(), rb.data(), ra.size() * sizeof(double)) == 0,
         "C ABI: the installed blocks reproduce isc_cells_in_window", Fmt("%d vs %d cells (or their rows differ)", na, nb));
   Check(isc_world_set_block(wb, 5, 5, ra.data(), 1) == -1, "C ABI: a row outside the block is rejected");
   isc_world_free(wa);
   isc_world_free(wb);
   isc_params_free(ip);
}

// The packed-block disk store (ABI 8): a second world of the same key takes every block from the file (same cells,
// nothing packed); another key or a damaged file means a repack and a rewritten file.
void BlockStoreTest()
{
   namespace fs = std::filesystem;
   std::error_code ec;
   const fs::path dir = fs::temp_directory_path(ec) /
      ("isc_blockstore_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
   const std::string d = dir.u8string();
   const Params p = Cheap();
#if defined(__EMSCRIPTEN__)
   // No file system in the viewer's WASM: never a store (the page keeps its own in local storage).
   World w0(1249, p);
   Check(!w0.SetCacheDir(d) && w0.Store() == nullptr, "block store: none under Emscripten (no file system)");
   return;
#endif
   std::vector<Cell> ca, cb, cc, cd;
   long packedA = 0;
   {
      World a(1249, p);
      Check(a.SetCacheDir(d) && a.Store() && !a.Store()->Path().empty(), "block store: cache directory made and accepted");
      a.CellsInRect(-30, -30, 30, 30, ca);
      packedA = a.Stats().blocksPacked;
      const bool flushed = a.FlushCache();
      Check(packedA == 4 && a.Stats().blocksFromStore == 0 && flushed, "block store: every touched block packed and written",
            Fmt("packed %ld (want 4: a 60 um rect reaches 2 x 2 blocks), from store %ld, flush %d, pending %zu, stored %zu",
                packedA, a.Stats().blocksFromStore, (int)flushed, a.Store()->Pending(), a.Store()->Size()));
   }   // (the destructor flushes too)
   const auto size = fs::file_size(dir / BLOCK_STORE_FILE, ec);
   Check(size > 20 && size < 64 * 1024, "block store: a small file (5 doubles per cell)", Fmt("%llu bytes", (unsigned long long)size));
   {
      World b(1249, p);
      const bool okB = b.SetCacheDir(d);
      Check(okB && (long)b.Store()->Size() == packedA, "block store: a new world of the same key loads the blocks",
            Fmt("ok %d, loaded %zu of %ld", (int)okB, b.Store() ? b.Store()->Size() : (size_t)0, packedA));
      b.CellsInRect(-30, -30, 30, 30, cb);
      Same(DiffCells(ca, cb), "block store: the same cells (every field)");
      Check(b.Stats().blocksPacked == 0 && b.Stats().blocksFromStore == packedA, "block store: nothing packed, every block from the file",
            Fmt("packed %ld, from the store %ld", b.Stats().blocksPacked, b.Stats().blocksFromStore));
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
      Check(c.Stats().blocksPacked == packedA && c.Stats().blocksFromStore == 0, "block store: repacked with the other parameters",
            Fmt("packed %ld, from the store %ld", c.Stats().blocksPacked, c.Stats().blocksFromStore));
      Check(!DiffCells(ca, cc).empty(), "block store: ... into other cells", "the same cells");
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
      Same(DiffCells(ca, cd), "block store: ... and the world packs as usual");
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

// The build must not contract a*b - c into a fused multiply-add anywhere (every "same bits as the JS" argument depends
// on it, link-time optimisation included): with x = 1 + 2^-27, x*x rounds to 1 + 2^-26 and x*x - 1 is exactly 2^-26; a
// fused evaluation keeps the 2^-54 term.
void NoContraction()
{
   volatile double x = 1.0 + 7.450580596923828125e-9; // 1 + 2^-27
   const double xv = x;
   volatile double r = xv * xv - 1.0;
   Check(r == 1.490116119384765625e-8, "no floating-point contraction (x*x - 1 with x = 1 + 2^-27 is exactly 2^-26)",
         Fmt("got %.17g", (double)r));
}

// ABI 15: fiducial beads (fiducials.h; the equality with the JS twin is the golden 'fiducials' lines and scope_parity's
// bead movies). Determinism under any query order, the Poisson density, the diameter spread, the three heights on a
// World's own surface, the cells untouched, the C ABI.
void Fiducials()
{
   auto key = [](const FiducialBead& b) { return std::make_tuple(b.tx, b.ty, b.i); };
   auto sorted = [&](std::vector<FiducialBead> v) {
      std::sort(v.begin(), v.end(), [&](const FiducialBead& a, const FiducialBead& b) { return key(a) < key(b); });
      return v;
   };
   auto diff = [&](const std::vector<FiducialBead>& a, const std::vector<FiducialBead>& b) -> std::string {
      if (a.size() != b.size()) return Fmt("%zu vs %zu beads", a.size(), b.size());
      for (size_t i = 0; i < a.size(); i++)
         if (key(a[i]) != key(b[i]) || a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z || a[i].d != b[i].d)
            return Fmt("bead #%zu: tile (%d, %d) #%d (%.9g, %.9g, %.9g) d %.6g vs tile (%d, %d) #%d (%.9g, %.9g, %.9g) d %.6g", i,
                       a[i].tx, a[i].ty, a[i].i, a[i].x, a[i].y, a[i].z, a[i].d, b[i].tx, b[i].ty, b[i].i, b[i].x, b[i].y, b[i].z, b[i].d);
      return "";
   };
   const auto flat = [](double, double) { return 0.0; };
   // Determinism: one query = the union of tiles queried in another order, by a fresh field.
   {
      const Params p = CheapWith({ { "fidDensity", 3 } });
      FiducialField A(99, p, flat), B(99, p, flat);
      std::vector<FiducialBead> whole, parts;
      A.InRect(-7.3, -5.1, 9.2, 8.7, whole);
      for (double x = 9.2; x > -7.3; x -= 1.7)
         for (double y = -5.1; y < 8.7; y += 2.3)
            B.InRect(std::max(-7.3, x - 1.7), y, x, std::min(8.7, y + 2.3), parts);
      Check(!whole.empty(), "fiducials: beads in the window", "none");
      Same(diff(sorted(whole), sorted(parts)), "fiducials: one query = the union of its pieces in any order");
   }
   // Density (Poisson) and the diameter spread.
   {
      const double S = 40, rho = 2, cv = 12;
      FiducialField F(5, CheapWith({ { "fidDensity", rho }, { "fidDiameterNm", 200 }, { "fidDiameterCvPct", cv } }), flat);
      std::vector<FiducialBead> b;
      F.InRect(0, 0, S, S, b);
      double m = 0, m2 = 0, zbad = 0;
      for (const FiducialBead& e : b) {
         m += e.d;
         m2 += e.d * e.d;
         zbad = std::max(zbad, std::fabs(e.z - e.d / 2000.0));
      }
      const double n = (double)b.size(), mean = m / n, sd = std::sqrt(m2 / n - mean * mean), want = rho * S * S;
      Check(std::fabs(n - want) < 4 * std::sqrt(want), "fiducials: Poisson count within 4 sigma",
            Fmt("%.0f beads, mean %.0f +- 4 x %.1f", n, want, std::sqrt(want)));
      Check(std::fabs(mean / 200 - 1) < 0.01, "fiducials: diameter mean", Fmt("%.2f nm (200 +- 1 %%)", mean));
      Check(std::fabs(sd / mean * 100 / cv - 1) < 0.05, "fiducials: diameter spread", Fmt("CV %.2f %% (12 +- 5 %%)", 100 * sd / mean));
      Check(zbad == 0, "fiducials: Coverslip puts each centre at its radius", Fmt("worst %.3g um off", zbad));
   }
   // The heights on a cell field's own surface (World::SurfaceHeightAt), and the cells do not move.
   {
      const double x0 = 55, y0 = -5, x1 = 70, y1 = 10;
      World plain = Fresh(1249, Cheap());
      World top = Fresh(1249, CheapWith({ { "fidDensity", 4 }, { "fidHeight", 1 } }));
      World layer = Fresh(1249, CheapWith({ { "fidDensity", 4 }, { "fidHeight", 2 }, { "fidLayerUm", 8 } }));
      // (Fresh takes the bank's blocks: compare with a world that packs them itself)
      World own(1249, CheapWith({ { "fidDensity", 4 }, { "fidHeight", 1 } }));
      std::vector<Cell> ca, cb;
      plain.CellsInRect(x0, y0, x1, y1, ca);
      own.CellsInRect(x0, y0, x1, y1, cb);
      Check(!ca.empty(), "fiducials: cells in the window", "none");
      Same(DiffCells(ca, cb), "fiducials: the cells do not move (fid* outside the packing fingerprint)");
      std::vector<FiducialBead> bt, bl;
      top.FiducialsInRect(x0, y0, x1, y1, bt);
      layer.FiducialsInRect(x0, y0, x1, y1, bl);
      double topErr = 0, onCells = 0, maxSurf = 0;
      for (const FiducialBead& b : bt) {
         const double s = plain.SurfaceHeightAt(b.x, b.y);
         topErr = std::max(topErr, std::fabs(b.z - (s + b.d / 2000.0)));
         onCells += s > 0;
         maxSurf = std::max(maxSurf, s);
      }
      std::string layerBad = bl.empty() ? "no beads" : "";
      for (const FiducialBead& b : bl) {
         const double r = b.d / 2000.0, s = plain.SurfaceHeightAt(b.x, b.y);
         if (layerBad.empty() && !(b.z >= r && b.z <= 8 - r + 1e-12 && b.z >= s + r))
            layerBad = Fmt("bead at (%g, %g): z %.6g, radius %.4g, surface %.4g, layer 8 um", b.x, b.y, b.z, r, s);
      }
      Check(topErr == 0, "fiducials: OnTop sits each bead on the sample's surface", Fmt("worst %.3g um off", topErr));
      Check(onCells > 0 && maxSurf > 1, "fiducials: OnTop beads on cells", Fmt("%.0f of %zu on cells, highest surface %.2f um", onCells, bt.size(), maxSurf));
      Same(layerBad, "fiducials: Layer keeps beads in the gel and out of the cells");
      Check(bl.size() < bt.size(), "fiducials: Layer drops the beads inside cells", Fmt("%zu in the layer vs %zu on top", bl.size(), bt.size()));
   }
   // The C ABI.
   {
      IscParams* ip = AbiParams({ { "fidDensity", 2 }, { "fidHeight", 1 } });
      IscWorld* w = isc_world_new(1249, ip);
      const int32_t n = isc_fiducials_in_window(w, 55, -5, 70, 10, nullptr, 0);
      std::vector<double> rows((size_t)std::max(0, n) * ISC_FIDUCIAL_STRIDE);
      const int32_t m = isc_fiducials_in_window(w, 55, -5, 70, 10, rows.data(), n);
      World ref = Fresh(1249, CheapWith({ { "fidDensity", 2 }, { "fidHeight", 1 } }));
      std::vector<FiducialBead> b;
      ref.FiducialsInRect(55, -5, 70, 10, b);
      std::string d = n > 0 && m == n && (size_t)n == b.size() ? "" : Fmt("%d rows (%d filled) vs %zu beads", n, m, b.size());
      for (size_t i = 0; d.empty() && i < b.size(); i++) {
         const double* r = &rows[i * ISC_FIDUCIAL_STRIDE];
         if (!(r[0] == b[i].tx && r[1] == b[i].ty && r[2] == b[i].i && r[3] == b[i].x && r[4] == b[i].y && r[5] == b[i].z &&
               r[6] == b[i].d))
            d = Fmt("row %zu: (%g, %g, %g) vs (%g, %g, %g)", i, r[3], r[4], r[5], b[i].x, b[i].y, b[i].z);
      }
      Same(d, "fiducials: C ABI rows = the World's beads");
      Check(isc_fiducials_in_window(w, 1, 0, 0, 1, nullptr, 0) == -1, "fiducials: C ABI rejects an empty rect");
      isc_world_free(w);
      isc_params_free(ip);
   }
}


// ABI 12: the DNA-origami nanorulers (origami.h; the equality with the JS twin is label_parity.mjs and the golden
// 'origami' lines). The shape's invariants and its worm-like-chain statistics, the field's Matern spacing and density,
// a nanoruler World: sites independent of the query history, the docking strands only, the strand subset and
// incorporation, no cells, 8 threads = 1 thread, and the C ABI rows.
void Origami()
{
   auto P = [](std::initializer_list<std::pair<const char*, double>> kv) {
      Params p;
      p.specimen = SPECIMEN_NANORULERS;
      for (const auto& e : kv) SetParam(p, e.first, e.second);
      NormalizeOrigamiParams(p);
      return p;
   };
   const OrigamiDesign& d = OrigamiDesignAt(0);
   const OrigamiGeometry G0 = OrigamiGeometryOf(d, P({}));
   const double L = G0.L, D = G0.H, R = D / 2;
   auto spotX = [&](int k) { return d.spots[k].u * G0.s; };   // straight frame, from the centre
   auto mol = [](uint32_t h1) { OrigamiMolecule m{}; m.h1 = h1; return m; };
   // Geometry: rigid = identity, an infinite Lp too, the centreline keeps its length, another molecule bends otherwise.
   {
      auto maxMove = [&](const OrigamiShape& S) {
         double e = 0;
         for (int j = 0; j < 500; j++) {
            const double x = (std::fmod(j * 0.6180339887, 1.0) - 0.5) * L, y = (std::fmod(j * 0.7548776662, 1.0) - 0.5) * D;
            double X, Y;
            OrigamiTransform(S, x, y, X, Y);
            e = std::max(e, std::max(std::fabs(X - x), std::fabs(Y - y)));
         }
         return e;
      };
      const double e0 = maxMove(OrigamiBentShape(d, P({ { "origamiBend", 0 } }), 1));
      const double eInf = maxMove(OrigamiBentShape(d, P({ { "origamiLpNm", 1e12 } }), 1));
      Check(e0 < 1e-9, "origami: a rigid rod maps to itself", Fmt("moves a point by %.3g nm", e0));
      Check(eInf < 1e-3, "origami: Lp -> infinity maps to itself", Fmt("moves a point by %.3g nm", eInf));
      const Params p = P({ { "origamiLpNm", 273.1 } });
      const OrigamiShape S = OrigamiBentShape(d, p, 7);
      double len = 0, X0, Y0, th;
      OrigamiCenterline(S, -L / 2, X0, Y0, th);
      for (int j = 1; j <= 2000; j++) {
         double X, Y;
         OrigamiCenterline(S, -L / 2 + L * j / 2000, X, Y, th);
         len += jsm::hypot(X - X0, Y - Y0);
         X0 = X;
         Y0 = Y;
      }
      Check(std::fabs(len - L) < 1e-3, "origami: bending keeps the centreline's length", Fmt("%.6f nm vs %.0f", len, L));
      const double a7 = S.endAngle, a8 = OrigamiBentShape(d, p, 8).endAngle;
      Check(std::fabs(a7 - a8) > 1e-6, "origami: another molecule bends otherwise", Fmt("end angles %.9g and %.9g", a7, a8));
   }
   {
      const Params p = P({ { "origamiLpNm", 273.1 }, { "origamiLinkerNm", 0 }, { "origamiHeights", 1 }, { "origamiAttachNm", 0 } });
      const OrigamiShape S = OrigamiBentShape(d, p, 3);
      std::vector<OrigamiSite> sites;
      OrigamiMoleculeSites(d, p, mol(3), S, 1, 1, sites, true);
      double lo = INF, hi = 0, centre = 0;
      for (const OrigamiSite& s : sites) {
         double cx, cy, th;
         OrigamiCenterline(S, s.rest[0], cx, cy, th);
         const double r = jsm::hypot(jsm::hypot(s.bent[0] - cx, s.bent[1] - cy), s.z * 1000 - R);
         lo = std::min(lo, r);
         hi = std::max(hi, r);
      }
      for (int k = 0; k < d.nSpots; k++) {
         double sx = 0, sy = 0;
         int n = 0;
         for (const OrigamiSite& s : sites)
            if (s.k == k) { sx += s.bent[0]; sy += s.bent[1]; n++; }
         double ax, ay, th;
         OrigamiCenterline(S, spotX(k), ax, ay, th);
         centre = std::max(centre, jsm::hypot(sx / n - ax, sy / n - ay));
      }
      Check(sites.size() == 15, "origami: 15 designed strands", Fmt("%zu", sites.size()));
      Check(std::fabs(lo - R) < 1e-9 && std::fabs(hi - R) < 1e-9, "origami: the strands on the rod surface",
            Fmt("%.9f-%.9f nm from the axis (radius %.1f)", lo, hi, R));
      Check(centre < 1e-9, "origami: each ring centred on the axis", Fmt("%.3g nm off", centre));
      const Params q = P({});
      sites.clear();
      OrigamiMoleculeSites(d, q, mol(5), OrigamiBentShape(d, q, 5), 1, 1, sites, true);
      std::string flat = sites.empty() ? "no sites" : "";
      for (const OrigamiSite& s : sites)
         if (flat.empty() && s.z != (q.origamiAttachNm + R) / 1000) flat = Fmt("strand at z %.9g um, want %.9g", s.z, (q.origamiAttachNm + R) / 1000);
      Same(flat, "origami: 2D, every strand at attach + D/2");
   }
   // The site spacing (origamiSpacingNm): a rod keeps its 20 nm ends; the 2D logo is a rigid face-up hex map.
   {
      Check(L == 200 && D == 13 && spotX(0) == -80 && spotX(2) == 80, "origami: 80 nm spacing = the 200 nm rod, spots at 20/100/180 nm",
            Fmt("L %g, D %g, spots at %g and %g from the centre", L, D, spotX(0), spotX(2)));
      std::string bad;
      for (double sp : { 640.0, 40.0, 5.0 }) {
         const Params p = P({ { "origamiSpacingNm", sp }, { "origamiBend", 0 }, { "origamiLinkerNm", 0 } });
         std::vector<OrigamiSite> sites;
         OrigamiMoleculeSites(d, p, mol(5), OrigamiBentShape(d, p, 5), 1, 1, sites, true);
         if (bad.empty() && OrigamiGeometryOf(d, p).L != 2 * sp + 40) bad = Fmt("%g nm: rod %g nm long", sp, OrigamiGeometryOf(d, p).L);
         for (const OrigamiSite& s : sites)
            if (bad.empty() && std::fabs(s.rest[0] - (s.k - 1) * sp) > 1e-9) bad = Fmt("%g nm: spot %d at %.9g nm", sp, s.k, s.rest[0]);
      }
      Same(bad, "origami: spots one spacing apart, 20 nm ends (640, 40, 5 nm)");
      int dl = -1;
      for (int i = 0; i < OrigamiDesignCount(); i++)
         if (!std::strcmp(OrigamiDesignAt(i).id, "TUDelftLogo")) dl = i;
      Check(dl > 0, "origami: the TU Delft logo design exists");
      if (dl > 0) {
         const OrigamiDesign& dd = OrigamiDesignAt(dl);
         const Params p = P({ { "origamiDesign", (double)dl }, { "origamiSpacingNm", 20 }, { "origamiLinkerNm", 0 } });
         std::vector<OrigamiSite> sites;
         OrigamiMoleculeSites(dd, p, mol(77), OrigamiBentShape(dd, p, 77), 1, 1, sites, true);
         double nn = INF;
         std::string rigid;
         for (size_t a = 0; a < sites.size(); a++) {
            if (rigid.empty() && !(sites[a].rest[2] == dd.heightNm / 2 && std::fabs(sites[a].bent[0] - sites[a].rest[0]) < 1e-9 &&
                                   std::fabs(sites[a].bent[1] - sites[a].rest[1]) < 1e-9))
               rigid = Fmt("strand %zu: rest (%g, %g, %g), bent (%g, %g)", a, sites[a].rest[0], sites[a].rest[1], sites[a].rest[2],
                           sites[a].bent[0], sites[a].bent[1]);
            for (size_t b = a + 1; b < sites.size(); b++)
               nn = std::min(nn, jsm::hypot(sites[a].rest[0] - sites[b].rest[0], sites[a].rest[1] - sites[b].rest[1]));
         }
         Check(sites.size() == 37 && std::fabs(nn - 20) < 1e-9, "origami: logo = 37 strands 20 nm apart",
               Fmt("%zu strands, nearest neighbours %.9f nm", sites.size(), nn));
         Same(rigid, "origami: logo rigid, on the top face");
         Check(p.origamiStrandsPerSpot == 1, "origami: logo clamps strands per spot to 1", Fmt("%d", (int)p.origamiStrandsPerSpot));
      }
   }
   // Statistics at the default Lp (1500 nm): end angle, tangent correlation, spot distances vs the 2D WLC.
   {
      const Params p = P({ { "origamiLinkerNm", 0 } });
      const double Lp = p.origamiLpNm;
      const int M = 4000;
      double e1 = 0, e2 = 0;
      double corr[9] = {};
      double d2[3] = {};
      const int pa[3] = { 0, 1, 0 }, pb[3] = { 1, 2, 2 };
      for (int j = 0; j < M; j++) {
         const uint32_t h = OrigamiMoleculeH1(42, j, 0, 0);
         const OrigamiShape S = OrigamiBentShape(d, p, h);
         e1 += S.endAngle;
         e2 += S.endAngle * S.endAngle;
         double X, Y, th0, th;
         OrigamiCenterline(S, -L / 2, X, Y, th0);
         for (int q = 0; q <= 8; q++) {
            OrigamiCenterline(S, -L / 2 + L * q / 8, X, Y, th);
            corr[q] += std::cos(th - th0);
         }
         std::vector<OrigamiSite> sites;
         OrigamiMoleculeSites(d, p, mol(h), S, 1, 1, sites, true);
         double c[3][2] = {};
         for (const OrigamiSite& s : sites) { c[s.k][0] += s.bent[0] / 5; c[s.k][1] += s.bent[1] / 5; }
         for (int q = 0; q < 3; q++) {
            const double dx = c[pa[q]][0] - c[pb[q]][0], dy = c[pa[q]][1] - c[pb[q]][1];
            d2[q] += dx * dx + dy * dy;
         }
      }
      const double mean = e1 / M, sd = std::sqrt(e2 / M - mean * mean), pred = std::sqrt(L / Lp);
      Check(std::fabs(sd / pred - 1) < 0.04, "origami: end-angle RMS = sqrt(L/Lp) within 4 %",
            Fmt("%.3f deg vs %.3f", sd * 180 / jsm::PI, pred * 180 / jsm::PI));
      double dev = 0;
      int at = 0;
      for (int q = 0; q <= 8; q++) {
         const double e = std::fabs(corr[q] / M - std::exp(-(L * q / 8) / (2 * Lp)));
         if (e > dev) { dev = e; at = q; }
      }
      Check(dev < 0.02, "origami: tangent correlation = exp(-s/2Lp) within 0.02", Fmt("off by %.4f at s = %g nm", dev, L * at / 8));
      for (int q = 0; q < 3; q++) {
         const double s = spotX(pb[q]) - spotX(pa[q]);
         const double th = std::sqrt(4 * Lp * s * (1 - (2 * Lp / s) * (1 - std::exp(-s / (2 * Lp)))));
         const double rms = std::sqrt(d2[q] / M);
         Check(std::fabs(rms / th - 1) < 0.005, Fmt("origami: spots %d-%d RMS distance = the 2D worm-like chain (0.5 %%)", pa[q], pb[q]),
               Fmt("%.3f nm vs %.3f", rms, th));
      }
   }
   // The field: minimum spacing and the Matern-II density.
   {
      OrigamiField F(1234, P({ { "origamiDensity", 2 }, { "origamiMinSpacingNm", 250 } }));
      const double S = 20;
      std::vector<OrigamiMolecule> all;
      F.MoleculesInRect(-1, -1, S + 1, S + 1, all, -F.ReachUm());
      double dmin = INF;
      size_t in = 0;
      for (size_t a = 0; a < all.size(); a++) {
         if (all[a].x >= 0 && all[a].x < S && all[a].y >= 0 && all[a].y < S) in++;
         for (size_t b = a + 1; b < all.size(); b++) dmin = std::min(dmin, jsm::hypot(all[a].x - all[b].x, all[a].y - all[b].y));
      }
      const double r = 0.25, lamII = (1 - std::exp(-2 * jsm::PI * r * r)) / (jsm::PI * r * r), got = in / (S * S);
      Check(dmin >= 0.25, "origami: the minimum spacing holds", Fmt("closest centres %.1f nm (250)", dmin * 1000));
      Check(std::fabs(got / lamII - 1) < 0.05, "origami: Matern-II density (5 %)", Fmt("%.3f /um^2 vs %.3f", got, lamII));
   }
   // A nanoruler World.
   {
      Params p;
      p.specimen = SPECIMEN_NANORULERS;
      Kinetics k;
      k.activationRatePerSec = 0.05; k.onSec = 0.3; k.offSec = 0; k.bleachProb = 1; k.photonCV = 0.3;
      const Label lab = MakeLabel(LabelMode::DnaPaint, 1.0, k);
      auto world = [&](const Params& q, const Label& l) {
         World w(77, q);
         w.SetLabel(STRUCTURE_ORIGAMI, l);
         return w;
      };
      World w1 = world(p, lab), w2 = world(p, lab);
      std::vector<WorldDye> junk, s1, s2, all;
      w2.SitesInWindow(-30, -30, -20, -20, -1, 1, junk);
      w1.SitesInWindow(0, 0, 8, 8, -1, 1, s1);
      w2.SitesInWindow(0, 0, 8, 8, -1, 1, s2);
      Check(!s1.empty(), "origami world: strands in the window", "none");
      Same(DiffDyes(s1, s2), "origami world: sites independent of the query history");
      w1.SitesInWindow(0, 0, 30, 30, -1, 1, all);
      std::string other;
      for (const WorldDye& s : all)
         if (other.empty() && s.structure != STRUCTURE_ORIGAMI) other = DyeStr(s);
      Same(other, "origami world: docking strands only");
      std::vector<Cell> cells;
      w1.CellsInRect(0, 0, 30, 30, cells);
      Cell c;
      Check(cells.empty() && !w1.FindCell(0, 0, c) && w1.BlockCells(0, 0).empty(), "origami world: no cells",
            Fmt("%zu cells in the rect", cells.size()));
      Params p3 = p;
      p3.origamiStrandsPerSpot = 3;
      World w3 = world(p3, lab), w4 = world(p, MakeLabel(LabelMode::DnaPaint, 0.8, k));
      std::vector<WorldDye> sub, inc;
      w3.SitesInWindow(0, 0, 30, 30, -1, 1, sub);
      w4.SitesInWindow(0, 0, 30, 30, -1, 1, inc);
      auto key = [](const WorldDye& s) { return std::make_tuple(s.cx, s.cy, s.mtIndex, s.k, s.n, s.x, s.y); };
      std::vector<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, double, double>> keys;
      for (const WorldDye& s : all) keys.push_back(key(s));
      std::sort(keys.begin(), keys.end());
      auto missing = [&](const std::vector<WorldDye>& v, int maxN) -> std::string {
         if (v.empty()) return "no strands";
         for (const WorldDye& s : v)
            if (s.n >= maxN || !std::binary_search(keys.begin(), keys.end(), key(s))) return DyeStr(s) + " is not one of the 5-strand set";
         return "";
      };
      Same(missing(sub, 3), "origami world: strands per spot 3 = ring positions 0-2 of the same strands");
      Same(missing(inc, 1 << 30), "origami world: incorporation 80 % keeps a subset");
      const double frac = (double)inc.size() / all.size();
      Check(std::fabs(frac - 0.8) < 0.03, "origami world: incorporation 80 % keeps 80 % of the strands",
            Fmt("%.1f %% of %zu", 100 * frac, all.size()));
      if (kThreads) {
         World a = world(p, lab), b = world(p, lab);
         std::string diff;
         size_t total = 0;
         for (int f = 0; f < 6 && diff.empty(); f++) {
            const double s = 1.5 * f;
            std::vector<WorldEvent> ea, eb;
            SetWorldThreads(1);
            a.EventsInWindow(s - 6, -6, s + 6, 6, -INF, INF, 20 + f * 0.1, 20 + (f + 1) * 0.1, ea);
            if (f % 2) a.Prefetch(s - 8, -8, s + 8, 8, -INF, INF, 20, 22, 1e9);
            SetWorldThreads(8);
            b.EventsInWindow(s - 6, -6, s + 6, 6, -INF, INF, 20 + f * 0.1, 20 + (f + 1) * 0.1, eb);
            if (f % 2) b.Prefetch(s - 8, -8, s + 8, 8, -INF, INF, 20, 22, 1e9);
            SetWorldThreads(0);
            const std::string dd = DiffOrdered(ea, eb);
            if (!dd.empty()) diff = Fmt("frame %d: ", f) + dd;
            total += ea.size();
         }
         Same(diff, "origami world: 8 threads = 1 thread (DNA-PAINT events)");
         Check(total > 50, "origami world: blinks over 6 frames", Fmt("%zu (want > 50)", total));
      } else
         Skip("origami world: 8 threads = 1 thread", "no threads in WASM");
      std::vector<WorldEvent> cont;
      w1.ContinuousInWindow(0, 0, 8, 8, -INF, INF, 0, cont);
      Check(cont.empty(), "origami world: DNA-PAINT has no continuous windows", Fmt("%zu", cont.size()));
   }
   // The C ABI.
   {
      IscParams* ip = isc_params_new();
      isc_params_set(ip, "specimen", ISC_SPECIMEN_NANORULERS);
      IscWorld* w = isc_world_new(77, ip);
      IscParams* cp = AbiParams();
      IscWorld* cw = isc_world_new(77, cp);
      std::vector<double> rows(ISC_ORIGAMI_STRIDE * 64), cell(ISC_CELL_STRIDE * 4);
      const int32_t n = isc_origami_in_window(w, 0, 0, 6, 6, rows.data(), 64);
      Check(n > 0 && n <= 64, "C ABI: isc_origami_in_window finds molecules", Fmt("%d", n));
      Check(isc_origami_in_window(cw, 0, 0, 6, 6, rows.data(), 0) == 0, "C ABI: no molecules in a cell field");
      Check(isc_cells_in_window(w, 0, 0, 30, 30, cell.data(), 4) == 0, "C ABI: no cells in a nanoruler field");
      Check(isc_world_set_cache_dir(w, "") == 0, "C ABI: the cache directory call accepted on a nanoruler world");
      std::vector<double> o(2 * 200), st(ISC_ORIGAMI_SITE_STRIDE * 32);
      int32_t lit = 0, designed = 0;
      std::string rowBad;
      for (int32_t j = 0; j < n && rowBad.empty(); j++) {
         const double* r = &rows[(size_t)j * ISC_ORIGAMI_STRIDE];
         const int32_t no = isc_origami_outline(w, (int32_t)r[0], (int32_t)r[1], (int32_t)r[2], 64, o.data(), 200);
         const int32_t ns = isc_origami_sites(w, (int32_t)r[0], (int32_t)r[1], (int32_t)r[2], st.data(), 32);
         if (!(no == 2 * (64 + 16) && ns == 15 && r[3] == 0))
            rowBad = Fmt("molecule %d: outline %d values (want %d), %d strands (15), design %g (0)", j, no, 2 * (64 + 16), ns, r[3]);
         designed += ns;
         for (int32_t q = 0; q < ns; q++) lit += st[(size_t)q * ISC_ORIGAMI_SITE_STRIDE + 10] != 0;
      }
      Same(rowBad, "C ABI: isc_origami_outline / _sites rows");
      Check(lit > 0 && lit < designed, "C ABI: some designed strands are not fluorescent", Fmt("%d of %d", lit, designed));
      Check(isc_origami_outline(w, 0, 0, 999999, 64, o.data(), 200) == -1, "C ABI: isc_origami_outline -1 for no molecule");
      Check(isc_origami_outline(cw, 0, 0, 0, 64, o.data(), 200) == -1, "C ABI: isc_origami_outline -1 in a cell field");
      double kin[ISC_KIN_ROW] = {};
      for (int s = 0; s < ISC_STRUCT_COUNT; s++) {
         kin[1 + s * ISC_KIN_COUNT + ISC_KIN_ACTIVATION_RATE] = 0.01;
         kin[1 + s * ISC_KIN_COUNT + ISC_KIN_ON_SEC] = 0.3;
         kin[1 + s * ISC_KIN_COUNT + ISC_KIN_BLEACH_PROB] = 1;
      }
      Check(ISC_KIN_ROW == 1 + 6 * ISC_STRUCT_COUNT, "C ABI: a kinetics row = 1 + 6 per structure", Fmt("%d", (int)ISC_KIN_ROW));
      Check(isc_world_set_kinetics_history(w, kin, 1) == 0, "C ABI: a kinetics row for every structure accepted");
      kin[0] = 1;
      Check(isc_world_set_kinetics_history(w, kin, 1) == -1, "C ABI: a kinetics history not starting at 0 rejected (-1)");
      isc_world_free(w);
      isc_world_free(cw);
      isc_params_free(ip);
      isc_params_free(cp);
   }
}

// Nuclear pores (issue 15, npcs.h) and the nuclear lamina (ABI 16). The JS twin's numbers: label_parity, golden (npc
// and lam lines).
bool InsideNucleus(const Cell& c, const Pt3& q)
{
   if (!c.nucShaped) {
      const double ncr = jsm::cos(-c.nucRot), nsr = jsm::sin(-c.nucRot);
      const double na = c.nucLong / 2, nb = c.nucShort / 2, nh = c.nucHeight / 2;
      const double ex = q.x - c.nucOffX, ey = q.y - c.nucOffY;
      const double ux = (ex * ncr - ey * nsr) / na, uy = (ex * nsr + ey * ncr) / nb, uz = (q.z - c.nucZ) / nh;
      return ux * ux + uy * uy + uz * uz < 1;
   }
   const NucBall B = NucBallLocal(c, q.x, q.y);
   const double zeta = (q.z - c.nucZ) / ((c.nucHeight / 2) * B.H * (q.z < c.nucZ ? c.nucKDown : c.nucKUp));
   return std::fabs(zeta) < 1 && B.s < NucSectionW(zeta, zeta < 0 ? c.nucFBot : c.nucFTop);
}
double CapAreaFraction(const NucSurfaceMesh& M)
{
   double a = 0;
   for (size_t t = 0; t < M.tris.size(); t++)
      if (M.tris[t].piece != 0) a += M.cdf[t] - (t ? M.cdf[t - 1] : 0);
   return a / M.area;
}
// The cap share of points (pooled over cells) against the caps' share of the area: binomial, 4 sigma.
std::string CapShare(long onCaps, long n, double expected)
{
   const double sigma = std::sqrt(expected * (1 - expected / std::max<long>(n, 1)));
   if (n > 0 && std::fabs(onCaps - expected) <= 4 * sigma + 1) return "";
   return Fmt("%ld of %ld on the caps, %.1f +- 4 x %.1f expected", onCaps, n, expected, sigma);
}
// On the membrane with an outward normal: 1 nm out is outside the nucleus, 1 nm in inside.
bool OnMembrane(const Cell& c, const Pt3& p, const Pt3& n)
{
   const Pt3 out = { p.x + n.x * 1e-3, p.y + n.y * 1e-3, p.z + n.z * 1e-3 };
   const Pt3 in = { p.x - n.x * 1e-3, p.y - n.y * 1e-3, p.z - n.z * 1e-3 };
   return InsideNucleus(c, in) && !InsideNucleus(c, out);
}
Params WithPores() { return CheapWith({ { "npcDensity", Params().npcDensity } }); }

void Npcs()
{
   const uint32_t seed = 1249;
   const Params p = WithPores();
   World w = Fresh(seed, p);
   std::vector<Cell> cells;
   w.CellsInRect(-30, -30, 30, 30, cells);
   Check(cells.size() >= 2, "pores: cells near the origin", Fmt("%zu (want 2)", cells.size()));
   if (cells.size() < 2) return;
   cells.resize(2);
   // Placement over two cells.
   long pores = 0, want = 0, onCaps = 0;
   double capExpected = 0, worstGap = INF;
   std::string count, gap, membrane, frame, order;
   auto dot = [](const Pt3& a, const Pt3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
   for (const Cell& c : cells) {
      std::vector<Npc> v;
      if (!w.CellNpcs(c.cx, c.cy, v)) { Check(false, "pores: CellNpcs finds the cell", Fmt("(%d, %d)", c.cx, c.cy)); return; }
      const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
      const long wc = (long)std::floor(p.npcDensity * M.area + 0.5);
      if (count.empty() && (long)v.size() != wc) count = Fmt("cell (%d, %d): %zu pores, want %ld", c.cx, c.cy, v.size(), wc);
      want += wc;
      pores += (long)v.size();
      capExpected += v.size() * CapAreaFraction(M);
      for (size_t i = 0; i < v.size(); i++) {
         const Npc& q = v[i];
         onCaps += q.piece != 0;
         if (order.empty() && i > 0 && !(q.cand > v[i - 1].cand)) order = Fmt("cell (%d, %d) pore %zu", c.cx, c.cy, i);
         if (membrane.empty() && !OnMembrane(c, q.c, q.n)) membrane = Fmt("cell (%d, %d) pore %zu at (%.6f, %.6f, %.6f)", c.cx, c.cy, i, q.c.x, q.c.y, q.c.z);
         const Pt3 x = { q.e1.y * q.e2.z - q.e1.z * q.e2.y, q.e1.z * q.e2.x - q.e1.x * q.e2.z, q.e1.x * q.e2.y - q.e1.y * q.e2.x };
         const double err = std::max({ std::fabs(dot(q.n, q.n) - 1), std::fabs(dot(q.e1, q.e1) - 1), std::fabs(dot(q.e2, q.e2) - 1),
                                       std::fabs(dot(q.n, q.e1)), std::fabs(dot(q.n, q.e2)), std::fabs(dot(q.e1, q.e2)),
                                       std::fabs(x.x - q.n.x) + std::fabs(x.y - q.n.y) + std::fabs(x.z - q.n.z) });
         if (frame.empty() && err > 1e-12) frame = Fmt("cell (%d, %d) pore %zu: off by %.3g", c.cx, c.cy, i, err);
         for (size_t j = 0; j < i; j++) {
            const double dx = q.c.x - v[j].c.x, dy = q.c.y - v[j].c.y, dz = q.c.z - v[j].c.z;
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            worstGap = std::min(worstGap, d);
            if (gap.empty() && d < p.npcMinDistNm * 1e-3) gap = Fmt("cell (%d, %d): pores %zu and %zu %.1f nm apart", c.cx, c.cy, j, i, d * 1000);
         }
      }
   }
   Same(count, "pores: count = round(density x area) per cell");
   Check(pores > 1000, "pores: enough pores to test", Fmt("%ld over 2 cells", pores));
   Same(gap, Fmt("pores: none closer than npcMinDistNm (%.0f nm)", p.npcMinDistNm));
   Same(membrane, "pores: on the membrane, normal outward");
   Same(frame, "pores: (n, e1, e2) orthonormal and right-handed (e1 x e2 = n)");
   Same(order, "pores: in candidate order");
   Same(CapShare(onCaps, pores, capExpected), "pores: uniform by area (cap share)");
   // Attachment points: 64, rings at -/+ sep/2, mean radius = the mean of |(R + r cos phi, r sin phi)| over the arc.
   {
      double S[NPC_SITES * 3];
      NpcAttachmentPoints(p, S);
      double r = 0, zs = 0;
      for (int k = 0; k < NPC_SITES; k++) {
         r += std::hypot(S[3 * k], S[3 * k + 1]);
         zs += S[3 * k + 2] * (k < 32 ? -1 : 1);
      }
      const double ar = p.npcCornerSpreadNm / 2;
      double rWant = 0;
      for (double dg : { -67.5, -22.5, 22.5, 67.5 })
         rWant += std::hypot(p.npcRadiusNm + ar * std::cos(dg * jsm::PI / 180), ar * std::sin(dg * jsm::PI / 180)) / 4;
      Check(std::fabs(r / NPC_SITES - rWant) < 1e-9, "pores: attachment points' mean radius", Fmt("%.6f nm vs %.6f", r / NPC_SITES, rWant));
      Check(std::fabs(zs / NPC_SITES - p.npcRingSepNm / 2) < 1e-9, "pores: rings at -/+ half the separation",
            Fmt("%.6f nm vs %.6f", zs / NPC_SITES, p.npcRingSepNm / 2));
   }
   const Cell& c0 = cells[0];
   auto diffPores = [](const std::vector<Npc>& a, const std::vector<Npc>& b) -> std::string {
      if (a.size() != b.size() || a.empty()) return Fmt("%zu vs %zu pores", a.size(), b.size());
      for (size_t i = 0; i < a.size(); i++)
         if (!(a[i].c.x == b[i].c.x && a[i].c.y == b[i].c.y && a[i].c.z == b[i].c.z && a[i].e1.x == b[i].e1.x && a[i].cand == b[i].cand))
            return Fmt("pore %zu: (%.9g, %.9g, %.9g) cand %d vs (%.9g, %.9g, %.9g) cand %d", i, a[i].c.x, a[i].c.y, a[i].c.z, a[i].cand,
                       b[i].c.x, b[i].c.y, b[i].c.z, b[i].cand);
      return "";
   };
   // The same pores from a fresh world after other queries; npc* params move no cell; density 0 = none.
   {
      World w2 = Fresh(seed, p);
      std::vector<WorldDye> junk;
      w2.SitesInWindow(40, 40, 44, 44, -INF, INF, junk);
      std::vector<Npc> a, b;
      w.CellNpcs(c0.cx, c0.cy, a);
      w2.CellNpcs(c0.cx, c0.cy, b);
      Same(diffPores(a, b), "pores: independent of the query history");
      Params q = p;
      q.npcDensity = 0;
      Check(PackingFingerprint(q) == PackingFingerprint(p), "pores: npc* params leave the packing fingerprint");
      World w0 = Fresh(seed, q);
      std::vector<Npc> none;
      Check(w0.CellNpcs(c0.cx, c0.cy, none) && none.empty(), "pores: npcDensity 0 = no pores", Fmt("%zu", none.size()));
   }
   // Pore dyes over the first nucleus (4 x 4 um): structure 1 alone, tiling, density3d bit 1, 8 threads = 1.
   double nx, ny;
   LocalToWorld(c0, c0.nucOffX, c0.nucOffY, nx, ny);
   const double X0 = nx - 2, Y0 = ny - 2, X1 = nx + 2, Y1 = ny + 2;
   auto poresOnly = [&](World& a, const Label& l) {
      a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::DnaPaint, 0));
      a.SetLabel(STRUCTURE_NPC, l);
   };
   {
      World a = Fresh(seed, p), b = Fresh(seed, p);
      poresOnly(a, MakeLabel(LabelMode::dSTORM, 0.6));
      poresOnly(b, MakeLabel(LabelMode::dSTORM, 0.6));
      std::vector<WorldDye> all, tiles;
      a.SitesInWindow(X0, Y0, X1, Y1, -INF, INF, all);
      std::string bad;
      for (const WorldDye& d : all)
         if (bad.empty() && !(d.structure == STRUCTURE_NPC && d.mtIndex == -1 && d.k >= 0 && d.k < NPC_SITES)) bad = DyeStr(d);
      Check(all.size() > 300, "pore dyes: in a 4 x 4 um window over a nucleus", Fmt("%zu (want > 300)", all.size()));
      Same(bad, "pore dyes: structure 1, sites 0-63");
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++) b.SitesInWindow(X0 + 2 * i, Y0 + 2 * j, X0 + 2 * (i + 1), Y0 + 2 * (j + 1), -INF, INF, tiles);
      Same(DiffDyes(Sorted(all), Sorted(tiles)), "pore dyes: 2 x 2 tiles = the whole window");
      std::vector<float> g(4 * 4);
      World m = Fresh(seed, p);
      m.SetLabel(STRUCTURE_NPC, MakeLabel(LabelMode::dSTORM, 0.6));
      const long n1 = m.Density3dInWindow(X0, Y0, X1, Y1, -INF, INF, 4, 4, 1, 1u << STRUCTURE_NPC, g.data());
      Check(n1 == (long)all.size(), "pore dyes: density3d bit 1 counts the pore dyes alone", Fmt("%ld vs %zu", n1, all.size()));
   }
   if (kThreads)
      for (LabelMode mode : { LabelMode::DnaPaint, LabelMode::dSTORM }) {
         Kinetics k;
         k.activationRatePerSec = 0.05; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5;
         World t1 = Fresh(seed, p), t8 = Fresh(seed, p);
         for (World* t : { &t1, &t8 }) {
            t->SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::dSTORM, 0.02, k));
            t->SetLabel(STRUCTURE_NPC, MakeLabel(mode, 0.6, k));
         }
         std::vector<WorldEvent> e1, e8;
         SetWorldThreads(1);
         t1.EventsInWindow(X0, Y0, X1, Y1, -INF, INF, 20, 25, e1);
         SetWorldThreads(8);
         t8.EventsInWindow(X0, Y0, X1, Y1, -INF, INF, 20, 25, e8);
         SetWorldThreads(0);
         size_t npc = 0;
         for (const WorldEvent& e : e1) npc += e.structure == STRUCTURE_NPC;
         const char* name = mode == LabelMode::DnaPaint ? "DNA-PAINT" : "dSTORM";
         Check(npc > 0, Fmt("pore dyes, %s: pore blinks in [20, 25) s", name), "none");
         Same(DiffOrdered(e1, e8), Fmt("pore dyes, %s: 8 threads = 1 thread", name));
      }
   else
      Skip("pore dyes: 8 threads = 1 thread", "no threads in WASM");
   // The label linkage through the C ABI, on one world: NaN = the structure's legacy numbers = those numbers given;
   // another linkage moves the same dyes (the same ids).
   {
      IscParams* ip = AbiParams({ { "npcDensity", p.npcDensity } });
      IscWorld* cw = isc_world_new(seed, ip);
      isc_params_free(ip);
      double off[ISC_LABEL_COUNT] = { 0 };
      isc_world_set_label(cw, ISC_STRUCT_MICROTUBULE, off, 1);
      auto sites = [&](const double* lk) {
         double v[ISC_LABEL_COUNT] = { 0.6, 1, ISC_MODE_DNA_PAINT, 0.01, 0.05, 1, 1, 0.5, 0, 0, ISC_ORIENT_FREE, 90, 0, 0, 0, 0,
                                       lk[0], lk[1], lk[2], lk[3] };
         std::vector<double> out;
         if (isc_world_set_label(cw, ISC_STRUCT_NPC, v, ISC_LABEL_COUNT) == 0) {
            const int32_t n = isc_sites_in_window(cw, X0, Y0, X1, Y1, -INF, INF, nullptr, 0);
            out.resize((size_t)std::max(0, n) * ISC_SITE_STRIDE);
            isc_sites_in_window(cw, X0, Y0, X1, Y1, -INF, INF, out.data(), n);
         }
         return out;
      };
      const double nanLk[4] = { NAN, NAN, NAN, NAN }, legacy[4] = { 0, 0, 2, 5 }, igg[4] = { 19.3, 15.3, 0.5, 0.5 };
      const std::vector<double> a = sites(nanLk), b = sites(legacy), g = sites(igg);
      Check(!a.empty() && a == b, "linkage: NaN = the pores' built-in numbers (0, 0, 2, 5) given explicitly",
            Fmt("%zu vs %zu values (or they differ)", a.size(), b.size()));
      std::vector<double> ia, ig, both;
      for (size_t i = 0; i < a.size(); i += ISC_SITE_STRIDE) ia.push_back(a[i + 3]);
      for (size_t i = 0; i < g.size(); i += ISC_SITE_STRIDE) ig.push_back(g[i + 3]);
      std::sort(ia.begin(), ia.end());
      std::sort(ig.begin(), ig.end());
      std::set_intersection(ia.begin(), ia.end(), ig.begin(), ig.end(), std::back_inserter(both));
      Check(g != a, "linkage: another linkage moves the dyes");
      // (the window edge lets a few dyes in or out)
      Check(both.size() > ia.size() * 9 / 10, "linkage: ... the same dyes (ids)", Fmt("%zu of %zu ids shared", both.size(), ia.size()));
      double neg[ISC_LABEL_COUNT] = { 0.6, 1, ISC_MODE_DNA_PAINT, 0.01, 0.05, 1, 1, 0.5, 0, 0, 0, 90, 0, 0, 0, 0, INFINITY, 0, 0, 0 };
      Check(isc_world_set_label(cw, ISC_STRUCT_NPC, neg, ISC_LABEL_COUNT) == -1, "linkage: an infinite length rejected");
      // isc_cell_npcs: the rows, the cap convention, -1 without a cell.
      const int32_t n = isc_cell_npcs(cw, c0.cx, c0.cy, nullptr, 0);
      std::vector<double> rows((size_t)std::max(0, n) * ISC_NPC_STRIDE);
      const int32_t n2 = isc_cell_npcs(cw, c0.cx, c0.cy, rows.data(), n);
      std::vector<Npc> v;
      w.CellNpcs(c0.cx, c0.cy, v);
      std::string d = n > 0 && n2 == n && (size_t)n == v.size() ? "" : Fmt("%d rows (%d filled) vs %zu pores", n, n2, v.size());
      for (int32_t i = 0; d.empty() && i < n; i++) {
         const double* r = &rows[(size_t)i * ISC_NPC_STRIDE];
         if (!(r[0] == v[i].c.x && r[1] == v[i].c.y && r[2] == v[i].c.z && r[3] == v[i].n.x && r[8] == v[i].e1.z))
            d = Fmt("row %d: (%.9g, %.9g, %.9g) vs (%.9g, %.9g, %.9g)", i, r[0], r[1], r[2], v[i].c.x, v[i].c.y, v[i].c.z);
      }
      Same(d, "C ABI: isc_cell_npcs rows = the pores");
      Check(isc_cell_npcs(cw, 999999, 999999, nullptr, 0) == -1, "C ABI: isc_cell_npcs -1 without a cell");
      Check(isc_cell_npcs(cw, c0.cx, c0.cy, nullptr, 5) == -1, "C ABI: isc_cell_npcs -1 for a null buffer with a capacity");
      isc_world_free(cw);
   }
   // Orientation: Fixed about the pore axis (polar angle from n).
   {
      World a = Fresh(seed, p);
      Label l = MakeLabel(LabelMode::DnaPaint, 0.3);
      l.orientation.mode = OrientationMode::Fixed;
      l.orientation.polarDeg = 30;
      poresOnly(a, l);
      std::vector<WorldDye> ds;
      a.SitesInWindow(X0, Y0, X1, Y1, -INF, INF, ds);
      std::vector<Npc> v;
      a.CellNpcs(c0.cx, c0.cy, v);
      long tried = 0;
      std::string bad;
      for (size_t i = 0; i < ds.size() && tried < 200; i++) {
         if (ds[i].cx != c0.cx || ds[i].cy != c0.cy) continue;
         Pt3 dir;
         tried++;
         const Npc* q = nullptr;
         for (const Npc& x : v)
            if (x.cand == ds[i].n) q = &x;
         if (!a.DyeOrientationOf(ds[i], dir) || !q) { if (bad.empty()) bad = DyeStr(ds[i]) + ": no orientation or pore"; continue; }
         const double c = dir.x * q->n.x + dir.y * q->n.y + dir.z * q->n.z;
         if (bad.empty() && std::fabs(c - jsm::cos(30 * jsm::PI / 180)) > 1e-12) bad = DyeStr(ds[i]) + Fmt(": cos %.15g", c);
      }
      Check(tried > 50, "pore dyes: dyes of the cell to orient", Fmt("%ld", tried));
      Same(bad, "pore dyes: Fixed orientation at the polar angle from the pore axis");
   }
}

// ABI 16: the nuclear lamina (structure 2). Points uniform by area on the envelope at round(density x area) per cell,
// none within laminNpcExclNm / 2 of a pore centre, outward unit normals; the pores unchanged by the lamin* params; the
// dyes structure 2 only, within the linkage's reach of their point, tiled, by density3d bit 2, on 8 threads = 1, and
// the other targets' dyes unchanged; isc_cell_lamin = the points.
void Lamin()
{
   const uint32_t seed = 1249;
   Params p = WithPores();
   p.laminDensity = 20;
   p.laminNpcExclNm = 120;   // wider than the default 40 nm, so the exclusion rejects candidates
   World w = Fresh(seed, p);
   std::vector<Cell> cells;
   w.CellsInRect(-30, -30, 30, 30, cells);
   Check(cells.size() >= 2, "lamina: cells near the origin", Fmt("%zu (want 2)", cells.size()));
   if (cells.size() < 2) return;
   cells.resize(2);
   const double rEx = p.laminNpcExclNm / 2 * 1e-3;
   long pts = 0, onCaps = 0, skipped = 0;
   double capExpected = 0, nearest = INF;
   std::string count, membrane, normal, order, inPore;
   for (const Cell& c : cells) {
      std::vector<LaminPoint> v;
      std::vector<Npc> pores;
      if (!w.CellLamin(c.cx, c.cy, v) || !w.CellNpcs(c.cx, c.cy, pores)) { Check(false, "lamina: CellLamin finds the cell"); return; }
      const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
      const long wc = (long)std::floor(p.laminDensity * M.area + 0.5);
      if (count.empty() && (long)v.size() != wc) count = Fmt("cell (%d, %d): %zu points, want %ld", c.cx, c.cy, v.size(), wc);
      pts += (long)v.size();
      capExpected += v.size() * CapAreaFraction(M);
      for (size_t i = 0; i < v.size(); i++) {
         const LaminPoint& q = v[i];
         onCaps += q.piece != 0;
         if (i > 0) skipped += q.cand - v[i - 1].cand - 1;
         if (order.empty() && i > 0 && !(q.cand > v[i - 1].cand)) order = Fmt("cell (%d, %d) point %zu", c.cx, c.cy, i);
         if (membrane.empty() && !OnMembrane(c, q.c, q.n)) membrane = Fmt("cell (%d, %d) point %zu", c.cx, c.cy, i);
         const double nn = q.n.x * q.n.x + q.n.y * q.n.y + q.n.z * q.n.z;
         if (normal.empty() && std::fabs(nn - 1) > 1e-12) normal = Fmt("cell (%d, %d) point %zu: |n|^2 = %.15g", c.cx, c.cy, i, nn);
         for (const Npc& o : pores) {
            const double dx = q.c.x - o.c.x, dy = q.c.y - o.c.y, dz = q.c.z - o.c.z;
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            nearest = std::min(nearest, d);
            if (inPore.empty() && d < rEx) inPore = Fmt("cell (%d, %d) point %zu: %.1f nm from a pore centre", c.cx, c.cy, i, d * 1000);
         }
      }
   }
   Same(count, "lamina: count = round(density x area) per cell");
   Check(pts > 1000, "lamina: enough points to test", Fmt("%ld over 2 cells", pts));
   Same(inPore, Fmt("lamina: none within %.0f nm of a pore centre", rEx * 1000));
   Check(skipped > 0, "lamina: the pore exclusion rejected candidates", "none rejected");
   Same(membrane, "lamina: on the membrane, normal outward");
   Same(normal, "lamina: unit normals");
   Same(order, "lamina: in candidate order");
   Same(CapShare(onCaps, pts, capExpected), "lamina: uniform by area (cap share)");
   const Cell& c0 = cells[0];
   // The same points from a fresh world after other queries; the pores unchanged by the lamin* params; lamin* outside
   // the packing fingerprint; the core's default density 0 = no lamina.
   {
      World w2 = Fresh(seed, p);
      std::vector<WorldDye> junk;
      w2.SitesInWindow(40, 40, 44, 44, -INF, INF, junk);
      std::vector<LaminPoint> a, b;
      w.CellLamin(c0.cx, c0.cy, a);
      w2.CellLamin(c0.cx, c0.cy, b);
      std::string d = a.size() == b.size() && !a.empty() ? "" : Fmt("%zu vs %zu points", a.size(), b.size());
      for (size_t i = 0; d.empty() && i < a.size(); i++)
         if (!(a[i].c.x == b[i].c.x && a[i].c.y == b[i].c.y && a[i].c.z == b[i].c.z && a[i].n.z == b[i].n.z && a[i].cand == b[i].cand))
            d = Fmt("point %zu: (%.9g, %.9g, %.9g) vs (%.9g, %.9g, %.9g)", i, a[i].c.x, a[i].c.y, a[i].c.z, b[i].c.x, b[i].c.y, b[i].c.z);
      Same(d, "lamina: independent of the query history");
      const Params q = WithPores();   // the core's default laminDensity 0
      World w0 = Fresh(seed, q);
      std::vector<Npc> pa, pb;
      w.CellNpcs(c0.cx, c0.cy, pa);
      w0.CellNpcs(c0.cx, c0.cy, pb);
      std::string dp = pa.size() == pb.size() && !pa.empty() ? "" : Fmt("%zu vs %zu pores", pa.size(), pb.size());
      for (size_t i = 0; dp.empty() && i < pa.size(); i++)
         if (!(pa[i].c.x == pb[i].c.x && pa[i].cand == pb[i].cand)) dp = Fmt("pore %zu moved", i);
      Same(dp, "lamina: lamin* params move no pore");
      Check(PackingFingerprint(q) == PackingFingerprint(p), "lamina: lamin* params leave the packing fingerprint");
      std::vector<LaminPoint> none;
      Check(q.laminDensity == 0 && w0.CellLamin(c0.cx, c0.cy, none) && none.empty(), "lamina: the default density 0 = none",
            Fmt("density %g, %zu points", q.laminDensity, none.size()));
   }
   // Lamina dyes over the first nucleus (4 x 4 um), the pores and microtubules labelled thinly too.
   double nx, ny;
   LocalToWorld(c0, c0.nucOffX, c0.nucOffY, nx, ny);
   const double X0 = nx - 2, Y0 = ny - 2, X1 = nx + 2, Y1 = ny + 2;
   Linkage igg;
   igg.binderNm = 19.3; igg.binderFlexNm = 15.3; igg.linkerMinNm = 0.5; igg.linkerMaxNm = 0.5;
   auto labelled = [&](World& a, const Label& l) {
      a.SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::dSTORM, 0.01));
      a.SetLabel(STRUCTURE_NPC, MakeLabel(LabelMode::dSTORM, 0.01));
      a.SetLabel(STRUCTURE_LAMIN, l);
   };
   {
      Label on = MakeLabel(LabelMode::dSTORM, 0.6);
      on.linkage = igg;
      World a = Fresh(seed, p), b = Fresh(seed, p), z = Fresh(seed, p);
      labelled(a, on);
      labelled(b, on);
      labelled(z, MakeLabel(LabelMode::dSTORM, 0));
      std::vector<WorldDye> all, tiles, offDyes, lam, rest;
      a.SitesInWindow(X0, Y0, X1, Y1, -INF, INF, all);
      z.SitesInWindow(X0, Y0, X1, Y1, -INF, INF, offDyes);
      for (const WorldDye& d : all) (d.structure == STRUCTURE_LAMIN ? lam : rest).push_back(d);
      Check(lam.size() > 100, "lamina dyes: in a 4 x 4 um window over a nucleus", Fmt("%zu (want > 100)", lam.size()));
      std::string bad;
      for (const WorldDye& d : lam)
         if (bad.empty() && !(d.mtIndex == -1 && d.k == 0)) bad = DyeStr(d);
      Same(bad, "lamina dyes: one site per point");
      std::string offLam;
      for (const WorldDye& d : offDyes)
         if (offLam.empty() && d.structure == STRUCTURE_LAMIN) offLam = DyeStr(d);
      Same(offLam, "lamina dyes: none with the lamina's label off");
      Same(DiffDyes(rest, offDyes), "lamina dyes: the other targets' dyes unchanged by the lamina's label");
      // Each dye within the linkage's reach of its point.
      std::vector<LaminPoint> v;
      a.CellLamin(c0.cx, c0.cy, v);
      std::unordered_map<int32_t, const LaminPoint*> byCand;
      for (const LaminPoint& q : v) byCand[q.cand] = &q;
      const double reach = LinkageReachNm(igg) * 1e-3 + 1e-9;
      std::string far;
      size_t mine = 0;
      for (const WorldDye& d : lam) {
         if (d.cx != c0.cx || d.cy != c0.cy || !far.empty()) continue;   // (dyes of a neighbouring cell)
         mine++;
         const auto it = byCand.find(d.n);
         if (it == byCand.end()) { far = DyeStr(d) + ": no point of that candidate"; continue; }
         double px, py;
         LocalToWorld(c0, it->second->c.x, it->second->c.y, px, py);
         const double r = std::sqrt((d.x - px) * (d.x - px) + (d.y - py) * (d.y - py) + (d.z - it->second->c.z) * (d.z - it->second->c.z));
         if (r > reach) far = DyeStr(d) + Fmt(": %.1f nm from its point (reach %.1f)", r * 1000, reach * 1000);
      }
      Check(mine > 50, "lamina dyes: dyes of the first cell to check", Fmt("%zu", mine));
      Same(far, "lamina dyes: within the linkage's reach of their point");
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++) b.SitesInWindow(X0 + 2 * i, Y0 + 2 * j, X0 + 2 * (i + 1), Y0 + 2 * (j + 1), -INF, INF, tiles);
      Same(DiffDyes(Sorted(all), Sorted(tiles)), "lamina dyes: 2 x 2 tiles = the whole window");
      std::vector<float> g(4 * 4);
      const long n1 = a.Density3dInWindow(X0, Y0, X1, Y1, -INF, INF, 4, 4, 1, 1u << STRUCTURE_LAMIN, g.data());
      Check(n1 == (long)lam.size(), "lamina dyes: density3d bit 2 counts the lamina dyes alone", Fmt("%ld vs %zu", n1, lam.size()));
      World off = Fresh(seed, WithPores());
      off.SetLabel(STRUCTURE_LAMIN, MakeLabel(LabelMode::dSTORM, 0.6));
      std::vector<WorldDye> od;
      off.SitesInWindow(X0, Y0, X1, Y1, -INF, INF, od);
      std::string any;
      for (const WorldDye& d : od)
         if (any.empty() && d.structure == STRUCTURE_LAMIN) any = DyeStr(d);
      Same(any, "lamina dyes: none at density 0, even labelled");
   }
   if (kThreads)
      for (LabelMode mode : { LabelMode::DnaPaint, LabelMode::dSTORM }) {
         Kinetics k;
         k.activationRatePerSec = 0.05; k.onSec = 0.05; k.offSec = 0.5; k.bleachProb = 0.5;
         World t1 = Fresh(seed, p), t8 = Fresh(seed, p);
         for (World* t : { &t1, &t8 }) {
            t->SetLabel(STRUCTURE_MT, MakeLabel(LabelMode::dSTORM, 0.02, k));
            t->SetLabel(STRUCTURE_LAMIN, MakeLabel(mode, 0.6, k));
         }
         std::vector<WorldEvent> e1, e8;
         SetWorldThreads(1);
         t1.EventsInWindow(X0, Y0, X1, Y1, -INF, INF, 20, 25, e1);
         SetWorldThreads(8);
         t8.EventsInWindow(X0, Y0, X1, Y1, -INF, INF, 20, 25, e8);
         SetWorldThreads(0);
         size_t lam = 0;
         for (const WorldEvent& e : e1) lam += e.structure == STRUCTURE_LAMIN;
         const char* name = mode == LabelMode::DnaPaint ? "DNA-PAINT" : "dSTORM";
         Check(lam > 0, Fmt("lamina dyes, %s: lamina blinks in [20, 25) s", name), "none");
         Same(DiffOrdered(e1, e8), Fmt("lamina dyes, %s: 8 threads = 1 thread", name));
      }
   else
      Skip("lamina dyes: 8 threads = 1 thread", "no threads in WASM");
   // isc_cell_lamin: the rows, the cap convention, -1 without a cell.
   {
      IscParams* ip = AbiParams({ { "npcDensity", p.npcDensity }, { "laminDensity", p.laminDensity }, { "laminNpcExclNm", p.laminNpcExclNm } });
      IscWorld* cw = isc_world_new(seed, ip);
      isc_params_free(ip);
      const int32_t n = isc_cell_lamin(cw, c0.cx, c0.cy, nullptr, 0);
      std::vector<double> rows((size_t)std::max(0, n) * ISC_LAMIN_STRIDE);
      const int32_t n2 = isc_cell_lamin(cw, c0.cx, c0.cy, rows.data(), n);
      std::vector<LaminPoint> v;
      w.CellLamin(c0.cx, c0.cy, v);
      std::string d = n > 0 && n2 == n && (size_t)n == v.size() ? "" : Fmt("%d rows (%d filled) vs %zu points", n, n2, v.size());
      for (int32_t i = 0; d.empty() && i < n; i++) {
         const double* r = &rows[(size_t)i * ISC_LAMIN_STRIDE];
         if (!(r[0] == v[i].c.x && r[1] == v[i].c.y && r[2] == v[i].c.z && r[3] == v[i].n.x && r[5] == v[i].n.z))
            d = Fmt("row %d: (%.9g, %.9g, %.9g) vs (%.9g, %.9g, %.9g)", i, r[0], r[1], r[2], v[i].c.x, v[i].c.y, v[i].c.z);
      }
      Same(d, "C ABI: isc_cell_lamin rows = the points");
      Check(isc_cell_lamin(cw, 999999, 999999, nullptr, 0) == -1, "C ABI: isc_cell_lamin -1 without a cell");
      Check(isc_cell_lamin(cw, c0.cx, c0.cy, nullptr, 5) == -1, "C ABI: isc_cell_lamin -1 for a null buffer with a capacity");
      isc_world_free(cw);
   }
}


// ABI 14: the clock map (clockmap.h). A uniform map is the ABI 11 queries bit for bit and in order; a shaped map is
// every dye on its own clock (a brute force per dye); the answers do not depend on the caches, the query history or
// the threads; a pose change rebuilds no schedule; the C ABI reads the flat buffer and refuses bad ones.
namespace clockmap_checks {

ClockMap MapWith(const StructClock& mt, const Kinetics& other)
{
   ClockMap m;
   m.s.assign(STRUCTURE_COUNT, StructClock());
   for (StructClock& c : m.s) { c.hist.tStart = { 0.0 }; c.hist.kin = { other }; }
   m.s[STRUCTURE_MT] = mt;
   return m;
}

KineticsHistory Hist(std::vector<double> t, std::vector<Kinetics> k)
{
   KineticsHistory h;
   h.tStart = std::move(t);
   h.kin = std::move(k);
   return h;
}

ClockTerm Term(int grid, int depth, double scale, double sx = 0, double sy = 0, double zp = 0, double ox = 0, double oy = 0)
{
   ClockTerm t;
   t.grid = grid; t.depth = depth; t.scale = scale; t.sx = sx; t.sy = sy; t.zp = zp; t.ox = ox; t.oy = oy;
   return t;
}

// A wavy field with a step (the soft and the hard parts of a beam), on 0.25 um nodes around the window.
ClockGrid Wavy(double x0, double y0, double x1, double y1, double phase, double step)
{
   ClockGrid g;
   g.h = 0.25;
   g.x0 = x0 - 12;
   g.y0 = y0 - 12;
   g.nx = (int32_t)((x1 - x0 + 24) / g.h) + 1;
   g.ny = (int32_t)((y1 - y0 + 24) / g.h) + 1;
   const double xm = (x0 + x1) / 2;
   for (int32_t j = 0; j < g.ny; j++)
      for (int32_t i = 0; i < g.nx; i++) {
         const double u = g.x0 + i * g.h, v = g.y0 + j * g.h;
         g.v.push_back(1 + 0.8 * std::sin(1.3 * u + phase) * std::cos(0.7 * v) + (u > xm ? step : 0.0));
      }
   return g;
}

// The flat buffer of isc_world_set_clock_map.
std::vector<double> Encode(const ClockMap& m)
{
   std::vector<double> v = { (double)ISC_CLOCK_MAP_FORMAT, (double)m.grids.size(), (double)m.depths.size() };
   for (const ClockGrid& g : m.grids) {
      v.insert(v.end(), { g.x0, g.y0, g.h, (double)g.nx, (double)g.ny });
      v.insert(v.end(), g.v.begin(), g.v.end());
   }
   for (const ClockDepth& d : m.depths) {
      v.insert(v.end(), { d.z0, d.dz, (double)d.v.size() });
      v.insert(v.end(), d.v.begin(), d.v.end());
   }
   auto field = [&](const ClockField& f) {
      v.push_back((double)f.size());
      for (const ClockTerm& t : f) v.insert(v.end(), { (double)t.grid, (double)t.depth, t.ox, t.oy, t.zp, t.sx, t.sy, t.scale });
   };
   for (const StructClock& c : m.s) {
      v.insert(v.end(), { (double)c.mode, (double)c.hist.Size() });
      for (size_t e = 0; e < c.hist.Size(); e++) {
         const Kinetics& q = c.hist.kin[e];
         v.insert(v.end(), { c.hist.tStart[e], q.activationRatePerSec, q.onSec, q.offSec, q.bleachProb, q.photonCV,
                             q.initialOnSec });
      }
      v.push_back((double)c.bound.size());
      for (const ClockField& b : c.bound) field(b);
      field(c.total);
      v.push_back(c.totalC);
      field(c.rate);
      v.push_back(c.rateC);
      v.push_back(c.tOpen);
      v.push_back((double)c.steps.size());
      for (const ClockStep& st : c.steps) {
         v.insert(v.end(), { st.t0, st.t1, st.c });
         field(st.field);
      }
      field(c.bleachTotal);
      field(c.bleachRate);
   }
   return v;
}

struct Q { double x0, y0, x1, y1, zMin, zMax; };

// Brute force: every dye of the window on its own, its whole schedule (no windows, no caches).
void BruteEvents(uint32_t seed, World& w, const ClockMap& m, const Label& l, const Q& q, double t0, double t1,
                 std::vector<WorldEvent>& out)
{
   const StructClock& c = m.s[STRUCTURE_MT];
   std::vector<WorldDye> dyes;
   w.SitesInWindow(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, dyes);
   std::vector<Blink> b;
   for (const WorldDye& d : dyes) {
      if (d.structure != STRUCTURE_MT) continue;
      const uint32_t h1 = DyeH1(seed, d.cx, d.cy, d.mtIndex);
      b.clear();
      if (c.mode == CLOCK_DOSE) {
         const double tau = m.Eval(c.total, d.x, d.y, d.z, c.totalC), R = m.Eval(c.rate, d.x, d.y, d.z, c.rateC);
         if (!(R > 0)) continue;
         KineticsHistory h = Hist({ 0.0 }, { c.hist.kin[0] });
         for (size_t e = 1; e < c.hist.Size(); e++) {
            const double B = m.Eval(c.bound[e - 1], d.x, d.y, d.z);
            if (B > h.tStart.back()) { h.tStart.push_back(B); h.kin.push_back(c.hist.kin[e]); }
            else h.kin.back() = c.hist.kin[e];
         }
         LabelSchedule(h1, d.k, d.n, l, &b, nullptr, INF, &h);
         const double a0 = tau + R * (t0 - c.tOpen), a1 = tau + R * (t1 - c.tOpen);
         for (const Blink& x : b)
            if (x.tOn < a1 && x.tOff > a0)
               out.push_back({ d.x, d.y, d.z, c.tOpen + (x.tOn - tau) / R, c.tOpen + (x.tOff - tau) / R, x.brightness, 0.0,
                               d.id, (uint8_t)STRUCTURE_MT, STATE_BLINK });
      } else {
         const double E = ActivationDraw(h1, d.k, d.n);
         double H = 0, tA = NAN;
         for (const ClockStep& st : c.steps) {
            const double inc = m.Eval(st.field, d.x, d.y, d.z, st.c);
            if (inc > 0 && E <= H + inc) {
               tA = std::min(st.t1, std::max(st.t0, st.t0 + (st.t1 - st.t0) * ((E - H) / inc)));
               break;
            }
            H = H + inc;
         }
         if (std::isnan(tA)) {
            const double S = m.Eval(c.rate, d.x, d.y, d.z, c.rateC);
            tA = S > 0 ? c.tOpen + (E - H) / S : INF;
         }
         if (!(tA < t1)) continue;
         LabelScheduleActivated(h1, d.k, d.n, l, &c.hist, tA, &b, nullptr);
         for (const Blink& x : b)
            if (x.tOn < t1 && x.tOff > t0)
               out.push_back({ d.x, d.y, d.z, x.tOn, x.tOff, x.brightness, 0.0, d.id, (uint8_t)STRUCTURE_MT, STATE_BLINK });
      }
   }
}

// ... the continuous windows of a dose clock, in the queries' order (dye order).
void BruteWindows(uint32_t seed, World& w, const ClockMap& m, const Label& l, const Q& q, double tMin,
                  std::vector<MapWindow>& out)
{
   const StructClock& c = m.s[STRUCTURE_MT];
   std::vector<WorldDye> dyes;
   w.SitesInWindow(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, dyes);
   std::vector<ContWindow> cw;
   for (const WorldDye& d : dyes) {
      if (d.structure != STRUCTURE_MT) continue;
      const double tau = m.Eval(c.total, d.x, d.y, d.z, c.totalC), R = m.Eval(c.rate, d.x, d.y, d.z, c.rateC);
      KineticsHistory h = Hist({ 0.0 }, { c.hist.kin[0] });
      for (size_t e = 1; e < c.hist.Size(); e++) {
         const double B = m.Eval(c.bound[e - 1], d.x, d.y, d.z);
         if (B > h.tStart.back()) { h.tStart.push_back(B); h.kin.push_back(c.hist.kin[e]); }
         else h.kin.back() = c.hist.kin[e];
      }
      cw.clear();
      LabelSchedule(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, l, nullptr, &cw, INF, &h);
      for (const ContWindow& x : cw) {
         double on, off;
         if (R > 0) {
            on = c.tOpen + (x.tOn - tau) / R;
            off = c.tOpen + (x.tOff - tau) / R;
         } else {
            if (x.tOn > tau || !(x.tOff > tau)) continue;
            on = -INF;
            off = INF;
         }
         if (x.state == STATE_ALWAYS_ON || x.state == STATE_PRE) {   // the photon budget: its hazard reaches the draw
            const double B = m.Eval(c.bleachTotal, d.x, d.y, d.z), r = m.Eval(c.bleachRate, d.x, d.y, d.z);
            if (!c.bleachTotal.empty() || !c.bleachRate.empty())
               off = std::min(off, r > 0 ? c.tOpen + (x.aux - B) / r : x.aux > B ? INF : -INF);
         }
         if (off > tMin)
            out.push_back({ { d.x, d.y, d.z, on, off, 1.0, x.aux, d.id, (uint8_t)STRUCTURE_MT, x.state }, tau, R });
      }
   }
}


std::vector<WorldEvent> Events(World& w, const Q& q, double t0, double t1)
{
   std::vector<WorldEvent> e;
   w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, t0, t1, e);
   return e;
}

std::vector<MapWindow> Windows(World& w, const Q& q, double tMin)
{
   std::vector<MapWindow> e;
   w.ContinuousInMap(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, tMin, e);
   return e;
}

void Run()
{
   const Params p = Cheap();
   const uint32_t seed = 1249;
   Cell c0;
   if (!FirstCell(seed, p, c0)) return;
   const Q q = { c0.x + 4, c0.y - 3, c0.x + 8, c0.y + 1, -5, 50 };
   Kinetics kin;
   kin.activationRatePerSec = 0.05; kin.onSec = 0.05; kin.offSec = 1.0; kin.bleachProb = 0.5; kin.photonCV = 0.5;
   Kinetics pre = kin;
   pre.initialOnSec = 2;
   Kinetics k2 = pre;
   k2.activationRatePerSec *= 3; k2.onSec *= 0.5; k2.initialOnSec = 0.5;
   Label palm = MakeLabel(LabelMode::PALM, 0.05, kin);
   palm.preState = true;
   const Label storm = MakeLabel(LabelMode::dSTORM, 0.05, pre), paint = MakeLabel(LabelMode::DnaPaint, 0.03, kin),
               wf = MakeLabel(LabelMode::WideField, 0.03, kin);
   const double frames[][2] = { { 120, 120.05 }, { 0, 100 }, { 3, 3.05 }, { 60, 62 }, { 29.9, 30.2 } };
   std::string err;
   // Three worlds for every check: a label change keeps the cells and drops the dyes' schedules and map state; Tiny's
   // dye cache holds less than one window.
   World A = Labelled(seed, p, storm), B = Labelled(seed, p, storm), Tiny = Labelled(seed, p, storm, 2, 1000);
   auto use = [&](World& w, const Label& l) {
      w.SetLabel(STRUCTURE_MT, l);
      SetHist(w, {}, {});
      w.ClearClockMap();
      return std::ref(w);
   };

   // ---- Uniform maps = the ABI 11 queries, bit for bit and in order (one and two segments).
   struct U { const char* name; Label l; int mode; std::vector<double> t; std::vector<Kinetics> k; };
   const double Tb = 30.0;
   const U us[] = {
      { "dSTORM, dose", storm, CLOCK_DOSE, { 0.0 }, { pre } },
      { "dSTORM, global", storm, CLOCK_GLOBAL, { 0.0 }, { pre } },
      { "dSTORM, dose, epoch at 30 s", storm, CLOCK_DOSE, { 0.0, Tb }, { pre, k2 } },
      { "dSTORM, global, segment at 30 s", storm, CLOCK_GLOBAL, { 0.0, Tb }, { pre, k2 } },
      { "PALM (pre), activation", palm, CLOCK_ACTIVATION, { 0.0 }, { kin } },
      { "DNA-PAINT, global", paint, CLOCK_GLOBAL, { 0.0 }, { kin } },
      { "DNA-PAINT, global, segment at 30 s", paint, CLOCK_GLOBAL, { 0.0, Tb }, { kin, k2 } },
      { "DNA-PAINT under a dose clock (read on T)", paint, CLOCK_DOSE, { 0.0, Tb }, { kin, k2 } },
      { "WideField, dose", wf, CLOCK_DOSE, { 0.0 }, { kin } },
   };
   for (const U& u : us) {
      const std::string N = std::string("uniform map, ") + u.name + ": ";
      World& a = use(A, u.l);
      World& b = use(B, u.l);
      if (u.t.size() > 1) SetHist(a, u.t, u.k);
      StructClock c;
      c.mode = u.mode;
      c.hist = Hist(u.t, u.k);
      if (u.mode == CLOCK_DOSE) {
         c.rateC = 1;
         for (size_t e = 1; e < u.t.size(); e++) c.bound.push_back({ Term(-1, -1, u.t[e]) });
      }
      if (u.mode == CLOCK_ACTIVATION) c.rateC = u.l.kin.activationRatePerSec;
      Check(b.SetClockMap(MapWith(c, kin), &err), N + "accepted", err);
      std::string diff;
      size_t n = 0, nc = 0;
      for (const auto& f : frames) {
         std::vector<WorldEvent> ea;
         a.EventsInWindow(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, f[0], f[1], ea);
         const std::string d = DiffOrdered(ea, Events(b, q, f[0], f[1]));
         if (diff.empty() && !d.empty()) diff = Fmt("blinks in [%g, %g): ", f[0], f[1]) + d;
         n += ea.size();
      }
      for (double tMin : { -INF, 1.0, 45.0 }) {
         std::vector<WorldEvent> ca, cb;
         a.ContinuousInWindow(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, tMin, ca);
         for (const MapWindow& x : Windows(b, q, tMin)) cb.push_back(x.e);
         const std::string d = DiffOrdered(ca, cb);
         if (diff.empty() && !d.empty()) diff = Fmt("windows from %g: ", tMin) + d;
         nc += ca.size();
      }
      Check(n > 0 || nc > 0, N + "blinks or windows to compare", "none");
      Same(diff, N + "= the ABI 11 queries, bit for bit and in order");
   }
   // PALM with two segments: the activation from a step (the same hazard, other arithmetic): within 1e-9 s.
   {
      Kinetics off = kin;
      off.activationRatePerSec = 0.02;
      World& a = use(A, palm);
      World& b = use(B, palm);
      SetHist(a, { 0.0, Tb }, { kin, off });
      StructClock c;
      c.mode = CLOCK_ACTIVATION;
      c.hist = Hist({ 0.0, Tb }, { kin, off });
      ClockStep st;
      st.t0 = 0; st.t1 = Tb; st.c = kin.activationRatePerSec * Tb;
      c.steps = { st };
      c.tOpen = Tb;
      c.rateC = off.activationRatePerSec;
      b.SetClockMap(MapWith(c, kin));
      double worst = 0;
      std::string count;
      for (const auto& f : frames) {
         std::vector<WorldEvent> ea;
         a.EventsInWindow(q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, f[0], f[1], ea);
         std::vector<WorldEvent> eb = Events(b, q, f[0], f[1]);
         auto byId = [](std::vector<WorldEvent>& v) {
            std::sort(v.begin(), v.end(), [](const WorldEvent& x, const WorldEvent& y) { return std::tie(x.id, x.tOn) < std::tie(y.id, y.tOn); });
         };
         byId(ea);
         byId(eb);
         if (count.empty() && ea.size() != eb.size()) count = Fmt("[%g, %g): %zu vs %zu blinks", f[0], f[1], ea.size(), eb.size());
         for (size_t i = 0; i < std::min(ea.size(), eb.size()); i++)
            worst = std::max({ worst, std::fabs(ea[i].tOn - eb[i].tOn), std::fabs(ea[i].tOff - eb[i].tOff) });
      }
      Same(count, "uniform map, PALM activation step at 30 s: the same blinks as the ABI 11 history");
      Check(worst <= 1e-9, "uniform map, PALM activation step at 30 s: times within 1e-9 s", Fmt("%.3g s", worst));
   }

   // ---- Shaped maps = every dye on its own clock (brute force), rates 0, 0.3, 1, 2.5 and a shaped rate.
   ClockMap shapedBase;
   shapedBase.grids = { Wavy(q.x0, q.y0, q.x1, q.y1, 0.0, 0.5), Wavy(q.x0, q.y0, q.x1, q.y1, 1.7, 0.3),
                        Wavy(q.x0, q.y0, q.x1, q.y1, 2.9, 0.0) };
   ClockDepth depth;
   depth.z0 = 0; depth.dz = 0.5;
   depth.v = { 1.0, 0.85, 0.7, 0.62, 0.5, 0.41, 0.33, 0.3 };
   shapedBase.depths = { depth };
   auto doseMap = [&](double rate, bool shapedRate, double tOpen) {
      StructClock c;
      c.mode = CLOCK_DOSE;
      c.hist = Hist({ 0.0, 25.0 }, { pre, k2 });
      c.bound = { { Term(0, 0, 25.0, 0.7, -0.3, 1.0) } };
      c.total = { Term(0, 0, 25.0, 0.7, -0.3, 1.0), Term(1, -1, tOpen - 25.0, 0, 0, 0, 0.4, -0.2) };
      if (shapedRate) c.rate = { Term(2, 0, rate, 0.7, -0.3, 1.0) };
      else c.rateC = rate;
      c.tOpen = tOpen;
      ClockMap m = shapedBase;
      m.s = MapWith(c, kin).s;
      return m;
   };
   for (const auto& rv : { std::make_pair(0.0, false), std::make_pair(0.3, false), std::make_pair(1.0, false),
                           std::make_pair(2.5, false), std::make_pair(1.0, true) }) {
      const std::string N = Fmt("shaped dose map, rate %s %.1f: ", rv.second ? "field" : "const", rv.first);
      const ClockMap m = doseMap(rv.first, rv.second, 50.0);
      World& w = use(B, storm);
      Check(w.SetClockMap(m, &err), N + "accepted", err);
      std::string diff;
      size_t n = 0, nc = 0;
      for (const auto& f : { std::make_pair(50.0, 50.05), std::make_pair(55.0, 58.0), std::make_pair(20.0, 20.1),
                             std::make_pair(0.0, 120.0), std::make_pair(80.0, 80.05) }) {
         std::vector<WorldEvent> eb;
         BruteEvents(seed, w, m, storm, q, f.first, f.second, eb);
         const std::string d = DiffEvents(Events(w, q, f.first, f.second), eb);
         if (diff.empty() && !d.empty()) diff = Fmt("blinks in [%g, %g): ", f.first, f.second) + d;
         n += eb.size();
      }
      for (double tMin : { -INF, 49.0, 60.0 }) {
         std::vector<MapWindow> cb;
         BruteWindows(seed, w, m, storm, q, tMin, cb);
         const std::string d = DiffWindows(Windows(w, q, tMin), cb);
         if (diff.empty() && !d.empty()) diff = Fmt("windows from %g: ", tMin) + d;
         nc += cb.size();
      }
      Same(diff, N + "= every dye on its own clock (brute force; windows in order)");
      if (rv.first == 0) Check(n == 0, N + "no blinks at rate 0", Fmt("%zu", n));
      else Check(n > 0, N + "blinks to compare", "none");
   }
   // WideField with a photon budget: the windows end at the bleach (its hazard a field by tOpen, a shaped rate after,
   // one rate 0), asked at ever later times on one map (the block's ends in T made once) and again after a map change.
   for (const double rb : { 0.0, 0.04 }) {
      const std::string N = Fmt("WideField dose map with a bleach (rate %.2f): ", rb);
      ClockMap m = doseMap(1.0, true, 50.0);
      StructClock& c = m.s[STRUCTURE_MT];
      c.hist = Hist({ 0.0 }, { kin });
      c.bound.clear();
      c.bleachTotal = { Term(1, 0, 0.012 * 50, 0.7, -0.3, 1.0) };
      if (rb > 0) c.bleachRate = { Term(2, -1, rb) };
      World& w = use(B, wf);
      Check(w.SetClockMap(m, &err), N + "accepted", err);
      std::string diff;
      size_t n52 = 0, n90 = 0;
      for (double tMin : { -INF, 49.0, 52.0, 60.0, 90.0, 55.0 }) {
         std::vector<MapWindow> cb;
         BruteWindows(seed, w, m, wf, q, tMin, cb);
         const std::string d = DiffWindows(Windows(w, q, tMin), cb);
         if (diff.empty() && !d.empty()) diff = Fmt("windows from %g: ", tMin) + d;
         if (tMin == 52.0) n52 = cb.size();
         if (tMin == 90.0) n90 = cb.size();
      }
      Same(diff, N + "= every dye on its own (brute force, in order)");
      ClockMap m2 = m;
      m2.s[STRUCTURE_MT].bleachTotal.push_back(Term(0, -1, 0.3));
      w.SetClockMap(m2);
      std::vector<MapWindow> cb;
      BruteWindows(seed, w, m2, wf, q, 55.0, cb);
      Same(DiffWindows(Windows(w, q, 55.0), cb), N + "after a map change = brute force");
      ClockMap plain = m;
      plain.s[STRUCTURE_MT].bleachTotal.clear();
      plain.s[STRUCTURE_MT].bleachRate.clear();
      std::vector<MapWindow> all;
      BruteWindows(seed, w, plain, wf, q, 52.0, all);
      Check(n52 > 0 && n52 < all.size(), N + "the bleach ends some windows", Fmt("open at 52 s: %zu of %zu", n52, all.size()));
      if (rb == 0) Check(n90 == n52, N + "no further bleaching without a rate", Fmt("open at 52 s %zu, at 90 s %zu", n52, n90));
      else Check(n90 < n52, N + "with a rate, fewer open as time goes on", Fmt("open at 52 s %zu, at 90 s %zu", n52, n90));
   }
   // PALM: three steps (sheared, a depth table, a constant), then a shaped rate.
   auto palmMap = [&](double rateScale) {
      StructClock c;
      c.mode = CLOCK_ACTIVATION;
      c.hist = Hist({ 0.0 }, { kin });
      ClockStep s0, s1, s2;
      s0.t0 = 0; s0.t1 = 10; s0.field = { Term(0, 0, 0.02 * 10, 0.7, -0.3, 1.0) };
      s1.t0 = 10; s1.t1 = 25; s1.c = 1e-3; s1.field = { Term(1, -1, 0.05 * 15) };
      s2.t0 = 25; s2.t1 = 40; s2.field = { Term(2, 0, 0.01 * 15, 0.7, -0.3, 1.0) };
      c.steps = { s0, s1, s2 };
      c.rate = { Term(0, -1, 0.03 * rateScale) };
      c.rateC = 1e-4;
      c.tOpen = 40;
      ClockMap m = shapedBase;
      m.s = MapWith(c, kin).s;
      return m;
   };
   {
      const ClockMap m = palmMap(1.0);
      World& w = use(B, palm);
      Check(w.SetClockMap(m, &err), "activation map: accepted", err);
      std::string diff;
      size_t n = 0;
      auto cmp = [&](const ClockMap& mm, double t0, double t1) {
         std::vector<WorldEvent> eb;
         BruteEvents(seed, w, mm, palm, q, t0, t1, eb);
         const std::string d = DiffEvents(Events(w, q, t0, t1), eb);
         if (diff.empty() && !d.empty()) diff = Fmt("[%g, %g): ", t0, t1) + d;
         n += eb.size();
      };
      for (const auto& f : { std::make_pair(40.0, 40.05), std::make_pair(45.0, 47.0), std::make_pair(12.0, 12.5),
                             std::make_pair(0.0, 100.0), std::make_pair(41.0, 41.05) })
         cmp(m, f.first, f.second);
      Same(diff, "activation map (3 steps): = every dye's own walk");
      // A pose change: a fourth step (the open record closed) and another rate.
      ClockMap m2 = m;
      ClockStep s3;
      s3.t0 = 40; s3.t1 = 46; s3.c = 1e-4 * 6; s3.field = { Term(0, -1, 0.03 * 6) };
      m2.s[STRUCTURE_MT].steps.push_back(s3);
      m2.s[STRUCTURE_MT].rate = { Term(1, -1, 0.04) };
      m2.s[STRUCTURE_MT].tOpen = 46;
      w.SetClockMap(m2);
      diff.clear();
      for (const auto& f : { std::make_pair(46.0, 46.05), std::make_pair(50.0, 52.0) }) cmp(m2, f.first, f.second);
      Same(diff, "activation map, after a pose change (a 4th step): = every dye's own walk");
      Check(n > 0, "activation map: blinks to compare", "none");
   }

   // ---- Caches and query history: a tiny dye cache, another query order = the same answers in the same order.
   {
      const ClockMap m = doseMap(1.0, true, 50.0), pm = palmMap(1.0);
      for (int pass = 0; pass < 2; pass++) {
         World& a = use(A, pass ? palm : storm);
         World& b = use(Tiny, pass ? palm : storm);
         a.SetClockMap(pass ? pm : m);
         b.SetClockMap(pass ? pm : m);
         // b: a different history first (late, long, early).
         for (const auto& f : { std::make_pair(150.0, 150.05), std::make_pair(0.0, 200.0), std::make_pair(1.0, 1.05) })
            Events(b, q, f.first, f.second);
         std::string diff;
         for (const auto& f : { std::make_pair(50.0, 50.05), std::make_pair(52.0, 52.05), std::make_pair(30.0, 31.0),
                                std::make_pair(55.0, 55.05) }) {
            const std::string d = DiffOrdered(Events(a, q, f.first, f.second), Events(b, q, f.first, f.second));
            if (diff.empty() && !d.empty()) diff = Fmt("[%g, %g): ", f.first, f.second) + d;
         }
         if (diff.empty()) diff = DiffWindows(Windows(a, q, pass ? 0.0 : 49.0), Windows(b, q, pass ? 0.0 : 49.0));
         Same(diff, Fmt("map queries (%s): the same answers in the same order whatever the caches and the query history",
                        pass ? "PALM" : "dSTORM"));
      }
      // A pose change at 60 s (the dose map's total and rate move, its bound stays): no schedule rebuilt (the frame
      // before it on the old map; the schedules' horizon follows time, not the pose).
      World& a = use(A, storm);
      a.SetClockMap(m);
      Events(a, q, 59.95, 60.0);
      // (the open record [50, 60) folds into the total, the beam moves by 0.3 um)
      ClockMap moved = m;
      StructClock& mc = moved.s[STRUCTURE_MT];
      ClockTerm folded = mc.rate[0];
      folded.scale *= 10;
      mc.total.push_back(folded);
      mc.rate[0].ox = 0.3;
      mc.tOpen = 60;
      const long s0 = a.Stats().schedulesBuilt;
      a.SetClockMap(moved);
      std::vector<WorldEvent> eb;
      BruteEvents(seed, a, moved, storm, q, 60.0, 60.05, eb);
      Same(DiffEvents(Events(a, q, 60.0, 60.05), eb), "pose change: the moved map = brute force");
      Check(a.Stats().schedulesBuilt == s0, "pose change (the dose clock's present) rebuilds no schedule",
            Fmt("%ld rebuilt", a.Stats().schedulesBuilt - s0));
      // Prefetch changes no answer.
      World& c = use(A, storm);
      World& d = use(B, storm);
      c.SetClockMap(m);
      d.SetClockMap(m);
      c.PrefetchMap(q.x0 - 2, q.y0 - 2, q.x1 + 2, q.y1 + 2, -INF, INF, 50.0, 50.1, 1e9);
      Same(DiffOrdered(Events(c, q, 50.0, 50.05), Events(d, q, 50.0, 50.05)), "PrefetchMap changes no answer");
   }

   // ---- 8 threads = 1 thread.
   if (kThreads) {
      const ClockMap m = doseMap(1.0, true, 50.0), pm = palmMap(1.0);
      for (int pass = 0; pass < 2; pass++) {
         World& a = use(A, pass ? palm : storm);
         World& b = use(B, pass ? palm : storm);
         a.SetClockMap(pass ? pm : m);
         b.SetClockMap(pass ? pm : m);
         std::string diff;
         size_t n = 0;
         for (int f = 0; f < 6 && diff.empty(); f++) {
            const double s = f < 3 ? 0.9 * f : 4 + 0.5 * f;   // moves, then a jump (inside the map's grids)
            const Q r = { q.x0 - 3 + s, q.y0 - 3, q.x1 + 3 + s, q.y1 + 3, 0, 4 };
            const double t0 = pass ? 40 + f : 50 + f * 0.05, t1 = pass ? 41 + f : 50 + (f + 1) * 0.05;
            SetWorldThreads(1);
            const std::vector<WorldEvent> ea = Events(a, r, t0, t1);
            const std::vector<MapWindow> ca = Windows(a, r, 45);
            SetWorldThreads(8);
            const std::vector<WorldEvent> eb = Events(b, r, t0, t1);
            const std::vector<MapWindow> cb = Windows(b, r, 45);
            SetWorldThreads(0);
            std::string d = DiffOrdered(ea, eb);
            if (d.empty()) d = DiffWindows(ca, cb);
            if (!d.empty()) diff = Fmt("frame %d: ", f) + d;
            n += ea.size();
         }
         const char* name = pass ? "PALM" : "dSTORM";
         Same(diff, Fmt("map queries (%s): 8 threads = 1 thread, the same answers in the same order", name));
         Check(n > 20, Fmt("map queries (%s): blinks over 6 frames", name), Fmt("%zu (want > 20)", n));
      }
   } else
      Skip("map queries: 8 threads = 1 thread", "no threads in WASM");

   // ---- The C ABI: the flat buffer read back, bad buffers refused (and nothing changes).
   {
      const ClockMap m = doseMap(1.0, true, 50.0);
      IscParams* pa = AbiParams();
      IscWorld* cw = isc_world_new(seed, pa);
      isc_params_free(pa);
      const double lab[] = { storm.density, storm.fluorescentFraction, (double)(int)storm.mode, pre.activationRatePerSec,
                             pre.onSec, pre.offSec, pre.bleachProb, pre.photonCV, pre.initialOnSec };
      isc_world_set_label(cw, 0, lab, 9);
      std::vector<double> v = Encode(m);
      Check(isc_world_set_clock_map(cw, v.data(), (int32_t)v.size()) == 0, "isc_world_set_clock_map accepts the flat buffer");
      World& w = use(A, storm);
      w.SetClockMap(m);
      auto rowsOf = [&](double t0, double t1) {
         const int32_t nr = isc_events_in_map(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, t0, t1, nullptr, 0);
         std::vector<double> o((size_t)std::max(0, nr) * ISC_EVENT_STRIDE);
         isc_events_in_map(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, t0, t1, o.data(), nr);
         return o;
      };
      auto rowsExpected = [&](double t0, double t1) {
         std::vector<double> o;
         for (const WorldEvent& e : Events(w, q, t0, t1))
            o.insert(o.end(), { e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, (double)e.id, (double)e.structure,
                                (double)e.state, e.aux });
         return o;
      };
      auto firstDiff = [](const std::vector<double>& a, const std::vector<double>& b) -> std::string {
         if (a.size() != b.size()) return Fmt("%zu vs %zu values", a.size(), b.size());
         for (size_t i = 0; i < a.size(); i++)
            if (!(a[i] == b[i])) return Fmt("value %zu: %.17g vs %.17g", i, a[i], b[i]);
         return "";
      };
      const std::vector<double> before = rowsOf(50, 51);
      Check(!before.empty(), "C ABI: isc_events_in_map has rows", "none");
      Same(firstDiff(before, rowsExpected(50, 51)), "C ABI: isc_events_in_map rows = World::EventsInMap");
      {
         const int32_t nw = isc_continuous_in_map(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, 45, nullptr, 0);
         std::vector<double> o((size_t)std::max(0, nw) * ISC_MAP_WINDOW_STRIDE);
         isc_continuous_in_map(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, 45, o.data(), nw);
         std::vector<double> x;
         for (const MapWindow& mw : Windows(w, q, 45))
            x.insert(x.end(), { mw.e.x, mw.e.y, mw.e.z, mw.e.tOn, mw.e.tOff, mw.e.brightness, (double)mw.e.id,
                                (double)mw.e.structure, (double)mw.e.state, mw.e.aux, mw.clock, mw.rate });
         Same(firstDiff(o, x), "C ABI: isc_continuous_in_map rows = World::ContinuousInMap");
      }
      // Bad buffers: each refused, the map unchanged.
      std::vector<std::pair<const char*, std::vector<double>>> bad;
      auto bent = [&](const char* name, size_t at, double val) { std::vector<double> b = v; b[at] = val; bad.push_back({ name, b }); };
      auto enc = [&](const char* name, const ClockMap& x) { bad.push_back({ name, Encode(x) }); };
      bent("another format", 0, ISC_CLOCK_MAP_FORMAT + 1);
      bent("more grids than given", 1, 99);
      bent("a grid's nx 0", 6, 0);
      bent("a NaN grid value", 8, NAN);
      bad.push_back({ "a short buffer", std::vector<double>(v.begin(), v.end() - 1) });
      { std::vector<double> b = v; b.push_back(0); bad.push_back({ "a trailing value", b }); }
      { ClockMap x = m; x.s[STRUCTURE_MT].bound.clear(); enc("a dose clock with epochs but no bound", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].hist.tStart[1] = 0; enc("epoch starts not increasing", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].total[0].grid = 7; enc("a total term naming a missing grid", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].bleachRate = { Term(9, -1, 1.0) }; enc("a bleach term naming a missing grid", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].steps.push_back(ClockStep{ 0, 1, 0, {} }); enc("steps on a dose clock", x); }
      { ClockMap x = palmMap(1.0); x.s[STRUCTURE_MT].steps[1].t0 = 11; enc("a gap in an activation clock's steps", x); }
      { ClockMap x = palmMap(1.0); x.s[STRUCTURE_MT].tOpen = 41; enc("steps not ending at tOpen", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].hist.kin[0].onSec = 0; enc("bad kinetics", x); }
      { ClockMap x = m; x.s[STRUCTURE_MT].tOpen = -1; enc("a negative tOpen", x); }
      for (const auto& b : bad)
         Check(isc_world_set_clock_map(cw, b.second.data(), (int32_t)b.second.size()) == -1,
               std::string("isc_world_set_clock_map refuses ") + b.first);
      Same(firstDiff(rowsOf(50, 51), before), "C ABI: the refusals left the map as it was");
      // n = 0 removes the map: the ABI 11 answer.
      isc_world_set_clock_map(cw, nullptr, 0);
      const int32_t n11 = isc_events_in_window(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, 50, 51, nullptr, 0);
      std::vector<double> o11((size_t)std::max(0, n11) * ISC_EVENT_STRIDE);
      isc_events_in_window(cw, q.x0, q.y0, q.x1, q.y1, q.zMin, q.zMax, 50, 51, o11.data(), n11);
      Same(firstDiff(rowsOf(50, 51), o11), "C ABI: n = 0 removes the map (the ABI 11 answer)");
      isc_world_free(cw);
   }
}

} // namespace clockmap_checks

void ClockMapChecks() { clockmap_checks::Run(); }

struct Section {
   const char* name;
   std::function<void()> run;
};

// One entry per ctest test (tests/parity/CMakeLists.txt keeps the same list): `isc_world_tests <section>` runs one, no
// argument runs all. Each prints its time.
static const std::vector<Section>& Sections()
{
   static const std::vector<Section> s = {
      { "basics", [] { NoContraction(); BlockStoreTest(); } },
      { "determinism", [] { Determinism(); PackingOff(); } },
      { "dye_statistics", [] { DyeStatistics(); KineticsStats(); PersistentSites(); } },
      { "events", [] {
           Kinetics k;
           k.activationRatePerSec = 1.0 / 30; k.onSec = 0.03; k.offSec = 0.3; k.bleachProb = 0.25; k.photonCV = 0.3;
           EventQuery(MakeLabel(LabelMode::PALM, 0.1, k), "event query, PALM 10 %");
           Kinetics kp = k;
           kp.activationRatePerSec = 0.02;
           EventQuery(MakeLabel(LabelMode::DnaPaint, 0.3, kp), "event query, DNA-PAINT 30 %");
           Kinetics ks = k;
           ks.initialOnSec = 1.5;
           EventQuery(MakeLabel(LabelMode::dSTORM, 0.1, ks), "event query, dSTORM 10 % with an initial ON");
        } },
      { "caches", [] {
           CacheUnderLoad(LabelMode::PALM, "caches under load, PALM");
           CacheUnderLoad(LabelMode::DnaPaint, "caches under load, DNA-PAINT");
        } },
      { "label_model", LabelModel },
      { "kinetics_history", KineticsHistoryChecks },
      { "threads", [] {
           Threads(LabelMode::PALM, 0.3, "threads, PALM 30 %");
           Threads(LabelMode::DnaPaint, 0.3, "threads, DNA-PAINT 30 %");
        } },
      { "c_api", CApi },
      { "block_injection", BlockInjection },
      { "density3d", Density3d },
      { "optical_volume", OpticalVolume },
      { "edge_and_height", EdgeAndHeight },
      { "clock_map", ClockMapChecks },
      { "origami", Origami },
      { "npcs", Npcs },
      { "lamin", Lamin },
      { "fiducials", Fiducials },
   };
   return s;
}

int main(int argc, char** argv)
{
   std::setvbuf(stdout, nullptr, _IONBF, 0); // progress shows as it runs (ctest, pipes)
   const char* only = argc > 1 ? argv[1] : nullptr;
   bool found = false;
   const auto all = std::chrono::steady_clock::now();
   for (const Section& sec : Sections()) {
      if (only && std::strcmp(only, sec.name) != 0) continue;
      found = true;
      std::printf("[%s]\n", sec.name);
      const auto t0 = std::chrono::steady_clock::now();
      sec.run();
      std::printf("  [%s] %.0f ms\n", sec.name, Ms(t0));
   }
   if (!found) {
      std::printf("unknown section '%s'; sections:", only);
      for (const Section& sec : Sections()) std::printf(" %s", sec.name);
      std::printf("\n");
      return 2;
   }
   if (!only) std::printf("\nall sections: %.0f ms\n", Ms(all));
   if (g_failures) {
      std::printf("\n%d check(s) FAILED:\n", g_failures);
      for (const std::string& f : g_failed) std::printf("  %s\n", f.c_str());
   } else
      std::printf("\nall world checks passed\n");
   return g_failures ? 1 : 0;
}
