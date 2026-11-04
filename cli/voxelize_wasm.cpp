// SPDX-License-Identifier: BSD-3-Clause
// web/voxelize.html's module (target insiliscope_voxelize): the voxel grid, the stack made plane by plane, its
// description and slice labels and the TIFF's layout are Simulation/Voxelize.* -- the code of the ground truth's
// density stacks -- so the page and the ground truth make their stacks one way. The page keeps reading the CSV,
// choosing the rows and storing the planes; they are compressed here (vox::Deflate). Embedded in the page by
// tools/embed_web_module.mjs; ~180 KB.
#include "Voxelize.h"

#include <emscripten/emscripten.h>

#include <string>
#include <vector>

using namespace sim::vox;

namespace {

struct Handle
{
   Stack stack;
   std::vector<double> stats;   // maxes, sums, scales (a channel each)
   std::string desc;
   std::vector<std::string> labels;
   TiffLayout layout;
   std::vector<double> offsets;
   std::vector<unsigned char> plane;
   std::string deflated;
};

std::vector<std::string> Lines(const char* s)
{
   std::vector<std::string> out;
   std::string cur;
   for (; s && *s; ++s)
      if (*s == '\n')
      {
         out.push_back(cur);
         cur.clear();
      }
      else
         cur += *s;
   out.push_back(cur);
   return out;
}

Grid GridFrom(const double* g)
{
   Grid r;
   r.vx = g[0];
   r.vz = g[1];
   r.fx0 = static_cast<int64_t>(g[2]);
   r.fy0 = static_cast<int64_t>(g[3]);
   r.fz0 = static_cast<int64_t>(g[4]);
   r.nx = static_cast<uint32_t>(g[5]);
   r.ny = static_cast<uint32_t>(g[6]);
   r.nz = static_cast<uint32_t>(g[7]);
   return r;
}

} // namespace

extern "C" {

// The grid over a box (um) at voxels of vx x vx x vz um, grown until Voxels() x bytesPerVoxel fits cap (cap 0: as
// asked). out[0..8] = vx, vz, fx0, fy0, fz0, nx, ny, nz, the growth factor.
EMSCRIPTEN_KEEPALIVE void vox_grid(double x0, double y0, double z0, double x1, double y1, double z1, double vx, double vz,
                                   double bytesPerVoxel, double cap, double* out)
{
   Box b;
   b.Add(x0, y0, z0);
   b.Add(x1, y1, z1);
   double f = 1;
   const Grid g = cap > 0 ? GridWithin(b, vx, vz, bytesPerVoxel, cap, &f) : GridOf(b, vx, vz);
   const double v[9] = { g.vx, g.vz, static_cast<double>(g.fx0), static_cast<double>(g.fy0), static_cast<double>(g.fz0),
                         static_cast<double>(g.nx), static_cast<double>(g.ny), static_cast<double>(g.nz), f };
   for (int i = 0; i < 9; ++i)
      out[i] = v[i];
}

// A stack on grid g (vox_grid's first 8 numbers) of n rows: x, y, z (um), w (null: 1 each), ch (null: channel 0).
// The rows are copied: free them after.
EMSCRIPTEN_KEEPALIVE Handle* vox_stack(const double* g, int channels, int bits, const double* x, const double* y,
                                       const double* z, const double* w, const unsigned char* ch, int n)
{
   return new Handle{ Stack(GridFrom(g), channels, bits, x, y, z, w, ch, static_cast<size_t>(n)), {}, {}, {}, {}, {}, {}, {} };
}
EMSCRIPTEN_KEEPALIVE void vox_free(Handle* h) { delete h; }
EMSCRIPTEN_KEEPALIVE int vox_empty(Handle* h, int z) { return h->stack.Empty(static_cast<uint32_t>(z)) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE double vox_plane_bytes(Handle* h) { return static_cast<double>(h->stack.PlaneBytes()); }
// The first pass; returns maxes, sums and scales, a channel each.
EMSCRIPTEN_KEEPALIVE const double* vox_maxima(Handle* h)
{
   h->stack.Maxima();
   h->stats = h->stack.Maxes();
   h->stats.insert(h->stats.end(), h->stack.Sums().begin(), h->stack.Sums().end());
   h->stats.insert(h->stats.end(), h->stack.Scales().begin(), h->stack.Scales().end());
   return h->stats.data();
}
EMSCRIPTEN_KEEPALIVE void vox_fill(Handle* h, int z) { h->stack.Fill(static_cast<uint32_t>(z)); }
EMSCRIPTEN_KEEPALIVE double vox_plane_max(Handle* h, int ch) { return h->stack.PlaneMax(ch); }
// Channel ch of the filled plane as stored (vox_plane_bytes() bytes; scale 0: the channel's from vox_maxima).
EMSCRIPTEN_KEEPALIVE const unsigned char* vox_plane(Handle* h, int ch, double scale)
{
   h->plane.resize(h->stack.PlaneBytes());
   h->stack.Quantize(ch, h->plane.data(), scale);
   return h->plane.data();
}
// The same plane deflated (vox::Deflate: zlib format, the ground truth's density stacks' encoder); its length is
// vox_deflated_len(). (The page compressed with the browser's CompressionStream until 2026-11-04: zlib's encoder,
// ~10x the time, other bytes, the same pixels.)
EMSCRIPTEN_KEEPALIVE const unsigned char* vox_plane_deflated(Handle* h, int ch, double scale)
{
   h->plane.resize(h->stack.PlaneBytes());
   h->stack.Quantize(ch, h->plane.data(), scale);
   h->deflated.clear();
   Deflate(h->plane.data(), h->plane.size(), h->deflated);
   return reinterpret_cast<const unsigned char*>(h->deflated.data());
}
// n zero bytes deflated (an empty plane)
EMSCRIPTEN_KEEPALIVE const unsigned char* vox_zeros_deflated(Handle* h, double n)
{
   h->plane.assign(static_cast<size_t>(n), 0);
   h->deflated.clear();
   Deflate(h->plane.data(), h->plane.size(), h->deflated);
   return reinterpret_cast<const unsigned char*>(h->deflated.data());
}
EMSCRIPTEN_KEEPALIVE double vox_deflated_len(Handle* h) { return static_cast<double>(h->deflated.size()); }
// The description (after vox_maxima) and the slice labels: channels one per line, extra lines ending in \n.
EMSCRIPTEN_KEEPALIVE const char* vox_describe(Handle* h, const char* channels, const char* value, const char* unit,
                                              const char* extra)
{
   const std::vector<std::string> ch = Lines(channels);
   h->desc = Description(h->stack, ch, value, unit, extra);
   h->labels = Labels(h->stack.grid(), ch);
   return h->desc.c_str();
}
// The TIFF's layout for planes of these stored sizes (channel fastest); 0, or -1 (vox_error says why).
EMSCRIPTEN_KEEPALIVE int vox_layout(Handle* h, const double* sizes, int n, int compression)
{
   std::vector<uint64_t> s(static_cast<size_t>(n));
   for (int i = 0; i < n; ++i)
      s[static_cast<size_t>(i)] = static_cast<uint64_t>(sizes[i]);
   const Grid& g = h->stack.grid();
   h->layout = LayoutTiff(s, nullptr, g.nx, g.ny, h->stack.Bits(), 1.0 / g.vx, h->desc, h->labels, compression);
   h->offsets.assign(h->layout.offsets.begin(), h->layout.offsets.end());
   return h->layout.error.empty() ? 0 : -1;
}
EMSCRIPTEN_KEEPALIVE const char* vox_error(Handle* h) { return h->layout.error.c_str(); }
EMSCRIPTEN_KEEPALIVE const char* vox_head(Handle* h) { return h->layout.head.data(); }
EMSCRIPTEN_KEEPALIVE double vox_head_len(Handle* h) { return static_cast<double>(h->layout.head.size()); }
EMSCRIPTEN_KEEPALIVE const char* vox_ifd(Handle* h) { return h->layout.ifd.data(); }
EMSCRIPTEN_KEEPALIVE double vox_ifd_len(Handle* h) { return static_cast<double>(h->layout.ifd.size()); }
EMSCRIPTEN_KEEPALIVE const double* vox_offsets(Handle* h) { return h->offsets.data(); }
EMSCRIPTEN_KEEPALIVE double vox_first(Handle* h) { return static_cast<double>(h->layout.first); }

} // extern "C"
