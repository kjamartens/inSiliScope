// The clock map: see clockmap.h.
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "clockmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace isc {

namespace {

// FNV-1a over the bits of doubles and integers (as SetKineticsHistory's).
struct Fnv {
   uint64_t h = 0xcbf29ce484222325ull;
   void Bits(uint64_t u)
   {
      for (int byte = 0; byte < 8; byte++) { h ^= (u >> (8 * byte)) & 0xFF; h *= 0x100000001b3ull; }
   }
   void D(double v)
   {
      uint64_t u;
      std::memcpy(&u, &v, sizeof u);
      Bits(u);
   }
   uint64_t Done() const { return h ? h : 1; }
};

bool Finite(double v) { return std::isfinite(v); }

bool KinOk(const Kinetics& q)
{
   return Finite(q.activationRatePerSec) && q.activationRatePerSec >= 0 && Finite(q.onSec) && q.onSec > 0 &&
          Finite(q.offSec) && q.offSec >= 0 && Finite(q.bleachProb) && q.bleachProb > 0 && Finite(q.photonCV) &&
          q.photonCV >= 0 && Finite(q.initialOnSec) && q.initialOnSec >= 0;
}

void MixKin(Fnv& f, const Kinetics& q)
{
   f.D(q.activationRatePerSec); f.D(q.onSec); f.D(q.offSec);
   f.D(q.bleachProb); f.D(q.photonCV); f.D(q.initialOnSec);
}

} // namespace

double ClockGrid::At(double u, double w) const
{
   const double fx = (u - x0) / h, fy = (w - y0) / h;
   const double ix = std::floor(fx), iy = std::floor(fy);
   if (!(ix >= -1 && ix < nx && iy >= -1 && iy < ny)) return 0.0;   // (NaN too)
   const int32_t i = (int32_t)ix, j = (int32_t)iy;
   const double tx = fx - ix, ty = fy - iy;
   auto node = [&](int32_t a, int32_t b) {
      return a < 0 || a >= nx || b < 0 || b >= ny ? 0.0 : v[(size_t)b * (size_t)nx + (size_t)a];
   };
   const double v00 = node(i, j), v10 = node(i + 1, j), v01 = node(i, j + 1), v11 = node(i + 1, j + 1);
   const double a = v00 + (v10 - v00) * tx;
   const double b = v01 + (v11 - v01) * tx;
   return a + (b - a) * ty;
}

double ClockDepth::At(double z) const
{
   const size_t n = v.size();
   if (!n) return 1.0;
   const double f = (z - z0) / dz;
   if (!(f > 0)) return v[0];
   if (!(f < (double)(n - 1))) return v[n - 1];
   const double k = std::floor(f);
   const size_t i = (size_t)k;
   return v[i] + (v[i + 1] - v[i]) * (f - k);
}

double ClockMap::Term(const ClockTerm& t, double x, double y, double z) const
{
   double g = 1.0;
   if (t.grid >= 0) {
      const double dz = z - t.zp;
      g = grids[(size_t)t.grid].At(x - t.ox - dz * t.sx, y - t.oy - dz * t.sy);
   }
   const double d = t.depth >= 0 ? depths[(size_t)t.depth].At(z) : 1.0;
   return t.scale * g * d;
}

double ClockMap::Eval(const ClockField& f, double x, double y, double z, double c) const
{
   double s = c;
   for (const ClockTerm& t : f) s += Term(t, x, y, z);
   return s;
}

void ClockMap::Bounds(const ClockField& f, double x0, double x1, double y0, double y1, double z0, double z1, double c,
                      double& lo, double& hi) const
{
   lo = hi = c;
   for (const ClockTerm& t : f) {
      double glo = 1, ghi = 1;
      if (t.grid >= 0) {
         const ClockGrid& g = grids[(size_t)t.grid];
         // u = x - ox - (z - zp) sx over the box: the shear's extremes at the box's z ends.
         const double ea = (z0 - t.zp) * t.sx, eb = (z1 - t.zp) * t.sx;
         const double fa = (z0 - t.zp) * t.sy, fb = (z1 - t.zp) * t.sy;
         const double u0 = x0 - t.ox - std::max(ea, eb), u1 = x1 - t.ox - std::min(ea, eb);
         const double w0 = y0 - t.oy - std::max(fa, fb), w1 = y1 - t.oy - std::min(fa, fb);
         // The nodes of every cell the box touches (those outside the grid count as 0).
         const double i0 = std::floor((u0 - g.x0) / g.h), i1 = std::floor((u1 - g.x0) / g.h) + 1;
         const double j0 = std::floor((w0 - g.y0) / g.h), j1 = std::floor((w1 - g.y0) / g.h) + 1;
         if (!(i1 >= 0 && i0 < g.nx && j1 >= 0 && j0 < g.ny)) {
            glo = ghi = 0;
         } else {
            const int32_t a0 = (int32_t)std::max(0.0, i0), a1 = (int32_t)std::min((double)g.nx - 1, i1);
            const int32_t b0 = (int32_t)std::max(0.0, j0), b1 = (int32_t)std::min((double)g.ny - 1, j1);
            const bool outside = i0 < 0 || i1 > g.nx - 1 || j0 < 0 || j1 > g.ny - 1;
            glo = outside ? 0.0 : std::numeric_limits<double>::infinity();
            ghi = outside ? 0.0 : -std::numeric_limits<double>::infinity();
            for (int32_t j = b0; j <= b1; j++) {
               const double* row = g.v.data() + (size_t)j * (size_t)g.nx;
               for (int32_t i = a0; i <= a1; i++) {
                  glo = std::min(glo, row[i]);
                  ghi = std::max(ghi, row[i]);
               }
            }
         }
      }
      double dlo = 1, dhi = 1;
      if (t.depth >= 0) {
         const ClockDepth& d = depths[(size_t)t.depth];
         if (!d.v.empty()) {
            dlo = std::min(d.At(z0), d.At(z1));
            dhi = std::max(d.At(z0), d.At(z1));
            for (size_t k = 0; k < d.v.size(); k++) {
               const double zk = d.z0 + (double)k * d.dz;
               if (zk > z0 && zk < z1) { dlo = std::min(dlo, d.v[k]); dhi = std::max(dhi, d.v[k]); }
            }
         }
      }
      double p[4] = { t.scale * glo * dlo, t.scale * glo * dhi, t.scale * ghi * dlo, t.scale * ghi * dhi };
      lo += *std::min_element(p, p + 4);
      hi += *std::max_element(p, p + 4);
   }
   // Rounding: the per-dye sums may differ from these by a few ulps.
   const double m = 1e-9 * (std::fabs(lo) + std::fabs(hi) + 1e-300);
   lo -= m;
   hi += m;
}

const char* ClockMap::Finish()
{
   std::vector<uint64_t> gh(grids.size()), dh(depths.size());
   for (size_t i = 0; i < grids.size(); i++) {
      const ClockGrid& g = grids[i];
      if (!(g.nx >= 1 && g.ny >= 1 && Finite(g.x0) && Finite(g.y0) && Finite(g.h) && g.h > 0) ||
          g.v.size() != (size_t)g.nx * (size_t)g.ny)
         return "a clock grid is malformed";
      Fnv f;
      f.D(g.x0); f.D(g.y0); f.D(g.h); f.Bits((uint64_t)g.nx); f.Bits((uint64_t)g.ny);
      for (double v : g.v) {
         if (!Finite(v)) return "a clock grid holds a value that is not finite";
         f.D(v);
      }
      gh[i] = f.Done();
   }
   for (size_t i = 0; i < depths.size(); i++) {
      const ClockDepth& d = depths[i];
      if (!(Finite(d.z0) && Finite(d.dz) && d.dz > 0)) return "a depth table is malformed";
      Fnv f;
      f.D(d.z0); f.D(d.dz); f.Bits((uint64_t)d.v.size());
      for (double v : d.v) {
         if (!Finite(v)) return "a depth table holds a value that is not finite";
         f.D(v);
      }
      dh[i] = f.Done();
   }
   auto mixField = [&](Fnv& f, const ClockField& fld) -> const char* {
      f.Bits((uint64_t)fld.size());
      for (const ClockTerm& t : fld) {
         if (t.grid < -1 || t.grid >= (int32_t)grids.size() || t.depth < -1 || t.depth >= (int32_t)depths.size())
            return "a clock term names a grid or depth table that is not there";
         if (!(Finite(t.ox) && Finite(t.oy) && Finite(t.zp) && Finite(t.sx) && Finite(t.sy) && Finite(t.scale)))
            return "a clock term holds a value that is not finite";
         f.Bits(t.grid >= 0 ? gh[(size_t)t.grid] : 0);
         f.Bits(t.depth >= 0 ? dh[(size_t)t.depth] : 0);
         f.D(t.ox); f.D(t.oy); f.D(t.zp); f.D(t.sx); f.D(t.sy); f.D(t.scale);
      }
      return nullptr;
   };
   for (StructClock& c : s) {
      if (c.mode < CLOCK_GLOBAL || c.mode > CLOCK_ACTIVATION) return "unknown clock mode";
      const KineticsHistory& h = c.hist;
      if (!h.Size() || h.kin.size() != h.Size()) return "a clock needs its kinetics (one row per epoch)";
      for (size_t i = 0; i < h.Size(); i++) {
         if (!KinOk(h.kin[i])) return "bad kinetics in a clock";
         if (!Finite(h.tStart[i]) || (i == 0 ? h.tStart[i] != 0 : !(h.tStart[i] > h.tStart[i - 1])))
            return "epoch starts must be 0, then increasing";
      }
      if (!(Finite(c.tOpen) && c.tOpen >= 0 && Finite(c.totalC) && Finite(c.rateC))) return "bad clock constants";
      // The kinetics as an ABI 11 history in T (SetKineticsHistory's fingerprint).
      Fnv ft;
      for (size_t i = 0; i < h.Size(); i++) {
         ft.D(h.tStart[i]);
         MixKin(ft, h.kin[i]);
      }
      c.fpHistT = ft.Done();
      // The schedules: GLOBAL and a one-epoch DOSE clock as an ABI 11 history of their kinetics (the same
      // fingerprint: they share the blocks' cached schedules); DOSE with epochs adds its bounds.
      Fnv fs;
      if (c.mode == CLOCK_DOSE && h.Size() > 1) {
         fs.Bits(0x444f5345ull);   // "DOSE"
         if (c.bound.size() != h.Size() - 1) return "a dose clock needs one bound per epoch after the first";
         for (const Kinetics& q : h.kin) MixKin(fs, q);
         for (const ClockField& b : c.bound)
            if (const char* e = mixField(fs, b)) return e;
      } else {
         if (!c.bound.empty()) return "a dose clock needs one bound per epoch after the first (other clocks none)";
         if (c.mode == CLOCK_ACTIVATION) fs.Bits(0x41435456ull);   // "ACTV": blinks from the activation
         for (size_t i = 0; i < h.Size(); i++) {
            fs.D(c.mode == CLOCK_DOSE ? 0.0 : h.tStart[i]);
            MixKin(fs, h.kin[i]);
         }
      }
      c.fpSchedule = fs.Done();
      Fnv fn;
      fn.D(c.tOpen); fn.D(c.totalC); fn.D(c.rateC);
      if (const char* e = mixField(fn, c.total)) return e;
      if (const char* e = mixField(fn, c.rate)) return e;
      c.stepFp.clear();
      if (c.mode == CLOCK_ACTIVATION) {
         uint64_t chain = 0x5354455053ull;   // "STEPS"
         double t = 0;
         for (const ClockStep& st : c.steps) {
            if (!(Finite(st.t0) && Finite(st.t1) && Finite(st.c) && st.t0 == t && st.t1 > st.t0))
               return "activation steps must run contiguously from 0";
            t = st.t1;
            Fnv f;
            f.Bits(chain); f.D(st.t0); f.D(st.t1); f.D(st.c);
            if (const char* e = mixField(f, st.field)) return e;
            chain = f.Done();
            c.stepFp.push_back(chain);
         }
         if (!c.steps.empty() && t != c.tOpen) return "activation steps must end at tOpen";
         fn.Bits(chain);
      } else if (!c.steps.empty()) {
         return "only an activation clock has steps";
      }
      c.fpNow = fn.Done();
   }
   return nullptr;
}

double ActivationInSteps(const ClockMap& m, const StructClock& c, double x, double y, double z, double E, size_t& from,
                         double& H)
{
   for (size_t i = from; i < c.steps.size(); i++) {
      const ClockStep& st = c.steps[i];
      const double inc = m.Eval(st.field, x, y, z, st.c);
      const double Hn = H + inc;
      if (inc > 0 && E <= Hn) {
         const double t = st.t0 + (st.t1 - st.t0) * ((E - H) / inc);
         return std::min(st.t1, std::max(st.t0, t));
      }
      H = Hn;
      from = i + 1;
   }
   return std::numeric_limits<double>::quiet_NaN();
}

} // namespace isc
