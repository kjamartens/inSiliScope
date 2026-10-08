// insiliscope_cli -- headless cell-field movies: window/seed -> dye blinks
// (core) -> frames (the adapter's Simulation/ render code via
// sim::RenderScopeMovie, the same pipeline as the inSiliScope camera's
// precomputed CellField stack and the viewer's movie panel) -> 16-bit
// multi-page TIFF. PSF: the adapter's default GibsonLanniZernike (C++), or
// Gaussian with --psf-model Gaussian.
//
//   insiliscope_cli --out movie.tif [--seed 42] [--x 0 --y 0] [--frames 1000] ...
//   insiliscope_cli --help
// With drift (--drift-xy-nm-per-sqrt-sec / --drift-z-nm-per-sqrt-sec), the
// true per-frame drift goes to <out without .tif>.drift.csv. Diagnostic
// outputs (--setup-json, --photons-out, --psf-out, ...: scope_probes.h) are
// read-only views of the same movie for the docs' physics figures. --serve
// runs many commands in one process (one per stdin line).
#include "ScopeMovie.h"
#include "scope_probes.h"
#include "tiff_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void Usage()
{
   std::printf("usage: insiliscope_cli --out file.tif [--option value ...]\n\n"
               "options (default):\n");
   for (const sim::ScopeOption& o : sim::ScopeMovieOptions())
      std::printf("  --%-20s %-8g %s\n", o.name, o.value, o.help);
   std::printf("  --spec <file>          options from a file (name=value per line; Micro-Manager's Renderer\n"
               "                         WriteScopeSpecTo writes one), later options override\n"
               "  --p.<name> <value>     any core world parameter (the prototype's names)\n"
               "  --zern.<j> <waves>     Zernike coefficient j (0-27), replacing the preset's\n"
               "  --mt-dye.<field> <v>   override a field of the microtubules' dye (fluorescent-pct, qy, ext-coeff,\n"
               "  --dye<N>.<field> <v>   on-sec, off-sec, ...: DyeFieldNames), or of dye slot N (1-3)\n"
               "\n"
               "geometry instead of a movie (same world, no rendering):\n"
               "  --geometry-json <file> write the cells around x, y as JSON (ScopeGeometryJson)\n"
               "  --geometry-um <um>     side of that square (160)\n"
               "  --geometry-detail 0|1  also cytoplasm meshes and microtubules (0)\n"
               "\n"
               "diagnostic outputs (the docs' physics figures; read-only, no effect on the movie;\n"
               "without --out, only these are written):\n"
               "  --setup-json <file>    the resolved setup: camera, noise chain, light-path curves, labels and\n"
               "                         their states (spectra, rates, detected fraction), PSF request, BrightField\n"
               "  --presets-json <file>  the Quality and Drift preset tables (RenderPresets.h), Zernike presets\n"
               "  --photons-out <f.tif>  each frame's photon image before the camera (float32)\n"
               "  --history-before <k=v,...>  ... of a sample lit from clock 0 to start-sec with these options\n"
               "                         changed (lasers, filters, imager, dyes), the frames then under the spec's\n"
               "                         own (a rate history, as Micro-Manager's illumination history keeps it)\n"
               "  --psf-out <prefix>     the movie's PSF: .planes.tif, .cams.tif, .pupil.tif (wavefront), .json\n"
               "  --splat-out <f.tif>    one blink of 1 photon as a movie draws it (halo cut, interpolation)\n"
               "  --splat-at dx,dy,z     its offset from the centre pixel's centre (px) and height above focus (um)\n"
               "  --dyes-json <file>     dye sites, blink events and continuous windows in --dyes-rect\n"
               "  --dyes-rect x0,y0,x1,y1  world um (default: the FOV); --dyes-z zmin,zmax (um); --dyes-t t0,t1 (s)\n"
               "  --density-out <f.tif>  dye counts per z plane over the FOV (float32, as the mean field bins them)\n"
               "  --density-z zmin,zmax,nz  its planes (0,4,8); --density-up <n> cells per pixel (1)\n"
               "  --bf-screens-out <prefix>  the BrightField phase/attenuation screens per slice, .json\n"
               "  --nucleus-json <file>  the shaped nucleus surface of the cells within --geometry-um\n"
               "\n"
               "  --serve                one command per stdin line (its arguments separated by tabs), each\n"
               "                         answered as a separate run would be, then a line '@@isc-done <exit code>';\n"
               "                         the process keeps its world and PSF kernel between commands\n");
}

// "a,b,c" -> n numbers; false if fewer.
bool ParseList(const char* text, double* out, int n)
{
   std::stringstream ss(text);
   std::string item;
   int k = 0;
   while (k < n && std::getline(ss, item, ','))
      out[k++] = std::atof(item.c_str());
   return k == n;
}

int RunCli(int argc, char** argv)
{
   std::string out, geometryOut;
   double geometryUm = 160.0;
   bool geometryDetail = false;
   // Diagnostic outputs (scope_probes.h).
   std::string setupOut, presetsOut, photonsOut, psfOut, splatOut, dyesOut, densityOut, bfScreensOut, nucleusOut;
   double splatAt[3] = { 0, 0, 0 }, dyesRect[4] = { 0, 0, 0, 0 }, dyesZ[2] = { -1e9, 1e9 }, dyesT[2] = { 0, 1 };
   double densityZ[3] = { 0, 4, 8 }, densityUp = 1;
   bool haveDyesRect = false;
   std::string historyBefore;
   sim::ScopeSpec spec;
   for (int i = 1; i < argc; i++) {
      const std::string a = argv[i];
      if (a == "--help" || a == "-h") { Usage(); return 0; }
      if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
      if (a == "--geometry-json" && i + 1 < argc) { geometryOut = argv[++i]; continue; }
      if (a == "--geometry-um" && i + 1 < argc) { geometryUm = std::atof(argv[++i]); continue; }
      if (a == "--geometry-detail" && i + 1 < argc) { geometryDetail = std::atof(argv[++i]) != 0; continue; }
      if (a == "--history-before" && i + 1 < argc) { historyBefore = argv[++i]; continue; }
      if (i + 1 < argc) {
         struct { const char* name; std::string* dst; } paths[] = {
            { "--setup-json", &setupOut }, { "--presets-json", &presetsOut }, { "--photons-out", &photonsOut },
            { "--psf-out", &psfOut }, { "--splat-out", &splatOut }, { "--dyes-json", &dyesOut },
            { "--density-out", &densityOut }, { "--bf-screens-out", &bfScreensOut }, { "--nucleus-json", &nucleusOut } };
         bool took = false;
         for (auto& p : paths)
            if (a == p.name) { *p.dst = argv[++i]; took = true; break; }
         if (took) continue;
         struct { const char* name; double* dst; int n; bool* flag; } lists[] = {
            { "--splat-at", splatAt, 3, nullptr }, { "--dyes-rect", dyesRect, 4, &haveDyesRect },
            { "--dyes-z", dyesZ, 2, nullptr }, { "--dyes-t", dyesT, 2, nullptr }, { "--density-z", densityZ, 3, nullptr },
            { "--density-up", &densityUp, 1, nullptr } };
         for (auto& l : lists)
            if (a == l.name) {
               if (!ParseList(argv[++i], l.dst, l.n)) {
                  std::fprintf(stderr, "%s needs %d comma-separated numbers\n", l.name, l.n);
                  return 2;
               }
               if (l.flag) *l.flag = true;
               took = true;
               break;
            }
         if (took) continue;
      }
      if (a == "--spec" && i + 1 < argc) {
         // A spec file (name=value per line, e.g. Micro-Manager's Renderer
         // WriteScopeSpecTo): its options at this point, later ones override.
         std::ifstream f(argv[++i], std::ios::binary);
         std::stringstream text;
         text << f.rdbuf();
         std::string s = text.str(), err;
         s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
         if (!f || !sim::ParseScopeSpec(s, spec, err)) {
            std::fprintf(stderr, "--spec %s: %s\n", argv[i], f ? err.c_str() : "cannot read");
            return 2;
         }
         continue;
      }
      double v = 0.0;
      if (a.compare(0, 2, "--") != 0 || i + 1 >= argc || !sim::ScopeOptionValue(a.substr(2), argv[i + 1], v) ||
          !sim::ScopeSpecSet(spec, a.substr(2), v)) {
         std::fprintf(stderr, "unknown or incomplete option %s\n\n", a.c_str());
         Usage();
         return 2;
      }
      i++;
   }
   // --history-before: the past's spec, the movie's with those options changed.
   sim::ScopeSpec past = spec;
   {
      std::stringstream ss(historyBefore);
      std::string item;
      while (std::getline(ss, item, ',')) {
         const size_t eq = item.find('=');
         double v = 0.0;
         if (eq == std::string::npos || !sim::ScopeOptionValue(item.substr(0, eq), item.substr(eq + 1).c_str(), v) ||
             !sim::ScopeSpecSet(past, item.substr(0, eq), v)) {
            std::fprintf(stderr, "--history-before: bad option '%s'\n", item.c_str());
            return 2;
         }
      }
   }
   {
      // Diagnostic outputs first; without --out they are all that is written.
      std::string err;
      bool any = false, ok = true;
      auto run = [&](const std::string& path, const char* what, const std::function<bool()>& fn) {
         if (path.empty() || !ok) return;
         any = true;
         if (!fn()) { std::fprintf(stderr, "%s %s: %s\n", what, path.c_str(), err.c_str()); ok = false; return; }
         std::printf("%s: %s\n", path.c_str(), what);
      };
      run(setupOut, "setup", [&] { return probes::SetupJson(spec, setupOut, err); });
      run(presetsOut, "presets", [&] { return probes::PresetsJson(presetsOut, err); });
      run(psfOut, "PSF", [&] { return probes::Psf(spec, psfOut, err); });
      run(splatOut, "splat", [&] { return probes::Splat(spec, splatAt[0], splatAt[1], splatAt[2], splatOut, err); });
      run(dyesOut, "dyes", [&] {
         if (!haveDyesRect) {
            unsigned W, H;
            long n;
            sim::ScopeMovieDims(spec, W, H, n);
            const double um = sim::ScopeSpecGet(spec, "pixel-nm") / 1000.0, x = sim::ScopeSpecGet(spec, "x"),
                         y = sim::ScopeSpecGet(spec, "y");
            dyesRect[0] = x - W * um / 2; dyesRect[1] = y - H * um / 2; dyesRect[2] = x + W * um / 2; dyesRect[3] = y + H * um / 2;
         }
         return probes::DyesJson(spec, dyesRect, dyesZ[0], dyesZ[1], dyesT[0], dyesT[1], dyesOut, err);
      });
      run(densityOut, "dye density", [&] {
         return probes::Density(spec, static_cast<int>(densityZ[2]), densityZ[0], densityZ[1], static_cast<int>(densityUp),
                                densityOut, err);
      });
      run(bfScreensOut, "BrightField screens", [&] { return probes::BrightfieldScreens(spec, bfScreensOut, err); });
      run(nucleusOut, "nuclei", [&] { return probes::NucleusJson(spec, geometryUm, nucleusOut, err); });
      run(photonsOut, "photons", [&] {
         sim::ScopeMovieInfo pinfo;
         if (!probes::Photons(spec, photonsOut, pinfo, err, historyBefore.empty() ? nullptr : &past)) return false;
         std::printf("%s: %ld frames %ux%u (setup %.4f s), total %.4f s\n", photonsOut.c_str(), pinfo.frames, pinfo.width,
                     pinfo.height, pinfo.querySec, pinfo.totalSec);
         return true;
      });
      if (!ok) return 1;
      if (any && out.empty() && geometryOut.empty()) return 0;
   }
   if (!geometryOut.empty()) {
      std::string json, err;
      if (!sim::ScopeGeometryJson(spec, geometryUm, geometryDetail, json, err)) {
         std::fprintf(stderr, "%s\n", err.c_str());
         return 1;
      }
      FILE* f = std::fopen(geometryOut.c_str(), "wb");
      const bool wrote = f && std::fwrite(json.data(), 1, json.size(), f) == json.size();
      if (f) std::fclose(f);
      if (!wrote) { std::fprintf(stderr, "cannot write %s\n", geometryOut.c_str()); return 1; }
      std::printf("%s: geometry of %g um around (%g, %g), %zu bytes\n", geometryOut.c_str(), geometryUm,
                  sim::ScopeSpecGet(spec, "x"), sim::ScopeSpecGet(spec, "y"), json.size());
      return 0;
   }
   if (sim::ScopeSpecGet(spec, "prepare") >= 1) {
      // The world and the PSF kernel only (warms the memo and, with
      // --disk-cache 2, the kernel file); no TIFF.
      sim::ScopeMovieInfo info;
      std::string err;
      if (!sim::RenderScopeMovie(spec, [](long, const std::vector<uint16_t>&) { return true; }, info, err)) {
         std::fprintf(stderr, "%s\n", err.c_str());
         return 1;
      }
      std::printf("prepared: world %.2f s, PSF kernel %.2f s (disk-cache %g)\n", info.querySec, info.totalSec - info.querySec,
                  sim::ScopeSpecGet(spec, "disk-cache"));
      return 0;
   }
   if (out.empty()) { Usage(); return 2; }

   TiffWriter tif(out);
   if (!tif.ok()) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
   sim::ScopeMovieInfo info;
   std::string err;
   bool writeOk = true;
   const bool ok = sim::RenderScopeMovie(spec, [&](long f, const std::vector<uint16_t>& adu) {
      writeOk = tif.Page(adu, info.width, info.height, f == 0 ? info.description : "");
      return writeOk;
   }, info, err);
   if (!ok) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
   if (!writeOk) { std::fprintf(stderr, "write error\n"); return 1; }
   if (!info.driftNm.empty()) {
      // The true sample drift per frame (ground truth for drift correction).
      const std::string csv = (out.size() > 4 && out.compare(out.size() - 4, 4, ".tif") == 0 ? out.substr(0, out.size() - 4) : out) +
                              ".drift.csv";
      FILE* f = std::fopen(csv.c_str(), "wb");
      if (!f) { std::fprintf(stderr, "cannot write %s\n", csv.c_str()); return 1; }
      std::fprintf(f, "frame,dx_nm,dy_nm,dz_nm\n");
      for (size_t k = 0; k + 2 < info.driftNm.size(); k += 3)
         std::fprintf(f, "%zu,%.6f,%.6f,%.6f\n", k / 3, info.driftNm[k], info.driftNm[k + 1], info.driftNm[k + 2]);
      std::fclose(f);
      std::printf("%s: sample drift per frame\n", csv.c_str());
   }
   bool epi = false, trans = false;
   sim::ScopeLights(spec, epi, trans);
   if (trans && !epi)
      std::printf("%s: %ld frames %ux%u, BrightField (setup %.4f s), total %.4f s\n", out.c_str(), info.frames,
                  info.width, info.height, info.querySec, info.totalSec);
   else
      std::printf("%s: %ld frames %ux%u, %zu blinks, %ld dyes in continuous populations (setup %.4f s), total %.4f s\n",
                  out.c_str(), info.frames, info.width, info.height, info.blinks, info.dyes, info.querySec, info.totalSec);
   return 0;
}

// --serve: many runs in one process (the docs' figure builder sends its cli
// commands this way). Each stdin line is one command's arguments, separated by
// tabs; it runs exactly as a separate process would, except that the movie
// cache (world, scenes, PSF kernel memo) stays warm between commands.
int Serve()
{
   std::setvbuf(stdout, nullptr, _IONBF, 0); // the reader waits for the done line
   std::string line;
   while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      std::vector<std::string> args{ "insiliscope_cli" };
      std::stringstream ss(line);
      std::string a;
      while (std::getline(ss, a, '\t'))
         if (!a.empty()) args.push_back(a);
      std::vector<char*> argv;
      for (std::string& x : args) argv.push_back(&x[0]);
      argv.push_back(nullptr);
      const int code = RunCli(static_cast<int>(args.size()), argv.data());
      std::fflush(stderr);
      std::printf("@@isc-done %d\n", code);
   }
   return 0;
}

} // namespace

int main(int argc, char** argv)
{
   if (argc == 2 && std::string(argv[1]) == "--serve") return Serve();
   return RunCli(argc, argv);
}
