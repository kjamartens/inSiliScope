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

void ExcitationBeamsJson(Json& j, const sim::Excitation& ex)
{
   const sim::ExcitationSpec& es = ex.Spec();
   const sim::BeamSet& set = es.beams;
   const bool patterned = ex.Patterned();
   // The beams: a set's (the frame's state: rotated, phased), or the legacy beam as one row.
   std::vector<sim::Beam> beams = patterned ? ex.State().beams : std::vector<sim::Beam>();
   if (!patterned)
   {
      sim::Beam b;
      b.ux = es.bfp;
      b.polDeg = sim::kUnpolarizedDeg;   // unpolarized: the mean of s and p
      beams.push_back(b);
   }
   j.Key("beams");
   j.Open('[');
   for (size_t k = 0; k < beams.size(); ++k)
   {
      const sim::Beam& b = beams[k];
      j.Open('{');
      j.KNum("ux", b.ux);
      j.KNum("uy", b.uy);
      j.KNum("spot_um", b.spotUm > 0 ? b.spotUm : ex.SpotRadiusUm(es.fwhmPct / 100.0 * es.fovWidthUm));
      j.KBool("spot_from_fwhm", !(b.spotUm > 0));
      j.KNum("tilt_x_deg", b.tiltXDeg);
      j.KNum("tilt_y_deg", b.tiltYDeg);
      j.KNum("amp", b.amp);
      j.KNum("phase_turns", b.phaseTurns);
      j.KNum("pol_deg", b.polDeg);
      j.KNum("ellip_turns", b.ellipTurns);
      j.KNum("phase_step", b.phaseStep);
      j.KNum("nm", b.nm);
      const sim::BeamField f = sim::BeamFieldOf(b, es.lambdaRefNm, es.nMed, es.nImm, es.na, set.polarization);
      j.KNum("u", f.u);
      j.KStr("regime", sim::IlluminationRegimeNames()[f.clipped ? sim::REGIME_CLIPPED
                                                        : f.evanescent ? sim::REGIME_TIRF
                                                        : f.u > 0 ? sim::REGIME_HILO : sim::REGIME_EPI]);
      j.KNum("field_factor", patterned ? f.Intensity() : ex.F());
      j.KNum("angle_deg", f.evanescent ? 90.0 : std::asin(std::min(1.0, f.u)) * 180.0 / 3.14159265358979323846);
      j.Key("depth_nm");
      j.Open('{');
      for (int nm : sim::LaserLines())
      {
         double d = -1.0;
         if (f.evanescent)
            d = 1e-3 * nm / (4.0 * 3.14159265358979323846 * es.nMed * std::sqrt(f.u * f.u - 1.0)) * 1000.0;
         j.KNum(std::to_string(nm).c_str(), d);
      }
      j.Close('}');
      j.Close('}');
   }
   j.Close(']');
   j.Key("pattern");
   j.Open('{');
   j.KStr("type", sim::SimTypeNames()[static_cast<size_t>(patterned ? set.type : 0)]);
   j.KStr("polarization", sim::SimPolarizationNames()[static_cast<size_t>(set.polarization)]);
   j.KNum("phases", patterned ? set.phases : 1);
   j.KNum("angles", patterned ? set.angles : 1);
   j.KStr("order", sim::SimOrderNames()[static_cast<size_t>(set.order)]);
   j.KNum("period", patterned ? set.Period() : 1);
   j.KNum("frame0", static_cast<double>(es.frame));
   j.KNum("angle", patterned ? ex.State().light.angle : 0);
   j.KNum("phase", patterned ? ex.State().light.phase : 0);
   j.KNum("rotation_deg", patterned ? ex.State().rotDeg : 0.0);
   j.KBool("varies", ex.Varies());
   j.KNum("f_obj_um", es.fObjUm);
   j.KNum("pupil_radius_um", es.fObjUm * es.na);
   j.KNum("lambda_ref_nm", es.lambdaRefNm);
   j.KNum("spot_radius_um", ex.SpotRadiusUm(es.fwhmPct / 100.0 * es.fovWidthUm));
   j.Key("period_nm");
   j.Open('{');
   for (int nm : sim::LaserLines())
   {
      std::vector<double> per = ex.PeriodsUm(nm);
      for (double& v : per)
         v *= 1000.0;
      j.Key(std::to_string(nm).c_str());
      j.Array(per.data(), per.size());
   }
   j.Close('}');
   j.Close('}');
}

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
      ExcitationBeamsJson(j, ex);
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

bool IllumPreviewOf(const sim::ScopeSpec& spec, long long frame, double zUm, int n, int nz, IllumPreview& out,
                    std::string& err)
{
   out = IllumPreview();
   n = std::max(8, std::min(512, n));
   nz = std::max(8, std::min(512, nz));
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   sim::Excitation ex;
   std::vector<sim::ExcitationLine> sh;
   if (!sim::ScopeExcitation(spec, ex, sh, err))
      return false;
   const sim::ExcitationShares shares = sh.empty() ? sim::ExcitationShares{ { 640.0, 1.0 } } : sh;
   if (ex.Patterned())
      ex.SetFrame(frame);
   const sim::ExcitationSpec& es = ex.Spec();
   out.n = n;
   out.nz = nz;
   out.patterned = ex.Patterned();
   out.frame = frame;
   out.period = ex.Patterned() ? std::max(1, es.beams.Period()) : 1;
   out.angle = ex.Patterned() ? ex.State().light.angle : 0;
   out.phase = ex.Patterned() ? ex.State().light.phase : 0;
   out.uMax = ex.UMax();
   out.uSpan = 1.1 * ex.UMax();
   out.xySpanUm = 1.5 * R.W * R.p.pixelSizeNm / 1000.0;
   out.zMaxUm = 8.0;
   // The beams: a set's state (rotated, phased), or the legacy beam as one.
   std::vector<sim::Beam> beams = ex.Patterned() ? ex.State().beams : std::vector<sim::Beam>();
   if (!ex.Patterned())
   {
      sim::Beam b;
      b.ux = es.bfp;
      b.polDeg = sim::kUnpolarizedDeg;
      beams.push_back(b);
   }
   // A spot's size in u units: its radius in the pupil over the critical radius f n_s (the legacy width's spot when
   // the beam has none).
   const double fObjUm = es.beams.beams.empty()
                            ? std::max(1.0, sim::ScopeSpecGet(spec, "tube-length-mm")) * 1000.0 /
                                 std::max(1.0, sim::ScopeSpecGet(spec, "magnification"))
                            : es.fObjUm;
   sim::ExcitationSpec geo = es;
   geo.fObjUm = fObjUm;
   const sim::Excitation pair(geo);
   const double spotLegacy = pair.SpotRadiusUm(es.fwhmPct / 100.0 * es.fovWidthUm);
   std::vector<double> spotU(beams.size());
   for (size_t k = 0; k < beams.size(); ++k)
   {
      const sim::Beam& b = beams[k];
      const double w = b.spotUm > 0 ? b.spotUm : spotLegacy;
      spotU[k] = w / (fObjUm * es.nMed);
      const sim::BeamField f = sim::BeamFieldOf(b, es.lambdaRefNm, es.nMed, es.nImm, es.na, es.beams.polarization);
      const double v[IllumPreview::kBeamStride] = { b.ux, b.uy, spotU[k], b.amp, b.phaseTurns, b.polDeg, f.u,
                                                    f.evanescent ? 1.0 : 0.0, b.ellipTurns };
      out.beams.insert(out.beams.end(), v, v + IllumPreview::kBeamStride);
   }
   // The pupil seen from above: each beam's spot, a Gaussian of its 1/e^2 radius (intensity), x its amplitude squared.
   out.bfp.assign(static_cast<size_t>(n) * n, 0.0f);
   for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i)
      {
         const double u = (i + 0.5) / n * 2 * out.uSpan - out.uSpan, v = (j + 0.5) / n * 2 * out.uSpan - out.uSpan;
         double a = 0.0;
         for (size_t k = 0; k < beams.size(); ++k)
         {
            const double r2 = (u - beams[k].ux) * (u - beams[k].ux) + (v - beams[k].uy) * (v - beams[k].uy);
            const double w2 = std::max(1e-6, spotU[k] * spotU[k]);
            a += beams[k].amp * beams[k].amp * std::exp(-2.0 * r2 / w2);
         }
         out.bfp[static_cast<size_t>(j) * n + i] = static_cast<float>(a);
      }
   // The pattern on the plane at the section's height (seen from below; the focus unless one is asked for) and
   // through the axis (from the side), the excitation factor.
   out.xy.assign(static_cast<size_t>(n) * n, 0.0f);
   const double zf = zUm < 0 ? es.pivotUm : zUm;
   out.zUm = zf;
   for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i)
      {
         const double x = (i + 0.5) / n * out.xySpanUm - out.xySpanUm / 2, y = (j + 0.5) / n * out.xySpanUm - out.xySpanUm / 2;
         out.xy[static_cast<size_t>(j) * n + i] = static_cast<float>(ex.At(x, y, zf, shares));
      }
   out.xz.assign(static_cast<size_t>(nz) * n, 0.0f);
   for (int k = 0; k < nz; ++k)
      for (int i = 0; i < n; ++i)
      {
         const double x = (i + 0.5) / n * out.xySpanUm - out.xySpanUm / 2, z = (k + 0.5) / nz * out.zMaxUm;
         out.xz[static_cast<size_t>(k) * n + i] = static_cast<float>(ex.At(x, 0.0, z, shares));
      }
   return true;
}

} // namespace probes
