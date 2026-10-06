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
// true per-frame drift goes to <out without .tif>.drift.csv.
#include "ScopeMovie.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Usage()
{
   std::printf("usage: insiliscope_cli --out file.tif [--option value ...]\n\n"
               "options (default):\n");
   for (const sim::ScopeOption& o : sim::ScopeMovieOptions())
      std::printf("  --%-20s %-8g %s\n", o.name, o.value, o.help);
   std::printf("  --p.<name> <value>     any core world parameter (the prototype's names)\n"
               "  --zern.<j> <waves>     Zernike coefficient j (0-27), replacing the preset's\n"
               "  --mt-dye.<field> <v>   override a field of the microtubules' dye (fluorescent-pct, qy, ext-coeff,\n"
               "  --dye<N>.<field> <v>   on-sec, off-sec, ...: DyeFieldNames), or of dye slot N (1-3)\n"
               "\n"
               "geometry instead of a movie (same world, no rendering):\n"
               "  --geometry-json <file> write the cells around x, y as JSON (ScopeGeometryJson)\n"
               "  --geometry-um <um>     side of that square (160)\n"
               "  --geometry-detail 0|1  also cytoplasm meshes and microtubules (0)\n");
}

// Little-endian baseline TIFF, one uncompressed 16-bit grey page per frame.
class TiffWriter {
public:
   explicit TiffWriter(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
      if (f_) { const uint8_t hdr[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 }; std::fwrite(hdr, 1, 8, f_); prevIfdLink_ = 4; }
   }
   ~TiffWriter() { if (f_) std::fclose(f_); }
   bool ok() const { return f_ != nullptr; }
   bool Page(const std::vector<uint16_t>& px, uint32_t w, uint32_t h, const std::string& desc) {
      const uint32_t dataOff = Tell();
      std::fwrite(px.data(), 2, px.size(), f_);
      uint32_t descOff = 0, descLen = 0;
      if (!desc.empty()) { descOff = Tell(); descLen = (uint32_t)desc.size() + 1; std::fwrite(desc.c_str(), 1, descLen, f_); }
      if (Tell() & 1) std::fputc(0, f_);
      const uint32_t ifd = Tell();
      struct E { uint16_t tag, type; uint32_t count, value; };
      std::vector<E> e = { { 256, 4, 1, w }, { 257, 4, 1, h }, { 258, 3, 1, 16 }, { 259, 3, 1, 1 }, { 262, 3, 1, 1 } };
      if (descLen) e.push_back({ 270, 2, descLen, descOff });
      e.push_back({ 273, 4, 1, dataOff }); e.push_back({ 277, 3, 1, 1 }); e.push_back({ 278, 4, 1, h });
      e.push_back({ 279, 4, 1, w * h * 2 });
      const uint16_t n = (uint16_t)e.size();
      std::fwrite(&n, 2, 1, f_);
      for (const E& x : e) { std::fwrite(&x.tag, 2, 1, f_); std::fwrite(&x.type, 2, 1, f_); std::fwrite(&x.count, 4, 1, f_); std::fwrite(&x.value, 4, 1, f_); }
      const uint32_t next = 0;
      const long linkPos = (long)Tell();
      std::fwrite(&next, 4, 1, f_);
      std::fseek(f_, (long)prevIfdLink_, SEEK_SET);
      std::fwrite(&ifd, 4, 1, f_);
      std::fseek(f_, 0, SEEK_END);
      prevIfdLink_ = (uint32_t)linkPos;
      return !std::ferror(f_);
   }
private:
   uint32_t Tell() { return (uint32_t)std::ftell(f_); }
   std::FILE* f_;
   uint32_t prevIfdLink_ = 4;
};

} // namespace

int main(int argc, char** argv)
{
   std::string out, geometryOut;
   double geometryUm = 160.0;
   bool geometryDetail = false;
   sim::ScopeSpec spec;
   for (int i = 1; i < argc; i++) {
      const std::string a = argv[i];
      if (a == "--help" || a == "-h") { Usage(); return 0; }
      if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
      if (a == "--geometry-json" && i + 1 < argc) { geometryOut = argv[++i]; continue; }
      if (a == "--geometry-um" && i + 1 < argc) { geometryUm = std::atof(argv[++i]); continue; }
      if (a == "--geometry-detail" && i + 1 < argc) { geometryDetail = std::atof(argv[++i]) != 0; continue; }
      double v = 0.0;
      if (a.compare(0, 2, "--") != 0 || i + 1 >= argc || !sim::ScopeOptionValue(a.substr(2), argv[i + 1], v) ||
          !sim::ScopeSpecSet(spec, a.substr(2), v)) {
         std::fprintf(stderr, "unknown or incomplete option %s\n\n", a.c_str());
         Usage();
         return 2;
      }
      i++;
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
   if (sim::ScopeSpecGet(spec, "modality") == 1)
      std::printf("%s: %ld frames %ux%u, BrightField (setup %.2f s), total %.2f s\n", out.c_str(), info.frames,
                  info.width, info.height, info.querySec, info.totalSec);
   else
      std::printf("%s: %ld frames %ux%u, %zu blinks, %ld dyes in continuous populations (setup %.2f s), total %.2f s\n",
                  out.c_str(), info.frames, info.width, info.height, info.blinks, info.dyes, info.querySec, info.totalSec);
   return 0;
}
