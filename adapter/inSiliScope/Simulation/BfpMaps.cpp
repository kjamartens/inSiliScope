///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpMaps.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See BfpMaps.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BfpMaps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/stat.h>

namespace sim {

uint64_t BfpHashBytes(const void* data, size_t n, uint64_t h)
{
   const uint8_t* p = static_cast<const uint8_t*>(data);
   for (size_t i = 0; i < n; ++i)
   {
      h ^= p[i];
      h *= 1099511628211ULL;
   }
   return h;
}

double BfpImage::Sample(double ux, double uy, double t, double umax) const
{
   if (w <= 0 || h <= 0 || pages <= 0 || !(umax > 0))
      return 0.0;
   if (ux < -umax || ux > umax || uy < -umax || uy > umax)
      return 0.0;
   int p = static_cast<int>(std::floor(t * pages));
   p = std::min(std::max(p, 0), pages - 1);
   // Pixel centres at -umax + (i + 0.5) 2 umax / w; rows from +umax down.
   const double fx = (ux + umax) / (2.0 * umax) * w - 0.5;
   const double fy = (umax - uy) / (2.0 * umax) * h - 0.5;
   const double cx = std::min(std::max(fx, 0.0), static_cast<double>(w - 1));
   const double cy = std::min(std::max(fy, 0.0), static_cast<double>(h - 1));
   const int i0 = std::min(static_cast<int>(std::floor(cx)), std::max(0, w - 2));
   const int j0 = std::min(static_cast<int>(std::floor(cy)), std::max(0, h - 2));
   const int i1 = std::min(i0 + 1, w - 1), j1 = std::min(j0 + 1, h - 1);
   const double ax = cx - i0, ay = cy - j0;
   const float* pl = v.data() + static_cast<size_t>(p) * w * h;
   const double v00 = pl[static_cast<size_t>(j0) * w + i0], v10 = pl[static_cast<size_t>(j0) * w + i1];
   const double v01 = pl[static_cast<size_t>(j1) * w + i0], v11 = pl[static_cast<size_t>(j1) * w + i1];
   return (1 - ay) * ((1 - ax) * v00 + ax * v10) + ay * ((1 - ax) * v01 + ax * v11);
}

namespace {
struct Reader
{
   const uint8_t* d;
   size_t n;
   bool le;
   bool Ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
   uint32_t U16(size_t at) const
   {
      return le ? static_cast<uint32_t>(d[at] | (d[at + 1] << 8)) : static_cast<uint32_t>((d[at] << 8) | d[at + 1]);
   }
   uint32_t U32(size_t at) const
   {
      return le ? (static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
                   (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24))
                : ((static_cast<uint32_t>(d[at]) << 24) | (static_cast<uint32_t>(d[at + 1]) << 16) |
                   (static_cast<uint32_t>(d[at + 2]) << 8) | static_cast<uint32_t>(d[at + 3]));
   }
   uint64_t U64(size_t at) const
   {
      const uint64_t a = U32(at), b = U32(at + 4);
      return le ? (a | (b << 32)) : ((a << 32) | b);
   }
};
} // namespace

bool BfpReadTiff(const uint8_t* data, size_t n, BfpImage& out, std::string& err)
{
   out = BfpImage();
   if (n < 8 || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M')))
   {
      err = "not a TIFF file";
      return false;
   }
   Reader R{ data, n, data[0] == 'I' };
   if (R.U16(2) != 42)
   {
      err = "not a classic TIFF (BigTIFF is not read)";
      return false;
   }
   size_t ifd = R.U32(4);
   int pages = 0;
   while (ifd != 0 && pages < 100000)
   {
      if (!R.Ok(ifd, 2))
      {
         err = "a TIFF directory beyond the file's end";
         return false;
      }
      const uint32_t count = R.U16(ifd);
      if (!R.Ok(ifd + 2, static_cast<size_t>(count) * 12 + 4))
      {
         err = "a TIFF directory beyond the file's end";
         return false;
      }
      uint32_t w = 0, h = 0, bits = 8, comp = 1, spp = 1, fmt = 1, rowsPer = 0xFFFFFFFFu;
      std::vector<uint32_t> offs, lens;
      bool tiled = false;
      for (uint32_t e = 0; e < count; ++e)
      {
         const size_t at = ifd + 2 + static_cast<size_t>(e) * 12;
         const uint32_t tag = R.U16(at), type = R.U16(at + 2), cnt = R.U32(at + 4);
         const size_t tsize = type == 3 ? 2 : (type == 4 ? 4 : (type == 1 ? 1 : 4));
         const size_t valAt = static_cast<size_t>(cnt) * tsize <= 4 ? at + 8 : R.U32(at + 8);
         auto val = [&](uint32_t k) -> uint32_t {
            const size_t p = valAt + k * tsize;
            if (!R.Ok(p, tsize))
               return 0;
            return type == 3 ? R.U16(p) : (type == 1 ? data[p] : R.U32(p));
         };
         switch (tag)
         {
         case 256: w = val(0); break;
         case 257: h = val(0); break;
         case 258: bits = val(0); break;
         case 259: comp = val(0); break;
         case 277: spp = val(0); break;
         case 278: rowsPer = val(0); break;
         case 339: fmt = val(0); break;
         case 273:
            for (uint32_t k = 0; k < cnt; ++k)
               offs.push_back(val(k));
            break;
         case 279:
            for (uint32_t k = 0; k < cnt; ++k)
               lens.push_back(val(k));
            break;
         case 322:
         case 323:
         case 324: tiled = true; break;
         default: break;
         }
      }
      const size_t next = R.U32(ifd + 2 + static_cast<size_t>(count) * 12);
      if (comp != 1 || tiled || spp != 1)
      {
         err = "only uncompressed, untiled single-channel TIFFs are read (compression " + std::to_string(comp) +
               ", samples per pixel " + std::to_string(spp) + ")";
         return false;
      }
      if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h > (1u << 26))
      {
         err = "a TIFF page of " + std::to_string(w) + " x " + std::to_string(h) + " pixels";
         return false;
      }
      if (!(bits == 8 || bits == 16 || bits == 32 || (bits == 64 && fmt == 3)))
      {
         err = "a TIFF of " + std::to_string(bits) + " bits per sample";
         return false;
      }
      if (out.pages == 0)
      {
         out.w = static_cast<int>(w);
         out.h = static_cast<int>(h);
      }
      else if (out.w != static_cast<int>(w) || out.h != static_cast<int>(h))
      {
         err = "the TIFF's pages differ in size";
         return false;
      }
      const size_t bpp = bits / 8, rowBytes = static_cast<size_t>(w) * bpp;
      const size_t base = out.v.size();
      out.v.resize(base + static_cast<size_t>(w) * h);
      size_t row = 0;
      if (rowsPer == 0xFFFFFFFFu || rowsPer == 0)
         rowsPer = h;
      for (size_t s = 0; s < offs.size() && row < h; ++s)
      {
         const size_t rows = std::min<size_t>(rowsPer, h - row);
         if (!R.Ok(offs[s], rows * rowBytes))
         {
            err = "a TIFF strip beyond the file's end";
            return false;
         }
         for (size_t r = 0; r < rows; ++r, ++row)
            for (size_t i = 0; i < w; ++i)
            {
               const size_t p = offs[s] + r * rowBytes + i * bpp;
               double v;
               if (bits == 8)
                  v = fmt == 2 ? static_cast<int8_t>(data[p]) : data[p];
               else if (bits == 16)
                  v = fmt == 2 ? static_cast<int16_t>(R.U16(p)) : R.U16(p);
               else if (bits == 32)
               {
                  const uint32_t u = R.U32(p);
                  if (fmt == 3)
                  {
                     float f;
                     std::memcpy(&f, &u, 4);
                     v = f;
                  }
                  else
                     v = fmt == 2 ? static_cast<double>(static_cast<int32_t>(u)) : u;
               }
               else
               {
                  const uint64_t u = R.U64(p);
                  double f;
                  std::memcpy(&f, &u, 8);
                  v = f;
               }
               out.v[base + row * w + i] = static_cast<float>(v);
            }
      }
      (void)lens;
      if (row < h)
      {
         err = "a TIFF page with fewer rows than it says";
         return false;
      }
      out.pages++;
      ifd = next;
      pages++;
   }
   if (out.pages == 0)
   {
      err = "a TIFF without pages";
      return false;
   }
   out.hash = BfpHashBytes(data, n);
   return true;
}

namespace {
std::mutex& StoreMutex()
{
   static std::mutex m;
   return m;
}
std::map<std::string, std::shared_ptr<const BfpImage>>& Store()
{
   static std::map<std::string, std::shared_ptr<const BfpImage>> s;
   return s;
}
struct FileEntry
{
   long long mtime = 0, size = 0;
   std::shared_ptr<const BfpImage> image;
};
std::map<std::string, FileEntry>& FileCache()
{
   static std::map<std::string, FileEntry> c;
   return c;
}
std::map<std::string, std::string>& StoreErrors()
{
   static std::map<std::string, std::string> e;
   return e;
}
} // namespace

void BfpRegisterImage(const std::string& name, const uint8_t* data, size_t n)
{
   std::lock_guard<std::mutex> g(StoreMutex());
   if (!data || n == 0)
   {
      Store().erase(name);
      StoreErrors().erase(name);
      return;
   }
   auto im = std::make_shared<BfpImage>();
   std::string err;
   if (BfpReadTiff(data, n, *im, err))
   {
      Store()[name] = im;
      StoreErrors().erase(name);
   }
   else
   {
      Store().erase(name);
      StoreErrors()[name] = err;
   }
}

std::shared_ptr<const BfpImage> BfpFindImage(const std::string& name, std::string& err)
{
   std::lock_guard<std::mutex> g(StoreMutex());
   auto it = Store().find(name);
   if (it != Store().end())
      return it->second;
   auto e = StoreErrors().find(name);
   if (e != StoreErrors().end())
   {
      err = "img(\"" + name + "\"): " + e->second;
      return nullptr;
   }
   struct stat st;
   if (stat(name.c_str(), &st) != 0)
   {
      err = "img(\"" + name + "\"): no such file (and none loaded under that name)";
      return nullptr;
   }
   FileEntry& fe = FileCache()[name];
   if (fe.image && fe.mtime == static_cast<long long>(st.st_mtime) && fe.size == static_cast<long long>(st.st_size))
      return fe.image;
   std::ifstream f(name, std::ios::binary);
   std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   auto im = std::make_shared<BfpImage>();
   std::string e2;
   if (!BfpReadTiff(bytes.data(), bytes.size(), *im, e2))
   {
      err = "img(\"" + name + "\"): " + e2;
      FileCache().erase(name);
      return nullptr;
   }
   fe.mtime = static_cast<long long>(st.st_mtime);
   fe.size = static_cast<long long>(st.st_size);
   fe.image = im;
   return im;
}

} // namespace sim
