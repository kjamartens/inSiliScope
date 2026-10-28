// scope_setup -- see scope_setup.h. LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "scope_setup.h"

#include "ScopeResolved.h"

#include "Excitation.h"

#include "insiliscope/insiliscope.h"

namespace probes {

void SpectrumKey(Json& j, const char* key, const sim::Spectrum& v)
{
   j.Key(key);
   j.Array(v.data(), v.size());
}

void StateJson(Json& j, const sim::StateData& sd, const sim::StatePhysics& st, const sim::LightPath& lp)
{
   j.Open('{');
   j.KBool("emits", st.emits);
   if (st.emits)
   {
      j.KNum("excitation_per_s", st.excitationPerSec);
      j.KNum("emission_per_s", st.emissionPerSec);
      j.KNum("detected_fraction", st.detectedFraction);
      j.KNum("lambda_nm", st.lambdaNm);
      j.KNum("kernel_lambda_nm", sim::KernelWavelengthNm(st.lambdaNm));
      j.KNum("detected_per_s", st.detectedPerSec);
      j.KNum("ext_coeff", sd.extCoeff);
      j.KNum("qy", sd.qy);
      j.KStr("color", st.color ? st.color : "");
      sim::Spectrum ex, em;
      std::string e;
      if (sim::StateSpectra(sd, ex, em, e))
      {
         // The detected spectrum: emission x dichroic x emission filter x QE
         // (Spectra.h Detect's integrand; detected_fraction = its sum / em's).
         sim::Spectrum det(em.size());
         for (size_t i = 0; i < em.size(); ++i)
            det[i] = em[i] * lp.dichroicT[i] * lp.emissionT[i] * lp.qe[i];
         SpectrumKey(j, "ex", ex);
         SpectrumKey(j, "em", em);
         SpectrumKey(j, "detected", det);
      }
   }
   j.Close('}');
}

namespace {

// A dye state as the library has it: whether it is there and dark, and its spectra (a parametric state's shapes, else
// the spectra its keys name; either can be missing).
void DyeStateJson(Json& j, const sim::StateData& sd)
{
   j.Open('{');
   j.KBool("present", sd.present);
   j.KBool("is_dark", sd.isDark);
   if (sd.parametric)
   {
      SpectrumKey(j, "ex", sim::ParametricExcitation(sd.exPeakNm, sd.exWidthNm));
      SpectrumKey(j, "em", sim::ParametricEmission(sd.emPeakNm, sd.emWidthNm));
   }
   else
   {
      if (const sim::Spectrum* ex = sd.ex ? sim::SpectrumByKey(sd.ex) : nullptr)
         SpectrumKey(j, "ex", *ex);
      if (const sim::Spectrum* em = sd.em ? sim::SpectrumByKey(sd.em) : nullptr)
         SpectrumKey(j, "em", *em);
   }
   j.Close('}');
}

// A state's photophysics for the readout, or null (dark, absent).
void PhysicsJson(Json& j, const sim::StatePhysics& st)
{
   if (!st.emits)
   {
      j.Null();
      return;
   }
   j.Open('{');
   j.KNum("detected_fraction", st.detectedFraction);
   j.KNum("lambda_nm", st.lambdaNm);
   j.KNum("detected_per_s", st.detectedPerSec);
   j.Close('}');
}

} // namespace

bool ViewerSetupJson(const sim::ScopeSpec& spec, std::string& out, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   const sim::LightPath& lp = R.lp;
   Json j;
   j.digits = 17;
   j.exactInf = true;
   j.Open('{');

   j.Key("light_path");
   j.Open('{');
   j.Key("lasers");
   j.Open('[');
   for (const sim::LaserLine& l : lp.lasers)
   {
      j.Open('{');
      j.KNum("nm", l.nm);
      j.KNum("kw_per_cm2", l.kWPerCm2);
      j.Close('}');
   }
   j.Close(']');
   j.KStr("excitation_filter", lp.excitationFilterName);
   j.KStr("dichroic", lp.dichroicName);
   j.KStr("emission_filter", lp.emissionFilterName);
   j.KStr("qe_name", lp.qeName);
   SpectrumKey(j, "excitation_T", lp.excitationT);
   SpectrumKey(j, "dichroic_T", lp.dichroicT);
   SpectrumKey(j, "emission_T", lp.emissionT);
   SpectrumKey(j, "qe", lp.qe);
   j.Close('}');

   sim::Excitation ex;
   std::vector<sim::ExcitationLine> sh;
   std::string ee;
   if (sim::ScopeExcitation(spec, ex, sh, ee))
   {
      j.Key("excitation");
      j.Open('{');
      j.KStr("regime", sim::IlluminationRegimeNames()[static_cast<size_t>(ex.Regime())]);
      j.KNum("u", ex.U());
      j.KNum("u_max", ex.UMax());
      j.KNum("field_factor", ex.F());
      j.Key("depth_nm");
      j.Open('{');
      for (int nm : sim::LaserLines())
         j.KNum(std::to_string(nm).c_str(), ex.DepthNm(nm));
      j.Close('}');
      j.Close('}');
   }

   std::vector<double> zern;
   if (sim::ScopeZernike(spec, zern, ee))
   {
      j.Key("zernike");   // the pupil's coefficients, OSA 0-27, waves: the preset's with any zern.<j>
      j.Array(zern.data(), zern.size());
   }

   const double pixelUm = R.p.pixelSizeNm / 1000.0;
   j.Key("labels");
   j.Open('[');
   for (size_t s = 0; s < R.labels.size(); ++s)
   {
      const sim::LabelPhysics& L = R.labels[s];
      const sim::ModeData& md = L.eff.dye.modes[L.mode];
      j.Open('{');
      j.KNum("structure", static_cast<double>(s));
      j.KBool("off", L.off);
      j.KStr("mode", sim::DyeModeNames()[static_cast<size_t>(L.mode)]);
      // an off target has no dye (its library default is still in eff)
      j.KStr("dye_id", !L.off && L.eff.dye.id ? L.eff.dye.id : "");
      j.KStr("dye_name", !L.off && L.eff.dye.name ? L.eff.dye.name : "");
      j.KBool("generic", md.present && md.generic);
      j.Key("label_vector");
      j.Array(L.label.data(), L.label.size());
      j.KNum("on_s_now", L.onSecNow);
      j.KNum("initial_on_s", L.initialOnSec);
      j.KNum("excitation_scale", L.excitationScale);
      j.KNum("k_act_per_s", L.kActPerSec);
      j.KNum("imager_background_per_px_per_s", L.ImagerBackgroundPerPxPerSec(pixelUm));
      j.Key("main");
      PhysicsJson(j, L.main);
      j.Key("pre");
      PhysicsJson(j, L.pre);
      j.Key("dye_main");
      DyeStateJson(j, L.eff.dye.main);
      j.Key("dye_pre");
      DyeStateJson(j, L.eff.dye.pre);
      j.Close('}');
   }
   j.Close(']');

   j.Key("legacy_linkage");
   j.Open('{');
   for (int t = 0; t < sim::TargetCount(); ++t)
   {
      double nm[4];
      if (sim::LegacyLinkageNm(sim::TargetAt(t).id, nm))
      {
         j.Key(sim::TargetAt(t).id);
         j.Array(nm, 4);
      }
   }
   j.Close('}');

   j.Close('}');
   out = std::move(j.s);
   return true;
}

} // namespace probes
