// SPDX-License-Identifier: BSD-3-Clause
// A verbatim copy of vox::Deflate as of 2026-11-04 (before the 8-byte run scan, the pre-reversed codes and the
// zero-word Adler-32), kept so ground_truth_check can compare the current one with it byte for byte.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace deflate_ref {

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
inline std::vector<int> HuffLengths(std::vector<uint64_t> freq, int maxLen)
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
inline std::vector<uint32_t> CanonicalCodes(const std::vector<int>& len)
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

static const int kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                           131, 163, 195, 227, 258 };
static const int kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };

inline void Deflate(const unsigned char* b, size_t n, std::string& out)
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

} // namespace deflate_ref
