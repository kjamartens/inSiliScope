// insilicell_cli -- headless cell-field movies: window/seed -> dye blinks
// (core) -> frames (the adapter's Simulation/ render code via
// sim::RenderScopeMovie, the same pipeline as the inSiliCellScope camera's
// precomputed CellField stack and the viewer's movie panel) -> 16-bit
// multi-page TIFF. Gaussian PSF only.
//
//   insilicell_cli --out movie.tif [--seed 42] [--x 0 --y 0] [--frames 1000] ...
//   insilicell_cli --help
#include "ScopeMovie.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Usage()
{
   std::printf("usage: insilicell_cli --out file.tif [--option value ...]\n\n"
               "options (default):\n");
   for (const sim::ScopeOption& o : sim::ScopeMovieOptions())
      std::printf("  --%-20s %-8g %s\n", o.name, o.value, o.help);
   std::printf("  --p.<name> <value>     any core world parameter (the prototype's names)\n");
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
   std::string out;
   sim::ScopeSpec spec;
   for (int i = 1; i < argc; i++) {
      const std::string a = argv[i];
      if (a == "--help" || a == "-h") { Usage(); return 0; }
      if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
      if (a.compare(0, 2, "--") != 0 || i + 1 >= argc || !sim::ScopeSpecSet(spec, a.substr(2), std::atof(argv[i + 1]))) {
         std::fprintf(stderr, "unknown or incomplete option %s\n\n", a.c_str());
         Usage();
         return 2;
      }
      i++;
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
   std::printf("%s: %ld frames %ux%u, %zu blinks (query %.2f s), total %.2f s\n", out.c_str(), info.frames,
               info.width, info.height, info.blinks, info.querySec, info.totalSec);
   return 0;
}
