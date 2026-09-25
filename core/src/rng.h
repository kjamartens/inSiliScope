// Address-based RNG: pcg4d + hashUnit, bit-exact with the JS prototype
// (web/ cell_field_sim `pcg4d`/`hashUnit`/`hashStream`) and with demoCam's
// sim::Pcg4d (SMLMCounterRng.h). Pure uint32 arithmetic, so every platform
// (MSVC, clang, Emscripten/WASM, HLSL) produces identical bits.
//
// Integer semantics: JS does `seed|0`, `cx|0`, `k|0` (int32 wrap) and then
// Math.imul (uint32 wrap). Callers pass int32/uint32; hashStream's
// `base*4096+ctr` exceeds 2^32 for large bases and must WRAP, which the
// uint32 multiply below does.
#pragma once

#include <cstdint>

namespace isc {

struct Pcg4dOut { uint32_t a, b, c, d; };

inline Pcg4dOut Pcg4d(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
   a = a * 1664525u + 1013904223u;
   b = b * 1664525u + 1013904223u;
   c = c * 1664525u + 1013904223u;
   d = d * 1664525u + 1013904223u;
   a += b * d;
   b += c * a;
   c += a * b;
   d += b * c;
   a ^= a >> 16; b ^= b >> 16; c ^= c >> 16; d ^= d >> 16;
   a += b * d;
   b += c * a;
   c += a * b;
   d += b * c;
   return { a, b, c, d };
}

// One (0,1) uniform for address (seed, cx, cy, k): ((a>>9)+0.5) * 2^-23.
inline double HashUnit(uint32_t seed, int32_t cx, int32_t cy, uint32_t k)
{
   const uint32_t a = Pcg4d(seed, (uint32_t)cx, (uint32_t)cy, k).a;
   return ((double)(a >> 9) + 0.5) * 1.1920928955078125e-7;
}

// Sequential stream still addressed by (seed, cx, cy, base): draw i is
// HashUnit(seed, cx, cy, base*4096 + i) with uint32 wrap.
class HashStream {
public:
   HashStream(uint32_t seed, int32_t cx, int32_t cy, uint32_t base)
      : seed_(seed), cx_(cx), cy_(cy), base_(base * 4096u) {}
   double Next() { return HashUnit(seed_, cx_, cy_, base_ + (ctr_++)); }
private:
   uint32_t seed_; int32_t cx_, cy_; uint32_t base_; uint32_t ctr_ = 0;
};

} // namespace isc
