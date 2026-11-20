// ctest tiff_writer (2026-11-19): cli/tiff_writer.h's classic and BigTIFF files read back by a small reader of its
// own (the IFD chain, each page's size, offset and pixels, the description), the format chosen from the expected
// size, a page that would pass a classic file's 4 GB refused, a write to a missing folder said in words.
// `tiff_writer_check --big <dir>` also writes a 4.7 GB BigTIFF of mostly zero pages there and reads it back (not a
// ctest: put it on a compressed NTFS folder, or a disk with the room).
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "tiff_writer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Check(bool ok, const std::string& what)
{
   std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok) ++failures;
}

int64_t Tell64(std::FILE* f)
{
#ifdef _WIN32
   return _ftelli64(f);
#else
   return ftello(f);
#endif
}
bool Seek64(std::FILE* f, uint64_t at)
{
#ifdef _WIN32
   return _fseeki64(f, static_cast<int64_t>(at), SEEK_SET) == 0;
#else
   return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}

struct Page { uint32_t w = 0, h = 0, bits = 0; uint64_t offset = 0, bytes = 0; std::string desc; };

// The pages of a little-endian TIFF or BigTIFF (the tags this writer sets); false on anything else.
bool ReadPages(const std::string& path, bool& big, std::vector<Page>& pages, std::string& err)
{
   std::FILE* f = std::fopen(path.c_str(), "rb");
   if (!f) { err = "cannot open"; return false; }
   auto rd = [&](void* p, size_t n) { return std::fread(p, 1, n, f) == n; };
   auto u = [&](int n) { uint64_t v = 0; rd(&v, n); return v; };
   uint8_t hdr[4];
   bool ok = rd(hdr, 4) && hdr[0] == 'I' && hdr[1] == 'I';
   const unsigned magic = hdr[2] | hdr[3] << 8;
   big = magic == 43;
   uint64_t ifd = 0;
   if (ok && big) {
      ok = u(2) == 8 && u(2) == 0;
      ifd = u(8);
   }
   else if (ok && magic == 42) ifd = u(4);
   else ok = false;
   while (ok && ifd) {
      ok = Seek64(f, ifd);
      const uint64_t n = u(big ? 8 : 2);
      Page p;
      uint64_t descAt = 0, descLen = 0;
      for (uint64_t k = 0; ok && k < n; ++k) {
         const uint64_t tag = u(2), type = u(2), count = u(big ? 4 * 2 : 4);
         const int vb = big ? 8 : 4;
         const int64_t at = Tell64(f);
         const uint64_t raw = u(vb);
         const uint64_t v = type == 3 ? (raw & 0xFFFF) : type == 4 ? (raw & 0xFFFFFFFFull) : raw;
         if (tag == 256) p.w = static_cast<uint32_t>(v);
         if (tag == 257) p.h = static_cast<uint32_t>(v);
         if (tag == 258) p.bits = static_cast<uint32_t>(v);
         if (tag == 273) p.offset = v;
         if (tag == 279) p.bytes = v;
         if (tag == 270) { descLen = count; descAt = count <= static_cast<uint64_t>(vb) ? static_cast<uint64_t>(at) : v; }
      }
      ifd = u(big ? 8 : 4);
      if (descLen) {
         const int64_t back = Tell64(f);
         p.desc.resize(descLen);
         ok = ok && Seek64(f, descAt) && rd(&p.desc[0], descLen) && Seek64(f, static_cast<uint64_t>(back));
         if (!p.desc.empty() && p.desc.back() == '\0') p.desc.pop_back();
      }
      pages.push_back(p);
   }
   std::fclose(f);
   if (!ok) err = "not a TIFF this reader knows";
   return ok;
}

std::vector<uint16_t> ReadPixels(const std::string& path, const Page& p, size_t count)
{
   std::vector<uint16_t> px(count);
   std::FILE* f = std::fopen(path.c_str(), "rb");
   if (f && Seek64(f, p.offset) && std::fread(px.data(), 2, count, f) == count) {}
   else px.clear();
   if (f) std::fclose(f);
   return px;
}

void RoundTrip(const std::string& path, bool bigTiff, uint64_t expected, bool wantBig)
{
   const uint32_t w = 37, h = 11, n = 5;   // odd sizes: the pad byte
   TiffWriter t(path, expected, bigTiff);
   Check(t.ok() && t.big() == wantBig, path + ": the format from the expected size");
   std::vector<std::vector<uint16_t>> frames;
   for (uint32_t k = 0; k < n; ++k) {
      std::vector<uint16_t> px(static_cast<size_t>(w) * h);
      for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint16_t>(k * 1000 + i);
      frames.push_back(px);
      t.Page(px, w, h, k == 0 ? "the description of the movie" : (k == 2 ? "abc" : ""));
   }
   Check(t.Close() && t.pages() == n, path + ": written and closed");
   bool big = !wantBig;
   std::vector<Page> pages;
   std::string err;
   Check(ReadPages(path, big, pages, err) && big == wantBig && pages.size() == n, path + ": read back " + err);
   for (uint32_t k = 0; k < pages.size() && k < n; ++k) {
      const Page& p = pages[k];
      const bool same = p.w == w && p.h == h && p.bits == 16 && p.bytes == w * h * 2u &&
                        ReadPixels(path, p, frames[k].size()) == frames[k];
      Check(same, path + ": page " + std::to_string(k) + " size and pixels");
   }
   if (pages.size() == n)
      Check(pages[0].desc == "the description of the movie" && pages[2].desc == "abc" && pages[1].desc.empty(),
            path + ": descriptions (a short one in place)");
}

} // namespace

int main(int argc, char** argv)
{
   const std::string dir = argc > 2 && std::string(argv[1]) == "--big" ? argv[2] : argc > 1 ? argv[1] : ".";
   if (argc > 2 && std::string(argv[1]) == "--big") {
      // 140 pages of 4096 x 4096 (4.7 GB): every page zeros but its first row (the page number) and its last pixel.
      const std::string path = dir + "/tiff_writer_big.tif";
      const uint32_t s = 4096, n = 140;
      const auto t0 = std::chrono::steady_clock::now();
      {
         TiffWriter t(path, TiffWriter::Bytes(s, s, n, 2, 64), false);
         Check(t.big(), "the 4.7 GB file is a BigTIFF");
         std::vector<uint16_t> px(static_cast<size_t>(s) * s, 0);
         for (uint32_t k = 0; k < n && t.ok(); ++k) {
            for (uint32_t i = 0; i < s; ++i) px[i] = static_cast<uint16_t>(k + i);
            px.back() = static_cast<uint16_t>(7 * k + 1);
            t.Page(px, s, s, k == 0 ? "big" : "");
         }
         Check(t.Close() && t.pages() == n, "4.7 GB written: " + t.error());
      }
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      bool big = false;
      std::vector<Page> pages;
      std::string err;
      Check(ReadPages(path, big, pages, err) && big && pages.size() == n, "read back " + err);
      bool same = pages.size() == n;
      for (uint32_t k = 0; same && k < n; ++k) {
         const std::vector<uint16_t> row = ReadPixels(path, pages[k], s);
         std::vector<uint16_t> last(1);
         std::FILE* f = std::fopen(path.c_str(), "rb");
         same = row.size() == s && f && Seek64(f, pages[k].offset + 2ull * s * s - 2) && std::fread(last.data(), 2, 1, f) == 1;
         if (f) std::fclose(f);
         for (uint32_t i = 0; same && i < s; ++i) same = row[i] == static_cast<uint16_t>(k + i);
         same = same && last[0] == static_cast<uint16_t>(7 * k + 1);
      }
      Check(same, "every page's first row and last pixel, past 4 GB too (last page at " +
                  std::to_string(pages.empty() ? 0 : pages.back().offset / 1000000) + " MB)");
      std::printf("written in %.1f s\n", sec);
      return failures ? 1 : 0;
   }
   RoundTrip(dir + "/tiff_classic.tif", false, 0, false);
   RoundTrip(dir + "/tiff_big_forced.tif", true, 0, true);
   RoundTrip(dir + "/tiff_big_expected.tif", false, 5000000000ull, true);   // nothing large written: the size decides
   {
      TiffWriter t(dir + "/no_such_folder/x.tif");
      Check(!t.ok() && t.error().find("cannot open") != std::string::npos, "a missing folder: " + t.error());
   }
   {
      // A classic file expected small that would pass 4 GB: refused before the page, the message says so (the size
      // check comes before the pixels are read: a 65536 x 32769 page of 16-bit samples, 4.3 GB, from one sample).
      TiffWriter t(dir + "/tiff_refused.tif");
      const std::vector<uint16_t> one(1);
      const bool wrote = t.Page(one, 65536, 32769, "");
      Check(!wrote && t.error().find("4 GB of a classic TIFF") != std::string::npos, "past 4 GB in a classic file: " + t.error());
   }
   std::printf("%s\n", failures ? "FAILED" : "PASS");
   return failures ? 1 : 0;
}
