// ctest history_check: the exact light history (Simulation/LightHistory.h) and the clock map it gives the core (ABI 14).
//   - a fresh session's map is the uniform one: one epoch with the movie's own rows, the dose = T bit for bit, the same
//     buffer after frames at the same light;
//   - a dose (dSTORM) and a hazard (PALM) at the lattice's nodes = an independent sum over the frames (each frame's
//     own labels and Excitation::At: FlatTop epi, Gaussian HILO, FlatTop TIRF; poses, powers, dark gaps, a ratio
//     change); between nodes within the bilinear error;
//   - chapters: a power step keeps the chapter and the epochs, a ratio step starts both (the bound = the dose
//     before), a pose step no epoch; a DNA-PAINT imager change in the dark is an epoch in T;
//   - the map of a movie under a new light (the open record closed at T) = the map once its frame is counted, byte
//     for byte; a movie's averaged record = the per-pose fold;
//   - the core reads the buffers (dose clock and rate per dye = the host's, PALM's steps accepted);
//   - a 60 s HILO stage drag: the fold per frame <= 5 ms, a map <= 15 ms, memory <= 64 MB.
//
//   history_check          exit code 0 = all checks passed
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "DyeLibrary.h"
#include "Excitation.h"
#include "LightHistory.h"
#include "ScopeMovie.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok)
      g_failures++;
}

double Now()
{
   return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ScopeSpec Spec(const std::string& text)
{
   ScopeSpec s;
   std::string err;
   if (!ParseScopeSpec(text, s, err))
   {
      std::printf("FAIL  spec '%s': %s\n", text.c_str(), err.c_str());
      std::exit(1);
   }
   return s;
}

LightState Light(const ScopeSpec& s, bool on = true)
{
   LightState L;
   std::string err;
   if (!MakeLightState(s, on, L, err))
   {
      std::printf("FAIL  light: %s\n", err.c_str());
      std::exit(1);
   }
   return L;
}

std::vector<LabelPhysics> Labels(const ScopeSpec& cur, const KineticEnv& env)
{
   std::vector<LabelPhysics> l;
   std::string err;
   if (!ScopeEnvLabels(cur, env, l, err))
   {
      std::printf("FAIL  labels: %s\n", err.c_str());
      std::exit(1);
   }
   return l;
}

struct Box
{
   double x0, y0, x1, y1, z0, z1;
};
// The movie's query: the FOV and 2 um (ScopeMovie's margin) at heights z0..z1.
Box QueryBox(const ScopeSpec& s, double z0 = 0, double z1 = 8)
{
   const double um = ScopeSpecGet(s, "pixel-nm") / 1000.0, W = ScopeSpecGet(s, "size") * um;
   const double x = ScopeSpecGet(s, "x"), y = ScopeSpecGet(s, "y");
   return { x - W / 2 - 2, y - W / 2 - 2, x + W / 2 + 2, y + W / 2 + 2, z0, z1 };
}

std::shared_ptr<const ClockMapView> Map(LightHistory& h, const ScopeSpec& cur, const LightState& L, const Box& b)
{
   std::string err;
   auto m = h.BuildMap(cur, L, b.x0, b.y0, b.x1, b.y1, b.z0, b.z1, err);
   if (!m)
   {
      std::printf("FAIL  map: %s\n", err.c_str());
      std::exit(1);
   }
   return m;
}

// A frame of a synthetic acquisition: its spec (pose, light), whether a source is open, its exposure.
struct Frame
{
   ScopeSpec spec;
   bool on = true;
   double dt = 0.05;
};

// The independent sums over the frames, each at its own labels (ScopeEnvLabels of its env under cur) and its own
// excitation (Excitation::At at its axis): the dose a dSTORM dye at (x, y, z) has had, in seconds of the first lit
// frame's light, and a PALM dye's activation hazard (spont + lin a + quad a^2 per second).
class Brute
{
public:
   Brute(const ScopeSpec& cur, int st) : cur_(cur), st_(st) {}
   void Add(const Frame& f)
   {
      Item it;
      it.dt = f.dt;
      it.L = Light(f.spec, f.on);
      std::string key;
      for (const auto& kv : it.L.env)
         key += kv.first + std::to_string(kv.second) + ";";
      auto c = cache_.find(key);
      if (c == cache_.end())
         c = cache_.emplace(key, Labels(cur_, it.L.env)).first;
      it.labels = &c->second;
      items_.push_back(it);
   }
   double Dose(double x, double y, double z, double* scale = nullptr) const
   {
      double xRef = 0, s = 0, sc = 0;
      for (const Item& it : items_)
      {
         if (!it.L.lit)
            continue;
         const StatePhysics& m = (*it.labels)[static_cast<size_t>(st_)].main;
         if (!(xRef > 0))
            xRef = m.excitationPerSec;
         ExcitationShares sh;
         for (const StatePhysics::LineShare& l : m.lineShares)
            sh.push_back({ l.nm, l.share });
         const LightPose& p = it.L.poses[0];
         s += m.excitationPerSec * it.L.ex->At(x - p.ax, y - p.ay, z, sh) * it.dt;
         sc += m.excitationPerSec * it.L.ex->F() * it.dt;
      }
      if (scale)
         *scale = xRef > 0 ? sc / xRef : 0.0;
      return xRef > 0 ? s / xRef : 0.0;
   }
   double Hazard(double x, double y, double z, double* scale = nullptr) const
   {
      double s = 0, sc = 0;
      for (const Item& it : items_)
      {
         const LabelPhysics& C = (*it.labels)[static_cast<size_t>(st_)];
         double a = 0;
         if (it.L.lit)
         {
            ExcitationShares sh;
            for (const StatePhysics::LineShare& l : C.main.lineShares)
               sh.push_back({ l.nm, l.share });
            const LightPose& p = it.L.poses[0];
            a = it.L.ex->At(x - p.ax, y - p.ay, z, sh);
            sc += (C.actLin * it.L.ex->F() + C.actQuad * it.L.ex->F() * it.L.ex->F()) * it.dt;
         }
         s += (C.actSpont + C.actLin * a + C.actQuad * a * a) * it.dt;
      }
      if (scale)
         *scale = sc;
      return s;
   }

private:
   struct Item
   {
      LightState L;
      double dt = 0;
      const std::vector<LabelPhysics>* labels = nullptr;
   };
   ScopeSpec cur_;
   int st_;
   std::vector<Item> items_;
   std::map<std::string, std::vector<LabelPhysics>> cache_;
};

// Runs frames through a history and the brute force.
void Run(LightHistory& h, Brute* b, const std::vector<Frame>& frames)
{
   for (const Frame& f : frames)
   {
      h.Advance(f.dt, Light(f.spec, f.on));
      if (b)
         b->Add(f);
   }
}

std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0)
{
   char s[512];
   std::snprintf(s, sizeof s, f, a, b, c, d);
   return s;
}

// The nodes of the lattice in the box (a chapter's coordinates: x = u + z tan theta_s when sheared).
std::vector<std::array<double, 3>> NodePoints(const Box& bx, const LightState& L, const std::vector<double>& zs,
                                              int stride)
{
   std::vector<std::array<double, 3>> out;
   const bool flat = L.geo.profile == ILLUM_FLAT;
   const double h = flat ? kLightTileUm : LightLatticeUm(*L.ex);
   const double tanS = !flat && L.ex->Tilted() ? L.ex->TanS() : 0.0;
   for (double z : zs)
      for (long j = static_cast<long>(std::ceil(bx.y0 / h - 0.5)); (j + 0.5) * h <= bx.y1; j += stride)
         for (long i = static_cast<long>(std::ceil((bx.x0 - z * tanS) / h - 0.5)); (i + 0.5) * h + z * tanS <= bx.x1;
              i += stride)
            out.push_back({ (i + 0.5) * h + z * tanS, (j + 0.5) * h, z });
   return out;
}

// ---- the checks ----

const char* kDstorm = "size=64 npc-dye=None mt-dye=AF647 mt-mode=dSTORM light-preset=0 laser-640=2 laser-405=0 "
                      "laser-488=0 laser-561=0 laser-730=0";

void Fresh(IscWorld* w)
{
   const ScopeSpec s = Spec(std::string(kDstorm) + " illum-profile=Flat illum-bfp=0 x=10 y=-5");
   const LightState L = Light(s);
   const Box bx = QueryBox(s);
   LightHistory h;
   const auto m0 = Map(h, s, L, bx);
   const MapClock& c = m0->s[0];
   const std::vector<LabelPhysics> own = Labels(s, ScopeKineticEnv(s));
   bool rows = c.epochs.size() == 1;
   for (int k = 0; k < ISC_KIN_COUNT && rows; ++k)
      rows = c.epochs[0].kin[k] == own[0].label[static_cast<size_t>(ISC_LABEL_ACTIVATION_RATE + k)];
   Check(c.mode == ISC_CLOCK_DOSE && rows && c.total.empty() && c.rate.size() == 1 && c.rate[0].scale == 1.0 &&
            c.tOpen == 0.0 && c.bound.empty(),
         "fresh session: one dose epoch with the movie's own rows, rate 1, nothing before");
   for (int f = 0; f < 40; ++f)
      h.Advance(0.05, L);
   const auto m1 = Map(h, s, L, bx);
   Check(m1->buffer == m0->buffer, "40 frames at the same light: the same map, byte for byte");
   std::mt19937 rng(7);
   std::uniform_real_distribution<double> ux(bx.x0, bx.x1), uy(bx.y0, bx.y1), uz(0, 8);
   const double T = h.T();
   bool exact = true;
   for (int i = 0; i < 2000 && exact; ++i)
   {
      const double x = ux(rng), y = uy(rng), z = uz(rng);
      exact = m1->ClockAt(0, x, y, z, T) == T && m1->Rate(0, x, y, z) == 1.0;
   }
   Check(exact, Fmt("the dose = T = %.17g bit for bit at 2000 places of the query, the rate 1", T));
   Check(isc_world_set_clock_map(w, m1->buffer.data(), static_cast<int32_t>(m1->buffer.size())) == 0,
         "the core reads it");
   isc_world_set_clock_map(w, nullptr, 0);
}

// A dSTORM acquisition under a geometry: poses, a power step, dark frames, a z (pivot) step.
std::vector<Frame> Acquisition(const std::string& base, double x, double y, int scale = 1)
{
   std::vector<Frame> fr;
   auto at = [&](double px, double py, double z, double p640, bool on, int n) {
      for (int k = 0; k < n * scale; ++k)
      {
         Frame f;
         f.spec = Spec(base + Fmt(" x=%.17g y=%.17g z=%.17g laser-640=%.17g", px, py, z, p640));
         f.on = on;
         fr.push_back(f);
      }
   };
   at(x, y, 0.5, 2, true, 6);
   at(x + 3, y, 0.5, 2, true, 4);           // a stage step
   at(x + 3, y - 2.5, 0.5, 3, true, 5);     // and a power step
   at(x + 3, y - 2.5, 0.5, 3, false, 5);    // dark (time runs)
   at(x - 1.5, y + 1, 1.7, 2, true, 6);     // the focus up (HILO: another pivot)
   return fr;
}

void Dose(IscWorld* w, const char* name, const std::string& geo, const std::vector<double>& zs)
{
   const std::string base = std::string(kDstorm) + " " + geo;
   const double X = 10, Y = -5;
   const std::vector<Frame> fr = Acquisition(base, X, Y);
   const ScopeSpec cur = fr.back().spec;
   LightHistory h;
   Brute b(cur, 0);
   Run(h, &b, fr);
   const LightState L = Light(cur);
   const Box bx = QueryBox(cur, 0, 8);
   const double t0 = Now();
   const auto m = Map(h, cur, L, bx);
   const double ms = (Now() - t0) * 1000;
   const MapClock& c = m->s[0];
   double worst = 0, scale = 0, top = 0;
   size_t n = 0;
   for (const auto& p : NodePoints(bx, L, zs, 3))
   {
      const double want = b.Dose(p[0], p[1], p[2], &scale);
      const double got = m->ClockAt(0, p[0], p[1], p[2], h.T());
      worst = std::max(worst, std::fabs(got - want) / (1e-9 * std::fabs(want) + 1.5e-4 * scale));
      top = std::max(top, want);
      ++n;
   }
   Check(c.mode == ISC_CLOCK_DOSE && c.epochs.size() == 1 && h.Chapters() == 1 && worst <= 1.0 && top > 0.1 * scale,
         std::string(name) + Fmt(": the dose at %.0f nodes = the frames' sum (worst %.3f of the tolerance 1e-9 + the "
                                 "reach; most %.3f s); map %.1f ms",
                                 static_cast<double>(n), worst, top, ms));
   // Between nodes: the bilinear error, at most kLightLatticeErr of each record's peak (their sum: scale).
   std::mt19937 rng(11);
   std::uniform_real_distribution<double> ux(bx.x0, bx.x1), uy(bx.y0, bx.y1);
   double err = 0, rel = 0;
   for (int i = 0; i < 3000; ++i)
   {
      const double x = ux(rng), y = uy(rng), z = zs[static_cast<size_t>(i) % zs.size()];
      const double want = b.Dose(x, y, z), got = m->ClockAt(0, x, y, z, h.T());
      err = std::max(err, std::fabs(got - want));
      if (want >= 0.25 * top)
         rel = std::max(rel, std::fabs(got - want) / want);
   }
   const double hUm = L.geo.profile == ILLUM_FLAT ? kLightTileUm : LightLatticeUm(*L.ex);
   Check(err <= (kLightLatticeErr + 1.5e-4) * scale,
         std::string(name) + Fmt(": between nodes (%.4g um) within %.3g %% of the peak dose (%.3g %% where at least a "
                                 "quarter of the most)",
                                 hUm, err / scale * 100, rel * 100));
   // The core: the WideField label (one window per dye, its dose and rate) at the same light.
   const ScopeSpec wf = Spec(base + Fmt(" x=%.17g y=%.17g z=1.7 mt-mode=WideField", X - 1.5, Y + 1));
   LightHistory hw;
   std::vector<Frame> fw = fr;
   for (Frame& f : fw)
      f.spec["mt-mode"] = MODE_WIDEFIELD;
   Run(hw, nullptr, fw);
   const auto mw = Map(hw, wf, Light(wf), bx);
   const std::vector<LabelPhysics> lw = Labels(wf, ScopeKineticEnv(wf));
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
      isc_world_set_label(w, s, lw[static_cast<size_t>(s)].label.data(), ISC_LABEL_COUNT);
   const bool set = isc_world_set_clock_map(w, mw->buffer.data(), static_cast<int32_t>(mw->buffer.size())) == 0;
   const double qx = X - 1.5, qy = Y + 1;
   const int32_t cap = 400000;
   std::vector<double> rows(static_cast<size_t>(cap) * ISC_MAP_WINDOW_STRIDE);
   const int32_t nr = set ? isc_continuous_in_map(w, qx - 5, qy - 5, qx + 5, qy + 5, -1, 10, -INFINITY, rows.data(), cap)
                          : -1;
   size_t same = 0, lit = 0;
   for (int32_t i = 0; i < std::min(nr, cap); ++i)
   {
      const double* r = rows.data() + static_cast<size_t>(i) * ISC_MAP_WINDOW_STRIDE;
      same += r[10] == mw->Clock(0, r[0], r[1], r[2]) && r[11] == mw->Rate(0, r[0], r[1], r[2]);
      lit += r[11] > 0;
   }
   Check(set && nr > 100 && nr <= cap && same == static_cast<size_t>(nr) && lit > 0,
         std::string(name) + Fmt(": the core's dose and rate per dye = the host's (%.0f of %.0f windows, %.0f lit)",
                                 static_cast<double>(same), static_cast<double>(nr), static_cast<double>(lit)));
   isc_world_set_clock_map(w, nullptr, 0);
}

void Chapters()
{
   const std::string two = std::string(kDstorm) + " illum-profile=FlatTop illum-bfp=0 x=10 y=-5 laser-405=0.02";
   const ScopeSpec a = Spec(two), b = Spec(two + " laser-405=0.05"), p = Spec(two + " laser-640=3 laser-405=0.03");
   const ScopeSpec moved = Spec(two + " x=14");
   const Box bx = QueryBox(a);
   {
      LightHistory h;
      for (int f = 0; f < 10; ++f)
         h.Advance(0.05, Light(a));
      const auto m0 = Map(h, a, Light(a), bx);
      for (int f = 0; f < 10; ++f)
         h.Advance(0.05, Light(p));
      const auto m1 = Map(h, p, Light(p), bx);
      for (int f = 0; f < 10; ++f)
         h.Advance(0.05, Light(moved));
      const auto m2 = Map(h, moved, Light(moved), bx);
      bool sameRows = m1->s[0].epochs.size() == 1 && m2->s[0].epochs.size() == 1;
      for (int k = 0; k < ISC_KIN_COUNT && sameRows; ++k)
         sameRows = m0->s[0].epochs[0].kin[k] == m1->s[0].epochs[0].kin[k] &&
                    m0->s[0].epochs[0].kin[k] == m2->s[0].epochs[0].kin[k];
      Check(h.Chapters() == 1 && sameRows && h.Records() == 3,
            "both lines' powers x 1.5, then a stage move: one chapter, one epoch (the same rows: no schedule changes)");
   }
   {
      LightHistory h;
      Brute br(b, 0);
      std::vector<Frame> fr(10);
      for (Frame& f : fr)
         f.spec = a;
      Run(h, &br, fr);
      const double x = 10.0625, y = -4.9375, z = 0;   // a node of the 0.125 um lattice
      const double before = br.Dose(x, y, z);
      for (Frame& f : fr)
         f.spec = b;
      Run(h, nullptr, fr);
      const auto m = Map(h, b, Light(b), bx);
      const MapClock& c = m->s[0];
      const bool epochs = c.epochs.size() == 2 && c.bound.size() == 1 && std::fabs(c.epochs[1].tStart - 0.5) < 1e-12;
      const double bound = epochs ? m->Eval(c.bound[0], x, y, z) : -1;
      Check(h.Chapters() == 2 && epochs && std::fabs(bound - before) <= 1e-12 * before,
            Fmt("the 405 nm line x 2.5 (another ratio): a chapter and an epoch at T 0.5 s, its bound = the dose "
                "before (%.6f vs %.6f)",
                bound, before));
   }
   {
      // DNA-PAINT: the imager changes in the dark; binding needs no light, its rows change at that T.
      const std::string paint = "size=64 npc-dye=None mt-dye=ATTO655 mt-mode=DNA-PAINT illum-profile=Flat x=10 y=-5 ";
      const ScopeSpec p1 = Spec(paint + "mt-imager-nm=1"), p2 = Spec(paint + "mt-imager-nm=2");
      LightHistory h;
      for (int f = 0; f < 10; ++f)
         h.Advance(0.05, Light(p1));
      for (int f = 0; f < 4; ++f)
         h.Advance(0.05, Light(p2, false));
      for (int f = 0; f < 4; ++f)
         h.Advance(0.05, Light(p2));
      const auto m = Map(h, p2, Light(p2), bx);
      const MapClock& c = m->s[0];
      Check(c.mode == ISC_CLOCK_GLOBAL && c.epochs.size() == 2 && std::fabs(c.epochs[1].tStart - 0.5) < 1e-12 &&
               c.epochs[1].kin[ISC_KIN_ACTIVATION_RATE] == 2 * c.epochs[0].kin[ISC_KIN_ACTIVATION_RATE],
            "DNA-PAINT: the imager x 2 in the dark is an epoch in T at the change (binding twice as often)");
   }
}

void OpenRecord()
{
   const std::string base = std::string(kDstorm) + " illum-profile=Gaussian illum-bfp=0.8 y=-5";
   const ScopeSpec p1 = Spec(base + " x=10"), p2 = Spec(base + " x=10.5"), jitter = Spec(base + " x=10.02");
   const Box bx = QueryBox(p1);
   LightHistory h;
   const auto m0 = Map(h, p1, Light(p1), bx);
   for (int f = 0; f < 8; ++f)
      h.Advance(0.05, Light(f % 2 ? jitter : p1));
   const auto m1 = Map(h, p1, Light(p1), bx);
   Check(m1->buffer == m0->buffer && h.Records() == 1,
         "8 frames (every other 20 nm off): one record, the map of the movie's start, byte for byte");
   const auto v = Map(h, p2, Light(p2), bx);   // a movie under a new pose: the open record closed at T
   h.Advance(0.05, Light(p2));
   const auto r = Map(h, p2, Light(p2), bx);
   Check(v->buffer == r->buffer && h.Records() == 2,
         "a movie at a new pose before its frame is counted = after, byte for byte");
}

void Averaged()
{
   const std::string base = std::string(kDstorm) + " illum-profile=FlatTop illum-bfp=0.6 y=-5";
   std::vector<LightState> frames;
   LightHistory per, avg;
   for (int k = 0; k < 20; ++k)
   {
      const ScopeSpec s = Spec(base + Fmt(" x=%.17g", 10 + 0.1 * k));
      frames.push_back(Light(s));
      per.Advance(0.05, frames.back());
   }
   LightState merged;
   std::string err;
   const bool ok = MergeLightStates(frames, merged, err);
   avg.Advance(1.0, merged);
   const ScopeSpec end = Spec(base + " x=20");
   const LightState Le = Light(end);
   per.Advance(0.05, Le);
   avg.Advance(0.05, Le);
   const ScopeSpec s0 = Spec(base + " x=10");
   const Box bx = QueryBox(s0);
   const auto mp = Map(per, end, Le, bx), ma = Map(avg, end, Le, bx);
   double worst = 0, top = 0;
   for (const auto& p : NodePoints(bx, Le, { 0.0, 2.0 }, 2))
   {
      const double a = mp->ClockAt(0, p[0], p[1], p[2], per.T()), b = ma->ClockAt(0, p[0], p[1], p[2], avg.T());
      top = std::max(top, a);
      worst = std::max(worst, std::fabs(a - b));
   }
   Check(ok && merged.poses.size() == 20 && worst <= 1e-12 * top,
         Fmt("a drifting movie's record (20 poses, 0.1 um apart) = 20 records folded (worst %.2g of %.3f s)", worst, top));
   // The movie's own map: its rate = the poses' mean footprint.
   LightHistory fresh;
   const auto mm = Map(fresh, s0, merged, bx);
   double wr = 0;
   for (const auto& p : NodePoints(bx, Le, { 1.0 }, 2))
   {
      double want = 0;
      for (const LightState& f : frames)
         want += f.ex->At(p[0] - f.poses[0].ax, p[1] - f.poses[0].ay, p[2], { { 640, 1.0 } }) / 20;
      wr = std::max(wr, std::fabs(mm->Rate(0, p[0], p[1], p[2]) - want));
   }
   Check(wr <= 2e-4, Fmt("its movie's rate = the frames' mean excitation at the nodes (worst %.2g, the reach 1e-4)", wr));
}

void Palm(IscWorld* w)
{
   // mEos3.2 PALM: the 405 nm activation and the primed pair (Excitation.h: the activation lines fall off as the main
   // state's). 24 records (8 merged), two ratios.
   const std::string base = "size=64 npc-dye=None mt-dye=mEos3.2 mt-mode=PALM light-preset=0 laser-640=0 "
                            "illum-profile=FlatTop illum-bfp=0 y=-5 ";
   std::vector<Frame> fr;
   for (int k = 0; k < 24; ++k)
   {
      const double s = k % 3 == 2 ? 2.0 : 1.0;   // both lines x 2 (one ratio)
      const double l405 = k < 16 ? 0.01 : 0.03;  // then another ratio
      Frame f;
      f.spec = Spec(base + Fmt("x=%.17g laser-561=%.17g laser-405=%.17g", 2 + 0.75 * k, 1.0 * s, l405 * s) +
                    Fmt(" laser-488=%.17g laser-730=%.17g", 0.2 * s, 0.5 * s));
      f.on = k % 7 != 6;
      fr.push_back(f);
   }
   const ScopeSpec cur = fr.back().spec;
   LightHistory h;
   Brute b(cur, 0);
   Run(h, &b, fr);
   const LightState L = Light(cur);
   Box bx = QueryBox(fr[10].spec);
   bx.x0 -= 4;
   bx.x1 += 8;
   const auto m = Map(h, cur, L, bx);
   const MapClock& c = m->s[0];
   double worst = 0, scale = 0;
   size_t n = 0;
   for (const auto& p : NodePoints(bx, L, { 0.0, 3.0 }, 3))
   {
      const double want = b.Hazard(p[0], p[1], p[2], &scale);
      const double got = m->Hazard(0, p[0], p[1], p[2], h.T());
      // (G2, the primed pair's merged grid, is float: 1e-7.)
      worst = std::max(worst, std::fabs(got - want) / (2e-7 * std::fabs(want) + 1.5e-4 * scale + 1e-15));
      ++n;
   }
   char buf[256];
   std::snprintf(buf, sizeof buf,
                 "PALM: %zu steps (the merged past, %zu records), the hazard at %zu nodes = the frames' sum (worst %.3f "
                 "of the tolerance)",
                 c.steps.size(), h.Records(), n, worst);
   Check(c.mode == ISC_CLOCK_ACTIVATION && h.Chapters() == 2 && c.steps.size() >= 2 && c.steps[0].t1 > 0 &&
            worst <= 1.0,
         buf);
   const std::vector<LabelPhysics> lc = Labels(cur, ScopeKineticEnv(cur));
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
      isc_world_set_label(w, s, lc[static_cast<size_t>(s)].label.data(), ISC_LABEL_COUNT);
   const bool set = isc_world_set_clock_map(w, m->buffer.data(), static_cast<int32_t>(m->buffer.size())) == 0;
   const int32_t cap = 400000;
   std::vector<double> rows(static_cast<size_t>(cap) * ISC_MAP_WINDOW_STRIDE);
   const int32_t nr = set ? isc_continuous_in_map(w, 3.5, -9, 13.5, 0, -1, 10, -INFINITY, rows.data(), cap) : -1;
   size_t act = 0;
   for (int32_t i = 0; i < std::min(nr, cap); ++i)
      act += rows[static_cast<size_t>(i) * ISC_MAP_WINDOW_STRIDE + 10] < h.T();
   bool contiguous = !c.steps.empty() && c.steps[0].t0 == 0 && c.steps.back().t1 == c.tOpen;
   for (size_t i = 1; i < c.steps.size(); ++i)
      contiguous = contiguous && c.steps[i].t0 == c.steps[i - 1].t1 && c.steps[i].t1 > c.steps[i].t0;
   Check(set && contiguous && nr > 50 && nr <= cap && act > 0,
         Fmt("the core reads PALM's steps (set %.0f, contiguous %.0f): %.0f pre-state windows, %.0f activated by now",
             set, contiguous, nr, static_cast<double>(act)));
   isc_world_set_clock_map(w, nullptr, 0);
}

void Drag()
{
   // 60 s at 20 fps, the stage at 10 um/s under a HILO FlatTop beam at 256 px: a record (a fold) every frame.
   const std::string base = std::string(kDstorm) + " size=256 illum-profile=FlatTop illum-bfp=0.8 y=-5 ";
   LightHistory h;
   double fold = 0, worstFold = 0;
   for (int k = 0; k < 1200; ++k)
   {
      const LightState L = Light(Spec(base + Fmt("x=%.17g", 0.5 * k)));
      const double t0 = Now();
      h.Advance(0.05, L);
      const double dt = Now() - t0;
      fold += dt;
      worstFold = std::max(worstFold, dt);
   }
   const ScopeSpec cur = Spec(base + "x=600");
   const LightState L = Light(cur);
   const Box bx = QueryBox(cur, 0, 8);
   double t0 = Now();
   Map(h, cur, L, bx);
   const double first = (Now() - t0) * 1000;
   // The best of three new records (ctest -j 8 runs other checks beside it).
   double again = 1e9;
   std::shared_ptr<const ClockMapView> m;
   LightState prev = L;
   for (int r = 1; r <= 3; ++r)
   {
      h.Advance(0.05, prev);
      const ScopeSpec next = Spec(base + Fmt("x=%.17g", 600 + 0.5 * r));
      prev = Light(next);
      t0 = Now();
      m = Map(h, next, prev, bx);
      again = std::min(again, (Now() - t0) * 1000);
   }
   const double mb = h.Bytes() / 1048576.0;
   char b[300];
   std::snprintf(b, sizeof b,
                 "60 s HILO stage drag at 256 px: fold %.2f ms per frame (worst %.2f), a map %.1f ms (the first, with "
                 "its labels, %.1f; %.1f MB buffer), %.1f MB held",
                 fold / 1200 * 1000, worstFold * 1000, again, first, m->buffer.size() * 8 / 1048576.0, mb);
   Check(fold / 1200 * 1000 <= 5 && again <= 15 && mb <= 64, b);
}

} // namespace

int main()
{
   const double t0 = Now();
   IscParams* p = isc_params_new();
   IscWorld* w = isc_world_new(1249, p);
   Fresh(w);
   Dose(w, "FlatTop epi", "illum-profile=FlatTop illum-bfp=0", { 0.0, 2.5 });
   Dose(w, "Gaussian HILO", "illum-profile=Gaussian illum-fwhm=60 illum-bfp=0.8", { 0.0, 1.2, 3.7 });
   Dose(w, "FlatTop TIRF", "na=1.49 illum-profile=FlatTop illum-bfp=1.05", { 0.0, 0.05, 0.1, 0.25 });
   Chapters();
   OpenRecord();
   Averaged();
   Palm(w);
   Drag();
   isc_world_free(w);
   isc_params_free(p);
   std::printf("%s (%.1f s)\n", g_failures ? "FAILED" : "all passed", Now() - t0);
   return g_failures ? 1 : 0;
}
