///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpMaps.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The BFP light maps' images (2026-11-19, spec/ALGORITHM.md "BFP
//                light maps"): img("name.tif") in the equations (BfpExpr.h)
//                reads a TIFF -- uncompressed, 8 / 16 / 32-bit integers or
//                32 / 64-bit floats, one sample per pixel, any number of pages
//                (an ImageJ stack) -- over the pupil square [-umax, umax]^2:
//                column 0 at ux = -umax, row 0 at uy = +umax (an image as seen
//                from the sample's side, y up), sampled bilinearly between the
//                pixel centres, 0 outside the square; page p of P is the time
//                slots t in [p / P, (p + 1) / P). The values as stored (no
//                scaling). A file is looked up in the store first (the viewer
//                hands its files' bytes to the module, BfpRegisterImage) and
//                then on the file system (the cli's and Micro-Manager's paths,
//                relative to the working directory).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

struct BfpImage
{
   int w = 0, h = 0, pages = 0;
   std::vector<float> v;   // pages x h x w
   uint64_t hash = 0;      // of the file's bytes
   // The value at (ux, uy) in [-umax, umax]^2 at time t (0..1): bilinear, 0 outside.
   double Sample(double ux, double uy, double t, double umax) const;
};

// A TIFF's pages. False (err) on a compressed, tiled, multi-sample or otherwise unsupported file.
bool BfpReadTiff(const uint8_t* data, size_t n, BfpImage& out, std::string& err);

// The store: the bytes under a name (the viewer's uploads; an empty buffer removes it). Thread-safe.
void BfpRegisterImage(const std::string& name, const uint8_t* data, size_t n);
// The image of a name: the store's, else the file's (cached by path and modification); nullptr and err when neither.
std::shared_ptr<const BfpImage> BfpFindImage(const std::string& name, std::string& err);

uint64_t BfpHashBytes(const void* data, size_t n, uint64_t h = 1469598103934665603ULL);

} // namespace sim
