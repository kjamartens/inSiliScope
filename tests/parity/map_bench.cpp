// The clock map's speed check (ABI 14, spec/ALGORITHM.md "Exact light history"; not a test: tools/core_check.sh does
// not run it, the checks it prints are its own). One 25.6 um field of microtubules under a sheared (HILO-like) flat-top
// beam that moved, with two kinetics epochs (a 405-ratio step), read
//   (b) as one uniform EventsInWindow (today's cost with no shaped light),
//   (a) as today's clock regions (one SetKineticsHistory + EventsInWindow per region, ~2600),
//   and through the map: uniform (= (b) bit for bit), shaped (= a brute-force per-dye answer), a pose change, a new
//   chapter, PALM on an activation clock, the continuous windows.
//
//   isc_map_bench [seed] [--no-regions] [--dense]
// The labelling is the library's typical one (data/dyes/library.json: microtubules dSTORM 3 %, PALM 25 %); --dense
// labels 10 % for dSTORM (about 3x the blinks per frame).
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "clockmap.h"
#include "dyes.h"
#include "parallel.h"
#include "params.h"
#include "world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using Clk = std::chrono::steady_clock;
double Ms(Clk::time_point t0) { return std::chrono::duration<double, std::milli>(Clk::now() - t0).count(); }

const double FRAME = 0.05;   // s
const double W = 25.6;       // um: 256 px at 100 nm

struct Box { double x0, y0, x1, y1, z0, z1; };

Kinetics Storm()
{
   Kinetics k;
   k.activationRatePerSec = 0.02; k.onSec = 0.03; k.offSec = 1.0; k.bleachProb = 0.05; k.photonCV = 0.5;
   k.initialOnSec = 2.0;
   return k;
}

Label MakeLabel(LabelMode mode, double density, const Kinetics& kin, bool pre = false)
{
   Label l;
   l.mode = mode;
   l.density = density;
   l.kin = kin;
   l.preState = pre;
   return l;
}

World Labelled(uint32_t seed, const Params& p, const Label& l)
{
   World w(seed, p);
   w.SetLabel(STRUCTURE_MT, l);
   return w;
}

// The same events in the same order, every field.
bool Identical(const std::vector<WorldEvent>& a, const std::vector<WorldEvent>& b)
{
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++) {
      const WorldEvent &x = a[i], &y = b[i];
      if (x.x != y.x || x.y != y.y || x.z != y.z || x.tOn != y.tOn || x.tOff != y.tOff || x.brightness != y.brightness ||
          x.aux != y.aux || x.id != y.id || x.structure != y.structure || x.state != y.state)
         return false;
   }
   return true;
}

// The same events in any order.
bool SameSet(std::vector<WorldEvent> a, std::vector<WorldEvent> b)
{
   auto key = [](const WorldEvent& e) { return std::make_tuple(e.id, e.tOn, e.tOff, e.brightness, e.x, e.y, e.z, e.state); };
   auto lt = [&](const WorldEvent& x, const WorldEvent& y) { return key(x) < key(y); };
   std::sort(a.begin(), a.end(), lt);
   std::sort(b.begin(), b.end(), lt);
   if (a.size() != b.size()) return false;
   for (size_t i = 0; i < a.size(); i++)
      if (key(a[i]) != key(b[i])) return false;
   return true;
}

// A flat-top beam footprint at the coverslip (super-Gaussian, 1/e^2 half width w), on a grid of h in sheared tile
// coordinates around (cx, cy).
ClockGrid Footprint(double cx, double cy, double bx, double by, double w, double scale, double half, double h)
{
   ClockGrid g;
   g.h = h;
   g.x0 = cx - half;
   g.y0 = cy - half;
   g.nx = g.ny = (int32_t)std::ceil(2 * half / h) + 1;
   g.v.resize((size_t)g.nx * (size_t)g.ny);
   for (int32_t j = 0; j < g.ny; j++)
      for (int32_t i = 0; i < g.nx; i++) {
         const double u = g.x0 + i * h - bx, v = g.y0 + j * h - by;
         const double r2 = (u * u + v * v) / (w * w);
         g.v[(size_t)j * (size_t)g.nx + (size_t)i] = scale * std::exp(-2 * r2 * r2 * r2);
      }
   return g;
}

void AddInto(ClockGrid& a, const ClockGrid& b)
{
   for (size_t i = 0; i < a.v.size(); i++) a.v[i] += b.v[i];
}

ClockTerm GridTerm(int grid, double shear)
{
   ClockTerm t;
   t.grid = grid;
   t.sx = shear;
   return t;
}

KineticsHistory Hist(std::vector<double> tStart, std::vector<Kinetics> kin)
{
   KineticsHistory h;
   h.tStart = std::move(tStart);
   h.kin = std::move(kin);
   return h;
}

ClockMap UniformMap(const StructClock& c)
{
   ClockMap m;
   m.s.assign(STRUCTURE_COUNT, StructClock());
   for (StructClock& x : m.s) x.hist = Hist({ 0.0 }, { Storm() });
   m.s[STRUCTURE_MT] = c;
   return m;
}

// The brute-force answer of a dose map: every dye of the box on its own, LabelSchedule unwindowed.
void BruteDose(uint32_t seed, World& w, const ClockMap& m, const Label& l, const Box& q, double t0, double t1,
               std::vector<WorldEvent>& out)
{
   const StructClock& c = m.s[STRUCTURE_MT];
   std::vector<WorldDye> dyes;
   w.SitesInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, dyes);
   std::vector<Blink> b;
   for (const WorldDye& d : dyes) {
      if (d.structure != STRUCTURE_MT) continue;
      const double tau = m.Eval(c.total, d.x, d.y, d.z, c.totalC), R = m.Eval(c.rate, d.x, d.y, d.z, c.rateC);
      if (!(R > 0)) continue;
      KineticsHistory h = Hist({ 0.0 }, { c.hist.kin[0] });
      for (size_t e = 1; e < c.hist.Size(); e++) {
         const double B = m.Eval(c.bound[e - 1], d.x, d.y, d.z);
         if (B > h.tStart.back()) { h.tStart.push_back(B); h.kin.push_back(c.hist.kin[e]); }
         else h.kin.back() = c.hist.kin[e];
      }
      b.clear();
      LabelSchedule(DyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, l, &b, nullptr, INFINITY, &h);
      const double a0 = tau + R * (t0 - c.tOpen), a1 = tau + R * (t1 - c.tOpen);
      for (const Blink& x : b)
         if (x.tOn < a1 && x.tOff > a0)
            out.push_back({ d.x, d.y, d.z, c.tOpen + (x.tOn - tau) / R, c.tOpen + (x.tOff - tau) / R, x.brightness, 0.0,
                            d.id, (uint8_t)STRUCTURE_MT, STATE_BLINK });
   }
}

// ... of an activation map: each dye's hazard walked over the steps, then its blinks from its activation.
void BruteActivation(uint32_t seed, World& w, const ClockMap& m, const Label& l, const Box& q, double t0, double t1,
                     std::vector<WorldEvent>& out)
{
   const StructClock& c = m.s[STRUCTURE_MT];
   std::vector<WorldDye> dyes;
   w.SitesInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, dyes);
   std::vector<Blink> b;
   for (const WorldDye& d : dyes) {
      if (d.structure != STRUCTURE_MT) continue;
      const uint32_t h1 = DyeH1(seed, d.cx, d.cy, d.mtIndex);
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
         tA = S > 0 ? c.tOpen + (E - H) / S : INFINITY;
      }
      if (!(tA < t1)) continue;
      b.clear();
      LabelScheduleActivated(h1, d.k, d.n, l, &c.hist, tA, &b, nullptr);
      for (const Blink& x : b)
         if (x.tOn < t1 && x.tOff > t0)
            out.push_back({ d.x, d.y, d.z, x.tOn, x.tOff, x.brightness, 0.0, d.id, (uint8_t)STRUCTURE_MT, STATE_BLINK });
   }
}

struct Timed { double first, mean, max; size_t events; long schedules; };

// n consecutive frames from t (the first one apart: the cold query), each a fresh vector.
template <class Q>
Timed Frames(World& w, double t, int n, Q query)
{
   Timed r = { 0, 0, 0, 0, 0 };
   const long s0 = w.Stats().schedulesBuilt;
   for (int f = 0; f < n; f++) {
      std::vector<WorldEvent> ev;
      const auto c0 = Clk::now();
      query(ev, t + f * FRAME, t + (f + 1) * FRAME);
      const double ms = Ms(c0);
      if (f == 0) { r.first = ms; r.events = ev.size(); continue; }
      r.mean += ms / (n - 1);
      r.max = std::max(r.max, ms);
   }
   r.schedules = w.Stats().schedulesBuilt - s0;
   return r;
}

void Line(const char* what, const Timed& t)
{
   std::printf("  %-44s first %8.2f ms   then %7.2f ms (max %7.2f)   %6zu events, %ld schedules\n", what, t.first, t.mean,
               t.max, t.events, t.schedules);
}

} // namespace

int main(int argc, char** argv)
{
   uint32_t seed = 1249;
   bool regions = true;
   double stormPct = 3;
   for (int i = 1; i < argc; i++) {
      if (!std::strcmp(argv[i], "--no-regions")) regions = false;
      else if (!std::strcmp(argv[i], "--dense")) stormPct = 10;
      else seed = (uint32_t)std::strtoul(argv[i], nullptr, 10);
   }
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   const auto tAll = Clk::now();
   Params p;
   p.npcDensity = 0;   // the microtubules alone
   std::printf("map bench: seed %u, %.1f um field, dSTORM %.0f %% / PALM 25 %% of the sites, threads %d\n", seed, W,
               stormPct, WorldThreads());

   // The field: centred on a cell near the origin, every height.
   std::vector<Cell> near;
   {
      World probe(seed, p);
      probe.CellsInRect(-30, -30, 30, 30, near);
   }
   if (near.empty()) { Check(false, "cells near the origin"); return 1; }
   const double cx = near[0].x, cy = near[0].y;
   const Box q = { cx - W / 2, cy - W / 2, cx + W / 2, cy + W / 2, -1.0, 12.0 };
   const Kinetics k1 = Storm();
   Kinetics k2 = k1;
   k2.activationRatePerSec *= 3;   // the 405-ratio step of chapter 2
   const Label storm = MakeLabel(LabelMode::dSTORM, stormPct / 100, k1);
   const double T = 60.0;   // the frames' sample time
   const auto dyesIn = [&](World& w) {
      std::vector<WorldDye> d;
      w.SitesInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, d);
      return d.size();
   };

   // ---- (b) today, no shaped light: one uniform EventsInWindow per frame, and the uniform map = it bit for bit.
   std::printf("\n(b) one uniform query, and the uniform map\n");
   Timed tb;
   {
      World w = Labelled(seed, p, storm);
      auto c0 = Clk::now();
      const size_t n = dyesIn(w);
      std::printf("  field built (cells, dyes): %.0f ms, %zu dyes\n", Ms(c0), n);
      tb = Frames(w, T, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInWindow", tb);

      // Uniform maps of each class against the ABI 11 queries (fresh worlds: no shared cache).
      struct U { const char* name; Label l; int mode; };
      Label palm = MakeLabel(LabelMode::PALM, 0.1, k1, true);
      Label paint = MakeLabel(LabelMode::DnaPaint, 0.3, k1);
      Label wf = MakeLabel(LabelMode::WideField, 0.1, k1);
      const U us[] = { { "dSTORM, dose clock", storm, CLOCK_DOSE }, { "dSTORM, global clock", storm, CLOCK_GLOBAL },
                       { "PALM (pre), activation clock", palm, CLOCK_ACTIVATION },
                       { "DNA-PAINT, global clock", paint, CLOCK_GLOBAL }, { "WideField, dose clock", wf, CLOCK_DOSE } };
      for (const U& u : us) {
         World a = Labelled(seed, p, u.l), b = Labelled(seed, p, u.l);
         StructClock c;
         c.mode = u.mode;
         c.hist = Hist({ 0.0 }, { u.l.kin });
         if (u.mode == CLOCK_DOSE) c.rateC = 1;
         if (u.mode == CLOCK_ACTIVATION) c.rateC = u.l.kin.activationRatePerSec;
         std::string err;
         const bool set = b.SetClockMap(UniformMap(c), &err);
         bool same = set;
         size_t n = 0;
         for (double t0 : { 0.0, 7.3, T, T + 0.05, 2.0 }) {
            std::vector<WorldEvent> ea, eb;
            a.EventsInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, t0, t0 + FRAME, ea);
            b.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, t0, t0 + FRAME, eb);
            same = same && Identical(ea, eb);
            n += ea.size();
         }
         std::vector<WorldEvent> ca;
         std::vector<MapWindow> cb;
         a.ContinuousInWindow(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, 0.0, ca);
         b.ContinuousInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, 0.0, cb);
         std::vector<WorldEvent> cbe;
         for (const MapWindow& x : cb) cbe.push_back(x.e);
         const bool sameC = Identical(ca, cbe);
         std::printf("      %-30s %6zu blinks in 5 frames %s, %6zu windows %s %s\n", u.name, n,
                     same ? "identical" : "DIFFERENT", ca.size(), sameC ? "identical" : "DIFFERENT", err.c_str());
         Check(same && sameC, "a uniform map = the ABI 11 queries, bit for bit and in order");
      }
   }

   // ---- The shaped map: a sheared flat-top beam (HILO: u = x - 1.5 z), chapter 1 [0, 25) at pose P0, chapter 2
   // (405 x 3) [25, 40) at P1 and [40, 50) at P2, the open record from 50 at P3; dose per second 1 at the beam's peak.
   const double shear = 1.5, wBeam = 14.0, half = W / 2 + 12 * shear + 6, h = 0.25;
   auto beam = [&](double ox, double oy, double scale) { return Footprint(cx, cy, cx + ox, cy + oy, wBeam, scale, half, h); };
   ClockMap shaped;
   shaped.s.assign(STRUCTURE_COUNT, StructClock());
   for (StructClock& x : shaped.s) x.hist = Hist({ 0.0 }, { k1 });
   {
      ClockGrid ch2 = beam(3, 1, 15);
      AddInto(ch2, beam(-2, 2, 10));
      shaped.grids = { beam(0, 0, 25), ch2, beam(0, 0, 1) };
      StructClock& c = shaped.s[STRUCTURE_MT];
      c.mode = CLOCK_DOSE;
      c.hist = Hist({ 0.0, 25.0 }, { k1, k2 });
      c.bound = { { GridTerm(0, shear) } };
      c.total = { GridTerm(0, shear), GridTerm(1, shear) };
      c.rate = { GridTerm(2, shear) };
      c.tOpen = 50;
   }
   // The pose change at T: the open record [50, 60) folds into chapter 2, the beam moves by (1, -1) um.
   ClockMap moved = shaped;
   {
      AddInto(moved.grids[1], beam(0, 0, 10));
      moved.grids[2] = beam(1, -1, 1);
      moved.s[STRUCTURE_MT].tOpen = T;
   }
   // A new chapter at T (405 x 3 again: a third epoch from the dose at T).
   ClockMap chapter = moved;
   {
      Kinetics k3 = k2;
      k3.activationRatePerSec *= 3;
      StructClock& c = chapter.s[STRUCTURE_MT];
      c.hist = Hist({ 0.0, 25.0, T }, { k1, k2, k3 });
      c.bound = { { GridTerm(0, shear) }, { GridTerm(0, shear), GridTerm(1, shear) } };
   }

   std::printf("\n(map) the shaped dose clock: dSTORM, two epochs, a sheared flat-top beam that moved\n");
   Timed tm, tmPose, tmChap, tcOn = { 0, 0, 0, 0, 0 };
   {
      World w = Labelled(seed, p, storm);
      dyesIn(w);
      std::string err;
      Check(w.SetClockMap(shaped, &err), "SetClockMap takes the shaped map");
      if (!err.empty()) std::printf("      %s\n", err.c_str());
      const long d0 = w.Stats().mapDyes;
      tm = Frames(w, 50.0, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInMap, frames from tOpen", tm);
      std::printf("      dye clocks read: %ld (once per present)\n", w.Stats().mapDyes - d0);
      // Against the brute force, at a few frames.
      bool same = true;
      size_t n = 0;
      for (double t0 : { 50.0, 55.5, 59.95 }) {
         std::vector<WorldEvent> em, eb;
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, t0, t0 + FRAME, em);
         BruteDose(seed, w, shaped, storm, q, t0, t0 + FRAME, eb);
         same = same && SameSet(em, eb);
         n += em.size();
      }
      std::printf("      %zu blinks in 3 frames vs the brute force: %s\n", n, same ? "the same" : "DIFFERENT");
      Check(same, "the shaped dose map = every dye on its own clock (brute force)");

      // The pose change: no schedule rebuilt.
      w.SetClockMap(moved, &err);
      tmPose = Frames(w, T, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInMap, after a pose change", tmPose);
      Check(tmPose.schedules == 0, "a pose change (dose clock) rebuilds no schedule");
      {
         std::vector<WorldEvent> em, eb;
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, T + 0.4, T + 0.45, em);
         BruteDose(seed, w, moved, storm, q, T + 0.4, T + 0.45, eb);
         Check(SameSet(em, eb), "... and still = the brute force");
      }
      // A new chapter: every schedule anew (its epochs moved).
      w.SetClockMap(chapter, &err);
      tmChap = Frames(w, T, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInMap, after a new chapter", tmChap);
      {
         std::vector<WorldEvent> em, eb;
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, T + 0.4, T + 0.45, em);
         BruteDose(seed, w, chapter, storm, q, T + 0.4, T + 0.45, eb);
         Check(SameSet(em, eb), "... and = the brute force with three epochs");
      }
      // The continuous windows (dSTORM initial ON).
      Timed& tc = tcOn;
      for (int f = 0; f < 6; f++) {
         std::vector<MapWindow> cw;
         const auto c0 = Clk::now();
         w.ContinuousInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, T, cw);
         const double ms = Ms(c0);
         if (f == 0) { tc.first = ms; tc.events = cw.size(); }
         else { tc.mean += ms / 5; tc.max = std::max(tc.max, ms); }
      }
      Line("ContinuousInMap (initial ON windows)", tc);
   }
   {
      World w = Labelled(seed, p, MakeLabel(LabelMode::WideField, 0.1, k1));
      dyesIn(w);
      w.SetClockMap(shaped);
      Timed tc = { 0, 0, 0, 0, 0 };
      for (int f = 0; f < 6; f++) {
         std::vector<MapWindow> cw;
         const auto c0 = Clk::now();
         w.ContinuousInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, T, cw);
         const double ms = Ms(c0);
         if (f == 0) { tc.first = ms; tc.events = cw.size(); }
         else { tc.mean += ms / 5; tc.max = std::max(tc.max, ms); }
      }
      Line("ContinuousInMap (WideField)", tc);
   }

   // ---- PALM on an activation clock: eight records over [0, 50) at moving poses (hazard 0.01/s at the peak plus a
   // spontaneous 1e-5/s), the open one from 50; then a pose change at T.
   std::printf("\n(map) PALM on the activation clock: 8 records, then a pose change\n");
   Timed tp, tpPose;
   {
      Kinetics kp = k1;
      kp.bleachProb = 0.3;
      kp.offSec = 0.5;
      const Label palm = MakeLabel(LabelMode::PALM, 0.25, kp, true);
      const double act = 0.01, spont = 1e-5;
      ClockMap pm;
      pm.s.assign(STRUCTURE_COUNT, StructClock());
      for (StructClock& x : pm.s) x.hist = Hist({ 0.0 }, { kp });
      StructClock& c = pm.s[STRUCTURE_MT];
      c.mode = CLOCK_ACTIVATION;
      c.hist = Hist({ 0.0 }, { kp });
      for (int r = 0; r < 8; r++) {
         const double t0 = r * 6.25, t1 = (r + 1) * 6.25;
         pm.grids.push_back(beam(2.0 * std::cos(r), 2.0 * std::sin(r), act * (t1 - t0)));
         ClockStep st;
         st.t0 = t0; st.t1 = t1; st.c = spont * (t1 - t0);
         st.field = { GridTerm((int)pm.grids.size() - 1, shear) };
         c.steps.push_back(st);
      }
      pm.grids.push_back(beam(0, 0, act));
      c.rate = { GridTerm((int)pm.grids.size() - 1, shear) };
      c.rateC = spont;
      c.tOpen = 50;
      ClockMap pm2 = pm;
      {
         StructClock& c2 = pm2.s[STRUCTURE_MT];
         pm2.grids.push_back(beam(0, 0, act * (T - 50)));
         ClockStep st;
         st.t0 = 50; st.t1 = T; st.c = spont * (T - 50);
         st.field = { GridTerm((int)pm2.grids.size() - 1, shear) };
         c2.steps.push_back(st);
         pm2.grids.push_back(beam(1, -1, act));
         c2.rate = { GridTerm((int)pm2.grids.size() - 1, shear) };
         c2.tOpen = T;
      }
      World w = Labelled(seed, p, palm);
      dyesIn(w);
      std::string err;
      Check(w.SetClockMap(pm, &err), "SetClockMap takes the PALM map");
      if (!err.empty()) std::printf("      %s\n", err.c_str());
      long w0 = w.Stats().mapWalks;
      tp = Frames(w, 50.0, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInMap (PALM), frames from tOpen", tp);
      std::printf("      activation walks: %ld\n", w.Stats().mapWalks - w0);
      bool same = true;
      size_t n = 0;
      for (double t0 : { 50.0, 59.95 }) {
         std::vector<WorldEvent> em, eb;
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, t0, t0 + FRAME, em);
         BruteActivation(seed, w, pm, palm, q, t0, t0 + FRAME, eb);
         same = same && SameSet(em, eb);
         n += em.size();
      }
      std::printf("      %zu blinks in 2 frames vs the brute force: %s\n", n, same ? "the same" : "DIFFERENT");
      Check(same, "the PALM activation map = every dye's own walk (brute force)");
      w.SetClockMap(pm2, &err);
      w0 = w.Stats().mapWalks;
      tpPose = Frames(w, T, 21, [&](std::vector<WorldEvent>& e, double a, double b) {
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, a, b, e);
      });
      Line("EventsInMap (PALM), after a pose change", tpPose);
      std::printf("      activation walks: %ld\n", w.Stats().mapWalks - w0);
      {
         std::vector<WorldEvent> em, eb;
         w.EventsInMap(q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, T + 0.4, T + 0.45, em);
         BruteActivation(seed, w, pm2, palm, q, T + 0.4, T + 0.45, eb);
         Check(SameSet(em, eb), "... and = the brute force after the pose change");
      }
   }

   // ---- (a) today's clock regions: ~2600 regions (here 51 x 51 squares of the field), each its own history (the
   // epoch start at its own clock) and query.
   Timed ta = { 0, 0, 0, 0, 0 };
   if (regions) {
      std::printf("\n(a) clock regions: 51 x 51 regions, one history and one query each\n");
      World w = Labelled(seed, p, storm);
      dyesIn(w);
      const int nr = 51;
      const double s = W / nr;
      for (int f = 0; f < 3; f++) {
         size_t ev = 0;
         const auto c0 = Clk::now();
         for (int j = 0; j < nr; j++)
            for (int i = 0; i < nr; i++) {
               const double rx = q.x0 + i * s, ry = q.y0 + j * s;
               const double clock = 40 + 20 * std::exp(-((rx - cx) * (rx - cx) + (ry - cy) * (ry - cy)) / 400.0);
               std::vector<Kinetics> kin;
               for (int st = 0; st < STRUCTURE_COUNT; st++) kin.push_back(k1);
               for (int st = 0; st < STRUCTURE_COUNT; st++) kin.push_back(k2);
               w.SetKineticsHistory({ 0.0, 20 + 0.001 * (j * nr + i) }, kin);
               std::vector<WorldEvent> e;
               const double t0 = clock + f * FRAME;
               w.EventsInWindow(rx, ry, rx + s, ry + s, q.z0, q.z1, t0, t0 + FRAME, e);
               ev += e.size();
            }
         const double ms = Ms(c0);
         if (f == 0) { ta.first = ms; ta.events = ev; }
         else { ta.mean += ms / 2; ta.max = std::max(ta.max, ms); }
      }
      Line("2601 regions per frame", ta);
   }

   std::printf("\ngates (plan P0):\n");
   auto gate = [](bool ok, const char* what, double v, double lim) {
      std::printf("  %s  %-52s %8.2f (<= %.2f)\n", ok ? "ok  " : "MISS", what, v, lim);
   };
   gate(tm.mean <= 8 && tm.mean <= 2.5 * tb.mean, "warm map query, ms (and <= 2.5x uniform)", tm.mean,
        std::min(8.0, 2.5 * tb.mean));
   gate(tm.first <= 2 * tb.first, "cold map query vs 2x uniform cold, ms", tm.first, 2 * tb.first);
   gate(tmPose.schedules == 0, "schedules rebuilt by a dose-clock pose change", (double)tmPose.schedules, 0);
   gate(tpPose.first <= 60, "PALM pose change, first frame, ms", tpPose.first, 60);
   gate(tcOn.mean <= 3, "warm continuous query (initial ON), ms", tcOn.mean, 3);
   std::printf("\n%s (%d failed checks), %.1f s\n", g_failures ? "FAILED" : "checks passed", g_failures, Ms(tAll) / 1000);
   return g_failures ? 1 : 0;
}
