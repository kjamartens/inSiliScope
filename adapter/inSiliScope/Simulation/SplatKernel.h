///////////////////////////////////////////////////////////////////////////////
// FILE:          SplatKernel.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inner loop of the diffraction-PSF splat (SplatRows in
//                PsfGeneratorBridge), compiled twice: SplatSse2.cpp with the
//                build's baseline instruction set and SplatAvx2.cpp with
//                AVX2 (chosen at run time by SplatAvx2Available()). Both
//                run the per-pixel operations of the original splat in the
//                original order on the same operands, so every pixel is bit
//                for bit what SplatPsfKernel gave before (ctest sr_render
//                checks the three against the verbatim old code).
//
//                Speed comes from the column-polyphase copy of the block
//                sums (PsfKernelCache::polySums): the taps of consecutive
//                camera pixels sit `os` floats apart in blockSums but next
//                to each other in polySums, so a whole pixel row is a few
//                contiguous multiply-add loops over independent lanes (no
//                reassociation; the compiler vectorises them as plain
//                loops). Pixels whose taps fall partly outside the block-sum
//                array (the outermost one or two of a row) and plans without
//                polySums (Fft placement) take the original scalar path.
//
//                This header keeps to plain types on purpose: the AVX2
//                translation unit must not instantiate inline functions
//                shared with the rest of the program (the linker could pick
//                its AVX2 copy for everyone).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstddef>
#include <cstdint>

namespace sim {

// Everything one SplatRows call needs, in plain types.
struct SplatArgs
{
   float* img = nullptr;          // the frame, row-major
   unsigned width = 0;
   int yLo = 0, yHi = 0;          // rows [yLo, yHi) to add
   int os = 1;                    // kernel oversampling
   int camRad = 0;                // half-width in camera pixels
   int bw = 0;                    // block-sum array width (and height)
   int qw = 0;                    // bw / os (polySums row length per phase)
   const float* B = nullptr;      // block sums of the plane (bw x bw)
   const float* P = nullptr;      // polySums of the plane, or nullptr
   int x0 = 0, y0 = 0;            // centre camera pixel
   int bx = 0, by = 0;            // block-sum index of the first tap of (x0, y0)
   int nTaps = 1;                 // 1, 2 or 4
   const double* wx = nullptr;    // tap weights (nTaps each)
   const double* wy = nullptr;
   double photons = 0.0;
   // The halo cut's columns per row of this plane (PsfHaloSpans), indexed
   // dy + camRad; nullptr: every column of the square.
   const int16_t* spanLo = nullptr;
   const int16_t* spanHi = nullptr;
};

namespace splat_sse2 {
void SplatRows(const SplatArgs& a);
}
namespace splat_avx2 {
// True when this build has an AVX2 copy and the CPU and OS support it.
bool Available();
void SplatRows(const SplatArgs& a);   // falls back to splat_sse2 when !Available()
}

} // namespace sim
