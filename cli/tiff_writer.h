// Little-endian baseline TIFF, one uncompressed grey page per frame: 16-bit
// unsigned (the camera's ADU) or 32-bit float (the cli's diagnostic outputs,
// scope_probes.h). LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class TiffWriter {
public:
   explicit TiffWriter(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
      if (f_) { const uint8_t hdr[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 }; std::fwrite(hdr, 1, 8, f_); prevIfdLink_ = 4; }
   }
   ~TiffWriter() { if (f_) std::fclose(f_); }
   TiffWriter(const TiffWriter&) = delete;
   TiffWriter& operator=(const TiffWriter&) = delete;
   bool ok() const { return f_ != nullptr; }
   bool Page(const std::vector<uint16_t>& px, uint32_t w, uint32_t h, const std::string& desc) {
      return Write(px.data(), 2, 1, w, h, desc);
   }
   bool Page(const std::vector<float>& px, uint32_t w, uint32_t h, const std::string& desc) {
      return Write(px.data(), 4, 3, w, h, desc);
   }
private:
   // bytes per sample 2 (uint16) or 4 (float32); sampleFormat 1 = unsigned, 3 = IEEE float.
   bool Write(const void* px, uint32_t bytes, uint16_t sampleFormat, uint32_t w, uint32_t h, const std::string& desc) {
      const uint32_t dataOff = Tell();
      std::fwrite(px, bytes, static_cast<size_t>(w) * h, f_);
      uint32_t descOff = 0, descLen = 0;
      if (!desc.empty()) { descOff = Tell(); descLen = (uint32_t)desc.size() + 1; std::fwrite(desc.c_str(), 1, descLen, f_); }
      if (Tell() & 1) std::fputc(0, f_);
      const uint32_t ifd = Tell();
      struct E { uint16_t tag, type; uint32_t count, value; };
      std::vector<E> e = { { 256, 4, 1, w }, { 257, 4, 1, h }, { 258, 3, 1, bytes * 8 }, { 259, 3, 1, 1 }, { 262, 3, 1, 1 } };
      if (descLen) e.push_back({ 270, 2, descLen, descOff });
      e.push_back({ 273, 4, 1, dataOff }); e.push_back({ 277, 3, 1, 1 }); e.push_back({ 278, 4, 1, h });
      e.push_back({ 279, 4, 1, w * h * bytes });
      if (sampleFormat != 1) e.push_back({ 339, 3, 1, sampleFormat });
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
   uint32_t Tell() { return (uint32_t)std::ftell(f_); }
   std::FILE* f_;
   uint32_t prevIfdLink_ = 4;
};
