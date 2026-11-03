// scope_panels -- see scope_panels.h. LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "scope_panels.h"

#include "scope_setup.h"

#include "DyeLibrary.h"
#include "Excitation.h"
#include "LightPath.h"
#include "ScopeResolved.h"
#include "SimPattern.h"
#include "Spectra.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>

namespace probes {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string Fmt(const char* f, double v)
{
   char b[64];
   std::snprintf(b, sizeof b, f, v);
   return b;
}

// The name of an option's value (ScopeOptionNames), or the number.
std::string OptionName(const sim::ScopeSpec& spec, const char* option)
{
   const double v = sim::ScopeSpecGet(spec, option);
   const std::vector<std::string>* names = sim::ScopeOptionNames(option);
   const int i = static_cast<int>(v);
   if (names && i >= 0 && i < static_cast<int>(names->size()))
      return (*names)[static_cast<size_t>(i)];
   return Fmt("%g", v);
}

// "name=value ..." (spaces, commas or semicolons); unparsable tokens are skipped.
std::map<std::string, double> ParseParams(const std::string& text)
{
   std::map<std::string, double> out;
   size_t i = 0;
   while (i < text.size())
   {
      while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == ';'))
         ++i;
      size_t j = i;
      while (j < text.size() && text[j] != ' ' && text[j] != ',' && text[j] != ';')
         ++j;
      const std::string tok = text.substr(i, j - i);
      const size_t eq = tok.find('=');
      if (eq != std::string::npos)
      {
         char* end = nullptr;
         const std::string val = tok.substr(eq + 1);
         const double v = std::strtod(val.c_str(), &end);
         if (end && *end == 0 && !val.empty())
            out[tok.substr(0, eq)] = v;
      }
      i = j;
   }
   return out;
}

double Param(const std::map<std::string, double>& p, const char* name, double def)
{
   const auto it = p.find(name);
   return it == p.end() ? def : it->second;
}

// An overlay of an image (scope_panels.h): k and up to six numbers in its order.
struct Overlay
{
   const char* k;
   std::vector<double> v;
   const char* role = "line";
   bool dash = false, arrow = false;
};

struct Control
{
   const char* name;
   std::string label, unit, note;
   double min, max, step, value, reset;
};

// The panel being written: the header's JSON and the floats.
struct Builder
{
   Json j;
   std::vector<float>& data;
   explicit Builder(std::vector<float>& d) : data(d) {}

   // columns > 0: the views in a grid of that many columns (filled in rows), else side by side as they fit.
   void Begin(const char* id, const std::string& title, const std::string& caption, const std::vector<Control>& controls,
              int columns = 0)
   {
      j.Open('{');
      j.KStr("id", id);
      j.KStr("title", title);
      j.KStr("caption", caption);
      if (columns > 0)
         j.KNum("columns", columns);
      j.Key("controls");
      j.Open('[');
      for (const Control& c : controls)
      {
         j.Open('{');
         j.KStr("name", c.name);
         j.KStr("label", c.label);
         j.KStr("unit", c.unit);
         j.KNum("min", c.min);
         j.KNum("max", c.max);
         j.KNum("step", c.step);
         j.KNum("value", c.value);
         j.KNum("reset", c.reset);
         j.KStr("note", c.note);
         j.Close('}');
      }
      j.Close(']');
      j.Key("views");
      j.Open('[');
   }
   void End()
   {
      j.Close(']');
      j.Close('}');
   }
   // The floats of a view: offset and len.
   void Floats(const float* v, size_t n)
   {
      j.KNum("offset", static_cast<double>(data.size()));
      j.KNum("len", static_cast<double>(n));
      data.insert(data.end(), v, v + n);
   }
   // An image w x h; src row 0 at the top unless flip (then src row 0 is the bottom row).
   struct Image
   {
      const char* name;
      std::string title, note;
      const char* map = "heat";
      double gamma = 1;
      const char* peakOf = nullptr;   // scaled to that view's peak, else to peak (> 0), else to its own
      double peak = 0;
      const char* link = nullptr;   // images of one panel with the same link zoom together (centred alike, in um)
      bool resample = false;        // made again for the part shown when zoomed (params <name>.x0, .y0, .span)
      double window[3] = { 0, 0, 1 };   // the part of the whole image these pixels cover (x0, y0, span; as shown)
      bool pixelated = false;
      double extentW = 0, extentH = 0;
      double barUm = 0;   // > 0: a scale bar, the whole image this many um wide (as shown, not the window's part)
      std::vector<Overlay> overlays;
   };
   void AddImage(const Image& im, const float* src, int w, int h, bool flip)
   {
      j.Open('{');
      j.KStr("type", "image");
      j.KStr("name", im.name);
      j.KStr("title", im.title);
      j.KStr("note", im.note);
      j.KNum("w", w);
      j.KNum("h", h);
      std::vector<float> rows(static_cast<size_t>(w) * h);
      for (int y = 0; y < h; ++y)
      {
         const float* r = src + static_cast<size_t>(flip ? h - 1 - y : y) * w;
         std::copy(r, r + w, rows.begin() + static_cast<size_t>(y) * w);
      }
      Floats(rows.data(), rows.size());
      j.KStr("map", im.map);
      j.KNum("gamma", im.gamma);
      if (im.peakOf)
         j.KStr("peakOf", im.peakOf);
      if (im.peak > 0)
         j.KNum("peak", im.peak);
      if (im.link)
         j.KStr("link", im.link);
      if (im.resample)
         j.KBool("resample", true);
      if (im.window[0] != 0 || im.window[1] != 0 || im.window[2] != 1)
      {
         j.Key("window");
         j.Array(im.window, 3);
      }
      j.KBool("pixelated", im.pixelated);
      if (im.barUm > 0)
         j.KNum("scalebarUm", im.barUm);
      if (im.extentW > 0 && im.extentH > 0)
      {
         j.Key("extent");
         const double e[2] = { im.extentW, im.extentH };
         j.Array(e, 2);
      }
      j.Key("overlays");
      j.Open('[');
      for (const Overlay& o : im.overlays)
      {
         static const char* const kKeys[][6] = { { "x", "y", "r" }, { "x1", "y1", "x2", "y2" }, { "x", "y", "w", "h" },
                                                 { "x", "y", "r", "a0", "a1" } };
         const std::string k = o.k;
         const int which = k == "circle" ? 0 : k == "line" ? 1 : k == "rect" ? 2 : 3;
         j.Open('{');
         j.KStr("k", o.k);
         for (size_t i = 0; i < o.v.size() && i < 6 && kKeys[which][i]; ++i)
            j.KNum(kKeys[which][i], o.v[i]);
         j.KStr("role", o.role);
         if (o.dash)
            j.KBool("dash", true);
         if (o.arrow)
            j.KBool("arrow", true);
         j.Close('}');
      }
      j.Close(']');
      j.Close('}');
   }
   void AddRows(const char* name, const std::string& title, const std::vector<std::pair<std::string, std::string>>& rows)
   {
      j.Open('{');
      j.KStr("type", "rows");
      j.KStr("name", name);
      j.KStr("title", title);
      j.Key("rows");
      j.Open('[');
      for (const auto& r : rows)
      {
         j.Open('[');
         j.Str(r.first);
         j.Str(r.second);
         j.Close(']');
      }
      j.Close(']');
      j.Close('}');
   }
};

// ---- the light in the sample and the back focal plane (IllumPreviewOf) ----

// The frame control (a pattern's program), shared by both panels.
void FrameControl(const IllumPreview& P, std::vector<Control>& c)
{
   if (P.period > 1)
      c.push_back({ "frame", "frame", "", "the frame program's state", 0, double(P.period - 1), 1,
                    double(((P.frame % P.period) + P.period) % P.period), 0 });
}

// One line on the state: "frame 3 of 15: angle 0, phase 3 (6 beams)" or "one beam at u = 0.95".
std::string StateCaption(const IllumPreview& P)
{
   const size_t nb = P.beams.size() / IllumPreview::kBeamStride;
   if (!P.patterned)
      return nb == 1 ? "one beam at u = " + Fmt("%.2f", P.beams[6]) : std::to_string(nb) + " beams";
   return "frame " + std::to_string(((P.frame % P.period) + P.period) % P.period) + " of " + std::to_string(P.period) +
          ": angle " + std::to_string(P.angle) + ", phase " + std::to_string(P.phase) + " (" + std::to_string(nb) +
          " beams)";
}

std::string IllumTitle(const sim::ScopeSpec& spec, const IllumPreview& P)
{
   return std::string(P.patterned ? OptionName(spec, "sim-type") : "one beam") + ", BFP position " +
          Fmt("%.2f", sim::ScopeSpecGet(spec, "illum-bfp"));
}

// The part of a view shown (params <name>.x0, .y0, .span: a zoomed image; the whole one by default), anti-aliased by
// a Gaussian of kIllumSigmaPx pixels (stripes finer than ~1.5 pixels are cut to ~1 %: grey, not moire).
constexpr double kIllumSigmaPx = 0.7;
constexpr int kIllumMaxPx = 1600;   // the most pixels an image asks for across
IllumWindow WindowParam(const std::map<std::string, double>& p, const std::string& name)
{
   IllumWindow w;
   w.span = std::max(1e-4, std::min(1.0, Param(p, (name + ".span").c_str(), 1)));
   w.x0 = std::max(0.0, std::min(1 - w.span, Param(p, (name + ".x0").c_str(), 0)));
   w.y0 = std::max(0.0, std::min(1 - w.span, Param(p, (name + ".y0").c_str(), 0)));
   w.sigmaPx = kIllumSigmaPx;
   return w;
}

// An image's width in pixels: <name>.px (the page's canvas in device pixels, so it is drawn 1:1: a browser's scaling of
// fringes a few pixels apart beats into moire), else the default.
int PixelsParam(const std::map<std::string, double>& p, const std::string& name, int byDefault)
{
   const double px = Param(p, (name + ".px").c_str(), 0);
   return px > 0 ? std::max(32, std::min(kIllumMaxPx, static_cast<int>(std::lround(px)))) : byDefault;
}

bool IlluminationPanel(const sim::ScopeSpec& spec, const std::map<std::string, double>& p, int scale, Builder& b,
                       std::string& err)
{
   IllumPreview P;
   const double z = Param(p, "z", -1);
   const IllumWindow wxy = WindowParam(p, "xy"), wxz = WindowParam(p, "xz");
   // the x-z image keeps its shape (160 x 96) at any width
   const int nxy = PixelsParam(p, "xy", 160 * scale), nxz = PixelsParam(p, "xz", 160 * scale),
             nz = std::max(8, static_cast<int>(std::lround(nxz * 0.6)));
   if (!IllumPreviewOf(spec, static_cast<long long>(Param(p, "frame", 0)), z, nxy, nz, P, err, wxy, wxz, nxz))
      return false;
   // one scale at every zoom: the whole section's peak (a coarse image's samples land near the fringes' peaks), or
   // this image's if higher
   IllumPreview C;
   float ref = 0;
   if (IllumPreviewOf(spec, static_cast<long long>(Param(p, "frame", 0)), z, 64, 32, C, err))
      ref = *std::max_element(C.xy.begin(), C.xy.end());
   ref = std::max(ref, *std::max_element(P.xy.begin(), P.xy.end()));
   std::vector<Control> c;
   FrameControl(P, c);
   c.push_back({ "z", "height z", "\xC2\xB5m", "double-click: back to the focus", 0, P.zMaxUm, 0.02, P.zUm, -1 });
   b.Begin("illumination", "Light in the sample: " + IllumTitle(spec, P),
           StateCaption(P) + "; the section at z = " + Fmt("%.2f", P.zUm) + " \xC2\xB5m" + (z < 0 ? " (the focus)" : ""),
           c);
   const int n = P.n, nx = P.nxz;
   // a place given as a fraction of the whole image (as shown) -> the window's pixels
   auto wx = [](const IllumWindow& w, double fx, int size) { return (fx - w.x0) / w.span * size; };
   auto wy = [](const IllumWindow& w, double fy, int size) { return (fy - w.y0) / w.span * size; };
   auto setWindow = [](Builder::Image& im, const IllumWindow& w) {
      im.resample = true;
      im.window[0] = w.x0;
      im.window[1] = w.y0;
      im.window[2] = w.span;
   };
   Builder::Image xy{ "xy" };
   xy.title = "Section at z = " + Fmt("%.2f", P.zUm) + " \xC2\xB5m, from below";
   xy.note = "the excitation over 1.5\xC3\x97 the field (the field framed)";
   setWindow(xy, wxy);
   xy.peak = ref;
   if (scale >= 2)
      xy.barUm = P.xySpanUm;
   xy.overlays.push_back({ "rect", { wx(wxy, 1 / 6.0, n), wy(wxy, 1 / 6.0, n), wx(wxy, 5 / 6.0, n) - wx(wxy, 1 / 6.0, n),
                                     wy(wxy, 5 / 6.0, n) - wy(wxy, 1 / 6.0, n) } });
   b.AddImage(xy, P.xy.data(), n, n, true);
   Builder::Image xz{ "xz" };
   xz.title = "Side, x-z through the axis";
   xz.note = "the coverslip at the bottom, " + Fmt("%g", P.zMaxUm) + " \xC2\xB5m up; dashed: the section's height";
   xz.peak = ref;   // the section's scale
   setWindow(xz, wxz);
   if (scale >= 2)
      xz.barUm = P.xySpanUm;
   // the coverslip (half a row up from the bottom, as before) and the section's height
   const double yCover = wy(wxz, 1 - 0.5 / nz, nz);
   xz.overlays.push_back({ "line", { 0, yCover, double(nx), yCover } });
   if (P.zMaxUm > 0 && P.zUm >= 0 && P.zUm <= P.zMaxUm)
   {
      const double yz = wy(wxz, 1 - (0.5 + P.zUm / P.zMaxUm * nz) / nz, nz);
      Overlay l{ "line", { 0, yz, double(nx), yz } };
      l.dash = true;
      xz.overlays.push_back(l);
   }
   b.AddImage(xz, P.xz.data(), nx, nz, true);
   b.End();
   return true;
}

// The regime as words (the excitation as a movie uses it; the old BFP readout's).
std::string RegimeText(const sim::ScopeSpec& spec)
{
   sim::Excitation ex;
   std::vector<sim::ExcitationLine> sh;
   std::string e;
   if (!sim::ScopeExcitation(spec, ex, sh, e))
      return "";
   std::vector<sim::LaserLine> lines;
   double nm = 640;
   if (sim::ScopeSampleLines(spec, lines, e))
      for (const sim::LaserLine& l : lines)
         if (l.kWPerCm2 > 0)
            nm = l.nm;
   const double d = ex.DepthNm(nm);
   const std::string regime = sim::IlluminationRegimeNames()[static_cast<size_t>(ex.Regime())];
   const std::string ang = Fmt("%.0f", std::asin(std::min(1.0, ex.U())) * 180 / kPi);
   const std::string at = " at " + Fmt("%.0f", nm) + " nm";
   if (ex.Patterned())
      return regime == "TIRF"      ? "TIRF-SIM: the beams evanescent, 1/e depth " + Fmt("%.0f", d) + " nm" + at
             : regime == "Clipped" ? "the beams beyond the pupil edge (" + Fmt("%.3f", ex.UMax()) + "), no light"
                                   : "the beams " + ang + "\xC2\xB0 from the axis in the medium (propagating)";
   if (regime == "TIRF")
      return "TIRF: 1/e depth " + Fmt("%.0f", d) + " nm" + at + ", field \xC3\x97" + Fmt("%.2f", ex.F());
   if (regime == "HILO")
      return "HILO: sheet " + ang + "\xC2\xB0 from the axis in the medium" +
             (d > 0 ? ", " + Fmt("%.1f", d / 1000) + " \xC2\xB5m thick" : "");
   if (regime == "Clipped")
      return "Clipped: beyond the pupil edge (" + Fmt("%.3f", ex.UMax()) + "), no light";
   return "Epi: the beam straight up";
}

// The regime as one word (Epi, HILO, TIRF, Clipped; under a pattern SIM or TIRF-SIM).
std::string RegimeWord(const sim::ScopeSpec& spec)
{
   sim::Excitation ex;
   std::vector<sim::ExcitationLine> sh;
   std::string e;
   if (!sim::ScopeExcitation(spec, ex, sh, e))
      return "";
   const std::string regime = sim::IlluminationRegimeNames()[static_cast<size_t>(ex.Regime())];
   if (ex.Patterned())
      return regime == "TIRF" ? "TIRF-SIM" : regime == "Clipped" ? "Clipped" : "SIM";
   return regime;
}

bool BfpPanel(const sim::ScopeSpec& spec, const std::map<std::string, double>& p, int scale, Builder& b,
              std::string& err)
{
   IllumPreview P;
   if (!IllumPreviewOf(spec, static_cast<long long>(Param(p, "frame", 0)), -1, 160 * scale, 8, P, err))
      return false;
   std::vector<Control> c;
   FrameControl(P, c);
   b.Begin("bfp", "Back focal plane: " + IllumTitle(spec, P), StateCaption(P), c);
   const int n = P.n;
   const double s = n / (2 * P.uSpan);
   Builder::Image im{ "bfp" };
   im.title = "Back focal plane, from above";
   im.note = "dashed: the critical circle u = 1; solid: the pupil edge; each beam's spot ringed, its polarization a bar "
             "(circular: an arrow); evanescent beams in blue";
   Overlay crit{ "circle", { n / 2.0, n / 2.0, s } };
   crit.dash = true;
   im.overlays.push_back(crit);
   im.overlays.push_back({ "circle", { n / 2.0, n / 2.0, P.uMax * s } });
   for (size_t k = 0; k + IllumPreview::kBeamStride <= P.beams.size(); k += IllumPreview::kBeamStride)
   {
      const double* bm = &P.beams[k];
      const double ux = bm[0], uy = bm[1], spotU = bm[2], polDeg = bm[5], ellip = bm[8];
      const double x = n / 2.0 + ux * s, y = n / 2.0 - uy * s;
      const char* role = bm[7] > 0.5 ? "evanescent" : "line";
      Overlay spot{ "circle", { x, y, std::max(3.0, spotU * s) } };
      spot.role = role;
      im.overlays.push_back(spot);
      // the polarization: a bar across the plane of incidence (s) or along it (p), by the angle; a circular beam (an
      // ellipticity of a quarter turn) a circular arrow around its spot
      const double phi = std::atan2(uy, ux), a = (polDeg < -180 ? 45 : polDeg) * kPi / 180;
      const double L = std::max(5.0, spotU * s + 4);
      if (std::abs(std::abs(ellip) - 0.25) < 0.05)
      {
         const double a0 = -kPi / 2;
         Overlay arc{ "arc", { x, y, L + 2, a0, a0 + 1.6 * kPi } };
         arc.role = role;
         arc.arrow = true;
         im.overlays.push_back(arc);
      }
      else
      {
         const double dir = phi + kPi / 2 - a;
         Overlay bar{ "line", { x - L * std::cos(dir), y + L * std::sin(dir), x + L * std::cos(dir), y - L * std::sin(dir) } };
         bar.role = role;
         im.overlays.push_back(bar);
      }
   }
   b.AddImage(im, P.bfp.data(), n, n, true);
   std::vector<std::pair<std::string, std::string>> rows;
   const std::string regime = RegimeText(spec);
   if (!regime.empty())
      rows.push_back({ "Light", regime });
   rows.push_back({ "Pattern", P.patterned ? OptionName(spec, "sim-type") + ", polarization " + OptionName(spec, "sim-polarization")
                                           : "none (one beam)" });
   b.AddRows("state", "", rows);
   b.End();
   return true;
}

// ---- the PSF (MakeScopePsfPreview, kept for the next plane) ----

// A tile's x-y crop: every sample above this share of the in-focus peak (the core and the first ring; the light
// within it is ~90 % at NA 1.4, 99 % would take ~4x the side: the rings carry the rest).
constexpr double kPsfTilePeak = 0.01;

// The half side (kernel samples) of the smallest square about the kernel's centre sample that holds every sample of
// the in-focus plane above peakShare of its peak.
int PsfCropHalf(const sim::ScopePsfPreview& pv, double peakShare)
{
   const int n = pv.size, c = n / 2, mid = (pv.nz - 1) / 2;
   const float* pl = pv.planes.data() + static_cast<size_t>(mid) * n * n;
   float peak = 0;
   for (size_t i = 0, e = static_cast<size_t>(n) * n; i < e; ++i)
      peak = std::max(peak, pl[i]);
   int h = 0;
   for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
         if (pl[static_cast<size_t>(y) * n + x] > peakShare * peak)
            h = std::max(h, std::max(std::abs(x - c), std::abs(y - c)));
   return std::min(h, c);
}

bool PsfPanel(const sim::ScopeSpec& spec, const std::map<std::string, double>& p, int scale, Builder& b, std::string& err)
{
   static std::string lastKey;
   static sim::ScopePsfPreview pv;
   static float stackPeak = 0, camPeak = 0;   // the stack's brightest sample and camera pixel
   std::string key;
   for (const auto& kv : spec)
      key += kv.first + "=" + Fmt("%.17g", kv.second) + " ";
   if (key != lastKey || pv.nz == 0)
   {
      lastKey.clear();
      pv = sim::ScopePsfPreview();
      if (!sim::MakeScopePsfPreview(spec, pv, err))
         return false;
      stackPeak = pv.planes.empty() ? 0.0f : *std::max_element(pv.planes.begin(), pv.planes.end());
      camPeak = pv.cams.empty() ? 0.0f : *std::max_element(pv.cams.begin(), pv.cams.end());
      lastKey = key;
   }
   const double pixelNm = sim::ScopeSpecGet(spec, "pixel-nm"), stepUm = pv.zStepNm / 1000;
   const int mid = (pv.nz - 1) / 2;
   const int f = pv.nz <= 1 ? 0
                            : std::max(0, std::min(pv.nz - 1, static_cast<int>(std::lround(Param(p, "dz", 0) / stepUm)) + mid));
   const double dz = (f - (pv.nz - 1) / 2.0) * stepUm;
   // Filled, a 2 x 2 grid: x-y and x-z, the kernel above, the camera pixels below; the x-z cut runs through the
   // emitter along the angle (0: along x, counterclockwise as drawn). A tile: the kernel's x-y, cropped to the
   // centred square of every sample above 1 % of the in-focus peak, and its x-z along x over the whole window (as the
   // square root of the intensity). Every image is scaled to the whole stack's peak (the camera's to its own): defocus
   // dims the x-y images.
   const bool filled = scale >= 2, cut = filled && pv.nz > 1;
   const double angleDeg = cut ? std::max(0.0, std::min(180.0, Param(p, "angle", 0))) : 0;
   std::vector<Control> c;
   if (!pv.gaussian && pv.nz > 1)
      c.push_back({ "dz", "z \xE2\x88\x92 focus", "\xC2\xB5m", "the emitter's distance from the focal plane; double-click: in focus",
                    -mid * stepUm, (pv.nz - 1 - mid) * stepUm, stepUm, dz, 0 });
   if (cut)
      c.push_back({ "angle", "x\xE2\x80\x93z cut", "\xC2\xB0", "the direction of the x-z cut in the x-y plane (0: along x, "
                    "counterclockwise); double-click: along x", 0, 180, 5, angleDeg, 0 });
   const std::string model = pv.gaussian ? "Gaussian (no defocus)"
                                         : "Gibson-Lanni + Zernike, " + OptionName(spec, "psf-zernike-preset");
   b.Begin("psf", "PSF \xC2\xB7 " + Fmt("%.0f", pv.lambdaNm) + " nm",
           model + ", NA " + Fmt("%g", sim::ScopeSpecGet(spec, "na")) + "; " +
              (pv.gaussian ? std::string("in focus") : "z \xE2\x88\x92 focus = " + Fmt("%+.2f", dz) + " \xC2\xB5m") +
              "; scaled to the in-focus peak, the x\xE2\x80\x93z images as \xE2\x88\x9A intensity",
           c, cut ? 2 : 0);
   const double kUm = pv.size * pixelNm / pv.oversampling / 1000, cUm = pv.camSize * pixelNm / 1000;
   const double ca = std::cos(angleDeg * kPi / 180), sa = std::sin(angleDeg * kPi / 180);
   // the cut runs from edge to edge of the square x-y image (corner to corner at 45 degrees): its length over the side
   const double reach = 1 / std::max(std::fabs(ca), std::fabs(sa));
   const std::string cutName = angleDeg == 0 ? "x\xE2\x80\x93z" : "x\xE2\x80\xB2\xE2\x80\x93z at " + Fmt("%g", angleDeg) + "\xC2\xB0";
   // the cut drawn on an x-y image of n pixels a side (dashed, through the centre)
   auto cutLine = [&](Builder::Image& im, int n) {
      if (!cut)
         return;
      const double h = n / 2.0, r = h * reach;
      Overlay l{ "line", { h - r * ca, h + r * sa, h + r * ca, h - r * sa } };
      l.dash = true;
      im.overlays.push_back(l);
   };
   // an x-z image of nOut columns along the cut (over the side x reach: the x-y image's edge to edge), +z at the top;
   // at(z, u, v) = the plane z's value at (u, v) in its pixels from the centre, u along x, v down
   auto addXz = [&](const char* name, const std::string& title, double extentUm, int nOut, double unitsPerCol, bool pixelated,
                    const std::function<float(int, double, double)>& at) {
      std::vector<float> xz(static_cast<size_t>(pv.nz) * nOut);
      for (int z = 0; z < pv.nz; ++z)
         for (int x = 0; x < nOut; ++x)
         {
            const double r = (x + 0.5 - nOut / 2.0) * unitsPerCol * reach;
            xz[static_cast<size_t>(z) * nOut + x] = at(z, r * ca, -r * sa);
         }
      Builder::Image s{ name };
      s.title = title;
      s.map = "grey";
      s.gamma = 0.5;
      s.pixelated = pixelated;
      s.extentW = extentUm * reach;
      s.extentH = pv.nz * stepUm;
      if (filled)
         s.barUm = s.extentW;
      const double y = pv.nz - 1 - f + 0.5;
      Overlay l{ "line", { 0, y, double(nOut), y } };
      l.role = "focus";
      l.dash = true;
      s.overlays.push_back(l);
      b.AddImage(s, xz.data(), nOut, pv.nz, true);
   };
   Builder::Image k{ "kernel" };
   k.title = (filled ? "x\xE2\x80\x93y, " : "") + std::to_string(pv.oversampling) + "\xC3\x97 oversampled (" +
             Fmt("%.1f", pixelNm / pv.oversampling) + " nm)";
   k.map = "grey";
   k.extentW = k.extentH = kUm;
   if (filled)
      k.link = "xy";   // zoomed with the camera's x-y below it
   cutLine(k, pv.size);
   k.peak = stackPeak;   // the whole PSF's peak: a defocused plane dims, as in the x-z beside it
   if (filled)
      k.barUm = kUm;
   if (filled)
      b.AddImage(k, pv.planes.data() + static_cast<size_t>(f) * pv.size * pv.size, pv.size, pv.size, false);
   else
   {
      const int n = pv.size, h = PsfCropHalf(pv, kPsfTilePeak), m = 2 * h + 1, c0 = n / 2;
      std::vector<float> crop(static_cast<size_t>(m) * m);
      const float* pl = pv.planes.data() + static_cast<size_t>(f) * n * n;
      for (int y = 0; y < m; ++y)
         std::copy(pl + static_cast<size_t>(c0 - h + y) * n + (c0 - h), pl + static_cast<size_t>(c0 - h + y) * n + (c0 - h) + m,
                   crop.begin() + static_cast<size_t>(y) * m);
      k.extentW = k.extentH = m * pixelNm / pv.oversampling / 1000;
      b.AddImage(k, crop.data(), m, m, false);
   }
   if (pv.nz > 1)
   {
      // the kernel bilinear between its samples (the centre at (size - 1) / 2), zero outside
      const int n = pv.size;
      const double c0 = (n - 1) / 2.0;
      addXz("xz", cutName + ", oversampled", kUm, n, 1, false, [&](int z, double u, double v) {
         const double x = c0 + u, y = c0 + v;
         const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
         const double fx = x - x0, fy = y - y0;
         const float* pl = pv.planes.data() + static_cast<size_t>(z) * n * n;
         auto px = [&](int xi, int yi) { return xi < 0 || yi < 0 || xi >= n || yi >= n ? 0.0 : double(pl[static_cast<size_t>(yi) * n + xi]); };
         return static_cast<float>((1 - fy) * ((1 - fx) * px(x0, y0) + fx * px(x0 + 1, y0)) +
                                   fy * ((1 - fx) * px(x0, y0 + 1) + fx * px(x0 + 1, y0 + 1)));
      });
   }
   if (!filled)
   {
      b.End();
      return true;
   }
   Builder::Image cam{ "camera" };
   cam.title = filled ? "x\xE2\x80\x93y, " + Fmt("%.3g", pixelNm) + " nm camera pixels, the emitter at a pixel centre"
                      : Fmt("%.3g", pixelNm) + " nm camera pixels";
   cam.map = "grey";
   cam.pixelated = true;
   cam.extentW = cam.extentH = cUm;
   cam.link = "xy";
   cam.peak = camPeak;
   cam.barUm = cUm;
   cutLine(cam, pv.camSize);
   b.AddImage(cam, pv.cams.data() + static_cast<size_t>(f) * pv.camSize * pv.camSize, pv.camSize, pv.camSize, false);
   if (cut)
   {
      // the pixel each point of the cut falls in (oversampling columns per pixel, so a slanted cut shows each pixel's
      // stretch along it)
      const int n = pv.camSize, os = std::max(1, pv.oversampling);
      const double c0 = n / 2.0;
      addXz("camxz", cutName + ", camera pixels", cUm, n * os, 1.0 / os, true, [&](int z, double u, double v) {
         const int xi = static_cast<int>(std::floor(c0 + u)), yi = static_cast<int>(std::floor(c0 + v));
         if (xi < 0 || yi < 0 || xi >= n || yi >= n)
            return 0.0f;
         return pv.cams[(static_cast<size_t>(z) * n + yi) * n + xi];
      });
   }
   b.End();
   return true;
}

// ---- the spectra and what reaches the camera per label ----

// A dye state's spectra as the library has them (a parametric state's shapes, else its keys'); false when absent.
bool DyeSpectra(const sim::StateData& sd, sim::Spectrum& ex, sim::Spectrum& em)
{
   ex.clear();
   em.clear();
   if (sd.parametric)
   {
      ex = sim::ParametricExcitation(sd.exPeakNm, sd.exWidthNm);
      em = sim::ParametricEmission(sd.emPeakNm, sd.emWidthNm);
   }
   else
   {
      if (const sim::Spectrum* s = sd.ex ? sim::SpectrumByKey(sd.ex) : nullptr)
         ex = *s;
      if (const sim::Spectrum* s = sd.em ? sim::SpectrumByKey(sd.em) : nullptr)
         em = *s;
   }
   return !ex.empty() || !em.empty();
}

double CentroidNm(const sim::Spectrum& v)
{
   double s = 0, w = 0;
   for (size_t i = 0; i < v.size(); ++i)
   {
      s += v[i] * sim::SpectraGridNm(static_cast<int>(i));
      w += v[i];
   }
   return w > 0 ? s / w : 600;
}

// The labels shown: the mounted specimen's targets that are on, (target, structure).
std::vector<std::pair<const sim::TargetData*, int>> ShownLabels(const sim::ScopeSpec& spec, const sim::ScopeResolved& R)
{
   std::vector<std::pair<const sim::TargetData*, int>> out;
   const std::string specimen = sim::SpecimenIds()[static_cast<size_t>(sim::ScopeSpecimen(spec))];
   for (int t = 0; t < sim::TargetCount(); ++t)
   {
      const sim::TargetData& T = sim::TargetAt(t);
      if (specimen == T.specimen && T.structure < static_cast<int>(R.labels.size()) && !R.labels[T.structure].off)
         out.push_back({ &T, T.structure });
   }
   return out;
}

std::string Thousands(double v)
{
   std::string s = Fmt("%.0f", v), o;
   const size_t n = s.size();
   for (size_t i = 0; i < n; ++i)
   {
      o += s[i];
      if (s[i] != '-' && (n - 1 - i) % 3 == 0 && i + 1 < n)
         o += ',';
   }
   return o;
}

std::string Detected(const sim::StatePhysics& st)
{
   if (!st.emits)
      return "dark";
   return Fmt("%.1f", 100 * st.detectedFraction) + " % detected at " + Fmt("%.0f", st.lambdaNm) + " nm, " +
          Thousands(st.detectedPerSec) + " ph/s";
}

bool SpectraPanel(const sim::ScopeSpec& spec, const std::map<std::string, double>&, int, Builder& b, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   const sim::LightPath& lp = R.lp;
   const auto shown = ShownLabels(spec, R);
   b.Begin("spectra", "Spectra", "the light path's filters and the camera's QE, each label's excitation (dashed) and emission, the laser lines", {});
   const double x0 = 350, x1 = 800;
   const int i0 = static_cast<int>(x0 - sim::kSpectraGridMinNm), n = static_cast<int>(x1 - x0) + 1;
   auto series = [&](const sim::Spectrum& v, const std::string& label, const char* role, double nm,
                     std::vector<double> dash, bool fill) {
      std::vector<float> f(static_cast<size_t>(n), 0.0f);
      for (int i = 0; i < n && i0 + i < static_cast<int>(v.size()); ++i)
         f[static_cast<size_t>(i)] = static_cast<float>(v[static_cast<size_t>(i0 + i)]);
      b.j.Open('{');
      b.Floats(f.data(), f.size());
      b.j.KStr("label", label);
      b.j.KStr("role", role);
      b.j.KNum("nm", nm);
      b.j.Key("dash");
      b.j.Array(dash.data(), dash.size());
      b.j.KBool("fill", fill);
      b.j.Close('}');
   };
   b.j.Open('{');
   b.j.KStr("type", "curves");
   b.j.KStr("name", "spectra");
   b.j.KStr("title", "");
   b.j.KNum("x0", x0);
   b.j.KNum("x1", x1);
   b.j.KStr("unit", "nm");
   b.j.Key("series");
   b.j.Open('[');
   series(lp.emissionT, "emission filter: " + lp.emissionFilterName, "emfilter", 0, {}, true);
   series(lp.dichroicT, "dichroic: " + lp.dichroicName, "dichroic", 0, { 2, 2 }, false);
   if (std::any_of(lp.excitationT.begin(), lp.excitationT.end(), [](double v) { return v != 1; }))
      series(lp.excitationT, "excitation filter: " + lp.excitationFilterName, "exfilter", 0, { 6, 2, 1, 2 }, false);
   series(lp.qe, "camera QE" + (lp.qeName.empty() ? std::string() : ": " + lp.qeName), "qe", 0, { 1, 3 }, false);
   for (const auto& sl : shown)
   {
      const sim::LabelPhysics& L = R.labels[static_cast<size_t>(sl.second)];
      const std::string name = L.eff.dye.name ? L.eff.dye.name : "";
      for (int role = 0; role < 2; ++role)
      {
         const sim::StateData& sd = role == 0 ? L.eff.dye.main : L.eff.dye.pre;
         if (!sd.present || sd.isDark || (role == 1 && L.mode != sim::MODE_PALM))
            continue;
         sim::Spectrum ex, em;
         if (!DyeSpectra(sd, ex, em))
            continue;
         const std::string who = name + (role == 1 ? " (pre state)" : "");
         const double nm = !em.empty() ? CentroidNm(em) : CentroidNm(ex);   // its weighted emission wavelength
         if (!ex.empty())
            series(ex, who + " excitation", "ex", nm, { 3, 3 }, false);
         if (!em.empty())
            series(em, who + " emission", "em", nm, role == 1 ? std::vector<double>{ 5, 3 } : std::vector<double>{}, false);
      }
   }
   b.j.Close(']');
   b.j.Key("lines");
   b.j.Open('[');
   for (const sim::LaserLine& l : lp.lasers)
   {
      if (!(l.kWPerCm2 > 0))
         continue;   // blocked by the excitation filter
      b.j.Open('{');
      b.j.KNum("x", l.nm);
      b.j.KNum("nm", l.nm);
      b.j.KStr("label", Fmt("%g", l.nm) + " nm laser, " + Fmt("%.3g", l.kWPerCm2) + " kW/cm\xC2\xB2");
      b.j.Close('}');
   }
   b.j.Close(']');
   b.j.Close('}');
   std::vector<std::pair<std::string, std::string>> rows;
   if (shown.empty())
      rows.push_back({ "", "no labelled target (every dye None, 0 % labelled or no structure)" });
   const double pixelUm = R.p.pixelSizeNm / 1000.0;
   for (const auto& sl : shown)
   {
      const sim::LabelPhysics& L = R.labels[static_cast<size_t>(sl.second)];
      const std::string mode = sim::DyeModeNames()[static_cast<size_t>(L.mode)];
      const std::string who = std::string(L.eff.dye.name ? L.eff.dye.name : "") + ", " + mode;
      rows.push_back({ sl.first->name, who + ": " + Detected(L.main) + (L.mode == sim::MODE_WIDEFIELD ? "" : " while ON") });
      if (L.pre.emits)
         rows.push_back({ "", "pre state: " + Detected(L.pre) });
      if (L.mode == sim::MODE_DSTORM)
         rows.push_back({ "", "ON " + Fmt("%.3g", 1000 * L.onSecNow) + " ms, initial ON " + Fmt("%.3g", L.initialOnSec) +
                                 " s (excitation \xC3\x97" + Fmt("%.3g", L.excitationScale) + " of Dempsey 2011's)" });
      if (L.kActPerSec > 0)
         rows.push_back({ "", "activation " + Fmt("%.3g", L.kActPerSec) + " /s per dye" });
      const double bg = L.ImagerBackgroundPerPxPerSec(pixelUm);
      if (bg > 0)
         rows.push_back({ "", "free imager: " + Fmt("%.0f", bg) + " ph/px/s" });
      if (L.eff.dye.modes[L.mode].present && L.eff.dye.modes[L.mode].generic)
         rows.push_back({ "", "generic values: no data for this dye in this mode" });
   }
   b.AddRows("labels", "What reaches the camera", rows);
   b.End();
   return true;
}

// ---- the light path drawn ----

bool LightPathPanel(const sim::ScopeSpec& spec, const std::map<std::string, double>&, int, Builder& b, std::string& err)
{
   sim::ScopeResolved R;
   if (!sim::ScopeResolve(spec, R, err))
      return false;
   const sim::LightPath& lp = R.lp;
   bool epi = false, trans = false;
   sim::ScopeLights(spec, epi, trans);
   b.Begin("lightpath", "Light path", "click a part: its panel fills the view", {});
   Json& j = b.j;
   j.Open('{');
   j.KStr("type", "schematic");
   j.KStr("name", "lightpath");
   j.Key("elements");
   j.Open('[');
   // An element: what it is (label), which one and how it is set (value), the panel a click opens (station), more
   // words for its tooltip (note), "side": "right" for the parts off the axis (the lasers and their filter), and
   // "also": another panel that shows this part (a pointer over that panel marks it too).
   auto element = [&](const char* kind, const std::string& label, const std::string& value, const char* station,
                      const char* side = nullptr, const std::string& note = "", const char* also = nullptr) {
      j.Open('{');
      j.KStr("kind", kind);
      j.KStr("label", label);
      j.KStr("value", value);
      j.KStr("station", station ? station : "");
      if (side)
         j.KStr("side", side);
      if (!note.empty())
         j.KStr("note", note);
      if (also)
         j.KStr("also", also);
      j.Close('}');
   };
   if (trans)
      element("lamp", "Lamp", Fmt("%g", sim::ScopeSpecGet(spec, "bf-wavelength-nm")) + " nm, condenser NA " +
                                 Fmt("%g", sim::ScopeSpecGet(spec, "bf-condenser-na")), nullptr);
   const int sp = sim::ScopeSpecimen(spec);
   const std::string word = epi ? RegimeWord(spec) : "";   // the regime's word shown, its sentence in the tooltip
   element("sample", "Sample", std::string(sim::SpecimenAt(sp).name) + (word.empty() ? "" : ", " + word), "illumination",
           nullptr, epi ? RegimeText(spec) : "", "spectra");   // the dyes' spectra and photons
   element("objective", "Objective", Fmt("%g", sim::ScopeSpecGet(spec, "magnification")) + "\xC3\x97 NA " + Fmt("%g", lp.na) +
           ", n " + Fmt("%g", lp.immersionIndex), "psf");
   element("bfp", "Back focal plane",
           sim::ScopeSpecGet(spec, "sim-type") != 0 ? OptionName(spec, "sim-type")
                                                    : "beam at u = " + Fmt("%.2f", sim::ScopeSpecGet(spec, "illum-bfp")),
           "bfp");
   element("dichroic", "Dichroic", lp.dichroicName, "spectra");
   // a line's power as set (before the excitation filter: lp.lasers are after it) and its name; the custom line
   // (laser-custom-nm, laser-custom) comes last (ScopeLightPathSettings)
   const double customNm = sim::ScopeSpecGet(spec, "laser-custom-nm");
   auto isCustom = [&](size_t k) { return customNm > 0 && k + 1 == lp.lasers.size(); };
   auto setOf = [&](size_t k) {
      return sim::ScopeSpecGet(spec, isCustom(k) ? "laser-custom" : ("laser-" + Fmt("%g", lp.lasers[k].nm)).c_str());
   };
   auto laserName = [&](size_t k) { return Fmt("%g", lp.lasers[k].nm) + " nm" + (isCustom(k) ? " (custom)" : ""); };
   // the lines by wavelength (the drawing places the beams in this order: a custom line among the others)
   std::vector<size_t> byNm(lp.lasers.size());
   for (size_t k = 0; k < byNm.size(); ++k)
      byNm[k] = k;
   std::stable_sort(byNm.begin(), byNm.end(), [&](size_t a, size_t b) { return lp.lasers[a].nm < lp.lasers[b].nm; });
   std::string lines;
   for (size_t k : byNm)
      if (lp.lasers[k].kWPerCm2 > 0)
         lines += (lines.empty() ? "" : ", ") + laserName(k) + " " + Fmt("%.3g", sim::LaserIntensityAt(lp, lp.lasers[k].nm)) +
                  " kW/cm\xC2\xB2";
   element("lasers", "Lasers", !epi ? "shutter closed" : lines.empty() ? "all off" : lines, "spectra", "right");
   element("exfilter", "Excitation filter", lp.excitationFilterName, "spectra", "right");
   element("emfilter", "Emission filter", lp.emissionFilterName, "spectra");
   element("camera", "Camera", R.camera.preset + ", " + Fmt("%.3g", R.p.pixelSizeNm) + " nm pixels" +
           (lp.qeName.empty() ? "" : ", QE " + lp.qeName), "psf", nullptr, "", "spectra");   // its QE curve
   j.Close(']');
   j.Key("beams");
   j.Open('[');
   // A beam: its colour's wavelength, its strength where it starts (0..1 of the strongest of its kind), and the
   // fraction each part passes on ("pass": the parts in its order; the drawing dims the beam after each and ends it
   // where nearly nothing is left).
   using Pass = std::vector<std::pair<const char*, double>>;
   auto beam = [&](const char* kind, double nm, double weight, const std::string& label, const Pass& pass) {
      j.Open('{');
      j.KStr("kind", kind);
      j.KNum("nm", nm);
      j.KNum("weight", weight);
      j.KStr("label", label);
      j.Key("pass");
      j.Open('[');
      for (const auto& p : pass)
      {
         j.Open('{');
         j.KStr("at", p.first);
         j.KNum("f", p.second);
         j.Close('}');
      }
      j.Close(']');
      j.Close('}');
   };
   if (epi)
   {
      // The lasers as set, reflected by the dichroic.
      double top = 0;
      for (size_t k = 0; k < lp.lasers.size(); ++k)
         top = std::max(top, setOf(k));
      for (size_t k : byNm)
      {
         const double set = setOf(k), nm = lp.lasers[k].nm;
         if (!(set > 0))
            continue;
         beam("excitation", nm, top > 0 ? set / top : 1, laserName(k) + " laser",
              { { "exfilter", sim::SampleAt(lp.excitationT, nm) }, { "dichroic", 1 - sim::SampleAt(lp.dichroicT, nm) } });
      }
      // Each label's emission (its spectrum's mean through the dichroic, then the emission filter, then the camera's
      // QE), as strong as it is emitted.
      double most = 0;
      const auto shown = ShownLabels(spec, R);
      for (const auto& sl : shown)
         most = std::max(most, R.labels[static_cast<size_t>(sl.second)].main.emissionPerSec);
      for (const auto& sl : shown)
      {
         const sim::LabelPhysics& L = R.labels[static_cast<size_t>(sl.second)];
         sim::Spectrum ex, em;
         if (!L.main.emits || !(most > 0) || !DyeSpectra(L.eff.dye.main, ex, em) || em.empty())
            continue;
         double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
         for (size_t i = 0; i < em.size(); ++i)
         {
            const double a = em[i], b1 = a * lp.dichroicT[i], b2 = b1 * lp.emissionT[i];
            s0 += a;
            s1 += b1;
            s2 += b2;
            s3 += b2 * lp.qe[i];
         }
         const double nm = CentroidNm(em);
         beam("emission", nm, L.main.emissionPerSec / most,
              std::string(L.eff.dye.name ? L.eff.dye.name : "") + " (" + sl.first->name + ")",
              { { "dichroic", s0 > 0 ? s1 / s0 : 0 }, { "emfilter", s1 > 0 ? s2 / s1 : 0 }, { "camera", s2 > 0 ? s3 / s2 : 0 } });
      }
   }
   if (trans)
      beam("transmitted", sim::ScopeSpecGet(spec, "bf-wavelength-nm"), 1, "transmitted light", {});
   j.Close(']');
   j.Close('}');
   b.End();
   return true;
}

using PanelFn = bool (*)(const sim::ScopeSpec&, const std::map<std::string, double>&, int, Builder&, std::string&);
struct PanelDef
{
   const char* id;
   const char* title;
   const char* slot;
   PanelFn make;
};
// The panels: the drawn light path in the middle, then the tiles (the first half on the left, the rest on the right).
const PanelDef kPanels[] = {
   { "lightpath", "Light path", "centre", LightPathPanel },
   { "illumination", "Light in the sample", "side", IlluminationPanel },
   { "psf", "PSF", "side", PsfPanel },
   { "bfp", "Back focal plane", "side", BfpPanel },
   { "spectra", "Spectra", "side", SpectraPanel },
};

} // namespace

std::string ScopePanelsJson()
{
   Json j;
   j.Open('[');
   for (const PanelDef& p : kPanels)
   {
      j.Open('{');
      j.KStr("id", p.id);
      j.KStr("title", p.title);
      j.KStr("slot", p.slot);
      j.Close('}');
   }
   j.Close(']');
   return j.s;
}

bool MakePanel(const std::string& specText, const std::string& id, const std::string& params, int scale,
               PanelContent& out, std::string& err)
{
   out = PanelContent();
   for (const PanelDef& p : kPanels)
   {
      if (id != p.id)
         continue;
      sim::ScopeSpec spec;
      if (!sim::ParseScopeSpec(specText, spec, err))
         return false;
      Builder b(out.data);
      if (!p.make(spec, ParseParams(params), std::max(1, std::min(4, scale)), b, err))
         return false;
      out.header = std::move(b.j.s);
      return true;
   }
   err = "no panel '" + id + "'";
   return false;
}

} // namespace probes
