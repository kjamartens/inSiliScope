///////////////////////////////////////////////////////////////////////////////
// FILE:          bfp_maps_check.cpp
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   ctest `bfp_maps`: the BFP light maps (BfpExpr.h, BfpMaps.h,
//                BfpField.h; spec/ALGORITHM.md "BFP light maps").
//                  Parser:   statements, operators, functions, ';' arguments,
//                            locals, comments; the errors name the line.
//                  Images:   a TIFF map = the same equation's values.
//                  Field:    one wide coherent spot (epi, HILO, TIRF) = its
//                            analytic light: F(u) x the Gaussian envelope of
//                            its Fourier width, sheared by tan theta_s, x the
//                            evanescent decay; unpolarized = the mean of s and
//                            p; a tilt moves it by f tan(tilt); an incoherent
//                            spot = the coherent one; a focus angle linear in
//                            |u| focuses at f tan(angle per u) above the focal
//                            plane; the scan shortcuts (scanX, scanRot) = the
//                            same light computed slot by slot.
//                  Excitation: the maps as the Excitation's light (its
//                            map mode): the one-spot map = the legacy beam's
//                            light; the light history's planes (their tents)
//                            = the cycle mean at the planes; the plane mean
//                            prepared = made point by point; a stepped cycle's
//                            states average to the mean.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BfpExpr.h"
#include "BfpField.h"
#include "BfpMaps.h"
#include "Excitation.h"
#include "SimPattern.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_fail = 0;
constexpr double kPi = 3.14159265358979323846;

void Check(bool ok, const std::string& what)
{
   if (!ok)
   {
      std::printf("FAIL: %s\n", what.c_str());
      ++g_fail;
   }
}

std::string Num(double v)
{
   char b[64];
   std::snprintf(b, sizeof(b), "%.4g", v);
   return b;
}

double Run(const std::string& text, const char* map, double ux = 0.3, double uy = -0.2, double t = 0.25)
{
   BfpProgram p;
   std::string err;
   if (!p.Compile(text, err))
   {
      std::printf("  compile error: %s\n", err.c_str());
      return std::nan("");
   }
   double vars[kBfpVars] = {};
   vars[BV_UX] = ux;
   vars[BV_UY] = uy;
   vars[BV_R] = std::hypot(ux, uy);
   vars[BV_PHI] = std::atan2(uy, ux);
   vars[BV_T] = t;
   vars[BV_PI] = kPi;
   vars[BV_BFP] = 0.7;
   vars[BV_UNPOLARIZED] = kBfpUnpolarized;
   double maps[kBfpMaps];
   std::vector<double> sc;
   p.Run(vars, maps, sc, nullptr);
   const auto& names = BfpMapNames();
   for (size_t k = 0; k < names.size(); ++k)
      if (names[k] == map)
         return maps[k];
   return std::nan("");
}

void Parser()
{
   const double ux = 0.3, uy = -0.2;
   Check(std::fabs(Run("I = 1 + 2*3 - 4/8", "I") - 6.5) < 1e-15, "precedence");
   Check(std::fabs(Run("I = 2^3^2", "I") - 512.0) < 1e-12, "^ right associative");
   Check(std::fabs(Run("I = -2^2", "I") + 4.0) < 1e-15, "-2^2 = -(2^2)");
   Check(std::fabs(Run("a = ux*2; I = a + uy", "I") - (ux * 2 + uy)) < 1e-15, "locals");
   Check(std::fabs(Run("I = max(1; 3, 2)", "I") - 3.0) < 1e-15, "';' and ',' between arguments");
   Check(std::fabs(Run("# a comment\nI = hypot(ux, uy) # another\n\ntiltX = 2", "I") - std::hypot(ux, uy)) < 1e-15,
         "comments and newlines");
   Check(Run("I = (ux > 0) && (uy < 0) || 0", "I") == 1.0, "comparisons and logic");
   Check(Run("I = if(ux < 0; 5; 7)", "I") == 7.0, "if");
   Check(std::fabs(Run("I = spot(ux - 0.3, uy + 0.2, 0.1)", "I") - 1.0) < 1e-15, "spot at its centre");
   Check(std::fabs(Run("I = mod(-0.25, 1)", "I") - 0.75) < 1e-15, "mod is floored");
   Check(Run("pol = unpolarized", "pol") == kBfpUnpolarized, "unpolarized");
   Check(Run("I = 1", "coherent") == 1.0 && Run("I = 1", "duty") == 1.0 && Run("I = 1", "pol") == kBfpUnpolarized,
         "defaults");
   Check(Run("I = 1", "tiltX") == 0.0, "default tilt");
   struct Bad
   {
      const char* text;
      const char* expect;
   };
   const Bad bad[] = {
      { "I = 1 +", "line 1" },        { "I = foo(1)", "unknown function" }, { "I = bar", "unknown name" },
      { "ux = 1", "cannot be set" },  { "I = 1\nJ = (2", "line 2" },        { "I = max()", "takes" },
      { "I = img(1)", "img" },        { "I = 1 2", "expected" },
   };
   for (const Bad& b : bad)
   {
      BfpProgram p;
      std::string err;
      const bool ok = p.Compile(b.text, err);
      Check(!ok && err.find(b.expect) != std::string::npos,
            std::string("error for '") + b.text + "': '" + err + "' (expected '" + b.expect + "')");
   }
   // DependsOn.
   {
      BfpProgram p;
      std::string err;
      p.Compile("a = t*2; scanRot = 360*a; I = exp(-ux^2); tiltX = nm", err);
      const std::vector<int> none;
      Check(p.DependsOn(BM_SCAN_ROT, none) == BD_T && p.DependsOn(BM_I, none) == BD_U && p.DependsOn(BM_TILT_X, none) == BD_NM &&
               p.DependsOn(BM_PHASE, none) == 0,
            "DependsOn");
   }
}

// A float32 TIFF of `pages` pages (little-endian, one strip per page).
std::vector<uint8_t> WriteTiff(int w, int h, int pages, const std::vector<float>& v)
{
   std::vector<uint8_t> b;
   auto u16 = [&](uint32_t x) {
      b.push_back(static_cast<uint8_t>(x));
      b.push_back(static_cast<uint8_t>(x >> 8));
   };
   auto u32 = [&](uint32_t x) {
      for (int k = 0; k < 4; ++k)
         b.push_back(static_cast<uint8_t>(x >> (8 * k)));
   };
   b.push_back('I');
   b.push_back('I');
   u16(42);
   u32(8);
   const uint32_t entries = 9, ifdSize = 2 + entries * 12 + 4, page = static_cast<uint32_t>(w * h * 4);
   for (int p = 0; p < pages; ++p)
   {
      const uint32_t ifd = static_cast<uint32_t>(b.size()), data = ifd + ifdSize;
      const uint32_t next = p + 1 < pages ? data + page : 0;
      u16(entries);
      auto entry = [&](uint32_t tag, uint32_t type, uint32_t count, uint32_t value) {
         u16(tag);
         u16(type);
         u32(count);
         if (type == 3)
         {
            u16(value);
            u16(0);
         }
         else
            u32(value);
      };
      entry(256, 4, 1, static_cast<uint32_t>(w));
      entry(257, 4, 1, static_cast<uint32_t>(h));
      entry(258, 3, 1, 32);
      entry(259, 3, 1, 1);
      entry(262, 3, 1, 1);
      entry(273, 4, 1, data);
      entry(277, 3, 1, 1);
      entry(279, 4, 1, page);
      entry(339, 3, 1, 3);
      u32(next);
      const float* src = v.data() + static_cast<size_t>(p) * w * h;
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(src);
      b.insert(b.end(), bytes, bytes + page);
   }
   return b;
}

void Images()
{
   const int n = 64, pages = 2;
   const double umax = 1.4 / 1.337;
   std::vector<float> v(static_cast<size_t>(n) * n * pages);
   for (int p = 0; p < pages; ++p)
      for (int j = 0; j < n; ++j)
         for (int i = 0; i < n; ++i)
         {
            const double ux = -umax + (i + 0.5) * 2 * umax / n, uy = umax - (j + 0.5) * 2 * umax / n;
            v[(static_cast<size_t>(p) * n + j) * n + i] = static_cast<float>(ux + 2.0 * uy + 10.0 * p);
         }
   const std::vector<uint8_t> tif = WriteTiff(n, n, pages, v);
   BfpImage im;
   std::string err;
   Check(BfpReadTiff(tif.data(), tif.size(), im, err) && im.w == n && im.h == n && im.pages == pages, "TIFF read: " + err);
   // Bilinear between pixel centres: a linear map comes back exactly (inside the centres' square).
   double worst = 0;
   for (double ux : { -0.5, 0.0, 0.31, 0.77 })
      for (double uy : { -0.6, 0.1, 0.42 })
         for (double t : { 0.2, 0.7 })
            worst = std::max(worst, std::fabs(im.Sample(ux, uy, t, umax) - (ux + 2 * uy + (t < 0.5 ? 0 : 10))));
   Check(worst < 1e-5, "TIFF map = the equation (" + Num(worst) + ")");
   Check(im.Sample(1.2, 0, 0.2, umax) == 0.0, "0 outside the pupil square");
   BfpRegisterImage("check_map.tif", tif.data(), tif.size());
   BfpProgram p;
   Check(p.Compile("I = img(\"check_map.tif\")", err) && p.Images().size() == 1, "img() compiles");
   auto found = BfpFindImage("check_map.tif", err);
   Check(found && found->pages == pages, "the store finds it");
   Check(p.DependsOn(BM_I, { pages }) == (BD_U | BD_T) && p.DependsOn(BM_I, { 1 }) == BD_U, "a stack depends on t");
   BfpRegisterImage("check_map.tif", nullptr, 0);
   Check(!BfpFindImage("check_map.tif", err), "removed from the store");
}

BfpLightParams Base()
{
   BfpLightParams p;
   p.lines = { 640 };
   p.lambdaRefNm = 640;
   p.na = 1.4;
   p.nS = 1.337;
   p.nI = 1.518;
   p.fObjUm = 2000;
   p.envFwhmUm = 12.8;
   p.profile = 1;
   p.fovSpanUm = 12.8;
   p.pivotUm = 0.5;
   p.zSampleUm = 6;
   return p;
}

std::shared_ptr<const BfpLight> Make(BfpLightParams p, const std::string& text, double* sec = nullptr)
{
   p.text = text;
   std::string err;
   const auto t0 = std::chrono::steady_clock::now();
   auto L = BfpLight::Make(p, err);
   if (sec)
      *sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   Check(L != nullptr, "BfpLight::Make: " + err);
   return L;
}

const char* kSpot = "I = exp(-2*((ux - bfp)^2 + uy^2)/w^2)\n";

void Field()
{
   const double sig = 12.8 / 2.3548;
   // F(u) of a beam at u: the unpolarized single beam's (Excitation.h).
   // (NA 1.49: the TIRF spot clear of the critical angle and the pupil's edge.)
   auto Fof = [](double u) {
      Beam b;
      b.ux = u;
      b.polDeg = kUnpolarizedDeg;
      const BeamField s = BeamFieldOf(b, 640, 1.337, 1.518, 1.49, 0, true, false);
      const BeamField q = BeamFieldOf(b, 640, 1.337, 1.518, 1.49, 0, false, true);
      return 0.5 * (s.Intensity() + q.Intensity());
   };
   for (double u0 : { 0.0, 0.7, 1.06 })
   {
      BfpLightParams p = Base();
      p.na = 1.49;
      p.bfp = u0;
      double sec = 0;
      auto L = Make(p, std::string(kSpot) + "coherent = 1; pol = unpolarized", &sec);
      if (!L)
         continue;
      std::printf("  spot at u %.2f: %.3f s, %s\n", u0, sec, L->Info().c_str());
      const BfpLightField& M = L->Mean(0);
      const double F = Fof(u0);
      const bool ev = u0 > 1.0;
      const double tn = ev ? 0.0 : u0 / std::sqrt(1 - u0 * u0);
      const double d = ev ? 0.640 / (4 * kPi * 1.337 * std::sqrt(u0 * u0 - 1)) : 0.0;
      // (a) F(u0) x the envelope (propagating: the spot is narrow in u); (b) an independent sum of the same angular
      // spectrum, sampled at w / 4 directly (every case, the evanescent decays' spread included).
      double worstA = 0, worstB = 0;
      const double K = 2 * kPi * 1.337 / 0.640, ws = 12.8 / 1.1774, w = 2 / (K * ws);
      struct Pw
      {
         double ux, uy, a, kz, kap;
         std::complex<double> e[2][3];
      };
      std::vector<Pw> pw;
      double raw = 0;
      const double dt = w / 4;
      for (int j = -20; j <= 20; ++j)
         for (int i = -20; i <= 20; ++i)
         {
            Pw q;
            q.ux = u0 + i * dt;
            q.uy = j * dt;
            const double r = std::hypot(q.ux, q.uy);
            if (r > 1.49 / 1.337)
               continue;
            const double I = std::exp(-2 * ((q.ux - u0) * (q.ux - u0) + q.uy * q.uy) / (w * w));
            raw += I * dt * dt;
            q.a = std::sqrt(I) * dt * dt;
            q.kz = r < 1 ? std::sqrt(1 - r * r) : 0;
            q.kap = r > 1 ? std::sqrt(r * r - 1) : 0;
            const double phi = std::atan2(q.uy, q.ux);
            for (int run = 0; run < 2; ++run)
            {
               Beam b;
               b.ux = q.ux;
               b.uy = q.uy;
               const double a = run * kPi / 2, g = BeamFrameSign(phi);
               b.polDeg = std::atan2(g * std::cos(a - phi), g * std::sin(a - phi)) * 180 / kPi;
               LabField(BeamFieldOf(b, 640, 1.337, 1.518, 1.49, 0), phi, q.e[run][0], q.e[run][1], q.e[run][2]);
            }
            pw.push_back(q);
         }
      const double c = (K / (2 * kPi)) * (K / (2 * kPi)) * 2 * kPi * sig * sig / raw;
      auto direct = [&](double x, double y, double z) {
         double I = 0;
         for (int run = 0; run < 2; ++run)
            for (int k = 0; k < 3; ++k)
            {
               std::complex<double> E;
               for (const Pw& q : pw)
                  E += q.a * q.e[run][k] * std::exp(std::complex<double>(-K * q.kap * z, K * (q.ux * x + q.uy * y + q.kz * (z - 0.5))));
               I += 0.5 * std::norm(E);
            }
         return c * I;
      };
      for (double z : { 0.0, 0.05, 0.2, 2.0 })
         for (double x : { -6.0, -2.0, 0.0, 1.5, 5.0 })
            for (double y : { -4.0, 0.0, 3.0 })
            {
               const double got = M.At(x, y, z, p.pivotUm);
               if (!ev)
               {
                  const double xc = x - (z - p.pivotUm) * tn;
                  worstA = std::max(worstA, std::fabs(got - F * std::exp(-(xc * xc + y * y) / (2 * sig * sig))) / F);
               }
               worstB = std::max(worstB, std::fabs(got - direct(x, y, z)) / F);
            }
      (void)d;
      if (!ev)
         Check(worstA < 5e-3, "a wide spot at u " + Num(u0) + " = F x its envelope (worst " + Num(worstA) + ")");
      Check(worstB < 2e-3, "a wide spot at u " + Num(u0) + " = its angular spectrum summed directly (worst " + Num(worstB) + ")");
   }
   // Unpolarized = the mean of s and p (a HILO spot).
   {
      BfpLightParams p = Base();
      p.bfp = 0.8;
      auto U = Make(p, std::string(kSpot) + "pol = unpolarized");
      auto S = Make(p, std::string(kSpot) + "pol = 0");
      auto Pp = Make(p, std::string(kSpot) + "pol = 90");
      if (U && S && Pp)
      {
         double worst = 0;
         for (double x : { -3.0, 0.0, 2.0 })
            for (double z : { 0.3, 1.0 })
            {
               const double u = U->Mean(0).At(x, 0.5, z, 0.5), s = S->Mean(0).At(x, 0.5, z, 0.5), q = Pp->Mean(0).At(x, 0.5, z, 0.5);
               worst = std::max(worst, std::fabs(u - 0.5 * (s + q)) / std::max(1e-12, u));
            }
         Check(worst < 1e-4, "unpolarized = the mean of s and p (" + Num(worst) + ")");
      }
   }
   // A tilt moves the light by f tan(tilt); scanX the same; an incoherent spot = the coherent one.
   {
      BfpLightParams p = Base();
      p.bfp = 0.0;
      auto A = Make(p, std::string(kSpot));
      auto T = Make(p, std::string(kSpot) + "tiltX = 0.1");
      auto Sx = Make(p, std::string(kSpot) + "scanX = 0.1");
      auto Inc = Make(p, std::string(kSpot) + "coherent = 0");
      if (A && T && Sx && Inc)
      {
         const double sh = 2000 * std::tan(0.1 * kPi / 180);
         double wt = 0, ws = 0, wi = 0;
         for (double x : { -4.0, -1.0, 0.0, 2.5 })
            for (double y : { -2.0, 1.0 })
            {
               const double a = A->Mean(0).At(x, y, 0.5, 0.5);
               wt = std::max(wt, std::fabs(T->Mean(0).At(x + sh, y, 0.5, 0.5) - a));
               ws = std::max(ws, std::fabs(Sx->Mean(0).At(x + sh, y, 0.5, 0.5) - a));
               wi = std::max(wi, std::fabs(Inc->Mean(0).At(x, y, 0.5, 0.5) - a));
            }
         Check(wt < 3e-3, "tiltX moves the light by f tan(tilt) (" + Num(wt) + ")");
         Check(ws < 3e-3, "scanX = tiltX (" + Num(ws) + ")");
         Check(wi < 3e-3, "an incoherent wide spot = the coherent one (" + Num(wi) + ")");
      }
   }
   // Focus: a disc whose focus angle grows linearly with |u| focuses at f tan(angle per u) above the focal plane.
   {
      BfpLightParams p = Base();
      p.zSampleUm = 8;
      const double dz = 3.0, k = std::atan(dz / 2000.0) * 180 / kPi;   // deg per unit u
      double sec = 0;
      auto L = Make(p, "I = step(0.3 - r)\nfocus = " + Num(k) + "*r", &sec);
      if (L)
      {
         std::printf("  focused disc: %.3f s, %s\n", sec, L->Info().c_str());
         double best = -1, zb = 0;
         for (double z = -2.0; z <= 7.0; z += 0.05)
         {
            const double v = L->Mean(0).At(0, 0, z + 0.5, 0.5);
            if (v > best)
            {
               best = v;
               zb = z;
            }
         }
         Check(std::fabs(zb - dz) < 0.4, "the focus angle's waist at " + Num(zb) + " um (expected " + Num(dz) + ")");
      }
   }
   // The rotation shortcut: RingTIRF (scanRot) = the ring's spot moved round slot by slot (computed per slot).
   {
      BfpLightParams p = Base();
      p.na = 1.49;
      p.bfp = 1.06;
      p.slots = 16;
      double s1 = 0, s2 = 0;
      auto R = Make(p, BfpTypeEquations(SIM_RING_TIRF), &s1);
      auto D = Make(p, "c = cos(2*pi*t); s = sin(2*pi*t)\nI = exp(-2*((ux - bfp*c)^2 + (uy - bfp*s)^2)/w^2)\n"
                       "pol = unpolarized", &s2);
      if (R && D)
      {
         std::printf("  ring TIRF: shortcut %.3f s, per slot %.3f s\n", s1, s2);
         double worst = 0, peak = 0;
         for (double x : { -6.0, -2.0, 0.0, 3.0 })
            for (double y : { -1.0, 0.0, 4.0 })
               for (double z : { 0.0, 0.05, 0.2 })
               {
                  const double a = R->Mean(0).At(x, y, z, 0.5), b = D->Mean(0).At(x, y, z, 0.5);
                  peak = std::max(peak, b);
                  worst = std::max(worst, std::fabs(a - b));
               }
         Check(worst < 5e-3 * peak, "scanRot = the slots computed one by one (" + Num(worst / peak) + ")");
      }
   }
   // The light sheet: thin across x (its thickness / cos theta_s on a plane), long along y.
   {
      BfpLightParams p = Base();
      p.bfp = 0.9;
      double sec = 0;
      auto L = Make(p, BfpTypeEquations(SIM_LIGHT_SHEET), &sec);
      if (L)
      {
         std::printf("  light sheet: %.3f s, %s\n", sec, L->Info().c_str());
         // The half-maximum width across x at the focal plane.
         const BfpLightField& M = L->Mean(0);
         const double peak = M.At(0, 0, 0.5, 0.5);
         double lo = 0, hi = 0;
         for (double x = 0; x < 20; x += 0.01)
            if (M.At(x, 0, 0.5, 0.5) < peak / 2)
            {
               hi = x;
               break;
            }
         for (double x = 0; x > -20; x -= 0.01)
            if (M.At(x, 0, 0.5, 0.5) < peak / 2)
            {
               lo = x;
               break;
            }
         const double cs = std::sqrt(1 - 0.81), fw = hi - lo;
         Check(std::fabs(fw - 3.0 / cs) < 0.05 * 3.0 / cs, "the sheet's width on a plane " + Num(fw) + " (" + Num(3.0 / cs) + ")");
      }
   }
}

// The maps in the Excitation (its map mode, Excitation.h).
void InExcitation()
{
   ExcitationSpec es;
   es.profile = ILLUM_GAUSSIAN;
   es.fwhmPct = 100;
   es.fovSpanUm = 12.8;
   es.bfp = 0.6;
   es.pivotUm = 0.5;
   es.lambdaRefNm = 640;
   es.lineNm = { 640 };
   const ExcitationShares sh{ { 640, 1.0 } };
   BfpLightParams p = Base();
   p.profile = ILLUM_GAUSSIAN;
   p.bfp = es.bfp;
   const Excitation legacy(es);
   es.maps = Make(p, BfpTypeEquations(SIM_MAPS));
   if (!es.maps)
      return;
   const Excitation mapped(es);
   Check(mapped.Mapped() && mapped.Period() == 1 && !mapped.Clipped(), "the map mode is on");
   // The one-spot map (a wide spot: an envelope of the beam's width) = the legacy beam's light, near the focus, but
   // for the legacy's stretch: the legacy beam lands as P((x - (z - z_p) tan theta_s) cos theta_i, y) (a round beam
   // projected onto the plane), the map's round spot as its Fourier transform, round on the plane: P(x - ..., y).
   double peak = 0, worst = 0;
   const double ci = legacy.CosI(), ts = legacy.TanS();
   for (double z : { 0.3, 0.5, 1.0 })
      for (double x = -6; x <= 6; x += 1.5)
         for (double y : { -3.0, 0.0, 2.0 })
         {
            const double xs = (z - es.pivotUm) * ts;
            const double a = legacy.MeanAt(xs + (x - xs) / ci, y, z, sh), b = mapped.MeanAt(x, y, z, sh);
            peak = std::max(peak, a);
            worst = std::max(worst, std::fabs(a - b));
         }
   Check(worst < 0.03 * peak, "the one-spot map = the legacy beam (" + Num(worst / peak) + " of the peak)");
   // The light history's components: at each plane's own height the tents sum to the cycle mean there.
   double wt = 0;
   int planes = 0;
   for (int j = 0; j < mapped.ComponentCount(); ++j)
   {
      const Excitation::Component& c = mapped.ComponentAt(j);
      if (!c.depthTable || c.zMid < 0.0 || c.zMid > 3.0)
         continue;
      ++planes;
      for (double x : { -4.0, 0.0, 3.0 })
      {
         double sum = 0;
         for (int k = 0; k < mapped.ComponentCount(); ++k)
            sum += mapped.ComponentLateral(k, x, 1.0, c.zMid) * mapped.ComponentDepth(k, c.zMid, sh);
         wt = std::max(wt, std::fabs(sum - mapped.MeanAt(x, 1.0, c.zMid, sh)));
      }
   }
   Check(planes > 0 && wt < 1e-3 * peak, "the history's tents = the mean at their planes (" + Num(wt / peak) + ", " +
                                            std::to_string(planes) + " planes)");
   // The plane mean prepared once = point by point.
   Excitation::PlaneMeanPrep prep;
   mapped.PreparePlaneMean(0.4, 0.5, 0.05, sh, prep);
   double wp = 0;
   for (double x = -5; x <= 5; x += 0.37)
      wp = std::max(wp, std::fabs(mapped.PlaneMeanAt(x, 0.7, prep) - mapped.PlaneMean(x, 0.7, 0.4, 0.5, 0.05, sh)));
   Check(wp < 2e-3 * peak, "the plane mean prepared = made point by point (" + Num(wp / peak) + ")");
   // A stepped cycle: two states (the sheet moved by a scan, one per frame) average to the mean.
   BfpLightParams q = p;
   q.slots = 2;
   q.cycleFrames = 2;
   ExcitationSpec e2 = es;
   e2.maps = Make(q, std::string(kSpot) + "coherent = 0; scanX = if(t < 0.5; 0.1; -0.1)");
   if (!e2.maps)
      return;
   Excitation st(e2);
   Check(st.Period() == 2 && st.Varies(), "two states");
   double ws = 0, diff = 0;
   for (double x = -5; x <= 5; x += 1.0)
   {
      st.SetFrame(0);
      const double a = st.At(x, 0.5, 0.5, sh);
      st.SetFrame(1);
      const double b = st.At(x, 0.5, 0.5, sh);
      diff = std::max(diff, std::fabs(a - b));
      ws = std::max(ws, std::fabs(0.5 * (a + b) - st.MeanAt(x, 0.5, 0.5, sh)));
   }
   Check(diff > 0.05 * peak && ws < 1e-4 * peak, "the states differ (" + Num(diff / peak) + ") and average to the mean (" +
                                                    Num(ws / peak) + ")");
   // The imager background's map: finite and positive under the light.
   std::vector<ImagerSource> im{ { 1.0, sh } };
   const std::vector<double> bg = mapped.ImagerMap(32, 32, 0.4, 6.2, 6.2, 20.0, im);
   bool okBg = !bg.empty();
   for (double v : bg)
      okBg = okBg && std::isfinite(v) && v >= 0.0;
   Check(okBg && bg[16 * 32 + 16] > 0.0, "the imager background's map");
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   Parser();
   Images();
   Field();
   InExcitation();
   const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   std::printf("bfp_maps: %s (%d failure%s), %.2f s\n", g_fail ? "FAILED" : "ok", g_fail, g_fail == 1 ? "" : "s", sec);
   return g_fail ? 1 : 0;
}
