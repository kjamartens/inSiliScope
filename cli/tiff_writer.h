// Little-endian baseline TIFF, one uncompressed grey page per frame: 16-bit
// unsigned (the camera's ADU) or 32-bit float (the cli's diagnostic outputs,
// scope_probes.h). LICENSE: BSD-3-Clause (see LICENSE at the repository root)
//
// Large files (2026-11-19): a classic TIFF's offsets are 32-bit, so it ends at 4 GB. A file expected past that
// (the constructor's expectedBytes, TiffWriter::Bytes) is written as a BigTIFF (magic 43, 64-bit offsets: Fiji's
// Bio-Formats, tifffile / napari read it; plain ImageJ 1.x does not, 1.54f checked); below it the classic file is byte for byte the old one. A
// classic file that would pass 4 GB anyway is refused with a message, never written with wrapped offsets. File
// positions are 64-bit (Windows' long is 32-bit: ftell stopped at 2 GB). Every page is flushed and checked, so a full
// disk is reported at the page it hit (error()).
//
// A movie in strips (2026-11-20, docs/dev/LARGE_RECORDS.md): every frame's first rows come before any frame's next
// ones. PageRows writes a page with its first rows and leaves the rest of its data a hole (the file's zeros), Rows
// fills a page's later rows in place: once every row is written the file is the one Page writes, byte for byte.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class TiffWriter {
public:
   // The classic format's end: offsets are uint32.
   static constexpr uint64_t kClassicLimit = 0xFFFFFFFFull;

   // The bytes of a file of `pages` pages of w x h samples of `bytesPerSample`, with a description of descBytes on
   // the first (an upper bound: BigTIFF's tags).
   static uint64_t Bytes(uint64_t w, uint64_t h, uint64_t pages, uint64_t bytesPerSample, uint64_t descBytes) {
      return 16 + pages * (w * h * bytesPerSample + 8 + 8 + 13 * 20 + 8) + descBytes + 2;
   }

   // expectedBytes: the file's expected size (Bytes); at or above the classic limit (less a 64 MB margin for
   // descriptions and tags) the file is a BigTIFF. bigTiff forces it.
   explicit TiffWriter(const std::string& path, uint64_t expectedBytes = 0, bool bigTiff = false)
      : f_(std::fopen(path.c_str(), "wb")), path_(path),
        big_(bigTiff || expectedBytes + (64ull << 20) >= kClassicLimit) {
      if (!f_) { error_ = "cannot open " + path + " for writing: " + std::strerror(errno); return; }
      if (big_) {
         const uint8_t hdr[16] = { 'I', 'I', 43, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
         Put(hdr, 16);
         prevIfdLink_ = 8;
      } else {
         const uint8_t hdr[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
         Put(hdr, 8);
         prevIfdLink_ = 4;
      }
   }
   ~TiffWriter() { if (f_) std::fclose(f_); }
   TiffWriter(const TiffWriter&) = delete;
   TiffWriter& operator=(const TiffWriter&) = delete;
   bool ok() const { return f_ != nullptr && error_.empty(); }
   bool big() const { return big_; }
   uint64_t pages() const { return pages_; }
   uint64_t bytes() const { return end_; }
   // What went wrong (the file, the page, the system's reason), "" when nothing did.
   const std::string& error() const { return error_; }
   bool Page(const std::vector<uint16_t>& px, uint32_t w, uint32_t h, const std::string& desc) {
      return Write(px.data(), 2, 1, w, h, desc);
   }
   bool Page(const std::vector<float>& px, uint32_t w, uint32_t h, const std::string& desc) {
      return Write(px.data(), 4, 3, w, h, desc);
   }
   // A 16-bit page of which only rows 0 .. rows - 1 are given (rows x w samples); Rows writes the others.
   bool PageRows(const uint16_t* px, uint32_t rows, uint32_t w, uint32_t h, const std::string& desc) {
      return Write(px, 2, 1, w, h, desc, static_cast<uint64_t>(rows < h ? rows : h) * w * 2);
   }
   // Rows y0 .. y0 + rows - 1 of page `page` (0-based, written by PageRows or Page), rows x the page's width samples.
   bool Rows(uint64_t page, uint32_t y0, uint32_t rows, const uint16_t* px) {
      if (!ok()) return false;
      if (page >= pages_ || static_cast<uint64_t>(y0) + rows > pageH_[page]) {
         error_ = "cannot write " + path_ + ": rows " + std::to_string(y0) + ".." + std::to_string(y0 + rows) + " of page " +
                  std::to_string(page + 1) + " are not in it";
         return false;
      }
      const uint64_t keep = end_, row = static_cast<uint64_t>(pageW_[page]) * 2;
      errno = 0;
      if (Seek64(f_, static_cast<int64_t>(dataOff_[page] + y0 * row)) != 0) { Fail("seeking"); return false; }
      Put(px, static_cast<size_t>(rows * row));
      end_ = keep;
      if (Seek64(f_, static_cast<int64_t>(end_)) != 0) { Fail("seeking"); return false; }
      if (std::fflush(f_) != 0 || std::ferror(f_)) { Fail("writing"); return false; }
      return error_.empty();
   }
   // Closes the file; false when the last bytes could not be written.
   bool Close() {
      if (!f_) return error_.empty();
      const bool closed = std::fclose(f_) == 0;
      f_ = nullptr;
      if (!closed && error_.empty()) Fail("closing it");
      return error_.empty();
   }
private:
   static int64_t Tell64(std::FILE* f) {
#ifdef _WIN32
      return _ftelli64(f);
#else
      return static_cast<int64_t>(ftello(f));
#endif
   }
   static int Seek64(std::FILE* f, int64_t at) {
#ifdef _WIN32
      return _fseeki64(f, at, SEEK_SET);
#else
      return fseeko(f, static_cast<off_t>(at), SEEK_SET);
#endif
   }
   void Fail(const char* what) {
      const int e = errno;
      error_ = "cannot write " + path_ + " (" + what + ", page " + std::to_string(pages_ + 1) + ", " +
               std::to_string(end_ / 1000000) + " MB written): " + (e ? std::strerror(e) : "write error");
   }
   void Put(const void* p, size_t n) {
      if (!error_.empty()) return;
      if (std::fwrite(p, 1, n, f_) != n) Fail("writing");
      end_ += n;
   }
   template <class T> void PutLE(T v) { Put(&v, sizeof v); }   // (x86 / ARM / WebAssembly: little-endian)

   // bytes per sample 2 (uint16) or 4 (float32); sampleFormat 1 = unsigned, 3 = IEEE float.
   // given: the bytes of px written (the page's first rows), the rest left a hole; ~0: the whole page.
   bool Write(const void* px, uint32_t bytes, uint16_t sampleFormat, uint32_t w, uint32_t h, const std::string& desc,
              uint64_t given = ~0ull) {
      if (!ok()) return false;
      const uint64_t pageBytes = static_cast<uint64_t>(w) * h * bytes;
      if (given > pageBytes) given = pageBytes;
      const uint32_t descLen = desc.empty() ? 0 : static_cast<uint32_t>(desc.size()) + 1;
      if (!big_ && end_ + pageBytes + descLen + 2 + 2 + 11 * 12 + 4 > kClassicLimit) {
         error_ = "cannot write " + path_ + ": page " + std::to_string(pages_ + 1) + " would pass the 4 GB of a classic TIFF "
                  "(the file was expected to be smaller; insiliscope_cli --bigtiff 1 writes a BigTIFF)";
         return false;
      }
      const uint64_t dataOff = end_;
      errno = 0;
      Put(px, static_cast<size_t>(given));
      if (given < pageBytes) {
         end_ += pageBytes - given;   // (a hole: Rows fills it)
         if (error_.empty() && Seek64(f_, static_cast<int64_t>(end_)) != 0) { Fail("seeking"); return false; }
      }
      dataOff_.push_back(dataOff);
      pageW_.push_back(w);
      pageH_.push_back(h);
      uint64_t descOff = 0;
      if (descLen) { descOff = end_; Put(desc.c_str(), descLen); }
      if (end_ & 1) PutLE<uint8_t>(0);
      const uint64_t ifd = end_;
      struct E { uint16_t tag, type; uint32_t count; uint64_t value; };
      const uint16_t offType = big_ ? 16 : 4;   // LONG8 / LONG
      std::vector<E> e = { { 256, 4, 1, w }, { 257, 4, 1, h }, { 258, 3, 1, bytes * 8 }, { 259, 3, 1, 1 }, { 262, 3, 1, 1 } };
      if (descLen) e.push_back({ 270, 2, descLen, descOff });
      e.push_back({ 273, offType, 1, dataOff }); e.push_back({ 277, 3, 1, 1 }); e.push_back({ 278, 4, 1, h });
      e.push_back({ 279, offType, 1, pageBytes });
      if (sampleFormat != 1) e.push_back({ 339, 3, 1, sampleFormat });
      uint64_t linkPos = 0;
      if (big_) {
         PutLE<uint64_t>(e.size());
         for (const E& x : e) {
            PutLE(x.tag); PutLE(x.type); PutLE<uint64_t>(x.count);
            // the value left-justified in 8 bytes; a description of up to 8 bytes in place (never: it holds the spec)
            if (x.tag == 270 && descLen <= 8) { uint8_t v[8] = {}; std::memcpy(v, desc.c_str(), descLen); Put(v, 8); }
            else if (x.type == 3) { PutLE<uint16_t>(static_cast<uint16_t>(x.value)); PutLE<uint16_t>(0); PutLE<uint32_t>(0); }
            else if (x.type == 4) { PutLE<uint32_t>(static_cast<uint32_t>(x.value)); PutLE<uint32_t>(0); }
            else PutLE<uint64_t>(x.value);
         }
         linkPos = end_;
         PutLE<uint64_t>(0);
      } else {
         PutLE<uint16_t>(static_cast<uint16_t>(e.size()));
         for (const E& x : e) {
            PutLE(x.tag); PutLE(x.type); PutLE(x.count);
            if (x.tag == 270 && descLen <= 4) { uint8_t v[4] = {}; std::memcpy(v, desc.c_str(), descLen); Put(v, 4); }
            else PutLE<uint32_t>(static_cast<uint32_t>(x.value));   // (a SHORT left-justified: little-endian)
         }
         linkPos = end_;
         PutLE<uint32_t>(0);
      }
      // the previous page's link to this one
      if (!error_.empty()) return false;
      if (Seek64(f_, static_cast<int64_t>(prevIfdLink_)) != 0) { Fail("seeking"); return false; }
      const uint64_t keep = end_;
      if (big_) PutLE<uint64_t>(ifd); else PutLE<uint32_t>(static_cast<uint32_t>(ifd));
      end_ = keep;
      if (std::fseek(f_, 0, SEEK_END) != 0) { Fail("seeking"); return false; }
      if (std::fflush(f_) != 0) { Fail("writing"); return false; }   // a full disk shows here, at its page
      prevIfdLink_ = linkPos;
      if (!error_.empty()) return false;
      if (std::ferror(f_)) { Fail("writing"); return false; }
      ++pages_;
      return true;
   }
   std::FILE* f_;
   std::string path_, error_;
   bool big_ = false;
   uint64_t prevIfdLink_ = 4, end_ = 0, pages_ = 0;
   std::vector<uint64_t> dataOff_;   // each page's data (Rows)
   std::vector<uint32_t> pageW_, pageH_;
};
