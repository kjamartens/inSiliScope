// ctest scope_panels: the viewer's Microscope view panels (scope_panels.h).
//   - the list names every panel, each makes content for a cell field, SIM2D, BrightField, the bead sample and the
//     slide; every view's floats lie in the buffer, end to end, and an image's are its w x h;
//   - the illumination panel's section is IllumPreviewOf's bit for bit (rows top down), the PSF panel's kernel and
//     camera image MakeScopePsfPreview's plane at the focus;
//   - params reach the panels (the section's height, the PSF's defocus); the light path follows the shutters;
//   - an unknown panel fails.
//
//   scope_panels_check    exit code 0 = all checks passed
#include "scope_panels.h"
#include "scope_setup.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace probes;

namespace {
constexpr double kSigmaPx = 0.7;   // the illumination panel's anti-aliasing (kIllumSigmaPx in scope_panels.cpp)

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok) g_failures++;
}

// The number after "key": at or after pos (-1 if none).
double NumAfter(const std::string& s, const char* key, size_t pos, size_t* at = nullptr)
{
   const std::string k = std::string("\"") + key + "\":";
   const size_t i = s.find(k, pos);
   if (i == std::string::npos) return -1;
   if (at) *at = i + k.size();
   return std::strtod(s.c_str() + i + k.size(), nullptr);
}

// Every view's floats in the buffer, end to end; an image's len = w x h.
bool Layout(const PanelContent& c)
{
   size_t pos = 0, next = 0;
   double end = 0;
   for (;;) {
      size_t at = 0;
      const double off = NumAfter(c.header, "offset", pos, &at);
      if (off < 0) break;
      const double len = NumAfter(c.header, "len", at);
      if (off != end || len < 0) return false;
      end = off + len;
      pos = at;
   }
   // images: w, h come before their offset
   while ((next = c.header.find("\"type\":\"image\"", next)) != std::string::npos) {
      size_t at = 0;
      const double w = NumAfter(c.header, "w", next), h = NumAfter(c.header, "h", next), len = NumAfter(c.header, "len", next, &at);
      if (w * h != len) return false;
      next = at;
   }
   return end == static_cast<double>(c.data.size());
}

// The floats of the image named name.
const float* ImageOf(const PanelContent& c, const char* name, double& len)
{
   const size_t i = c.header.find(std::string("\"name\":\"") + name + "\"");
   if (i == std::string::npos) return nullptr;
   const double off = NumAfter(c.header, "offset", i);
   len = NumAfter(c.header, "len", i);
   return off >= 0 ? c.data.data() + static_cast<size_t>(off) : nullptr;
}

} // namespace

int main()
{
   const std::string list = ScopePanelsJson();
   const char* ids[] = { "lightpath", "illumination", "psf", "bfp", "spectra" };
   for (const char* id : ids)
      Check(list.find(std::string("\"id\":\"") + id + "\"") != std::string::npos, std::string("the list names ") + id);

   const std::string base = "size=64 psf-kernel-half-width-nm=3000 ";
   const char* specs[] = { "", "sim-type=SIM2D", "modality=1", "specimen=2", "specimen=3" };
   for (const char* extra : specs)
      for (const char* id : ids) {
         PanelContent c;
         std::string err;
         const bool ok = MakePanel(base + extra, id, "", 1, c, err);
         Check(ok && c.header.front() == '{' && Layout(c),
               std::string(id) + " [" + extra + "]" + (ok ? "" : ": " + err));
      }

   // the illumination's section = IllumPreviewOf's (row 0 the largest y)
   {
      sim::ScopeSpec s;
      std::string err;
      sim::ParseScopeSpec(base + "sim-type=SIM2D", s, err);
      IllumPreview P;
      PanelContent c;
      IllumWindow w2;   // the panel's: the whole image, anti-aliased
      w2.sigmaPx = kSigmaPx;
      const bool ok = IllumPreviewOf(s, 2, -1, 160, 96, P, err, w2, w2) &&
                      MakePanel(base + "sim-type=SIM2D", "illumination", "frame=2", 1, c, err);
      double len = 0;
      const float* xy = ok ? ImageOf(c, "xy", len) : nullptr;
      bool same = xy && len == P.xy.size();
      for (int y = 0; same && y < P.n; ++y)
         same = std::memcmp(xy + static_cast<size_t>(y) * P.n, P.xy.data() + static_cast<size_t>(P.n - 1 - y) * P.n,
                            sizeof(float) * P.n) == 0;
      Check(same, "the illumination's section = IllumPreviewOf's, bit for bit");
      // a zoomed window (the middle half) at 160 px = the whole image's middle at 320 px, one sample per pixel
      IllumPreview W, F;
      IllumWindow mid;
      mid.x0 = mid.y0 = 0.25;
      mid.span = 0.5;
      bool win = IllumPreviewOf(s, 2, -1, 160, 96, W, err, mid, mid) && IllumPreviewOf(s, 2, -1, 320, 192, F, err);
      double worst = 0, peak = 0;
      for (int j = 0; win && j < 160; ++j)
         for (int i = 0; i < 160; ++i)
         {
            const double a = W.xy[static_cast<size_t>(j) * 160 + i], f = F.xy[static_cast<size_t>(j + 80) * 320 + i + 80];
            worst = std::max(worst, std::fabs(a - f));
            peak = std::max(peak, std::fabs(f));
         }
      for (int k = 0; win && k < 96; ++k)   // x-z: rows from the bottom; the window's top half-span from 0.25 down
         for (int i = 0; i < 160; ++i)
            worst = std::max(worst, std::fabs(double(W.xz[static_cast<size_t>(k) * 160 + i]) - F.xz[static_cast<size_t>(k + 48) * 320 + i + 80]));
      Check(win && worst <= 1e-5 * peak, "a zoomed window = that part of the whole image (worst " + std::to_string(worst / std::max(peak, 1e-30)) + " of the peak)");
      // drawn 1:1: the images follow the page's canvas size (xy.px, xz.px). SIM2D at the critical angle (~240 nm
      // fringes): at 640 px over 9.6 um (~16 px a period) the anti-aliased image is the pattern at its pixel centres; a
      // 160 px image (~4 px a period) scaled up to 640 as a browser does (bilinear) is far from it: the moire seen
      {
         const std::string sim2d = base + "sim-type=SIM2D illum-bfp=1";
         PanelContent fine;
         double lf = 0, lz = 0;
         const auto t0 = std::chrono::steady_clock::now();
         const bool okp = MakePanel(sim2d, "illumination", "xy.px=640 xz.px=500", 1, fine, err);
         const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
         Check(okp && ImageOf(fine, "xy", lf) && lf == 640.0 * 640 && ImageOf(fine, "xz", lz) && lz == 500.0 * 300,
               "the light's images follow xy.px / xz.px (640 x 640, 500 x 300) in " + std::to_string(ms) + " ms");
         sim::ScopeSpec m;
         sim::ParseScopeSpec(sim2d, m, err);
         IllumPreview Ex, Fa, Ca;
         IllumWindow aa;
         aa.sigmaPx = kSigmaPx;
         const bool okm = IllumPreviewOf(m, 0, -1, 640, 8, Ex, err) && IllumPreviewOf(m, 0, -1, 640, 8, Fa, err, aa, aa) &&
                          IllumPreviewOf(m, 0, -1, 160, 8, Ca, err, aa, aa);
         // rms differences from the exact samples over the central 128 x 128 of 640, over their mean
         double dF = 0, dC = 0, mean = 0;
         for (int j = 256; okm && j < 384; ++j)
            for (int i = 256; i < 384; ++i)
            {
               const double e = Ex.xy[static_cast<size_t>(j) * 640 + i];
               // the coarse image at this place, bilinear between its pixel centres
               const double cx = (i + 0.5) / 4 - 0.5, cy = (j + 0.5) / 4 - 0.5;
               const int x0 = static_cast<int>(std::floor(cx)), y0 = static_cast<int>(std::floor(cy));
               const double fx = cx - x0, fy = cy - y0;
               auto C = [&](int x, int y) { return double(Ca.xy[static_cast<size_t>(y) * 160 + x]); };
               const double c = (1 - fy) * ((1 - fx) * C(x0, y0) + fx * C(x0 + 1, y0)) + fy * ((1 - fx) * C(x0, y0 + 1) + fx * C(x0 + 1, y0 + 1));
               dF += (Fa.xy[static_cast<size_t>(j) * 640 + i] - e) * (Fa.xy[static_cast<size_t>(j) * 640 + i] - e);
               dC += (c - e) * (c - e);
               mean += e;
            }
         mean /= 128 * 128;
         dF = std::sqrt(dF / (128 * 128)) / std::max(mean, 1e-30);
         dC = std::sqrt(dC / (128 * 128)) / std::max(mean, 1e-30);
         Check(okm && dF < 0.1 && dC > 2 * dF, "at the screen's pixel the image is the pattern: rms " + std::to_string(dF) +
                                                  " at 640 px, " + std::to_string(dC) + " for 160 px scaled up");
      }
      PanelContent z;
      Check(MakePanel(base + "sim-type=SIM2D", "illumination", "xy.x0=0.25 xy.y0=0.25 xy.span=0.5", 1, z, err) &&
               z.header.find("\"window\":[0.25,0.25,0.5]") != std::string::npos && Layout(z),
            "the panel takes the part shown (xy.x0, .y0, .span) and names it");
      PanelContent h;
      Check(MakePanel(base, "illumination", "z=1.5", 1, h, err) && h.header.find("z = 1.50") != std::string::npos,
            "the section's height follows z");
      PanelContent b;
      Check(MakePanel(base + "sim-type=SIM2D", "bfp", "frame=4", 2, b, err) &&
               b.header.find("frame 4 of") != std::string::npos && NumAfter(b.header, "w", 0) == 320,
            "the back focal plane follows the frame and the scale");
   }

   // the PSF's kernel and camera image at the focus = MakeScopePsfPreview's middle plane
   {
      sim::ScopeSpec s;
      std::string err;
      sim::ParseScopeSpec(base, s, err);
      sim::ScopePsfPreview pv;
      PanelContent c;
      const bool ok = sim::MakeScopePsfPreview(s, pv, err) && MakePanel(base, "psf", "", 2, c, err);
      const int mid = (pv.nz - 1) / 2;
      double lk = 0, lc = 0;
      const float* k = ok ? ImageOf(c, "kernel", lk) : nullptr;
      const float* cam = ok ? ImageOf(c, "camera", lc) : nullptr;
      Check(k && lk == double(pv.size) * pv.size &&
               std::memcmp(k, pv.planes.data() + static_cast<size_t>(mid) * pv.size * pv.size, sizeof(float) * pv.size * pv.size) == 0,
            "the PSF's kernel = the preview's plane at the focus");
      Check(cam && lc == double(pv.camSize) * pv.camSize &&
               std::memcmp(cam, pv.cams.data() + static_cast<size_t>(mid) * pv.camSize * pv.camSize,
                           sizeof(float) * pv.camSize * pv.camSize) == 0,
            "the PSF's camera image = the preview's at the focus");
      PanelContent d;
      const double dz = 4 * pv.zStepNm / 1000;
      double ld = 0;
      const bool okd = MakePanel(base, "psf", "dz=" + std::to_string(dz), 2, d, err);
      const float* kd = okd ? ImageOf(d, "kernel", ld) : nullptr;
      Check(kd && std::memcmp(kd, pv.planes.data() + static_cast<size_t>(mid + 4) * pv.size * pv.size, sizeof(float) * pv.size * pv.size) == 0,
            "the PSF follows dz");
      // filled: a 2 x 2 grid; the x-z cut along x = the kernel's and the camera's centre rows (+z at the top)
      PanelContent g;
      double lx = 0, lcx = 0;
      const bool okg = MakePanel(base, "psf", "", 2, g, err);
      const float* xz = okg ? ImageOf(g, "xz", lx) : nullptr;
      const float* cxz = okg ? ImageOf(g, "camxz", lcx) : nullptr;
      const int os = pv.oversampling, cn = pv.camSize, row = pv.nz - 1 - mid;
      bool camRow = cxz && lcx == double(pv.nz) * cn * os;
      for (int x = 0; camRow && x < cn * os; ++x)
         camRow = cxz[static_cast<size_t>(row) * cn * os + x] ==
                  pv.cams[(static_cast<size_t>(mid) * cn + cn / 2) * cn + x / os];
      Check(okg && g.header.find("\"columns\":2") != std::string::npos && Layout(g) && xz && lx == double(pv.nz) * pv.size &&
               std::memcmp(xz + static_cast<size_t>(row) * pv.size,
                           pv.planes.data() + (static_cast<size_t>(mid) * pv.size + pv.size / 2) * pv.size, sizeof(float) * pv.size) == 0 &&
               camRow,
            "the filled PSF: 2 x 2, the x-z cuts = the centre rows");
      PanelContent r;
      double lr = 0;
      const bool okr = MakePanel(base, "psf", "angle=90", 2, r, err);
      const float* rz = okr ? ImageOf(r, "xz", lr) : nullptr;
      bool col = rz != nullptr;   // at 90 degrees the cut runs up the image: the centre column, from the bottom row
      for (int x = 0; col && x < pv.size; ++x)
         col = std::fabs(rz[static_cast<size_t>(row) * pv.size + x] -
                         pv.planes[(static_cast<size_t>(mid) * pv.size + (pv.size - 1 - x)) * pv.size + pv.size / 2]) < 1e-6f;
      Check(col, "the x-z cut follows the angle");
      // at 45 degrees the cut runs corner to corner: the x-z image sqrt 2 x the side, its first column at the corner
      PanelContent d45;
      double l45 = 0;
      const bool ok45 = MakePanel(base, "psf", "angle=45", 2, d45, err);
      const float* z45 = ok45 ? ImageOf(d45, "xz", l45) : nullptr;
      const size_t dAt = d45.header.find("\"name\":\"xz\"");
      auto extentW = [&](size_t from) { size_t at = 0; return NumAfter(d45.header, "extent", from, &at) < -0.5 ? -1.0 : std::strtod(d45.header.c_str() + at + 1, nullptr); };
      const double kSide = extentW(d45.header.find("\"name\":\"kernel\"")),
                   dW = extentW(dAt);
      Check(z45 && std::fabs(dW - std::sqrt(2.0) * kSide) <= 1e-6 * kSide, "at 45 degrees the x-z cut is the diagonal (" +
               std::to_string(dW / std::max(kSide, 1e-30)) + " x the side)");
      // a tile: the kernel's x-y cropped to the centred square of every sample above 1 % of the in-focus peak, and the x-z
      PanelContent t;
      double lt = 0, ltz = 0;
      const bool okt = MakePanel(base, "psf", "", 1, t, err);
      const float* tk = okt ? ImageOf(t, "kernel", lt) : nullptr;
      const int m = static_cast<int>(std::lround(std::sqrt(lt))), n = pv.size, h = (m - 1) / 2;
      double in = 0, all = 0, outside = 0, peak = 0;
      const float* pl = pv.planes.data() + static_cast<size_t>(mid) * n * n;
      for (size_t i = 0; i < static_cast<size_t>(n) * n; ++i) { all += pl[i]; peak = std::max(peak, double(pl[i])); }
      for (int y = 0; y < n; ++y)   // the brightest sample outside the crop
         for (int x = 0; x < n; ++x)
            if (std::abs(x - n / 2) > h || std::abs(y - n / 2) > h) outside = std::max(outside, double(pl[static_cast<size_t>(y) * n + x]));
      bool same = tk && m * m == lt && m < n;
      for (int y = 0; same && y < m; ++y)
         for (int x = 0; same && x < m; ++x)
         {
            const float v = pl[static_cast<size_t>(n / 2 - h + y) * n + (n / 2 - h + x)];
            same = tk[static_cast<size_t>(y) * m + x] == v;
            in += v;
         }
      // its scale: the whole stack's peak (a defocused plane dims), not its own plane's
      const double stackPeak = *std::max_element(pv.planes.begin(), pv.planes.end());
      const size_t kAt = t.header.find("\"name\":\"kernel\"");
      Check(okt && std::fabs(NumAfter(t.header, "peak", kAt) - stackPeak) <= 1e-6 * stackPeak &&
               t.header.find("\"peak\":", kAt) < t.header.find("\"name\":\"xz\""),
            "the PSF tile's x-y is scaled to the whole stack's peak");
      {
         const double camStack = *std::max_element(pv.cams.begin(), pv.cams.end());
         const size_t gk = g.header.find("\"name\":\"kernel\""), gc = g.header.find("\"name\":\"camera\"");
         Check(okg && std::fabs(NumAfter(g.header, "peak", gk) - stackPeak) <= 1e-6 * stackPeak &&
                  g.header.find("\"peak\":", gk) < g.header.find("\"name\":\"xz\"") &&
                  std::fabs(NumAfter(g.header, "peak", gc) - camStack) <= 1e-6 * camStack,
               "the filled PSF's x-y images are scaled to the whole stack's peaks");
      }
      Check(okt && same && outside <= 0.01 * peak && in > 0.8 * all && ImageOf(t, "xz", ltz) && !ImageOf(t, "camera", lt) && Layout(t),
            "the PSF tile: x-y cropped to " + std::to_string(m) + " of " + std::to_string(n) + " samples (" +
               std::to_string(in / all) + " of the light), x-z");
   }

   // the light path: the lasers' shutter, the lamp, the pattern
   {
      PanelContent f, bf, sim;
      std::string err;
      MakePanel(base, "lightpath", "", 1, f, err);
      MakePanel(base + "modality=1", "lightpath", "", 1, bf, err);
      MakePanel(base + "sim-type=SIM2D", "lightpath", "", 1, sim, err);
      Check(f.header.find("\"kind\":\"excitation\"") != std::string::npos &&
               f.header.find("\"kind\":\"emission\"") != std::string::npos && f.header.find("\"kind\":\"lamp\"") == std::string::npos,
            "fluorescence: laser and emission beams, no lamp");
      Check(bf.header.find("\"kind\":\"lamp\"") != std::string::npos && bf.header.find("shutter closed") != std::string::npos &&
               bf.header.find("\"kind\":\"excitation\"") == std::string::npos,
            "BrightField: the lamp, the lasers' shutter closed");
      Check(sim.header.find("\"value\":\"SIM2D\"") != std::string::npos, "SIM2D in the back focal plane");
      // a custom laser line (its power is laser-custom) and a custom (parametric) dye are drawn
      PanelContent cu;
      MakePanel(base + "laser-custom-nm=532 laser-custom=1 mt-dye=Custom", "lightpath", "", 1, cu, err);
      // in wavelength order: the 532 nm line between 488 and 561 (each lit for this)
      PanelContent ord;
      MakePanel(base + "laser-custom-nm=532 laser-custom=1 laser-488=1 laser-561=1 laser-640=1", "lightpath", "", 1, ord, err);
      const size_t p488 = ord.header.find("\"label\":\"488 nm laser\""), p532 = ord.header.find("\"label\":\"532 nm (custom) laser\""),
                   p561 = ord.header.find("\"label\":\"561 nm laser\""), p640 = ord.header.find("\"label\":\"640 nm laser\"");
      Check(p488 < p532 && p532 < p561 && p561 < p640 && p640 != std::string::npos, "the laser beams in wavelength order");
      Check(cu.header.find("\"label\":\"532 nm (custom) laser\"") != std::string::npos &&
               cu.header.find("532 nm (custom) ") != std::string::npos &&
               cu.header.find("\"label\":\"Custom (parametric spectra)") != std::string::npos,
            "a custom laser line and a custom dye in the light path");
   }

   {
      PanelContent c;
      std::string err;
      Check(!MakePanel(base, "nope", "", 1, c, err) && !err.empty(), "an unknown panel fails");
   }

   std::printf("%s\n", g_failures ? "FAILED" : "all checks passed");
   return g_failures ? 1 : 0;
}
