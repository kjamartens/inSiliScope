///////////////////////////////////////////////////////////////////////////////
// FILE:          LightHistory.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See LightHistory.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "LightHistory.h"
#include "Parallel.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

namespace sim {

namespace {

constexpr double kH = kLightTileUm;

std::string Num(double v)
{
   char b[40];
   std::snprintf(b, sizeof b, "%.17g", v);
   return b;
}

bool IsLaserKey(const std::string& k) { return k.compare(0, 6, "laser-") == 0; }

// Nodes of a chapter's lattice: node i at (i + 1/2) h.
double NodeAt(long i, double h) { return (static_cast<double>(i) + 0.5) * h; }
long NodeAtOrAbove(double v, double h) { return static_cast<long>(std::ceil(v / h - 0.5)); }
long NodeAtOrBelow(double v, double h) { return static_cast<long>(std::floor(v / h - 0.5)); }
long FloorDiv(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool FlatBeam(const Excitation& ex) { return ex.Spec().profile == ILLUM_FLAT; }
bool ShearedBeam(const Excitation& ex) { return !FlatBeam(ex) && ex.Tilted(); }
// A pose's axis in the chapter's coordinates: u = x - z tan theta_s, where the beam's axis at the pivot plane is
// ax - pivot tan theta_s.
double PoseU(const Excitation& ex, const LightPose& p) { return ShearedBeam(ex) ? p.ax - p.pivot * ex.TanS() : p.ax; }
// A Flat beam's nodes (its lattice is kLightTileUm): those within its field stop and one tile more (every point of
// the stop between four of them).
void StopNodes(const LightPose& p, long& i0, long& j0, long& i1, long& j1)
{
   i0 = static_cast<long>(std::ceil(p.x0 / kH - 1.5));
   i1 = static_cast<long>(std::floor(p.x1 / kH + 0.5));
   j0 = static_cast<long>(std::ceil(p.y0 / kH - 1.5));
   j1 = static_cast<long>(std::floor(p.y1 / kH + 0.5));
}
bool SamePose(const Excitation& ex, const LightPose& a, const LightPose& b)
{
   if (FlatBeam(ex))
   {
      long a0, b0, a1, b1, c0, d0, c1, d1;
      StopNodes(a, a0, b0, a1, b1);
      StopNodes(b, c0, d0, c1, d1);
      return a0 == c0 && b0 == d0 && a1 == c1 && b1 == d1;
   }
   return std::fabs(PoseU(ex, a) - PoseU(ex, b)) <= kLightPoseTolUm && std::fabs(a.ay - b.ay) <= kLightPoseTolUm;
}

// Bilinear in lerp form over nodes node(i, j) (core ClockGrid::At's arithmetic on the lattice of origin 1/2 h).
template <class Node>
double Bilinear(const Node& node, double u, double w, double h)
{
   const double fx = (u - 0.5 * h) / h, fy = (w - 0.5 * h) / h;
   const double ix = std::floor(fx), iy = std::floor(fy);
   const long i = static_cast<long>(ix), j = static_cast<long>(iy);
   const double tx = fx - ix, ty = fy - iy;
   const double v00 = node(i, j), v10 = node(i + 1, j), v01 = node(i, j + 1), v11 = node(i + 1, j + 1);
   const double a = v00 + (v10 - v00) * tx;
   const double b = v01 + (v11 - v01) * tx;
   return a + (b - a) * ty;
}

double FootprintAt(const LightHistory::Footprint& f, double u, double w, double h)
{
   return Bilinear(
      [&](long i, long j) {
         const long a = i - f.i0, b = j - f.j0;
         return a < 0 || a >= f.nx || b < 0 || b >= f.ny ? 0.0
                                                          : f.v[static_cast<size_t>(b) * f.nx + static_cast<size_t>(a)];
      },
      u, w, h);
}

void KinOf(const LabelPhysics& L, double* kin)
{
   for (int k = 0; k < ISC_KIN_COUNT; ++k)
      kin[k] = L.label[static_cast<size_t>(ISC_LABEL_ACTIVATION_RATE + k)];
}
bool SameKin(const double* a, const double* b, int from = 0)
{
   for (int k = from; k < ISC_KIN_COUNT; ++k)
      if (!(a[k] == b[k]))
         return false;
   return true;
}

} // namespace

// ---- the light of a frame ----

double LightLatticeUm(const Excitation& ex)
{
   const double k = ex.MaxCurvature();
   double h = kLightTileUm;
   while (h > kLightTileUm / 16 && h * h / 8 * k > kLightLatticeErr)
      h /= 2;
   return h;
}

bool MakeLightState(const ScopeSpec& spec, bool lightOn, LightState& out, std::string& err)
{
   out = LightState();
   out.env = ScopeKineticEnv(spec);
   for (const auto& kv : out.env)
      if (!IsLaserKey(kv.first))
         out.envKey += kv.first + '=' + Num(kv.second) + ';';
   if (!ScopeSampleLines(spec, out.lines, err))
      return false;
   for (const LaserLine& l : out.lines)
      out.total += l.kWPerCm2;
   out.geo = ScopeExcitationSpec(spec);
   auto ex = std::make_shared<Excitation>(out.geo);
   out.ex = ex;
   out.lit = lightOn && out.total > 0 && !ex->Clipped();
   const ExcitationSpec& g = out.geo;
   out.lightKey = "p" + Num(g.profile) + ";u" + Num(g.bfp) + ";na" + Num(g.na) + ";ni" + Num(g.nImm) + ";ns" +
                  Num(g.nMed);
   if (g.profile != ILLUM_FLAT)
      out.lightKey += ";fw" + Num(g.fwhmPct) + ";fov" + Num(g.fovWidthUm);
   for (const LaserLine& l : out.lines)
   {
      char b[64];
      std::snprintf(b, sizeof b, ";%.17g:%.10g", l.nm, l.kWPerCm2 / out.total);
      out.lightKey += b;
   }
   unsigned W = 0, H = 0;
   long N = 0;
   ScopeMovieDims(spec, W, H, N);
   const double um = ScopeSpecGet(spec, "pixel-nm") / 1000.0;
   const double x = ScopeSpecGet(spec, "x"), y = ScopeSpecGet(spec, "y");
   LightPose p;
   // The engine's optical axis: the FOV centre pixel's centre ((W - 1) / 2 px from the FOV's corner).
   p.ax = x - W * um / 2 + (W - 1) / 2.0 * um;
   p.ay = y - H * um / 2 + (H - 1) / 2.0 * um;
   p.pivot = g.pivotUm;
   p.x0 = x - W * um / 2 - kLightLitMarginUm;
   p.x1 = x + W * um / 2 + kLightLitMarginUm;
   p.y0 = y - H * um / 2 - kLightLitMarginUm;
   p.y1 = y + H * um / 2 + kLightLitMarginUm;
   out.poses.push_back(p);
   return true;
}

bool MergeLightStates(const std::vector<LightState>& frames, LightState& out, std::string& err)
{
   if (frames.empty())
   {
      err = "a movie's light needs its frames";
      return false;
   }
   const LightState& f0 = frames[0];
   std::vector<std::pair<LightPose, size_t>> groups;
   for (const LightState& f : frames)
   {
      if (f.lit != f0.lit || f.envKey != f0.envKey || f.lightKey != f0.lightKey || f.total != f0.total ||
          f.poses.size() != 1 || !f.ex)
      {
         err = "a movie's frames under different lights";
         return false;
      }
      if (!groups.empty() && SamePose(*f0.ex, groups.back().first, f.poses[0]))
         groups.back().second++;
      else
         groups.push_back({ f.poses[0], 1 });
   }
   out = f0;
   out.poses.clear();
   for (const auto& gr : groups)
   {
      LightPose p = gr.first;
      p.w = static_cast<double>(gr.second) / static_cast<double>(frames.size());
      out.poses.push_back(p);
   }
   return true;
}

// ---- the host's clock map ----

double MapGrid::At(double u, double w) const
{
   const double fx = (u - x0) / h, fy = (w - y0) / h;
   const double ix = std::floor(fx), iy = std::floor(fy);
   if (!(ix >= -1 && ix < nx && iy >= -1 && iy < ny))
      return 0.0;
   const int32_t i = static_cast<int32_t>(ix), j = static_cast<int32_t>(iy);
   const double tx = fx - ix, ty = fy - iy;
   auto node = [&](int32_t a, int32_t b) {
      return a < 0 || a >= nx || b < 0 || b >= ny ? 0.0 : v[static_cast<size_t>(b) * nx + static_cast<size_t>(a)];
   };
   const double v00 = node(i, j), v10 = node(i + 1, j), v01 = node(i, j + 1), v11 = node(i + 1, j + 1);
   const double a = v00 + (v10 - v00) * tx;
   const double b = v01 + (v11 - v01) * tx;
   return a + (b - a) * ty;
}

double MapDepth::At(double z) const
{
   const size_t n = v.size();
   if (!n)
      return 1.0;
   const double f = (z - z0) / dz;
   if (!(f > 0))
      return v[0];
   if (!(f < static_cast<double>(n - 1)))
      return v[n - 1];
   const double k = std::floor(f);
   const size_t i = static_cast<size_t>(k);
   return v[i] + (v[i + 1] - v[i]) * (f - k);
}

double ClockMapView::Term(const MapTerm& t, double x, double y, double z) const
{
   double g = 1.0;
   if (t.grid >= 0)
   {
      const double dz = z - t.zp;
      g = grids[static_cast<size_t>(t.grid)].At(x - t.ox - dz * t.sx, y - t.oy - dz * t.sy);
   }
   const double d = t.depth >= 0 ? depths[static_cast<size_t>(t.depth)].At(z) : 1.0;
   return t.scale * g * d;
}

double ClockMapView::Eval(const MapField& f, double x, double y, double z, double c) const
{
   double s0 = c;
   for (const MapTerm& t : f)
      s0 += Term(t, x, y, z);
   return s0;
}

double ClockMapView::Clock(int st, double x, double y, double z) const
{
   const MapClock& c = s[static_cast<size_t>(st)];
   return Eval(c.total, x, y, z, c.totalC);
}

double ClockMapView::Rate(int st, double x, double y, double z) const
{
   const MapClock& c = s[static_cast<size_t>(st)];
   return Eval(c.rate, x, y, z, c.rateC);
}

double ClockMapView::ClockAt(int st, double x, double y, double z, double t) const
{
   const MapClock& c = s[static_cast<size_t>(st)];
   return Clock(st, x, y, z) + Rate(st, x, y, z) * (t - c.tOpen);
}

double ClockMapView::Hazard(int st, double x, double y, double z, double t) const
{
   const MapClock& c = s[static_cast<size_t>(st)];
   double H = 0;
   for (const MapStep& p : c.steps)
   {
      if (!(t > p.t0))
         break;
      const double inc = Eval(p.field, x, y, z, p.c);
      H += t >= p.t1 ? inc : inc * ((t - p.t0) / (p.t1 - p.t0));
   }
   if (t > c.tOpen)
      H += Rate(st, x, y, z) * (t - c.tOpen);
   return H;
}

void ClockMapView::Encode()
{
   std::vector<double>& b = buffer;
   b.clear();
   b.push_back(ISC_CLOCK_MAP_FORMAT);
   b.push_back(static_cast<double>(grids.size()));
   b.push_back(static_cast<double>(depths.size()));
   for (const MapGrid& g : grids)
   {
      b.push_back(g.x0);
      b.push_back(g.y0);
      b.push_back(g.h);
      b.push_back(g.nx);
      b.push_back(g.ny);
      b.insert(b.end(), g.v.begin(), g.v.end());
   }
   for (const MapDepth& d : depths)
   {
      b.push_back(d.z0);
      b.push_back(d.dz);
      b.push_back(static_cast<double>(d.v.size()));
      b.insert(b.end(), d.v.begin(), d.v.end());
   }
   auto field = [&](const MapField& f) {
      b.push_back(static_cast<double>(f.size()));
      for (const MapTerm& t : f)
      {
         const double v[ISC_CLOCK_TERM] = { static_cast<double>(t.grid), static_cast<double>(t.depth), t.ox, t.oy, t.zp,
                                            t.sx, t.sy, t.scale };
         b.insert(b.end(), v, v + ISC_CLOCK_TERM);
      }
   };
   for (const MapClock& c : s)
   {
      b.push_back(c.mode);
      b.push_back(static_cast<double>(c.epochs.size()));
      for (const MapEpoch& e : c.epochs)
      {
         b.push_back(e.tStart);
         b.insert(b.end(), e.kin, e.kin + ISC_KIN_COUNT);
      }
      b.push_back(static_cast<double>(c.bound.size()));
      for (const MapField& f : c.bound)
         field(f);
      field(c.total);
      b.push_back(c.totalC);
      field(c.rate);
      b.push_back(c.rateC);
      b.push_back(c.tOpen);
      b.push_back(static_cast<double>(c.steps.size()));
      for (const MapStep& p : c.steps)
      {
         b.push_back(p.t0);
         b.push_back(p.t1);
         b.push_back(p.c);
         field(p.field);
      }
   }
}

// ---- the history ----

void LightHistory::SparseGrid::Add(const Footprint& f, double a, double a2)
{
   if (!f.nx || !f.ny)
      return;
   const long fi1 = f.i0 + f.nx - 1, fj1 = f.j0 + f.ny - 1;
   if (Empty())
   {
      i0 = f.i0;
      j0 = f.j0;
      i1 = fi1;
      j1 = fj1;
   }
   else
   {
      i0 = std::min(i0, f.i0);
      j0 = std::min(j0, f.j0);
      i1 = std::max(i1, fi1);
      j1 = std::max(j1, fj1);
   }
   for (long cy = FloorDiv(f.j0, kChunk); cy <= FloorDiv(fj1, kChunk); ++cy)
      for (long cx = FloorDiv(f.i0, kChunk); cx <= FloorDiv(fi1, kChunk); ++cx)
      {
         const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
         Chunk& ch = chunks[key];
         if (ch.g.empty())
         {
            ch.g.assign(kChunkNodes, 0.0);
            ch.g2.assign(kChunkNodes, 0.0f);
         }
         const long ja = std::max(f.j0, cy * kChunk), jb = std::min(fj1, cy * kChunk + kChunk - 1);
         const long ia = std::max(f.i0, cx * kChunk), ib = std::min(fi1, cx * kChunk + kChunk - 1);
         for (long j = ja; j <= jb; ++j)
         {
            const double* src = f.v.data() + static_cast<size_t>(j - f.j0) * f.nx + static_cast<size_t>(ia - f.i0);
            const size_t row = static_cast<size_t>(j - cy * kChunk) * kChunk;
            for (long i = ia; i <= ib; ++i, ++src)
            {
               const size_t k = row + static_cast<size_t>(i - cx * kChunk);
               ch.g[k] += a * *src;
               ch.g2[k] += static_cast<float>(a2 * (*src * *src));
            }
         }
      }
}

double LightHistory::SparseGrid::G(long i, long j) const
{
   const long cx = FloorDiv(i, kChunk), cy = FloorDiv(j, kChunk);
   auto it = chunks.find((static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy));
   return it == chunks.end() ? 0.0
                             : it->second.g[static_cast<size_t>(j - cy * kChunk) * kChunk + static_cast<size_t>(i - cx * kChunk)];
}

double LightHistory::SparseGrid::G2(long i, long j) const
{
   const long cx = FloorDiv(i, kChunk), cy = FloorDiv(j, kChunk);
   auto it = chunks.find((static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy));
   return it == chunks.end() ? 0.0
                             : it->second.g2[static_cast<size_t>(j - cy * kChunk) * kChunk + static_cast<size_t>(i - cx * kChunk)];
}

void LightHistory::SetReference(Chapter& c, const LightState& light)
{
   c.lit = true;
   c.lightKey = light.lightKey;
   c.refEnv = light.env;
   c.refTotal = light.total;
   c.ex = light.ex;
   c.flat = FlatBeam(*light.ex);
   c.sheared = ShearedBeam(*light.ex);
   c.tanS = light.ex->TanS();
   c.cosI = light.ex->CosI();
   c.F = light.ex->F();
   c.h = c.flat ? kLightTileUm : LightLatticeUm(*light.ex);
   c.labels.reset();   // (a dark chapter's came from its env)
}

bool LightHistory::SameChapter(const Chapter& c, const LightState& light)
{
   return c.envKey == light.envKey && (!light.lit || !c.lit || c.lightKey == light.lightKey);
}

bool LightHistory::SamePoses(const Chapter& c, const std::vector<LightPose>& a, const std::vector<LightPose>& b)
{
   return a.size() == 1 && b.size() == 1 && SamePose(*c.ex, a[0], b[0]);
}

std::shared_ptr<const LightHistory::Footprint> LightHistory::MakeFootprint(const Chapter& c,
                                                                           const std::vector<LightPose>& poses)
{
   auto fp = std::make_shared<Footprint>();
   const double R = c.flat ? 0.0 : c.ex->ReachUm(kLightReach), Rx = R / c.cosI;
   std::vector<std::array<long, 4>> rects;
   long i0 = LONG_MAX, j0 = LONG_MAX, i1 = LONG_MIN, j1 = LONG_MIN;
   for (const LightPose& p : poses)
   {
      std::array<long, 4> r;
      if (c.flat)
         StopNodes(p, r[0], r[1], r[2], r[3]);
      else
      {
         const double au = PoseU(*c.ex, p);
         r = { NodeAtOrAbove(au - Rx, c.h), NodeAtOrAbove(p.ay - R, c.h), NodeAtOrBelow(au + Rx, c.h),
               NodeAtOrBelow(p.ay + R, c.h) };
      }
      rects.push_back(r);
      if (r[2] >= r[0] && r[3] >= r[1])
      {
         i0 = std::min(i0, r[0]);
         j0 = std::min(j0, r[1]);
         i1 = std::max(i1, r[2]);
         j1 = std::max(j1, r[3]);
      }
   }
   if (i1 < i0 || j1 < j0)
      return fp;
   fp->i0 = i0;
   fp->j0 = j0;
   fp->nx = static_cast<int>(i1 - i0 + 1);
   fp->ny = static_cast<int>(j1 - j0 + 1);
   fp->v.assign(static_cast<size_t>(fp->nx) * fp->ny, 0.0);
   const bool one = poses.size() == 1;
   for (size_t k = 0; k < poses.size(); ++k)
   {
      const LightPose& p = poses[k];
      const std::array<long, 4>& r = rects[k];
      if (r[2] < r[0] || r[3] < r[1])
         continue;
      const double au = c.flat ? 0.0 : PoseU(*c.ex, p);
      ParallelFor(static_cast<unsigned>(r[3] - r[1] + 1), [&](unsigned jj) {
         const long j = r[1] + static_cast<long>(jj);
         double* row = fp->v.data() + static_cast<size_t>(j - fp->j0) * fp->nx;
         const double dy = NodeAt(j, c.h) - p.ay;
         for (long i = r[0]; i <= r[2]; ++i)
         {
            const double f = c.flat ? 1.0 : c.ex->ProfileAt((NodeAt(i, c.h) - au) * c.cosI, dy);
            double& v = row[i - fp->i0];
            v = one ? f : v + p.w * f;
         }
      });
   }
   return fp;
}

void LightHistory::Merge(const Record& r)
{
   Chapter& c = chapters_[r.chapter];
   const double dt = r.t1 - r.t0;
   c.mergedSec += dt;
   if (r.lit && r.fp)
      c.grid.Add(*r.fp, r.mag * dt, r.mag * r.mag * dt);
}

void LightHistory::CloseOpen()
{
   if (!haveOpen_)
      return;
   recent_.push_back(std::move(open_));
   open_ = Record();
   haveOpen_ = false;
   ++version_;
   size_t n = 0;
   if (recent_.size() > static_cast<size_t>(kLightRecent))
      n = kLightMergeBatch;
   else if (recent_.front().t1 < T_ - 2 * kLightRecentSec)
      while (n < recent_.size() && recent_[n].t1 < T_ - kLightRecentSec)
         ++n;
   for (size_t i = 0; i < n; ++i)
      Merge(recent_[i]);
   recent_.erase(recent_.begin(), recent_.begin() + static_cast<long>(n));
}

void LightHistory::Advance(double dtSec, const LightState& light)
{
   if (!(dtSec > 0) || (light.lit && !light.ex))
      return;
   std::lock_guard<std::mutex> g(mutex_);
   if (chapters_.empty() || !SameChapter(chapters_.back(), light))
   {
      CloseOpen();
      Chapter c;
      c.tStart = T_;
      c.envKey = light.envKey;
      c.env = light.env;
      chapters_.push_back(std::move(c));
      ++version_;
   }
   const size_t ci = chapters_.size() - 1;
   Chapter& c = chapters_.back();
   if (light.lit && !c.lit)
      SetReference(c, light);
   const double mag = light.lit ? light.total / c.refTotal : 0.0;
   const double t1 = T_ + dtSec;
   if (haveOpen_ && open_.chapter == ci && open_.lit == light.lit &&
       (!light.lit || (open_.mag == mag && SamePoses(c, open_.poses, light.poses))))
   {
      open_.t1 = t1;
   }
   else
   {
      CloseOpen();
      open_.t0 = T_;
      open_.t1 = t1;
      open_.chapter = ci;
      open_.lit = light.lit;
      open_.mag = mag;
      open_.poses = light.poses;
      open_.fp = light.lit ? MakeFootprint(c, light.poses) : nullptr;
      haveOpen_ = true;
      ++version_;
   }
   T_ = t1;
}

bool LightHistory::LabelsOf(Chapter& c, const ScopeSpec& cur, const std::string& curKey, std::string& err)
{
   if (c.labels && c.labelsKey == curKey)
      return true;
   auto labels = std::make_shared<std::vector<LabelPhysics>>();
   if (!ScopeEnvLabels(cur, c.lit ? c.refEnv : c.env, *labels, err))
      return false;
   c.labels = labels;
   c.labelsKey = curKey;
   return true;
}

std::shared_ptr<const ClockMapView> LightHistory::BuildMap(const ScopeSpec& cur, const LightState& light, double x0,
                                                           double y0, double x1, double y1, double z0, double z1,
                                                           std::string& err)
{
   std::lock_guard<std::mutex> g(mutex_);
   return Build(cur, light, x0, y0, x1, y1, z0, z1, err);
}

std::shared_ptr<const ClockMapView> LightHistory::Build(const ScopeSpec& cur, const LightState& light, double x0,
                                                        double y0, double x1, double y1, double z0, double z1,
                                                        std::string& err)
{
   if (light.lit && !light.ex)
   {
      err = "a lit light without its excitation";
      return nullptr;
   }
   z0 = std::max(z0, -2.0);
   z1 = std::min(z1, 20.0);
   if (!(z1 >= z0))
      z1 = z0;

   // The chapters the movie sees: the history's, the last with the light's reference if the light makes it lit, or a
   // new one the light starts.
   const bool sameChapter = !chapters_.empty() && SameChapter(chapters_.back(), light);
   std::vector<Chapter*> chs;
   for (Chapter& c : chapters_)
      chs.push_back(&c);
   Chapter virt;
   if (!sameChapter)
   {
      virt.tStart = T_;
      virt.envKey = light.envKey;
      virt.env = light.env;
      if (light.lit)
         SetReference(virt, light);
      chs.push_back(&virt);
   }
   else if (light.lit && !chapters_.back().lit)
   {
      virt = chapters_.back();   // a dark chapter: no grid
      SetReference(virt, light);
      chs.back() = &virt;
   }
   const size_t lightCh = chs.size() - 1;
   const Chapter& lc = *chs[lightCh];
   const double mag = light.lit ? light.total / lc.refTotal : 0.0;
   const bool extends = haveOpen_ && sameChapter && open_.chapter == lightCh && open_.lit == light.lit &&
                        (!light.lit || (open_.mag == mag && SamePoses(lc, open_.poses, light.poses)));

   // The memo.
   const KineticEnv curEnv = ScopeKineticEnv(cur);
   std::string curKey = "sp" + Num(ScopeSpecGet(cur, "specimen")) + ";";
   for (const auto& kv : curEnv)
      curKey += kv.first + '=' + Num(kv.second) + ';';
   std::string key = Num(static_cast<double>(version_)) + "|" + (extends ? std::string("x") : Num(T_)) + "|" + curKey +
                     "|" + Num(x0) + "," + Num(y0) + "," + Num(x1) + "," + Num(y1) + "," + Num(z0) + "," + Num(z1) +
                     "|" + (light.lit ? "L" : "D") + light.envKey + "|" + light.lightKey + "|" + Num(light.total);
   for (const LightPose& p : light.poses)
      key += "|" + Num(p.ax) + "," + Num(p.ay) + "," + Num(p.pivot) + "," + Num(p.x0) + "," + Num(p.y0) + "," +
             Num(p.x1) + "," + Num(p.y1) + "," + Num(p.w);
   if (memo_ && key == memoKey_)
      return memo_;

   // The labels: the current spec's, and each chapter's light's under it.
   std::vector<LabelPhysics> curLabels;
   if (!ScopeEnvLabels(cur, curEnv, curLabels, err))
      return nullptr;
   for (Chapter* c : chs)
      if (!LabelsOf(*c, cur, curKey, err))
         return nullptr;
   const std::vector<int> mounted = ScopeMountedStructures(cur);

   // The records: the closed ones (the open one too when the light is another), and the rate's.
   std::vector<const Record*> closed;
   for (const Record& r : recent_)
      closed.push_back(&r);
   Record rateRec;
   const Record* rate = nullptr;
   double tOpen = T_;
   if (extends)
   {
      if (open_.lit)
         rate = &open_;
      tOpen = open_.t0;
   }
   else
   {
      if (haveOpen_)
         closed.push_back(&open_);   // its t1 is T
      if (light.lit)
      {
         rateRec.t0 = T_;
         rateRec.t1 = T_;
         rateRec.chapter = lightCh;
         rateRec.lit = true;
         rateRec.mag = mag;
         rateRec.poses = light.poses;
         rateRec.fp = MakeFootprint(lc, light.poses);
         rate = &rateRec;
      }
   }

   auto view = std::make_shared<ClockMapView>();
   ClockMapView& m = *view;
   m.T = T_;
   m.s.assign(ISC_STRUCT_COUNT, MapClock());

   // The grids, each once: a chapter's merged G (and G2), a record's footprint (and its square), over the box in the
   // chapter's coordinates.
   auto nodeRect = [&](const Chapter& c, long& ia, long& ja, long& ib, long& jb) {
      double ua = x0, ub = x1;
      if (c.sheared)
      {
         ua = std::min(x0 - z0 * c.tanS, x0 - z1 * c.tanS);
         ub = std::max(x1 - z0 * c.tanS, x1 - z1 * c.tanS);
      }
      ia = NodeAtOrBelow(ua, c.h) - 1;
      ib = NodeAtOrAbove(ub, c.h) + 1;
      ja = NodeAtOrBelow(y0, c.h) - 1;
      jb = NodeAtOrAbove(y1, c.h) + 1;
   };
   std::map<std::pair<const void*, int>, int32_t> gridOf;
   auto chapterGrid = [&](const Chapter& c, bool sq) -> int32_t {
      const std::pair<const void*, int> k(&c, sq ? 1 : 0);
      auto it = gridOf.find(k);
      if (it != gridOf.end())
         return it->second;
      int32_t idx = -1;
      long ia, ja, ib, jb;
      nodeRect(c, ia, ja, ib, jb);
      ia = std::max(ia, c.grid.i0);
      ja = std::max(ja, c.grid.j0);
      ib = std::min(ib, c.grid.i1);
      jb = std::min(jb, c.grid.j1);
      if (!c.grid.Empty() && ib >= ia && jb >= ja)
      {
         MapGrid gr;
         gr.x0 = NodeAt(ia, c.h);
         gr.y0 = NodeAt(ja, c.h);
         gr.h = c.h;
         gr.nx = static_cast<int32_t>(ib - ia + 1);
         gr.ny = static_cast<int32_t>(jb - ja + 1);
         gr.v.resize(static_cast<size_t>(gr.nx) * gr.ny);
         for (long j = ja; j <= jb; ++j)
            for (long i = ia; i <= ib; ++i)
               gr.v[static_cast<size_t>(j - ja) * gr.nx + static_cast<size_t>(i - ia)] =
                  sq ? c.grid.G2(i, j) : c.grid.G(i, j);
         idx = static_cast<int32_t>(m.grids.size());
         m.grids.push_back(std::move(gr));
      }
      gridOf[k] = idx;
      return idx;
   };
   auto recordGrid = [&](const Record& r, bool sq) -> int32_t {
      const std::pair<const void*, int> k(r.fp.get(), sq ? 1 : 0);
      auto it = gridOf.find(k);
      if (it != gridOf.end())
         return it->second;
      int32_t idx = -1;
      const Footprint& f = *r.fp;
      const Chapter& c = *chs[r.chapter];
      long ia, ja, ib, jb;
      nodeRect(c, ia, ja, ib, jb);
      ia = std::max(ia, f.i0);
      ja = std::max(ja, f.j0);
      ib = std::min(ib, f.i0 + f.nx - 1);
      jb = std::min(jb, f.j0 + f.ny - 1);
      if (f.nx && f.ny && ib >= ia && jb >= ja)
      {
         MapGrid gr;
         gr.x0 = NodeAt(ia, c.h);
         gr.y0 = NodeAt(ja, c.h);
         gr.h = c.h;
         gr.nx = static_cast<int32_t>(ib - ia + 1);
         gr.ny = static_cast<int32_t>(jb - ja + 1);
         gr.v.resize(static_cast<size_t>(gr.nx) * gr.ny);
         for (long j = ja; j <= jb; ++j)
            for (long i = ia; i <= ib; ++i)
            {
               const double v = f.v[static_cast<size_t>(j - f.j0) * f.nx + static_cast<size_t>(i - f.i0)];
               gr.v[static_cast<size_t>(j - ja) * gr.nx + static_cast<size_t>(i - ia)] = sq ? v * v : v;
            }
         idx = static_cast<int32_t>(m.grids.size());
         m.grids.push_back(std::move(gr));
      }
      gridOf[k] = idx;
      return idx;
   };
   // TIRF's depth factor of a structure's main state in a chapter (and its square), on kLightDepthStepUm.
   std::map<std::pair<std::pair<const void*, int>, int>, int32_t> depthOf;
   auto depthFor = [&](const Chapter& c, int st, bool sq) -> int32_t {
      if (!c.lit || !c.ex->Tirf())
         return -1;
      const auto k = std::make_pair(std::make_pair(static_cast<const void*>(&c), st), sq ? 1 : 0);
      auto it = depthOf.find(k);
      if (it != depthOf.end())
         return it->second;
      ExcitationShares sh;
      double dMax = 0;
      for (const StatePhysics::LineShare& l : (*c.labels)[static_cast<size_t>(st)].main.lineShares)
      {
         sh.push_back({ l.nm, l.share });
         dMax = std::max(dMax, c.ex->DecayUm(l.nm));
      }
      MapDepth d;
      d.z0 = 0;
      d.dz = kLightDepthStepUm;
      const double top = std::max(kLightDepthStepUm, std::min(std::max(z1, 0.0), 30.0 * dMax));
      const size_t n = static_cast<size_t>(std::ceil(top / d.dz)) + 1;
      for (size_t i = 0; i < n; ++i)
      {
         const double v = c.ex->Depth(static_cast<double>(i) * d.dz, sh);
         d.v.push_back(sq ? v * v : v);
      }
      const int32_t idx = static_cast<int32_t>(m.depths.size());
      m.depths.push_back(std::move(d));
      depthOf[k] = idx;
      return idx;
   };

   for (int st = 0; st < ISC_STRUCT_COUNT; ++st)
   {
      MapClock& mc = m.s[static_cast<size_t>(st)];
      const LabelPhysics& L = curLabels[static_cast<size_t>(st)];
      const bool on = !L.off && std::find(mounted.begin(), mounted.end(), st) != mounted.end();
      mc.tOpen = tOpen;
      auto labelOf = [&](size_t ci) -> const LabelPhysics& { return (*chs[ci]->labels)[static_cast<size_t>(st)]; };
      if (!on || L.mode == MODE_DNA_PAINT || (L.mode != MODE_DSTORM && L.mode != MODE_WIDEFIELD && L.mode != MODE_PALM))
      {
         // GLOBAL: T and the chapters' rows (an off target: its own).
         mc.mode = ISC_CLOCK_GLOBAL;
         mc.rateC = 1;
         if (!on)
         {
            MapEpoch e;
            KinOf(L, e.kin);
            mc.epochs.push_back(e);
            continue;
         }
         for (size_t ci = 0; ci < chs.size(); ++ci)
         {
            MapEpoch e;
            e.tStart = mc.epochs.empty() ? 0.0 : chs[ci]->tStart;
            e.chapter = static_cast<int>(ci);
            KinOf(labelOf(ci), e.kin);
            if (mc.epochs.empty() || !SameKin(e.kin, mc.epochs.back().kin))
               mc.epochs.push_back(e);
         }
         continue;
      }
      if (L.mode != MODE_PALM)
      {
         // DOSE: epochs of the lit chapters that excite the structure, equal rows in one.
         mc.mode = ISC_CLOCK_DOSE;
         std::vector<int> epochOf(chs.size(), -1);
         for (size_t ci = 0; ci < chs.size(); ++ci)
         {
            const LabelPhysics& C = labelOf(ci);
            if (!chs[ci]->lit || !C.main.emits || !(C.main.excitationPerSec > 0))
               continue;
            MapEpoch e;
            KinOf(C, e.kin);
            if (mc.epochs.empty() || !SameKin(e.kin, mc.epochs.back().kin))
            {
               e.tStart = mc.epochs.empty() ? 0.0 : chs[ci]->tStart;
               e.xRef = C.main.excitationPerSec;
               e.chapter = static_cast<int>(ci);
               mc.epochs.push_back(e);
            }
            epochOf[ci] = static_cast<int>(mc.epochs.size()) - 1;
         }
         if (mc.epochs.empty())
         {
            MapEpoch e;
            KinOf(L, e.kin);
            mc.epochs.push_back(e);
         }
         // A term per merged grid and record, in time order (each epoch's bound is the terms before it).
         auto term = [&](size_t ci, int32_t grid, double factor) {
            const Chapter& c = *chs[ci];
            const double dscale = labelOf(ci).main.excitationPerSec / mc.epochs[static_cast<size_t>(epochOf[ci])].xRef;
            MapTerm t;
            t.grid = grid;
            t.depth = depthFor(c, st, false);
            t.sx = c.sheared ? c.tanS : 0.0;
            t.scale = c.F * dscale * factor;
            return t;
         };
         std::vector<int> termEpoch;
         size_t ri = 0;
         for (size_t ci = 0; ci < chs.size(); ++ci)
         {
            const bool counts = epochOf[ci] >= 0;
            if (counts)
            {
               const int32_t gi = chapterGrid(*chs[ci], false);
               if (gi >= 0)
               {
                  mc.total.push_back(term(ci, gi, 1.0));
                  termEpoch.push_back(epochOf[ci]);
               }
            }
            for (; ri < closed.size() && closed[ri]->chapter == ci; ++ri)
            {
               const Record& r = *closed[ri];
               if (!counts || !r.lit || !r.fp)
                  continue;
               const int32_t gi = recordGrid(r, false);
               if (gi >= 0)
               {
                  mc.total.push_back(term(ci, gi, r.mag * (r.t1 - r.t0)));
                  termEpoch.push_back(epochOf[ci]);
               }
            }
         }
         for (size_t e = 1; e < mc.epochs.size(); ++e)
         {
            MapField b;
            for (size_t k = 0; k < mc.total.size(); ++k)
               if (termEpoch[k] < static_cast<int>(e))
                  b.push_back(mc.total[k]);
            mc.bound.push_back(std::move(b));
         }
         if (rate && epochOf[rate->chapter] >= 0)
         {
            const int32_t gi = recordGrid(*rate, false);
            if (gi >= 0)
               mc.rate.push_back(term(rate->chapter, gi, rate->mag));
         }
         continue;
      }
      // ACTIVATION (PALM): the hazard's steps and rate; blinks in T with the chapters' rows (their activation rates
      // unused).
      mc.mode = ISC_CLOCK_ACTIVATION;
      for (size_t ci = 0; ci < chs.size(); ++ci)
      {
         MapEpoch e;
         e.tStart = mc.epochs.empty() ? 0.0 : chs[ci]->tStart;
         e.chapter = static_cast<int>(ci);
         KinOf(labelOf(ci), e.kin);
         if (mc.epochs.empty() || !SameKin(e.kin, mc.epochs.back().kin, ISC_KIN_ON_SEC))
            mc.epochs.push_back(e);
      }
      auto lightTerms = [&](size_t ci, int32_t g1, int32_t g2, double a, MapField& f) {
         const Chapter& c = *chs[ci];
         const LabelPhysics& C = labelOf(ci);
         if (!c.lit)
            return;
         const double sx = c.sheared ? c.tanS : 0.0;
         if (C.actLin != 0 && g1 >= 0)
         {
            MapTerm t;
            t.grid = g1;
            t.depth = depthFor(c, st, false);
            t.sx = sx;
            t.scale = C.actLin * c.F * a;
            f.push_back(t);
         }
         if (C.actQuad != 0 && g2 >= 0)
         {
            MapTerm t;
            t.grid = g2;
            t.depth = depthFor(c, st, true);
            t.sx = sx;
            t.scale = C.actQuad * (c.F * c.F) * (a * a);
            f.push_back(t);
         }
      };
      double mergedSec = 0;
      for (Chapter* c : chs)
         mergedSec += c->mergedSec;
      if (mergedSec > 0)
      {
         MapStep p;
         p.t0 = 0;
         p.t1 = closed.empty() ? tOpen : closed.front()->t0;
         for (size_t ci = 0; ci < chs.size(); ++ci)
         {
            const Chapter& c = *chs[ci];
            p.c += labelOf(ci).actSpont * c.mergedSec;
            if (!c.grid.Empty())
            {
               // G and G2 hold mag and mag^2 already.
               const LabelPhysics& C = labelOf(ci);
               const int32_t g1 = C.actLin != 0 ? chapterGrid(c, false) : -1;
               const int32_t g2 = C.actQuad != 0 ? chapterGrid(c, true) : -1;
               lightTerms(ci, g1, g2, 1.0, p.field);
            }
         }
         if (p.t1 > p.t0)
            mc.steps.push_back(std::move(p));
      }
      for (const Record* rp : closed)
      {
         const Record& r = *rp;
         const LabelPhysics& C = labelOf(r.chapter);
         MapStep p;
         p.t0 = r.t0;
         p.t1 = r.t1;
         const double dt = r.t1 - r.t0;
         p.c = C.actSpont * dt;
         if (r.lit && r.fp)
         {
            // lin x mag dt, quad x mag^2 dt over the footprint and its square.
            const int32_t g1 = C.actLin != 0 ? recordGrid(r, false) : -1;
            const int32_t g2 = C.actQuad != 0 ? recordGrid(r, true) : -1;
            const Chapter& c = *chs[r.chapter];
            const double sx = c.sheared ? c.tanS : 0.0;
            if (g1 >= 0)
            {
               MapTerm t;
               t.grid = g1;
               t.depth = depthFor(c, st, false);
               t.sx = sx;
               t.scale = C.actLin * c.F * r.mag * dt;
               p.field.push_back(t);
            }
            if (g2 >= 0)
            {
               MapTerm t;
               t.grid = g2;
               t.depth = depthFor(c, st, true);
               t.sx = sx;
               t.scale = C.actQuad * (c.F * c.F) * (r.mag * r.mag) * dt;
               p.field.push_back(t);
            }
         }
         mc.steps.push_back(std::move(p));
      }
      const size_t rci = rate ? rate->chapter : lightCh;
      mc.rateC = labelOf(rci).actSpont;
      if (rate)
      {
         const LabelPhysics& C = labelOf(rate->chapter);
         const int32_t g1 = C.actLin != 0 ? recordGrid(*rate, false) : -1;
         const int32_t g2 = C.actQuad != 0 ? recordGrid(*rate, true) : -1;
         lightTerms(rate->chapter, g1, g2, rate->mag, mc.rate);
      }
   }
   m.Encode();
   memoKey_ = key;
   memo_ = view;
   return view;
}

double LightHistory::T() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return T_;
}

double LightHistory::DoseAt(double xUm, double yUm) const
{
   std::lock_guard<std::mutex> g(mutex_);
   double ref = 0;
   for (const Chapter& c : chapters_)
      if (c.lit)
      {
         ref = c.refTotal;
         break;
      }
   if (!(ref > 0))
      return 0.0;
   double dose = 0;
   for (size_t ci = 0; ci < chapters_.size(); ++ci)
   {
      const Chapter& c = chapters_[ci];
      if (!c.lit)
         continue;
      double v = c.grid.Empty() ? 0.0 : Bilinear([&](long i, long j) { return c.grid.G(i, j); }, xUm, yUm, c.h);
      for (const Record& r : recent_)
         if (r.chapter == ci && r.lit && r.fp)
            v += r.mag * (r.t1 - r.t0) * FootprintAt(*r.fp, xUm, yUm, c.h);
      if (haveOpen_ && open_.chapter == ci && open_.lit && open_.fp)
         v += open_.mag * (T_ - open_.t0) * FootprintAt(*open_.fp, xUm, yUm, c.h);
      dose += c.F * (c.refTotal / ref) * v;
   }
   return dose;
}

void LightHistory::Reset()
{
   std::lock_guard<std::mutex> g(mutex_);
   T_ = 0;
   ++version_;
   chapters_.clear();
   recent_.clear();
   haveOpen_ = false;
   open_ = Record();
   memoKey_.clear();
   memo_.reset();
}

size_t LightHistory::Bytes() const
{
   std::lock_guard<std::mutex> g(mutex_);
   size_t b = 0;
   for (const Chapter& c : chapters_)
      b += sizeof(Chapter) + c.grid.chunks.size() * (kChunkNodes * (sizeof(double) + sizeof(float)) + 64);
   for (const Record& r : recent_)
      b += sizeof(Record) + (r.fp ? r.fp->Bytes() : 0) + r.poses.size() * sizeof(LightPose);
   if (haveOpen_)
      b += sizeof(Record) + (open_.fp ? open_.fp->Bytes() : 0);
   if (memo_)
      b += memo_->buffer.capacity() * sizeof(double) * 2;   // the buffer and the grids it copies
   return b;
}

size_t LightHistory::Chapters() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return chapters_.size();
}

size_t LightHistory::Records() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return recent_.size() + (haveOpen_ ? 1 : 0);
}

} // namespace sim
