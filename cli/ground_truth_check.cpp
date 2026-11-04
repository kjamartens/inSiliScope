// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// ctest ground_truth: the ground truth a movie gives with a GroundTruthScope (Simulation/GroundTruth.h,
// docs/ground-truth.md).
//   - the frames are the same with and without it, bit for bit;
//   - a dSTORM nanoruler movie with beads: a row per frame; every row at least the threshold on the camera and no more
//     than its detected photons; a blink whose PSF window lies inside the image has all of them on the camera (the
//     kernel's planes sum to 1: within 1e-4, the sub-pixel interpolation's; no halo cut); every blink has its binding site; the beads are rows of kind fiducial;
//     the initial-ON dyes in the dye list but no dyes.csv among its files (localizations or a dye list, never both);
//     the same files from a second run;
//   - a WideField movie: dyes.csv and no localization table; every dye's photons at most its rate x the movie's time;
//     each density volume an ImageJ
//     float32 stack holding its dyes' emitted photons (the sum).
//
//   ground_truth_check          exit code 0 = all checks passed
#include "GroundTruth.h"
#include "Voxelize.h"
#include "ScopeMovie.h"
#include "deflate_reference.inc"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok)
      g_failures++;
}

// A movie of spec (frames as one buffer), with gt in scope when given.
bool Movie(const std::string& text, GroundTruth* gt, std::vector<uint16_t>& frames, std::string& err)
{
   ScopeSpec spec;
   if (!ParseScopeSpec(text, spec, err))
      return false;
   frames.clear();
   GroundTruthScope scope(gt);
   ScopeMovieInfo info;
   return RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
      frames.insert(frames.end(), adu.begin(), adu.end());
      return true;
   }, info, err);
}

std::string Join(const std::vector<std::pair<std::string, std::string>>& files)
{
   std::string s;
   for (const auto& f : files)
      s += f.first + '\n' + std::to_string(f.second.size()) + '\n' + f.second;
   return s;
}

// A minimal zlib inflater (stored, fixed and dynamic Huffman blocks; RFC 1950/1951): reads the deflated stacks and
// checks vox::Deflate. Throws on a malformed stream.
struct InBits
{
   const unsigned char* p;
   size_t n, i = 0;
   uint32_t acc = 0;
   int k = 0;
   int Get(int bits)
   {
      while (k < bits)
      {
         if (i >= n)
            throw 1;
         acc |= static_cast<uint32_t>(p[i++]) << k;
         k += 8;
      }
      const int v = static_cast<int>(acc & ((1u << bits) - 1));
      acc = bits == 32 ? 0 : acc >> bits;
      k -= bits;
      return v;
   }
};
struct Huff
{
   int count[16] = {};
   std::vector<int> sym;
   void Build(const int* len, int n)
   {
      std::fill(count, count + 16, 0);
      for (int i = 0; i < n; ++i)
         count[len[i]]++;
      int offs[16] = {};
      for (int l = 1; l < 15; ++l)
         offs[l + 1] = offs[l] + count[l];
      sym.assign(static_cast<size_t>(n), 0);
      for (int i = 0; i < n; ++i)
         if (len[i])
            sym[static_cast<size_t>(offs[len[i]]++)] = i;
   }
   int Decode(InBits& b) const
   {
      int code = 0, first = 0, index = 0;
      for (int l = 1; l < 16; ++l)
      {
         code |= b.Get(1);
         const int c = count[l];
         if (code - c < first)
            return sym[static_cast<size_t>(index + (code - first))];
         index += c;
         first = (first + c) << 1;
         code <<= 1;
      }
      throw 2;
   }
};
bool Inflate(const unsigned char* p, size_t n, std::string& out)
{
   static const int lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                  115, 131, 163, 195, 227, 258 };
   static const int lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
   static const int dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                  1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
   static const int dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                                 13, 13 };
   try
   {
      if (n < 6 || (p[0] & 15) != 8 || ((p[0] << 8) | p[1]) % 31)
         return false;
      InBits b{ p + 2, n - 6 };
      int last;
      do
      {
         last = b.Get(1);
         const int type = b.Get(2);
         if (type == 0)
         {
            b.acc = 0;
            b.k = 0;
            if (b.i + 4 > b.n)
               return false;
            const size_t len = b.p[b.i] | (b.p[b.i + 1] << 8);
            b.i += 4;
            if (b.i + len > b.n)
               return false;
            out.append(reinterpret_cast<const char*>(b.p + b.i), len);
            b.i += len;
            continue;
         }
         if (type == 3)
            return false;
         Huff lit, dist;
         int len[320];
         int nlen = 288, ndist = 30;
         if (type == 1)
         {
            for (int i = 0; i < 288; ++i)
               len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (int i = 0; i < 30; ++i)
               len[288 + i] = 5;
         }
         else
         {
            nlen = b.Get(5) + 257;
            ndist = b.Get(5) + 1;
            const int ncode = b.Get(4) + 4;
            static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int cl[19] = {};
            for (int i = 0; i < ncode; ++i)
               cl[order[i]] = b.Get(3);
            Huff lc;
            lc.Build(cl, 19);
            int at = 0;
            while (at < nlen + ndist)
            {
               const int sy = lc.Decode(b);
               if (sy < 16)
                  len[at++] = sy;
               else
               {
                  int v = 0, r;
                  if (sy == 16)
                  {
                     if (!at)
                        return false;
                     v = len[at - 1];
                     r = 3 + b.Get(2);
                  }
                  else if (sy == 17)
                     r = 3 + b.Get(3);
                  else
                     r = 11 + b.Get(7);
                  if (at + r > nlen + ndist)
                     return false;
                  while (r--)
                     len[at++] = v;
               }
            }
            std::memmove(len + 288, len + nlen, sizeof(int) * static_cast<size_t>(ndist));
         }
         lit.Build(len, nlen);
         dist.Build(len + 288, ndist);
         for (;;)
         {
            const int sy = lit.Decode(b);
            if (sy < 256)
               out.push_back(static_cast<char>(sy));
            else if (sy == 256)
               break;
            else
            {
               if (sy > 285)
                  return false;
               const size_t l = static_cast<size_t>(lbase[sy - 257] + b.Get(lext[sy - 257]));
               const int ds = dist.Decode(b);
               if (ds > 29)
                  return false;
               const size_t d = static_cast<size_t>(dbase[ds] + b.Get(dext[ds]));
               if (d > out.size())
                  return false;
               for (size_t k = 0; k < l; ++k)
                  out.push_back(out[out.size() - d]);
            }
         }
      } while (!last);
      // the Adler-32 after the deflate stream (byte aligned)
      const unsigned char* t = b.p + b.i;
      uint32_t a = 1, s2 = 0;
      for (unsigned char c : out)
      {
         a = (a + c) % 65521;
         s2 = (s2 + a) % 65521;
      }
      const uint32_t want = (static_cast<uint32_t>(t[0]) << 24) | (static_cast<uint32_t>(t[1]) << 16) |
                            (static_cast<uint32_t>(t[2]) << 8) | t[3];
      return want == ((s2 << 16) | a);
   }
   catch (int)
   {
      return false;
   }
}

// A density stack GroundTruth::Volumes wrote (Voxelize.h): an IFD per plane, each strip deflated (compression 8),
// PackBits (32773) or raw, 8 / 16 / 32 bits; sum = the samples x the description's insiliscope.scale; imagej: an ImageJ description
// with its unit, the IFD count = images=, the origin, and a slice label per plane (IJMetadata).
struct Tiff
{
   double sum = 0, scale = 1, top = 0;
   uint32_t w = 0, h = 0;
   long planes = 0;
   bool imagej = false;
   std::string desc;
   std::string Get(const std::string& key) const
   {
      const size_t at = desc.find("\n" + key + "=");
      if (at == std::string::npos)
         return "";
      const size_t from = at + key.size() + 2;
      return desc.substr(from, desc.find('\n', from) - from);
   }
};
Tiff ReadTiff(const std::string& b)
{
   Tiff t;
   uint32_t ifd;
   std::memcpy(&ifd, b.data() + 4, 4);
   bool sizes = true;
   uint32_t labels = 0;
   std::vector<std::string> planes;
   int bits = 32;
   while (ifd)
   {
      uint16_t n;
      std::memcpy(&n, b.data() + ifd, 2);
      uint32_t off = 0, count = 0, desc = 0, comp = 1;
      for (uint16_t i = 0; i < n; ++i)
      {
         const char* e = b.data() + ifd + 2 + 12 * i;
         uint16_t tag, type;
         uint32_t v, c;
         std::memcpy(&tag, e, 2);
         std::memcpy(&type, e + 2, 2);
         std::memcpy(&c, e + 4, 4);
         std::memcpy(&v, e + 8, 4);
         if (type == 3)
            v &= 0xFFFF;
         if (tag == 256) t.w = v;
         if (tag == 257) t.h = v;
         if (tag == 258) bits = static_cast<int>(v);
         if (tag == 259) comp = v;
         if (tag == 273) off = v;
         if (tag == 279) count = v;
         if (tag == 270) desc = v;
         if (tag == 50838) labels = c - 1;
      }
      if (desc && t.desc.empty())
         t.desc = std::string(b.data() + desc);
      std::string raw;
      if (comp == 8)
      {
         if (!Inflate(reinterpret_cast<const unsigned char*>(b.data()) + off, count, raw))
            raw.clear();
      }
      else if (comp == 32773)
         for (size_t i = 0; i < count;)
         {
            const int c = static_cast<signed char>(b[off + i++]);
            if (c >= 0) { raw.append(b, off + i, static_cast<size_t>(c) + 1); i += static_cast<size_t>(c) + 1; }
            else if (c != -128) { raw.append(static_cast<size_t>(1 - c), b[off + i]); ++i; }
         }
      else
         raw = b.substr(off, count);
      sizes = sizes && raw.size() == static_cast<size_t>(t.w) * t.h * static_cast<size_t>(bits / 8);
      planes.push_back(std::move(raw));
      t.planes++;
      std::memcpy(&ifd, b.data() + ifd + 2 + 12 * n, 4);
   }
   t.desc = "\n" + t.desc;
   if (bits != 32)
      t.scale = std::atof(t.Get("insiliscope.scale").c_str());
   for (const std::string& raw : planes)
      for (size_t i = 0; i + bits / 8 <= raw.size(); i += static_cast<size_t>(bits / 8))
      {
         double v;
         if (bits == 8)
            v = static_cast<unsigned char>(raw[i]);
         else if (bits == 16)
            v = static_cast<unsigned char>(raw[i]) + 256.0 * static_cast<unsigned char>(raw[i + 1]);
         else
         {
            float f;
            std::memcpy(&f, raw.data() + i, 4);
            v = f;
         }
         t.top = std::max(t.top, v);
         t.sum += v * t.scale;
      }
   t.imagej = sizes && t.desc.compare(0, 13, "\nImageJ=1.11a") == 0 && t.Get("unit") == "micron" &&
              !t.Get("zorigin").empty() && std::atol(t.Get("images").c_str()) == t.planes &&
              labels == static_cast<uint32_t>(t.planes);
   return t;
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();

   // ---- vox::Deflate: what the inflater reads back ----
   {
      std::vector<std::string> cases(5);
      cases[1].assign(1000000, '\0');                         // an empty plane
      for (size_t i = 0; i < 300000; ++i)                     // sparse values in zeros, runs of every length
         cases[2].push_back(static_cast<char>((i * 2654435761u >> 13) % 97 < 3 ? (i * 7) % 256 : 0));
      uint32_t r = 12345;
      for (size_t i = 0; i < 200000; ++i)                     // random bytes (no runs)
      {
         r = r * 1664525u + 1013904223u;
         cases[3].push_back(static_cast<char>(r >> 24));
      }
      for (size_t i = 0; i < 2500000; ++i)                    // more than one block (2^20 bytes each)
         cases[4].push_back(static_cast<char>(i % 1000 < 600 ? 0 : (i / 7) % 3));
      bool all = true;
      std::string sizes;
      for (const std::string& c : cases)
      {
         std::string z, back;
         vox::Deflate(reinterpret_cast<const unsigned char*>(c.data()), c.size(), z);
         all = all && Inflate(reinterpret_cast<const unsigned char*>(z.data()), z.size(), back) && back == c;
         sizes += " " + std::to_string(c.size()) + "->" + std::to_string(z.size());
      }
      std::string z;
      vox::Deflate(reinterpret_cast<const unsigned char*>(cases[1].data()), cases[1].size(), z);
      std::printf("      deflate:%s\n", sizes.c_str());
      Check(all && z.size() * 500 < cases[1].size(), "vox::Deflate: the inflater reads back every case (empty, zeros, "
                                                  "sparse, random, several blocks); zeros to under 1/500");
   }

   // ---- vox::Deflate = its verbatim 2026-11-04 copy (deflate_reference.inc), byte for byte; Stack::PlaneInto = Fill +
   //      Quantize ----
   {
      std::vector<std::string> cases;
      cases.emplace_back();
      cases.emplace_back(1, '\0');
      cases.emplace_back(1000003, '\0');
      cases.emplace_back(700001, '\xff');
      uint32_t r = 777;
      const auto next = [&r] { r = r * 1664525u + 1013904223u; return r >> 8; };
      for (int kind = 0; kind < 6; ++kind)
      {
         std::string c;
         const size_t n = kind == 5 ? 3 * (size_t(1) << 20) + 12345 : 200000 + static_cast<size_t>(kind) * 7777;
         while (c.size() < n)
         {
            // runs of every length 1..300 (around the 8-byte steps and 258), of zeros or of a value, between literals
            const size_t run = 1 + next() % (kind == 0 ? 12 : kind == 1 ? 40 : 300);
            const char v = kind == 2 ? static_cast<char>(next() % 3) : (next() % 4 == 0 ? static_cast<char>(next()) : '\0');
            c.append(std::min(run, n - c.size()), v);
            if (kind == 3 && c.size() < n)
               c.push_back(static_cast<char>(next()));
         }
         cases.push_back(c);
      }
      std::string bad;
      for (size_t k = 0; k < cases.size(); ++k)
      {
         const std::string& c = cases[k];
         // every start offset 0..8 (the 8-byte steps fall on other bytes)
         for (size_t off = 0; off < 9 && off <= c.size(); ++off)
         {
            std::string a, b;
            vox::Deflate(reinterpret_cast<const unsigned char*>(c.data()) + off, c.size() - off, a);
            deflate_ref::Deflate(reinterpret_cast<const unsigned char*>(c.data()) + off, c.size() - off, b);
            if (a != b && bad.empty())
               bad = " (case " + std::to_string(k) + " from byte " + std::to_string(off) + " differs)";
         }
      }
      Check(bad.empty(), "vox::Deflate = the verbatim old Deflate byte for byte, " + std::to_string(cases.size()) +
                            " cases x 9 offsets" + bad);

      // a stack of 3 channels, rows in random voxels (several per voxel), at 8, 16 and 32 bits
      vox::Box box;
      box.Add(0, 0, 0);
      box.Add(2, 1.5, 0.8);
      const vox::Grid g = vox::GridOf(box, 0.05, 0.1);
      std::vector<double> x, y, z, w;
      std::vector<uint8_t> ch;
      for (int i = 0; i < 40000; ++i)
      {
         x.push_back(2.0 * (next() % 100000) / 100000);
         y.push_back(1.5 * (next() % 100000) / 100000);
         z.push_back(0.5 * (next() % 100000) / 100000);   // the upper planes empty
         w.push_back(0.1 + (next() % 1000) / 7.0);
         ch.push_back(static_cast<uint8_t>(next() % 3));
      }
      std::string diff;
      for (int bits : { 8, 16, 32 })
      {
         vox::Stack st(g, 3, bits, x.data(), y.data(), z.data(), w.data(), ch.data(), x.size());
         st.Maxima();
         std::vector<unsigned char> qa(st.PlaneBytes()), qb(st.PlaneBytes());
         std::vector<float> acc;
         std::vector<uint32_t> touched;
         for (uint32_t pz = 0; pz < g.nz; ++pz)
         {
            st.Fill(pz);
            for (int c = 0; c < 3; ++c)
            {
               st.Quantize(c, qa.data());
               st.PlaneInto(pz, c, acc, touched, qb.data());
               if (qa != qb && diff.empty())
                  diff = " (" + std::to_string(bits) + " bits, plane " + std::to_string(pz) + ", channel " + std::to_string(c) + ")";
            }
         }
      }
      Check(diff.empty(), "vox::Stack::PlaneInto = Fill + Quantize, every plane and channel, 8/16/32 bits" + diff);
   }

   std::string err;
   char b[700];
   const std::string base = "specimen=1 world-seed=7 seed=7 size=64 psf-kernel-half-width-nm=1500 psf-halo-cut=0 "
                            "blink-min-photons=0 disk-cache=0 ";
   // ---- dSTORM nanorulers with beads ----
   const std::string storm = base + "ruler-mode=dSTORM frames=40 start-sec=5 fid-density=0.5";
   std::vector<uint16_t> plain, with;
   GroundTruth gt;
   const bool ok = Movie(storm, nullptr, plain, err) && Movie(storm, &gt, with, err);
   if (!ok)
      std::printf("      (%s)\n", err.c_str());
   Check(ok && !plain.empty() && plain == with, "the frames with the ground truth's sink = without, bit for bit");
   Check(gt.frames.size() == 40 && gt.frames.back().frame == 39, "a row per frame (40)");
   const int r = 15 + 2;   // the PSF window's half width (1500 nm at 100 nm), and a pixel either side
   long rows = 0, inside = 0, beads = 0, sites = 0;
   double worst = 0, below = 0, over = 0;
   for (const GroundTruth::Emitter& e : gt.emitters)
   {
      if (e.kind == GroundTruth::Fiducial)
      {
         beads++;
         continue;
      }
      rows++;
      sites += std::isfinite(e.siteXUm);
      below = std::max(below, gt.minPhotons - e.onCamera);
      over = std::max(over, e.onCamera / e.detected - 1);
      if (e.xPx >= r && e.yPx >= r && e.xPx < 64 - r && e.yPx < 64 - r)
      {
         inside++;
         worst = std::max(worst, std::fabs(e.onCamera / e.detected - 1));
      }
   }
   std::snprintf(b, sizeof b, "%ld blink rows: each at least %g photons on the camera (worst short by %.3g), none more than "
                 "detected (worst over by %.2g)", rows, gt.minPhotons, below, over);
   Check(rows > 20 && below <= 0 && over < 1e-4, b);
   std::snprintf(b, sizeof b, "%ld blinks with their PSF window inside the image: all their photons on the camera (worst %.2g)",
                 inside, worst);
   Check(inside > 5 && worst < 1e-4, b);
   std::snprintf(b, sizeof b, "every blink row has its binding site (%ld of %ld)", sites, rows);
   Check(sites == rows, b);
   std::snprintf(b, sizeof b, "the beads are rows of kind fiducial (%ld)", beads);
   Check(beads > 0 && gt.Blinking(), b);
   std::snprintf(b, sizeof b, "the initial-ON dyes in the dye list (%zu)", gt.Dyes().size());
   Check(!gt.Dyes().empty(), b);
   const auto has = [](const std::vector<std::pair<std::string, std::string>>& files, const std::string& prefix) {
      for (const auto& f : files)
         if (f.first.compare(0, prefix.size(), prefix) == 0)
            return true;
      return false;
   };
   const auto stormFiles = gt.Files({ 5, 300 });
   Check(has(stormFiles, "localizations_min5") && !has(stormFiles, "dyes.csv"),
         "dSTORM: the localization tables, no dyes.csv");
   GroundTruth again;
   std::vector<uint16_t> f2;
   Check(Movie(storm, &again, f2, err) && Join(again.Files({ 5, 300 })) == Join(gt.Files({ 5, 300 })),
         "the same files from a second run");

   // ---- WideField ----
   const std::string wf = base + "ruler-mode=WideField frames=4 size=32";
   GroundTruth g2;
   std::vector<uint16_t> f3;
   const bool okW = Movie(wf, &g2, f3, err);
   double rateOver = 0, emitted = 0;
   for (const GroundTruth::Dye& d : g2.Dyes())
   {
      rateOver = std::max(rateOver, d.detectedTotal - d.photonsPerSec * 4 * 0.05 * (1 + 1e-12));
      emitted += d.emittedTotal;
   }
   std::snprintf(b, sizeof b, "WideField: %zu dyes, each at most its rate x the movie's 0.2 s (worst over by %.3g photons)",
                 g2.Dyes().size(), rateOver);
   Check(okW && g2.Dyes().size() > 10 && rateOver <= 0, b);
   const auto wfFiles = g2.Files({ 5, 300 }, 10, 10);
   Check(has(wfFiles, "dyes.csv") && !has(wfFiles, "localizations_"), "WideField: dyes.csv, no localization table");
   double detected = 0;
   for (const GroundTruth::Dye& d : g2.Dyes())
      detected += d.detectedTotal;
   {
      Tiff t;
      int vols = 0;
      for (const auto& f : wfFiles)
         if (f.first.compare(0, 8, "density_") == 0)
         {
            t = ReadTiff(f.second);
            vols++;
         }
      // 8 bits, scaled to the largest voxel: each voxel within half a step of its photons
      const double bound = 0.5 * t.scale * static_cast<double>(g2.Dyes().size());
      std::snprintf(b, sizeof b, "WideField: %d density stack, an 8-bit ImageJ stack (deflated, an IFD and a label per "
                    "plane, the origin, %u x %u x %ld voxels of %s um) holding the dyes' detected photons (%.6g vs %.6g, "
                    "within %.3g), its largest voxel at 255", vols, t.w, t.h, t.planes, t.Get("insiliscope.voxel-um").c_str(),
                    t.sum, detected, bound);
      Check(vols == 1 && t.imagej && detected > 0 && std::fabs(t.sum - detected) <= bound && t.top == 255 &&
               t.Get("insiliscope.voxel-um") == "0.01,0.01,0.01", b);
   }
   // a cap the 10 nm voxels pass: the voxels grow (one factor for x, y and z), the README says so
   {
      const auto small = g2.Files({ 5, 300 }, 10, 10, 64 * 1024);
      Tiff t;
      std::string readme;
      for (const auto& f : small)
      {
         if (f.first.compare(0, 8, "density_") == 0)
            t = ReadTiff(f.second);
         if (f.first == "README.txt")
            readme = f.second;
      }
      const double vx = std::atof(t.Get("insiliscope.voxel-um").c_str());
      std::snprintf(b, sizeof b, "WideField under a 64 kB cap: voxels of %s um, %u x %u x %ld = %.0f bytes",
                    t.Get("insiliscope.voxel-um").c_str(), t.w, t.h, t.planes, static_cast<double>(t.w) * t.h * t.planes);
      Check(t.imagej && vx > 0.01 && static_cast<double>(t.w) * t.h * t.planes <= 64 * 1024 &&
               readme.find("would pass") != std::string::npos && std::fabs(t.sum - detected) <= 0.5 * t.scale * static_cast<double>(g2.Dyes().size()), b);
   }

   // ---- two WideField targets: one grid ----
   const std::string two = "world-seed=1128615014 seed=7 x=14.8 y=-6 size=32 frames=1 psf-kernel-half-width-nm=1500 "
                           "disk-cache=0 mt-mode=WideField mt-label-pct=2 npc-dye=Cy3B npc-mode=WideField";
   GroundTruth g3;
   std::vector<uint16_t> f4;
   const bool ok3 = Movie(two, &g3, f4, err);
   std::vector<Tiff> stacks;
   std::vector<std::string> names;
   for (const auto& f : g3.Files({ 5, 300 }, 20, 20))
      if (f.first.compare(0, 8, "density_") == 0)
      {
         stacks.push_back(ReadTiff(f.second));
         names.push_back(f.first);
      }
   bool same = stacks.size() == 2;
   for (const Tiff& t : stacks)
      same = same && t.imagej && t.w == stacks[0].w && t.h == stacks[0].h && t.planes == stacks[0].planes &&
             t.Get("insiliscope.origin-um") == stacks[0].Get("insiliscope.origin-um") &&
             t.Get("insiliscope.voxel-um") == stacks[0].Get("insiliscope.voxel-um");
   std::snprintf(b, sizeof b, "two WideField targets (%s): %zu stacks on one grid (%u x %u x %ld from %s um)",
                 ok3 ? (names.empty() ? "" : (names[0] + (names.size() > 1 ? ", " + names[1] : std::string())).c_str()) : err.c_str(),
                 stacks.size(), stacks.empty() ? 0 : stacks[0].w, stacks.empty() ? 0 : stacks[0].h,
                 stacks.empty() ? 0L : stacks[0].planes, stacks.empty() ? "" : stacks[0].Get("insiliscope.origin-um").c_str());
   Check(ok3 && same, b);

   std::printf("ground_truth: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
