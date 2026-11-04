// SPDX-License-Identifier: BSD-3-Clause
#include "Voxelize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sim {
namespace vox {

namespace {

std::string Fmt(const char* f, double v)
{
   char b[64];
   std::snprintf(b, sizeof b, f, v);
   return b;
}
std::string G(double v) { return Fmt("%.9g", v); }

void Put16(std::string& s, uint32_t v)
{
   s.push_back(static_cast<char>(v & 255));
   s.push_back(static_cast<char>((v >> 8) & 255));
}
void Put32(std::string& s, uint32_t v)
{
   Put16(s, v & 0xFFFF);
   Put16(s, v >> 16);
}
void Set32(std::string& s, size_t at, uint32_t v)
{
   for (int i = 0; i < 4; ++i)
      s[at + i] = static_cast<char>((v >> (8 * i)) & 255);
}

// UTF-8 to UTF-16 little-endian (ImageJ's labels).
std::string Utf16(const std::string& u)
{
   std::string o;
   for (size_t i = 0; i < u.size();)
   {
      const unsigned char c = static_cast<unsigned char>(u[i]);
      uint32_t cp = c;
      int more = 0;
      if (c >= 0xF0) { cp = c & 7; more = 3; }
      else if (c >= 0xE0) { cp = c & 15; more = 2; }
      else if (c >= 0xC0) { cp = c & 31; more = 1; }
      ++i;
      for (; more > 0 && i < u.size(); --more, ++i)
         cp = (cp << 6) | (static_cast<unsigned char>(u[i]) & 63);
      if (cp >= 0x10000)
      {
         cp -= 0x10000;
         Put16(o, 0xD800 + (cp >> 10));
         Put16(o, 0xDC00 + (cp & 1023));
      }
      else
         Put16(o, cp);
   }
   return o;
}

} // namespace

void Box::Add(double x, double y, double z)
{
   x0 = std::min(x0, x); x1 = std::max(x1, x);
   y0 = std::min(y0, y); y1 = std::max(y1, y);
   z0 = std::min(z0, z); z1 = std::max(z1, z);
}

Grid GridOf(const Box& b, double vxUm, double vzUm)
{
   Grid g;
   g.vx = vxUm;
   g.vz = vzUm;
   if (b.Empty() || !(vxUm > 0) || !(vzUm > 0))
      return g;
   g.fx0 = static_cast<int64_t>(std::floor(b.x0 / vxUm));
   g.fy0 = static_cast<int64_t>(std::floor(b.y0 / vxUm));
   g.fz0 = static_cast<int64_t>(std::floor(b.z0 / vzUm));
   g.nx = static_cast<uint32_t>(static_cast<int64_t>(std::floor(b.x1 / vxUm)) - g.fx0 + 1);
   g.ny = static_cast<uint32_t>(static_cast<int64_t>(std::floor(b.y1 / vxUm)) - g.fy0 + 1);
   g.nz = static_cast<uint32_t>(static_cast<int64_t>(std::floor(b.z1 / vzUm)) - g.fz0 + 1);
   return g;
}

Grid GridWithin(const Box& b, double vxUm, double vzUm, double bytesPerVoxel, double capBytes, double* factor)
{
   Grid g = GridOf(b, vxUm, vzUm);
   double f = 1;
   if (g.Voxels() * bytesPerVoxel > capBytes && capBytes > 0)
   {
      // from just below the cube-root estimate up, in steps of 0.5 nm of the lateral voxel
      const double nm = vxUm * 1000, f0 = std::cbrt(g.Voxels() * bytesPerVoxel / capBytes);
      for (double k = std::max(0.0, std::floor((f0 - 1) * nm / 0.5) - 2);; k += 1)
      {
         f = (nm + 0.5 * k) / nm;
         g = GridOf(b, vxUm * f, vzUm * f);
         if (g.Voxels() * bytesPerVoxel <= capBytes || f > 1e6)
            break;
      }
   }
   if (factor)
      *factor = f;
   return g;
}

Stack::Stack(const Grid& g, int channels, int bits, const double* x, const double* y, const double* z, const double* w,
             const uint8_t* ch, size_t n)
   : g_(g), nc_(std::max(1, channels)), bits_(bits == 8 || bits == 16 ? bits : 32), start_(g.nz + 1, 0),
     max_(nc_, 0.0), sum_(nc_, 0.0), scale_(nc_, 1.0)
{
   const auto clampIdx = [](double v, double p, int64_t f0, uint32_t n) {
      const int64_t i = static_cast<int64_t>(std::floor(v / p)) - f0;
      return static_cast<uint32_t>(std::min<int64_t>(static_cast<int64_t>(n) - 1, std::max<int64_t>(0, i)));
   };
   std::vector<uint32_t> at(n), iz(n);
   for (size_t k = 0; k < n; ++k)
   {
      const uint32_t ix = clampIdx(x[k], g.vx, g.fx0, g.nx), iy = clampIdx(y[k], g.vx, g.fy0, g.ny);
      iz[k] = clampIdx(z[k], g.vz, g.fz0, g.nz);
      at[k] = iy * g.nx + ix;
      ++start_[iz[k] + 1];
   }
   for (uint32_t k = 0; k < g.nz; ++k)
      start_[k + 1] += start_[k];
   std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
   at_.resize(n);
   w_.resize(n);
   ch_.resize(n);
   for (size_t k = 0; k < n; ++k)
   {
      const uint32_t j = fill[iz[k]]++;
      at_[j] = at[k];
      w_[j] = w ? w[k] : 1.0;
      ch_[j] = ch ? std::min<uint8_t>(ch[k], static_cast<uint8_t>(nc_ - 1)) : 0;
   }
}

void Stack::Fill(uint32_t z)
{
   const size_t plane = static_cast<size_t>(g_.nx) * g_.ny;
   if (acc_.empty())
      acc_.assign(static_cast<size_t>(nc_), std::vector<float>(plane, 0.0f));
   for (uint32_t t : touched_)
      for (std::vector<float>& a : acc_)
         a[t] = 0.0f;
   touched_.clear();
   for (uint32_t j = start_[z]; j < start_[z + 1]; ++j)
   {
      float& a = acc_[ch_[j]][at_[j]];
      a = static_cast<float>(static_cast<double>(a) + w_[j]);   // float32 sums (the page's Float32Array)
      touched_.push_back(at_[j]);
   }
}

double Stack::PlaneMax(int ch) const
{
   double m = 0;
   if (!acc_.empty())
      for (uint32_t t : touched_)
         m = std::max(m, static_cast<double>(acc_[static_cast<size_t>(ch)][t]));
   return m;
}

void Stack::Maxima()
{
   std::fill(max_.begin(), max_.end(), 0.0);
   std::fill(sum_.begin(), sum_.end(), 0.0);
   for (size_t j = 0; j < w_.size(); ++j)
      sum_[ch_[j]] += w_[j];
   for (uint32_t z = 0; z < g_.nz; ++z)
   {
      if (Empty(z))
         continue;
      Fill(z);
      for (int c = 0; c < nc_; ++c)
         max_[static_cast<size_t>(c)] = std::max(max_[static_cast<size_t>(c)], PlaneMax(c));
   }
   const double top = bits_ == 8 ? 255.0 : 65535.0;
   for (int c = 0; c < nc_; ++c)
      scale_[static_cast<size_t>(c)] = bits_ == 32 ? 1.0 : max_[static_cast<size_t>(c)] > 0 ? max_[static_cast<size_t>(c)] / top : 1.0;
}

void Stack::Quantize(int ch, unsigned char* out, double scale) const
{
   std::memset(out, 0, PlaneBytes());
   if (acc_.empty())
      return;
   const std::vector<float>& a = acc_[static_cast<size_t>(ch)];
   const double s = scale > 0 ? scale : scale_[static_cast<size_t>(ch)], top = bits_ == 8 ? 255.0 : 65535.0;
   for (uint32_t t : touched_)
   {
      const float v = a[t];
      if (v == 0.0f)
         continue;
      if (bits_ == 32)
      {
         std::memcpy(out + 4 * static_cast<size_t>(t), &v, 4);   // little-endian hosts (x86, WASM)
         continue;
      }
      const double q = std::min(top, std::max(0.0, std::floor(v / s + 0.5)));   // Math.round
      const uint32_t u = static_cast<uint32_t>(q);
      if (bits_ == 8)
         out[t] = static_cast<unsigned char>(u);
      else
      {
         out[2 * static_cast<size_t>(t)] = static_cast<unsigned char>(u & 255);
         out[2 * static_cast<size_t>(t) + 1] = static_cast<unsigned char>(u >> 8);
      }
   }
}

std::vector<std::string> Labels(const Grid& g, const std::vector<std::string>& channels)
{
   std::vector<std::string> out;
   const size_t nc = std::max<size_t>(1, channels.size());
   out.reserve(static_cast<size_t>(g.nz) * nc);
   for (uint32_t z = 0; z < g.nz; ++z)
      for (size_t c = 0; c < nc; ++c)
         out.push_back((nc > 1 ? channels[c] + ", " : std::string()) + "z = " + Fmt("%.4f", g.Oz() + (z + 0.5) * g.vz) +
                       " \xC2\xB5m");
   return out;
}

std::string Description(const Stack& s, const std::vector<std::string>& channels, const std::string& value,
                        const std::string& unit, const std::string& extra)
{
   const Grid& g = s.grid();
   const int nc = s.Channels(), bits = s.Bits();
   std::string d = "ImageJ=1.11a\nimages=" + std::to_string(static_cast<unsigned long long>(g.nz) * nc) + "\n";
   if (nc > 1)
      d += "channels=" + std::to_string(nc) + "\n";
   d += "slices=" + std::to_string(g.nz) + "\n";
   if (nc > 1)
      d += "hyperstack=true\nmode=composite\n";
   // ImageJ's origin is in voxels: x and y at the voxel corner (ImageJ's pixel coordinates), z at a slice's centre
   d += "unit=micron\nspacing=" + G(g.vz) + "\nloop=false\nxorigin=" + Fmt("%.6f", -g.Ox() / g.vx) +
        "\nyorigin=" + Fmt("%.6f", -g.Oy() / g.vx) + "\nzorigin=" + Fmt("%.6f", -g.Oz() / g.vz - 0.5) + "\n";
   if (nc == 1 && bits != 32)
      d += "cf=0\nc0=0\nc1=" + G(s.Scales()[0]) + "\nvunit=" + unit + "\n";
   double mx = 0;
   for (double m : s.Maxes())
      mx = std::max(mx, m);
   d += "min=0\nmax=" + G(bits == 32 ? mx : bits == 8 ? 255.0 : 65535.0) + "\n";
   d += "insiliscope.value=" + value + "\n" + extra + "insiliscope.channels=";
   for (size_t c = 0; c < channels.size(); ++c)
      d += (c ? ";" : "") + channels[c];
   d += "\n";
   if (bits != 32)
   {
      d += "insiliscope.scale=";
      for (size_t c = 0; c < s.Scales().size(); ++c)
         d += (c ? ";" : "") + G(s.Scales()[c]);
      d += " " + unit + " per step\n";
   }
   d += "insiliscope.origin-um=" + Fmt("%.6f", g.Ox()) + "," + Fmt("%.6f", g.Oy()) + "," + Fmt("%.6f", g.Oz()) +
        "\ninsiliscope.voxel-um=" + G(g.vx) + "," + G(g.vx) + "," + G(g.vz) + "\n";
   return d;
}

TiffLayout LayoutTiff(const std::vector<uint64_t>& sizes, const std::vector<int64_t>* sameAs, uint32_t nx, uint32_t ny,
                      int bits, double pixelsPerUm, const std::string& description,
                      const std::vector<std::string>& labels, int compression)
{
   TiffLayout L;
   std::string& s = L.head;
   s = "II";
   Put16(s, 42);
   Put32(s, 0);   // the first IFD's offset, below
   const auto even = [&] {
      if (s.size() & 1)
         s.push_back('\0');
   };
   const uint32_t descAt = static_cast<uint32_t>(s.size());
   s += description;
   s.push_back('\0');
   const uint32_t descLen = static_cast<uint32_t>(description.size() + 1);
   even();
   const uint32_t ratAt = static_cast<uint32_t>(s.size());
   Put32(s, static_cast<uint32_t>(std::lround(pixelsPerUm * 1000)));
   Put32(s, 1000);
   uint32_t metaAt = 0, metaLen = 0, cntAt = 0;
   if (!labels.empty())
   {
      // ImageJ's IJMetadata in the file's byte order: magic IJIJ, type labl with its count, then each label UTF-16
      std::vector<std::string> u;
      for (const std::string& l : labels)
         u.push_back(Utf16(l));
      metaAt = static_cast<uint32_t>(s.size());
      s += "JIJIlbal";
      Put32(s, static_cast<uint32_t>(labels.size()));
      for (const std::string& b : u)
         s += b;
      metaLen = static_cast<uint32_t>(s.size() - metaAt);
      even();
      cntAt = static_cast<uint32_t>(s.size());
      Put32(s, 12);
      for (const std::string& b : u)
         Put32(s, static_cast<uint32_t>(b.size()));
   }
   const size_t n = sizes.size();
   uint64_t at = s.size();
   L.offsets.resize(n);
   for (size_t k = 0; k < n; ++k)
   {
      const int64_t same = sameAs && k < sameAs->size() ? (*sameAs)[k] : -1;
      if (same >= 0 && static_cast<size_t>(same) < k)
         L.offsets[k] = L.offsets[static_cast<size_t>(same)];
      else
      {
         L.offsets[k] = at;
         at += sizes[k];
      }
   }
   L.first = at + (at & 1);   // the IFDs on a word boundary (after the last plane: the planes stay back to back)
   const auto tags = [&](size_t k) { return static_cast<uint16_t>(14 + (k == 0 ? 1 + (labels.empty() ? 0 : 2) : 0)); };
   uint64_t ifdBytes = 0;
   for (size_t k = 0; k < n; ++k)
      ifdBytes += 2 + 12 * static_cast<uint64_t>(tags(k)) + 4;
   if (L.first + ifdBytes > 0xFFFFFFFFull)
   {
      L.error = "the stack passes a TIFF's 4 GB: take bigger voxels";
      return L;
   }
   Set32(s, 4, static_cast<uint32_t>(L.first));
   std::string& f = L.ifd;
   f.reserve(static_cast<size_t>(ifdBytes));
   for (size_t k = 0; k < n; ++k)
   {
      Put16(f, tags(k));
      const auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
         Put16(f, tag);
         Put16(f, type);
         Put32(f, count);
         Put32(f, value);   // a SHORT's value in the first two bytes (little-endian)
      };
      entry(254, 4, 1, 0);
      entry(256, 4, 1, nx);
      entry(257, 4, 1, ny);
      entry(258, 3, 1, static_cast<uint32_t>(bits));
      entry(259, 3, 1, static_cast<uint32_t>(compression));
      entry(262, 3, 1, 1);
      if (k == 0)
         entry(270, 2, descLen, descAt);
      entry(273, 4, 1, static_cast<uint32_t>(L.offsets[k]));
      entry(277, 3, 1, 1);
      entry(278, 4, 1, ny);
      entry(279, 4, 1, static_cast<uint32_t>(sizes[k]));
      entry(282, 5, 1, ratAt);
      entry(283, 5, 1, ratAt);
      entry(296, 3, 1, 1);
      entry(339, 3, 1, bits == 32 ? 3 : 1);
      if (k == 0 && !labels.empty())
      {
         entry(50838, 4, static_cast<uint32_t>(1 + labels.size()), cntAt);
         entry(50839, 1, metaLen, metaAt);
      }
      Put32(f, k + 1 < n ? static_cast<uint32_t>(L.first + f.size() + 4) : 0);
   }
   return L;
}

void PackBits(const unsigned char* b, size_t n, std::string& out)
{
   size_t i = 0;
   while (i < n)
   {
      size_t j = i + 1;
      while (j < n && j - i < 128 && b[j] == b[i])
         ++j;
      if (j - i >= 2)
      {
         out.push_back(static_cast<char>(257 - (j - i)));
         out.push_back(static_cast<char>(b[i]));
         i = j;
         continue;
      }
      j = i + 1;
      while (j < n && j - i < 128 && !(j + 1 < n && b[j] == b[j + 1]))
         ++j;
      out.push_back(static_cast<char>(j - i - 1));
      out.append(reinterpret_cast<const char*>(b + i), j - i);
      i = j;
   }
}

namespace {

// Bits out, least significant first (deflate's order); Huffman codes reversed into it.
struct BitOut
{
   std::string& s;
   uint64_t acc = 0;
   int n = 0;
   void Put(uint32_t v, int bits)
   {
      acc |= static_cast<uint64_t>(v) << n;
      n += bits;
      while (n >= 8)
      {
         s.push_back(static_cast<char>(acc & 255));
         acc >>= 8;
         n -= 8;
      }
   }
   void Code(uint32_t code, int len)
   {
      uint32_t r = 0;
      for (int i = 0; i < len; ++i)
         r |= ((code >> i) & 1u) << (len - 1 - i);
      Put(r, len);
   }
   void Flush()
   {
      if (n > 0)
         s.push_back(static_cast<char>(acc & 255));
      acc = 0;
      n = 0;
   }
};

// Huffman code lengths for freq (0: unused), at most maxLen bits (the frequencies flattened until they fit), always
// at least two codes (a complete code: every inflater takes it).
std::vector<int> HuffLengths(std::vector<uint64_t> freq, int maxLen)
{
   const size_t n = freq.size();
   int used = 0;
   for (uint64_t f : freq)
      used += f > 0;
   for (size_t i = 0; used < 2 && i < n; ++i)
      if (!freq[i])
      {
         freq[i] = 1;
         ++used;
      }
   std::vector<int> len(n, 0);
   for (;;)
   {
      struct Node
      {
         int l, r;   // children (node indices), or l = -1 - symbol for a leaf
      };
      std::vector<Node> nodes;
      using P = std::pair<uint64_t, int>;
      std::vector<P> heap;
      for (size_t i = 0; i < n; ++i)
         if (freq[i])
         {
            nodes.push_back({ -1 - static_cast<int>(i), 0 });
            heap.push_back({ freq[i], static_cast<int>(nodes.size()) - 1 });
         }
      const auto cmp = [](const P& a, const P& b) { return a > b; };
      std::make_heap(heap.begin(), heap.end(), cmp);
      while (heap.size() > 1)
      {
         std::pop_heap(heap.begin(), heap.end(), cmp);
         const P a = heap.back();
         heap.pop_back();
         std::pop_heap(heap.begin(), heap.end(), cmp);
         const P b = heap.back();
         heap.pop_back();
         nodes.push_back({ a.second, b.second });
         heap.push_back({ a.first + b.first, static_cast<int>(nodes.size()) - 1 });
         std::push_heap(heap.begin(), heap.end(), cmp);
      }
      std::fill(len.begin(), len.end(), 0);
      int deepest = 0;
      std::vector<std::pair<int, int>> st{ { heap[0].second, 0 } };
      while (!st.empty())
      {
         const std::pair<int, int> e = st.back();
         st.pop_back();
         const Node nd = nodes[static_cast<size_t>(e.first)];
         if (nd.l < 0)
         {
            len[static_cast<size_t>(-1 - nd.l)] = e.second;
            deepest = std::max(deepest, e.second);
         }
         else
         {
            st.push_back({ nd.l, e.second + 1 });
            st.push_back({ nd.r, e.second + 1 });
         }
      }
      if (deepest <= maxLen)
         return len;
      for (uint64_t& f : freq)
         if (f)
            f = (f + 1) / 2;
   }
}

// Canonical codes from lengths (RFC 1951 3.2.2).
std::vector<uint32_t> CanonicalCodes(const std::vector<int>& len)
{
   int count[16] = {};
   for (int l : len)
      count[l]++;
   count[0] = 0;
   uint32_t next[16] = {}, code = 0;
   for (int b = 1; b < 16; ++b)
   {
      code = (code + static_cast<uint32_t>(count[b - 1])) << 1;
      next[b] = code;
   }
   std::vector<uint32_t> c(len.size(), 0);
   for (size_t i = 0; i < len.size(); ++i)
      if (len[i])
         c[i] = next[len[i]]++;
   return c;
}

const int kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                           131, 163, 195, 227, 258 };
const int kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };

} // namespace

void Deflate(const unsigned char* b, size_t n, std::string& out)
{
   out.push_back(static_cast<char>(0x78));   // zlib: deflate, 32 kB window; (0x7801 % 31 == 0)
   out.push_back(static_cast<char>(0x01));
   BitOut bo{ out };
   const size_t kBlock = size_t(1) << 20;
   std::vector<uint32_t> tok;   // a literal (0..255), or 257.. a length code | extra bits << 9 (distance 1)
   size_t i = 0;
   do
   {
      const size_t end = std::min(n, i + kBlock);
      tok.clear();
      std::vector<uint64_t> lit(286, 0), dist(30, 0);
      while (i < end)
      {
         size_t r = 0;
         if (i > 0)
            while (i + r < end && r < 258 && b[i + r] == b[i - 1])
               ++r;
         if (r >= 3)
         {
            int c = 28;
            while (kLenBase[c] > static_cast<int>(r))
               --c;
            tok.push_back(static_cast<uint32_t>(257 + c) | (static_cast<uint32_t>(r - static_cast<size_t>(kLenBase[c])) << 9));
            lit[static_cast<size_t>(257 + c)]++;
            dist[0]++;
            i += r;
         }
         else
         {
            tok.push_back(b[i]);
            lit[b[i]]++;
            ++i;
         }
      }
      lit[256] = 1;
      const std::vector<int> ll = HuffLengths(lit, 15), dl = HuffLengths(dist, 15);
      const std::vector<uint32_t> lc = CanonicalCodes(ll), dc = CanonicalCodes(dl);
      size_t hlit = 286, hdist = 30;
      while (hlit > 257 && !ll[hlit - 1])
         --hlit;
      while (hdist > 1 && !dl[hdist - 1])
         --hdist;
      // the code lengths, run-length coded (16: the previous 3-6 times, 17: 3-10 zeros, 18: 11-138 zeros)
      std::vector<int> seq(ll.begin(), ll.begin() + static_cast<std::ptrdiff_t>(hlit));
      seq.insert(seq.end(), dl.begin(), dl.begin() + static_cast<std::ptrdiff_t>(hdist));
      std::vector<std::pair<int, int>> rle;
      for (size_t k = 0; k < seq.size();)
      {
         const int v = seq[k];
         size_t run = 1;
         while (k + run < seq.size() && seq[k + run] == v)
            ++run;
         if (v == 0 && run >= 11)
         {
            const size_t r = std::min<size_t>(run, 138);
            rle.push_back({ 18, static_cast<int>(r - 11) });
            k += r;
         }
         else if (v == 0 && run >= 3)
         {
            rle.push_back({ 17, static_cast<int>(run - 3) });
            k += run;
         }
         else if (v != 0 && run >= 4)
         {
            rle.push_back({ v, 0 });
            size_t rest = run - 1;
            k += 1;
            while (rest >= 3)
            {
               const size_t r = std::min<size_t>(rest, 6);
               rle.push_back({ 16, static_cast<int>(r - 3) });
               k += r;
               rest -= r;
            }
         }
         else
         {
            rle.push_back({ v, 0 });
            ++k;
         }
      }
      std::vector<uint64_t> cf(19, 0);
      for (const auto& e : rle)
         cf[static_cast<size_t>(e.first)]++;
      const std::vector<int> cl = HuffLengths(cf, 7);
      const std::vector<uint32_t> cc = CanonicalCodes(cl);
      static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
      int hclen = 19;
      while (hclen > 4 && !cl[static_cast<size_t>(order[hclen - 1])])
         --hclen;
      bo.Put(i >= n ? 1 : 0, 1);   // the last block
      bo.Put(2, 2);                // dynamic Huffman
      bo.Put(static_cast<uint32_t>(hlit - 257), 5);
      bo.Put(static_cast<uint32_t>(hdist - 1), 5);
      bo.Put(static_cast<uint32_t>(hclen - 4), 4);
      for (int k = 0; k < hclen; ++k)
         bo.Put(static_cast<uint32_t>(cl[static_cast<size_t>(order[k])]), 3);
      for (const auto& e : rle)
      {
         const size_t s = static_cast<size_t>(e.first);
         bo.Code(cc[s], cl[s]);
         if (s == 16)
            bo.Put(static_cast<uint32_t>(e.second), 2);
         else if (s == 17)
            bo.Put(static_cast<uint32_t>(e.second), 3);
         else if (s == 18)
            bo.Put(static_cast<uint32_t>(e.second), 7);
      }
      for (uint32_t t : tok)
      {
         const size_t s = t & 511;
         bo.Code(lc[s], ll[s]);
         if (s > 256)
         {
            bo.Put(t >> 9, kLenExtra[s - 257]);
            bo.Code(dc[0], dl[0]);   // distance 1
         }
      }
      bo.Code(lc[256], ll[256]);
   } while (i < n);
   bo.Flush();
   uint32_t a = 1, s2 = 0;
   for (size_t k = 0; k < n;)
   {
      const size_t m = std::min<size_t>(n - k, 5552);
      for (size_t j = 0; j < m; ++j)
      {
         a += b[k + j];
         s2 += a;
      }
      a %= 65521;
      s2 %= 65521;
      k += m;
   }
   const uint32_t adler = (s2 << 16) | a;
   for (int k = 3; k >= 0; --k)
      out.push_back(static_cast<char>((adler >> (8 * k)) & 255));
}

} // namespace vox
} // namespace sim
