///////////////////////////////////////////////////////////////////////////////
// FILE:          SplatSse2.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The splat row kernel with the build's baseline instruction
//                set (x64 SSE2, WASM SIMD where the target has it). See
//                SplatKernel.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#define ISC_SPLAT_NS splat_sse2
#include "SplatKernel.inl"
