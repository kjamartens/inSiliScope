// scope_probes -- see scope_probes.h. LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "scope_probes.h"

#include "tiff_writer.h"

#include "ScopeResolved.h"

#include "BrightfieldRender.h"
#include "PsfGeneratorBridge.h"
#include "RenderPresets.h"
#include "SMLMZernike.h"
#include "Spectra.h"
#include "ZernikePsf.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace probes {
namespace {

// ---- a small JSON writer (numbers %.9g; NaN and infinities as null) ----
struct Json
{
   std::string s;
   bool first = true;
   void Sep()
   {
      if (!first)
         s += ',';
      first = false;
   }
   void Key(const char* k)
   {
      Sep();
      s += '"';
      s += k;
      s += "\":";
      first = true;
   }
   void Num(double v)
   {
      Sep();
      if (!std::isfinite(v))
      {
         s += "null";
         return;
      }
      char b[32];
      std::snprintf(b, sizeof b, "%.9g", v);
      s += b;
   }
   void Str(const std::string& v)
   {
      Sep();
      s += '"';
      for (char c : v)
      {
         if (c == '"' || c == '\\')
            s += '\\';
         s += c;
      }
      s += '"';
   }
   void Bool(bool v)
   {
      Sep();
      s += v ? "true" : "false";
   }
   void Open(char c)
   {
      Sep();
      s += c;
      first = true;
   }
   void Close(char c)
   {
      s += c;
      first = false;
   }
   template <class T> void Array(const T* v, size_t n)
   {
      Open('[');
      for (size_t i = 0; i < n; ++i)
         Num(static_cast<double>(v[i]));
      Close(']');
   }
   void KNum(const char* k, double v) { Key(k); Num(v); }
   void KStr(const char* k, const std::string& v) { Key(k); Str(v); }
   void KBool(const char* k, bool v) { Key(k); Bool(v); }
};

bool WriteText(const std::string& path, const std::string& text, std::string& err)
{
   FILE* f = std::fopen(path.c_str(), "wb");
   const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
   if (f)
      std::fclose(f);
   if (!ok)
      err = "cannot write " + path;
   return ok;
}

bool WriteStack(const std::string& path, const std::vector<float>& data, unsigned w, unsigned h, size_t pages,
                std::string& err)
{
   TiffWriter tif(path);
   if (!tif.ok())
   {
      err = "cannot write " + path;
      return false;
   }
   const size_t n = static_cast<size_t>(w) * h;
   std::vector<float> page(n);
   for (size_t k = 0; k < pages; ++k)
   {
      std::copy(data.begin() + static_cast<std::ptrdiff_t>(k * n), data.begin() + static_cast<std::ptrdiff_t>((k + 1) * n),
                page.begin());
      if (!tif.Page(page, w, h, ""))
      {
         err = "write error " + path;
         return false;
      }
   }
   return true;
}

const char* InterpName(sim::PsfInterpMode m)
{
   switch (m)
   {
   case sim::PsfInterpMode::Nearest: return "Nearest";
   case sim::PsfInterpMode::Linear: return "Linear";
   case sim::PsfInterpMode::Cubic: return "Cubic";
   default: return "Fft";
   }
}

// The emitting state a movie's PSF uses (as MakeScopePsfPreview: the main
// state, else the PALM pre state): its PSF wavelength rounded to a kernel's.
bool KernelLambda(const sim::ScopeSpec& spec, double& lambdaNm, std::string& err)
{
   sim::ScopeStateReadout st;
   if (!sim::ScopeLabelState(spec, 0, false, st, err))
      return false;
   if (!st.emits)
   {
      sim::ScopeStateReadout pre;
      if (sim::ScopeLabelState(spec, 0, true, pre, err) && pre.emits)
         st = pre;
      err.clear();
   }
   lambdaNm = sim::KernelWavelengthNm(st.emits ? st.lambdaNm : 670.0);
   return true;
}

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

int StructureMask()
{
   return (1 << ISC_STRUCT_COUNT) - 1;
}

} // namespace

bool SetupJson(const sim::ScopeSpec& spec, const std::string& path, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   auto O = [&](const char* n) { return sim::ScopeSpecGet(spec, n); };
   Json j;
   j.Open('{');
   j.Key("frame");
   j.Open('{');
   j.KNum("width", R.W);
   j.KNum("height", R.H);
   j.KNum("frames", R.N);
   j.KNum("exposure_s", R.expSec);
   j.KNum("start_s", R.t0Sec);
   j.KNum("pixel_nm", R.p.pixelSizeNm);
   j.KNum("seed", static_cast<double>(R.seed));
   j.KNum("world_seed", R.cf.seed);
   j.KNum("origin_x_um", R.q.originXUm);
   j.KNum("origin_y_um", R.q.originYUm);
   j.KNum("focus_um", R.q.zCullCentreUm);
   j.Close('}');

   const sim::ScopeResolvedCamera& c = R.camera;
   j.Key("camera");
   j.Open('{');
   j.KStr("preset", c.preset);
   j.KBool("emccd", c.emccd);
   j.KStr("qe_curve", c.qeCurve >= 0 && c.qeCurve < static_cast<int>(sim::CameraIds().size()) ? sim::CameraIds()[c.qeCurve] : "");
   j.KNum("qe_flat", c.qeFlat);
   j.KNum("dark_e_per_s", c.darkPerSec);
   j.KNum("gain_e_per_adu", c.gainElectronsPerAdu);
   j.KNum("offset_adu", c.offsetAdu);
   j.KNum("offset_std_adu", c.offsetStdAdu);
   j.KNum("read_noise_e", c.readNoiseElectrons);
   j.KNum("gain_std_frac", c.gainStdFraction);
   j.KNum("read_noise_std_frac", c.readNoiseStdFraction);
   j.KNum("em_gain", c.emGain);
   j.KNum("cic_e", c.cicElectrons);
   j.KNum("bit_depth", c.bitDepth);
   j.Close('}');

   // What the noise chain gets (fluorescence: QE 1, the detected fractions hold the QE curve).
   const sim::SimulationParams& p = R.p;
   j.Key("noise_chain");
   j.Open('{');
   j.KNum("qe", p.quantumEfficiency);
   j.KNum("dark_e_per_frame", p.darkCurrentElectronsPerFrame);
   j.KNum("gain_e_per_adu", p.gainPhotonsPerAdu);
   j.KNum("offset_adu", p.offsetAdu);
   j.KNum("offset_std_adu", p.offsetStdAdu);
   j.KNum("read_noise_e", p.readNoiseElectrons);
   j.KNum("gain_std_frac", p.pixelGainStdFraction);
   j.KNum("read_noise_std_frac", p.pixelReadNoiseStdFraction);
   j.KBool("emccd", p.emccd);
   j.KNum("em_gain", p.emGain);
   j.KNum("cic_e", p.cicElectrons);
   j.KNum("background_photons_per_frame", p.backgroundPhotons);
   j.Close('}');

   const sim::LightPath& lp = R.lp;
   j.Key("light_path");
   j.Open('{');
   j.Key("grid_nm");
   j.Open('[');
   j.Num(sim::SpectraGridNm(0));
   j.Num(sim::kSpectraGridStepNm);
   j.Num(sim::kSpectraGridN);
   j.Close(']');
   j.Key("lasers");
   j.Open('[');
   for (const sim::LaserLine& l : lp.lasers)
   {
      j.Open('{');
      j.KNum("nm", l.nm);
      j.KNum("kw_per_cm2", l.kWPerCm2);
      j.KNum("at_sample_kw_per_cm2", l.kWPerCm2 * (1 - sim::SampleAt(lp.dichroicT, l.nm)));
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
   j.KNum("eta", lp.eta);
   j.KNum("na", lp.na);
   j.KNum("immersion_index", lp.immersionIndex);
   j.KNum("chamber_height_um", lp.chamberHeightUm);
   j.KNum("background_qe", sim::BackgroundQe(lp));
   j.Close('}');

   j.Key("labels");
   j.Open('[');
   for (size_t s = 0; s < R.labels.size(); ++s)
   {
      const sim::LabelPhysics& L = R.labels[s];
      j.Open('{');
      j.KNum("structure", static_cast<double>(s));
      j.KStr("dye_id", L.eff.dye.id ? L.eff.dye.id : "");
      j.KStr("dye_name", L.eff.dye.name ? L.eff.dye.name : "");
      j.KStr("mode", sim::DyeModeNames()[static_cast<size_t>(L.mode)]);
      j.KNum("labelled_pct", sim::ScopeStructureLabelingPct(spec, static_cast<int>(s), L.mode));
      j.KNum("k_act_per_s", L.kActPerSec);
      j.KNum("excitation_scale", L.excitationScale);
      j.KNum("on_s_now", L.onSecNow);
      j.KNum("photon_budget", L.photonBudget);
      j.KNum("pre_photon_budget", L.prePhotonBudget);
      j.KNum("imager_nm", L.imagerNm);
      j.KNum("initial_on_s", L.initialOnSec);
      j.KNum("imager_background_per_px_per_s", L.ImagerBackgroundPerPxPerSec(R.p.pixelSizeNm / 1000.0));
      j.Key("label_vector");
      j.Array(L.label.data(), L.label.size());
      j.Key("main");
      StateJson(j, L.eff.dye.main, L.main, lp);
      j.Key("pre");
      StateJson(j, L.eff.dye.pre, L.pre, lp);
      j.Close('}');
   }
   j.Close(']');

   // The PSF a movie builds for the emitting state (ScopePsfRequest).
   double lambda = 0;
   std::string e;
   if (KernelLambda(spec, lambda, e))
   {
      sim::PsfGeneratorRequest req;
      j.Key("psf");
      j.Open('{');
      j.KNum("model", O("psf-model"));
      j.KNum("lambda_nm", lambda);
      if (sim::ScopePsfRequest(spec, lambda, req, e))
      {
         j.KNum("na", req.na);
         j.KNum("immersion_index", req.immersionIndex);
         j.KNum("sample_index", req.sampleIndex);
         j.KNum("sample_depth_nm", req.sampleDepthNm);
         j.KNum("oversampling", req.oversampling);
         j.KNum("kernel_half_width_px", req.kernelHalfWidthPx);
         j.KNum("nz", req.nz);
         j.KNum("z_step_nm", req.zStepNm);
         j.KNum("pupil_samples", sim::ZernikePupilSamples(req));
         j.KStr("interp", InterpName(req.interpMode));
         bool ok = false;
         const sim::ZernikeCoefficients z = sim::ParseZernikeCoefficients(req.zernikeCoefficients, ok);
         j.Key("zernike_waves");
         j.Array(z.data(), z.size());
      }
      j.KNum("halo_cut", O("psf-halo-cut"));
      j.Close('}');
   }

   // BrightField as the movie resolves it.
   sim::BrightfieldSpec bs;
   if (sim::ScopeBrightfieldSpec(spec, bs, e))
   {
      const sim::BrightfieldQuality q = bs.Resolved();
      j.Key("brightfield");
      j.Open('{');
      j.KNum("quality", bs.quality);
      j.KNum("sources", q.sources);
      j.KNum("upscale", q.upscale);
      j.KNum("sub", q.sub);
      j.KNum("slice_um", q.sliceUm);
      j.KNum("margin_um", q.marginUm);
      j.KNum("condenser_na", bs.condenserNa);
      j.KNum("wavelength_nm", bs.wavelengthNm);
      j.KNum("photons_per_px_per_s", O("bf-photons-per-px-per-sec"));
      j.KNum("qe_at_lamp", sim::SampleAt(lp.qe, bs.wavelengthNm));
      j.Close('}');
   }
   j.Close('}');
   return WriteText(path, j.s, err);
}

bool PresetsJson(const std::string& path, std::string& err)
{
   Json j;
   j.Open('{');
   j.Key("quality");
   j.Open('[');
   for (const sim::QualityPreset& q : sim::kQualityPresets)
   {
      j.Open('{');
      j.KStr("name", q.name);
      j.KNum("bf-quality", q.bf);
      j.KNum("psf-oversampling", q.os);
      j.KNum("wf-upscale", q.wf);
      j.KNum("psf-halo-cut", q.halo);
      j.Close('}');
   }
   j.Close(']');
   j.Key("drift");
   j.Open('[');
   for (const sim::DriftPreset& d : sim::kDriftPresets)
   {
      j.Open('{');
      j.KStr("name", d.name);
      j.KNum("drift-xy-speed-nm-per-sec", d.speedNmPerSec);
      j.KNum("drift-z-speed-nm-per-sec", d.speedNmPerSec);
      j.KNum("drift-xy-nm-per-sqrt-sec", d.walkNmPerSqrtSec);
      j.KNum("drift-z-nm-per-sqrt-sec", d.walkNmPerSqrtSec);
      j.Close('}');
   }
   j.Close(']');
   j.Key("zernike");
   j.Open('[');
   for (const std::string& name : sim::ZernikePresetNames())
   {
      const sim::ZernikeCoefficients z = sim::ZernikePresetCoefficients(name);
      j.Open('{');
      j.KStr("name", name);
      j.Key("waves");
      j.Array(z.data(), z.size());
      j.Close('}');
   }
   j.Close(']');
   j.Close('}');
   return WriteText(path, j.s, err);
}

bool Photons(const sim::ScopeSpec& spec, const std::string& path, sim::ScopeMovieInfo& info, std::string& err)
{
   TiffWriter tif(path);
   if (!tif.ok())
   {
      err = "cannot write " + path;
      return false;
   }
   bool writeOk = true;
   unsigned W = 0, H = 0;
   long n = 0;
   sim::ScopeMovieDims(spec, W, H, n);
   const bool ok = sim::RenderScopePhotons(spec, [&](long f, const std::vector<float>& photons) {
      writeOk = tif.Page(photons, W, H, f == 0 ? "insiliscope photons (before the camera)" : "");
      return writeOk;
   }, info, err);
   if (ok && !writeOk)
      err = "write error " + path;
   return ok && writeOk;
}

bool Psf(const sim::ScopeSpec& spec, const std::string& prefix, std::string& err)
{
   sim::ScopePsfPreview pv;
   if (!sim::MakeScopePsfPreview(spec, pv, err))
      return false;
   if (!WriteStack(prefix + ".planes.tif", pv.planes, static_cast<unsigned>(pv.size), static_cast<unsigned>(pv.size),
                   static_cast<size_t>(pv.nz), err) ||
       !WriteStack(prefix + ".cams.tif", pv.cams, static_cast<unsigned>(pv.camSize), static_cast<unsigned>(pv.camSize),
                   static_cast<size_t>(pv.nz), err))
      return false;
   // The Zernike wavefront (waves) over the pupil disc, row 0 = -y.
   sim::PsfGeneratorRequest req;
   std::string e;
   int pupilSamples = 0;
   sim::ZernikeCoefficients z = sim::ZeroZernikeCoefficients();
   if (sim::ScopePsfRequest(spec, pv.lambdaNm, req, e))
   {
      bool ok = false;
      z = sim::ParseZernikeCoefficients(req.zernikeCoefficients, ok);
      pupilSamples = sim::ZernikePupilSamples(req);
   }
   const int G = 129;
   std::vector<float> pupil(static_cast<size_t>(G) * G);
   for (int iy = 0; iy < G; ++iy)
      for (int ix = 0; ix < G; ++ix)
      {
         const double x = -1.0 + 2.0 * (ix + 0.5) / G, y = -1.0 + 2.0 * (iy + 0.5) / G, rho = std::hypot(x, y);
         pupil[static_cast<size_t>(iy) * G + ix] =
            rho <= 1.0 ? static_cast<float>(sim::ZernikeWavefrontWaves(z, rho, std::atan2(y, x)))
                       : std::numeric_limits<float>::quiet_NaN();
      }
   if (!WriteStack(prefix + ".pupil.tif", pupil, G, G, 1, err))
      return false;
   Json j;
   j.Open('{');
   j.KBool("gaussian", pv.gaussian);
   j.KNum("oversampling", pv.oversampling);
   j.KNum("size", pv.size);
   j.KNum("nz", pv.nz);
   j.KNum("cam_size", pv.camSize);
   j.KNum("z_step_nm", pv.zStepNm);
   j.KNum("lambda_nm", pv.lambdaNm);
   j.KNum("pixel_nm", sim::ScopeSpecGet(spec, "pixel-nm"));
   j.KNum("pupil_samples", pupilSamples);
   j.Key("zernike_waves");
   j.Array(z.data(), z.size());
   // Plane k is at defocus (k - (nz - 1) / 2) z_step (PsfKernelCache::NearestZIndex).
   j.Close('}');
   return WriteText(prefix + ".json", j.s, err);
}

bool Splat(const sim::ScopeSpec& spec, double dxPx, double dyPx, double zUm, const std::string& path,
           std::string& err)
{
   double lambda = 0;
   if (!KernelLambda(spec, lambda, err))
      return false;
   sim::PsfKernelCache kernel;
   if (!sim::ScopePsfKernel(spec, lambda, kernel, err))
      return false;
   if (!kernel.valid)
   {
      err = "--splat-out needs a kernel PSF (not psf-model 0)";
      return false;
   }
   const double halo = sim::ScopeSpecGet(spec, "psf-halo-cut");
   // The blinks' kernel, as FluorescenceMovie::Begin makes it.
   const sim::PsfKernelCache cut = sim::WithHaloCut(kernel, halo);
   const int camRad = kernel.halfWidthOversampled / std::max(1, kernel.oversampling);
   const unsigned n = static_cast<unsigned>(2 * camRad + 1);
   const double pixelNm = sim::ScopeSpecGet(spec, "pixel-nm");
   // One blink over the whole frame: x, y in pixels of the camera grid, where
   // an integer is a pixel centre (SplatSetup).
   sim::BlinkEvent ev;
   ev.xUm = (camRad + dxPx) * pixelNm / 1000.0;
   ev.yUm = (camRad + dyPx) * pixelNm / 1000.0;
   ev.zNm = zUm * 1000.0;
   ev.tStart = 0.0;
   ev.tEnd = 1.0;
   std::vector<float> img(static_cast<size_t>(n) * n, 0.0f);
   sim::RenderPhotonImage(img, n, n, std::vector<sim::BlinkEvent>(1, ev), 0, pixelNm, 1.0, 1.0, 0.0, 0.0, 0.0, &cut,
                          0.0);
   double sum = 0;
   long kept = 0;
   for (float v : img)
   {
      sum += v;
      kept += v != 0.0f;
   }
   if (!WriteStack(path, img, n, n, 1, err))
      return false;
   Json j;
   j.Open('{');
   j.KNum("size", n);
   j.KNum("cam_rad", camRad);
   j.KNum("dx_px", dxPx);
   j.KNum("dy_px", dyPx);
   j.KNum("z_um", zUm);
   j.KNum("z_index", kernel.NearestZIndex(zUm));
   j.KNum("lambda_nm", lambda);
   j.KNum("oversampling", kernel.oversampling);
   j.KStr("interp", InterpName(kernel.interpMode));
   j.KNum("halo_cut", halo);
   j.KNum("sum", sum);
   j.KNum("kept_pixels", static_cast<double>(kept));
   j.KNum("square_pixels", static_cast<double>(n) * n);
   j.KNum("kept_fraction_mean_over_planes", cut.halo ? cut.halo->keptFraction : 1.0);
   j.Close('}');
   return WriteText(path + ".json", j.s, err);
}

bool DyesJson(const sim::ScopeSpec& spec, const double rect[4], double zMin, double zMax, double t0, double t1,
              const std::string& path, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   sim::CellFieldSource src;
   if (!src.Configure(R.cf, err))
      return false;
   IscWorld* w = src.World();
   auto query = [&](int stride, const std::function<int32_t(double*, int32_t)>& fn, std::vector<double>& out) {
      int32_t cap = 4096;
      for (;;)
      {
         out.assign(static_cast<size_t>(cap) * stride, 0.0);
         const int32_t got = fn(out.data(), cap);
         if (got < 0)
            return false;
         if (got <= cap)
         {
            out.resize(static_cast<size_t>(got) * stride);
            return true;
         }
         cap = got + got / 4;
      }
   };
   std::vector<double> sites, events, cont;
   if (!query(ISC_SITE_STRIDE, [&](double* b, int32_t cap) {
          return isc_sites_in_window(w, rect[0], rect[1], rect[2], rect[3], zMin, zMax, b, cap);
       }, sites) ||
       !query(ISC_EVENT_STRIDE, [&](double* b, int32_t cap) {
          return isc_events_in_window(w, rect[0], rect[1], rect[2], rect[3], zMin, zMax, t0, t1, b, cap);
       }, events) ||
       !query(ISC_EVENT_STRIDE, [&](double* b, int32_t cap) {
          return isc_continuous_in_window(w, rect[0], rect[1], rect[2], rect[3], zMin, zMax, t0, b, cap);
       }, cont))
   {
      err = "dye query failed";
      return false;
   }
   Json j;
   j.Open('{');
   j.Key("rect");
   j.Array(rect, 4);
   j.KNum("z_min", zMin);
   j.KNum("z_max", zMax);
   j.KNum("t0", t0);
   j.KNum("t1", t1);
   j.KStr("sites_fields", "x y z id structure");
   j.KStr("events_fields", "x y z t_on t_off brightness id structure state aux");
   j.Key("sites");
   j.Array(sites.data(), sites.size());
   j.Key("events");
   j.Array(events.data(), events.size());
   j.Key("continuous");
   j.Array(cont.data(), cont.size());
   j.Close('}');
   return WriteText(path, j.s, err);
}

bool Density(const sim::ScopeSpec& spec, int nz, double zMin, double zMax, int up, const std::string& path,
             std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   sim::CellFieldSource src;
   if (!src.Configure(R.cf, err))
      return false;
   nz = std::max(1, nz);
   up = std::max(1, up);
   const double um = R.p.pixelSizeNm / 1000.0;
   const double x0 = R.q.originXUm, y0 = R.q.originYUm, x1 = x0 + R.W * um, y1 = y0 + R.H * um;
   const int nx = static_cast<int>(R.W) * up, ny = static_cast<int>(R.H) * up;
   std::vector<float> d(static_cast<size_t>(nx) * ny * nz, 0.0f);
   const int32_t total = isc_density3d_in_window(src.World(), x0, y0, x1, y1, zMin, zMax, nx, ny, nz, StructureMask(),
                                                  d.data());
   if (total < 0)
   {
      err = "density query failed";
      return false;
   }
   if (!WriteStack(path, d, static_cast<unsigned>(nx), static_cast<unsigned>(ny), static_cast<size_t>(nz), err))
      return false;
   Json j;
   j.Open('{');
   j.KNum("nx", nx);
   j.KNum("ny", ny);
   j.KNum("nz", nz);
   j.KNum("z_min", zMin);
   j.KNum("z_max", zMax);
   j.KNum("cell_um", um / up);
   j.KNum("total", total);
   j.Close('}');
   return WriteText(path + ".json", j.s, err);
}

bool BrightfieldScreens(const sim::ScopeSpec& spec, const std::string& prefix, std::string& err)
{
   sim::BrightfieldMovie bm;
   if (!bm.Begin(spec, false, err))
      return false;
   const unsigned nx = bm.GridNx(), ny = bm.GridNy();
   const size_t slices = static_cast<size_t>(std::max(0, bm.Slices()));
   if (!WriteStack(prefix + ".phase.tif", bm.Phase(), nx, ny, slices, err))
      return false;
   if (!bm.Atten().empty() && !WriteStack(prefix + ".atten.tif", bm.Atten(), nx, ny, slices, err))
      return false;
   sim::BrightfieldSpec bs;
   sim::ScopeBrightfieldSpec(spec, bs, err);
   const sim::BrightfieldQuality q = bs.Resolved();
   Json j;
   j.Open('{');
   j.KNum("grid_nx", nx);
   j.KNum("grid_ny", ny);
   j.KNum("slices", static_cast<double>(slices));
   j.KNum("z_top_um", bm.ZTopUm());
   j.KNum("object_z_um", bm.ObjectZUm());
   j.KNum("sources", bm.Sources());
   j.KNum("width", bm.Width());
   j.KNum("height", bm.Height());
   j.KNum("pixel_nm", sim::ScopeSpecGet(spec, "pixel-nm"));
   j.KNum("margin_um", q.marginUm);
   j.KNum("wavelength_nm", bs.wavelengthNm);
   j.KBool("atten", !bm.Atten().empty());
   j.Close('}');
   return WriteText(prefix + ".json", j.s, err);
}

bool NucleusJson(const sim::ScopeSpec& spec, double sizeUm, const std::string& path, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   sim::CellFieldSource src;
   if (!src.Configure(R.cf, err))
      return false;
   IscWorld* w = src.World();
   const double cx = sim::ScopeSpecGet(spec, "x"), cy = sim::ScopeSpecGet(spec, "y"), h = std::max(0.1, sizeUm) / 2;
   std::vector<double> cells(static_cast<size_t>(ISC_CELL_STRIDE) * 256);
   int32_t n = isc_cells_in_window(w, cx - h, cy - h, cx + h, cy + h, cells.data(), 256);
   if (n > 256)
   {
      cells.resize(static_cast<size_t>(ISC_CELL_STRIDE) * n);
      n = isc_cells_in_window(w, cx - h, cy - h, cx + h, cy + h, cells.data(), n);
   }
   if (n < 0)
   {
      err = "cell query failed";
      return false;
   }
   const int32_t slices = 17, pts = 64;
   std::vector<double> ring(static_cast<size_t>(slices) * pts * 3);
   Json j;
   j.Open('{');
   j.KNum("slices", slices);
   j.KNum("pts", pts);
   j.Key("cells");
   j.Open('[');
   for (int32_t i = 0; i < n; ++i)
   {
      const double* c = cells.data() + static_cast<size_t>(i) * ISC_CELL_STRIDE;
      const double px = c[2], py = c[3], rot = c[4], cr = std::cos(rot), sr = std::sin(rot);
      const int32_t got = isc_cell_nucleus_rings(w, static_cast<int32_t>(c[0]), static_cast<int32_t>(c[1]), slices, pts,
                                                 ring.data(), slices * pts);
      j.Open('{');
      j.KNum("x", px);
      j.KNum("y", py);
      j.Key("rings");
      j.Open('[');
      for (int32_t k = 0; k < std::max(0, got); ++k)
      {
         const double lx = ring[3 * k], ly = ring[3 * k + 1];
         j.Num(px + lx * cr - ly * sr);
         j.Num(py + lx * sr + ly * cr);
         j.Num(ring[3 * k + 2]);
      }
      j.Close(']');
      j.Close('}');
   }
   j.Close(']');
   j.Close('}');
   return WriteText(path, j.s, err);
}

} // namespace probes
