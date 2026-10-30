///////////////////////////////////////////////////////////////////////////////
// FILE:          Slide.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The fluorescent slide (Slide.h).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Slide.h"
#include "DyeLibrary.h"
#include "Parallel.h"
#include "Spectra.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sim {

namespace {

constexpr double kLn2 = 0.6931471805599453;
constexpr double kAvogadroPerMicroMUm3 = 6.02214076e23 * 1e-6 * 1e-15;   // molecules per um^3 per uM

} // namespace

const std::vector<std::string>& SlideDyeNames()
{
   static const std::vector<std::string> v = [] {
      std::vector<std::string> n = { "Mix" };
      for (const std::string& id : DyeIds())
         n.push_back(id);
      return n;
   }();
   return v;
}

bool SlidePhotophysics(const SlideOptions& o, const LightPath& lp, double pixelUm, std::vector<SlideComponent>& out,
                       std::string& err)
{
   out.clear();
   if (o.dye < 0 || o.dye >= static_cast<int>(SlideDyeNames().size()))
   {
      err = "slide-dye " + std::to_string(o.dye) + ": not one of 0.." + std::to_string(SlideDyeNames().size() - 1);
      return false;
   }
   // The dyes as (id, state, the excitation peak the half-time is read at): Mix = TetraSpeck's components as equivalent
   // dyes (data/fiducials/types.json), else the library dye's main state.
   struct One { std::string id; StateData st; double exPeakNm; };
   std::vector<One> dyes;
   if (o.dye == 0)
   {
      const int t = IndexOf(FiducialTypeIds(), "TetraSpeck");
      if (t < 0)
      {
         err = "slide: no TetraSpeck type in data/fiducials";
         return false;
      }
      const FiducialTypeData& T = FiducialTypeAt(t);
      for (int ci = 0; ci < T.componentCount; ++ci)
      {
         const FiducialComponentData& c = FiducialComponentAt(T.firstComponent + ci);
         const Spectrum* em = SpectrumByKey(c.em);
         if (!SpectrumByKey(c.ex) || !em)
         {
            err = std::string("slide: fiducial component ") + c.id + ": no spectra";
            return false;
         }
         double emMaxNm = 0, best = -1;
         for (int i = 0; i < kSpectraGridN; i++)
            if ((*em)[static_cast<size_t>(i)] > best)
            {
               best = (*em)[static_cast<size_t>(i)];
               emMaxNm = SpectraGridNm(i);
            }
         dyes.push_back({ c.id, { true, 0.0, emMaxNm, FiducialEquivalentExtCoeff(), FiducialEquivalentQy(), false, c.ex,
                                  c.em, nullptr, false, 0.0, 0.0, 0.0, 0.0 }, c.exPeakNm });
      }
   }
   else
   {
      const DyeData& d = DyeAt(o.dye - 1);
      dyes.push_back({ d.id, d.main, d.main.parametric ? d.main.exPeakNm : d.main.exMaxNm });
   }
   const double perUm3 = std::max(0.0, o.concMicroM) * kAvogadroPerMicroMUm3;
   for (const One& d : dyes)
   {
      StatePhysics st;
      if (!StatePhotophysics(d.st, lp, st, err))
         return false;
      if (!st.emits || !(st.detectedPerSec > 0))
         continue;
      Spectrum ex, em;
      if (!StateSpectra(d.st, ex, em, err))
         return false;
      const double kEmRef = d.st.qy * CrossSectionUm2(d.st.extCoeff, ex, d.exPeakNm) *
                            PhotonFluxPerUm2(kSlideBleachRefKWcm2, d.exPeakNm);
      SlideComponent c;
      c.id = d.id;
      c.perPxUmSec = perUm3 * pixelUm * pixelUm * st.detectedPerSec;
      c.bleachPerSec = o.halfSec > 0 && kEmRef > 0 ? st.emissionPerSec / kEmRef * kLn2 / o.halfSec : 0.0;
      c.lambdaNm = st.lambdaNm;
      for (const StatePhysics::LineShare& l : st.lineShares)
         c.lineShares.push_back({ l.nm, l.share });
      out.push_back(c);
   }
   return true;
}

std::string SlideLightPresetId(int dye)
{
   const std::string fallback = "Slide-561";
   if (dye <= 0 || dye >= static_cast<int>(SlideDyeNames().size()))
      return fallback;
   const DyeData& d = DyeAt(dye - 1);
   int mode = MODE_WIDEFIELD;
   if (!d.modes[mode].present)
      mode = IndexOf(DyeModeNames(), d.defaultMode ? d.defaultMode : "");
   const LightPresetData* q = mode >= 0 && d.modes[mode].lightPreset ? FindLightPreset(d.modes[mode].lightPreset) : nullptr;
   if (!q)
      return fallback;
   for (const std::string& id : LightPresetIds())
   {
      const LightPresetData* s = id.compare(0, 6, "Slide-") ? nullptr : FindLightPreset(id);
      if (s && !std::strcmp(s->dichroic, q->dichroic) && !std::strcmp(s->emissionFilter, q->emissionFilter))
         return id;
   }
   return fallback;
}

LightPast LightPastAt(double doseNow, double doseOther, double rate, double peakRate, double F)
{
   LightPast p;
   if (!(peakRate > 0))
      return p;   // dark now: nothing shows, no past needed
   const double r = rate / peakRate, bl = std::min(1.0, std::max(0.0, (r - 1e-3) / (1e-2 - 1e-3)));
   if (bl > 0)
      p.sec = bl * (doseNow / rate);
   const double other = (bl < 1 ? (1 - bl) * doseNow : 0.0) + doseOther;
   if (other > 0)
      p.exc = other * F / peakRate;
   return p;
}

void RenderSlide(std::vector<float>& img, const SlideMovie& M, long f)
{
   const size_t n = static_cast<size_t>(M.W) * M.H;
   const double T = M.expSec, tf = f * T;
   for (const SlideMovie::Comp& C : M.comps)
   {
      const SlideMovie::Geometry& G = M.geos[C.geo];
      const double k = C.c.bleachPerSec;
      ParallelFor(M.H, [&](unsigned y) {
         for (size_t i = static_cast<size_t>(y) * M.W, e = i + M.W; i < e; ++i)
         {
            const float b = C.base[i];
            if (!(b > 0))
               continue;
            double s = 1.0;
            if (k > 0)
            {
               // The column's mean survival over [t, t + T], each height weighted by its excitation (its emission).
               const double t = M.t0Sec + tf + (M.pastSec.empty() ? 0.0 : M.pastSec[i]);
               const double past = M.pastExc.empty() ? 0.0 : k * M.pastExc[i];
               double num = 0.0, den = 0.0;
               for (int j = 0; j < G.nodes; ++j)
               {
                  const double a = G.a[static_cast<size_t>(j) * n + i];
                  if (!(a > 0))
                     continue;
                  const double h = k * a, hT = h * T;
                  const double q = hT > 1e-9 ? -std::expm1(-hT) / hT : 1.0;
                  num += a * std::exp(-past - h * t) * q;
                  den += a;
               }
               s = den > 0 ? num / den : 1.0;
            }
            img[i] += static_cast<float>(b * s);
         }
      });
   }
}

} // namespace sim
