///////////////////////////////////////////////////////////////////////////////
// FILE:          SplatAvx2.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The splat row kernel compiled for AVX2 (this file alone
//                gets /arch:AVX2 or -mavx2: cli/CMakeLists.txt,
//                inSiliScope.vcxproj), used when Available() says the CPU
//                and OS have it. Without the flag (other architectures,
//                Emscripten) it is only a forwarder to the baseline copy.
//
//                Bit-exact with the baseline copy: no fused multiply-add is
//                ever contracted (MSVC /fp:precise without /fp:contract,
//                GCC/Clang -ffp-contract=off from the core's PUBLIC flags),
//                and the lanes are independent accumulators. ctest sr_render
//                compares both copies with the verbatim pre-2026-09-28 splat.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#if defined(__AVX2__)

#define ISC_SPLAT_NS splat_avx2
#include "SplatKernel.inl"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace sim {
namespace splat_avx2 {

namespace {
bool DetectAvx2()
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
   int r[4] = {0, 0, 0, 0};
   __cpuid(r, 0);
   if (r[0] < 7)
      return false;
   __cpuid(r, 1);
   const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
   if (!osxsave || !avx)
      return false;
   const unsigned long long xcr0 = _xgetbv(0);
   if ((xcr0 & 6) != 6)   // XMM and YMM state saved by the OS
      return false;
   __cpuidex(r, 7, 0);
   return (r[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
   __builtin_cpu_init();
   return __builtin_cpu_supports("avx2") != 0;
#else
   return false;
#endif
}
} // namespace

bool Available()
{
   static const bool have = DetectAvx2();
   return have;
}

} // namespace splat_avx2
} // namespace sim

#else // no AVX2 in this translation unit: forward to the baseline copy

#include "SplatKernel.h"

namespace sim {
namespace splat_avx2 {
bool Available() { return false; }
void SplatRows(const SplatArgs& a) { splat_sse2::SplatRows(a); }
} // namespace splat_avx2
} // namespace sim

#endif
