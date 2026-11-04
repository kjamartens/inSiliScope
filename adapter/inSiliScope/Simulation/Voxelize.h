// SPDX-License-Identifier: BSD-3-Clause
// Voxelize: points (x, y, z in um, a weight, a channel) summed per voxel into an ImageJ TIFF stack, one z plane at a
// time. The one implementation of the ground truth's density stacks (GroundTruth::Volumes: the cli and the viewer)
// and of the converter page web/voxelize.html (through a WASM of its own: cli/voxelize_wasm.cpp, embedded in the page
// by tools/embed_web_module.mjs). No dependencies.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace sim {
namespace vox {

// A box of points (um).
struct Box
{
   double x0 = std::numeric_limits<double>::infinity(), y0 = x0, z0 = x0;
   double x1 = -std::numeric_limits<double>::infinity(), y1 = x1, z1 = x1;
   void Add(double x, double y, double z);
   bool Empty() const { return !(x0 <= x1); }
};

// The grid: voxels of vx x vx x vz um anchored to the world (voxel i along x spans [i vx, (i + 1) vx)), from the
// voxel of the box's low corner to that of its high corner.
struct Grid
{
   double vx = 0, vz = 0;
   int64_t fx0 = 0, fy0 = 0, fz0 = 0;   // the first voxel's index along x, y, z
   uint32_t nx = 0, ny = 0, nz = 0;
   double Ox() const { return static_cast<double>(fx0) * vx; }   // voxel (0, 0, 0)'s corner, world um
   double Oy() const { return static_cast<double>(fy0) * vx; }
   double Oz() const { return static_cast<double>(fz0) * vz; }
   double Voxels() const { return static_cast<double>(nx) * ny * nz; }
};
Grid GridOf(const Box& b, double vxUm, double vzUm);
// The finest grid of at least vxUm x vxUm x vzUm whose stacks fit capBytes uncompressed (Voxels() x bytesPerVoxel,
// bytesPerVoxel counting every stack and channel): both voxel sides grown by one factor, the lateral one in steps of
// 0.5 nm. *factor = that factor (1: as asked).
Grid GridWithin(const Box& b, double vxUm, double vzUm, double bytesPerVoxel, double capBytes, double* factor);

// Points binned on a grid, plane by plane: the rows sorted by plane once (a counting sort), a plane's sums made when
// asked (float32, the rows in their order), then quantized: 8 / 16 bits = round(sum / scale), scale = the channel's
// largest voxel / 255 (65535); 32 bits the sums as they are (float32).
class Stack
{
public:
   // x, y, z in um; w the weight per row (null: 1 each); ch the channel per row (null: 0). A row outside the grid
   // goes to its edge voxel. The rows are copied.
   Stack(const Grid& g, int channels, int bits, const double* x, const double* y, const double* z, const double* w,
         const uint8_t* ch, size_t n);
   const Grid& grid() const { return g_; }
   int Channels() const { return nc_; }
   int Bits() const { return bits_; }
   size_t PlaneBytes() const { return static_cast<size_t>(g_.nx) * g_.ny * static_cast<size_t>(bits_ / 8); }
   bool Empty(uint32_t z) const { return start_[z + 1] == start_[z]; }
   // The first pass: each channel's largest voxel and its sum; sets Scales().
   void Maxima();
   const std::vector<double>& Maxes() const { return max_; }
   const std::vector<double>& Sums() const { return sum_; }
   const std::vector<double>& Scales() const { return scale_; }   // value per step (1 at 32 bits)
   // Plane z's sums, every channel; PlaneMax and Quantize read them.
   void Fill(uint32_t z);
   double PlaneMax(int ch) const;
   // Channel ch of the filled plane as stored (little-endian, nx x ny samples of Bits()), into out (PlaneBytes());
   // scale 0 = the channel's (Maxima), else that value per step.
   void Quantize(int ch, unsigned char* out, double scale = 0) const;
   // Fill + Quantize of one channel with the caller's scratch (planes on several threads at once): acc (nx x ny
   // floats, zero on entry and on return) and touched; the same bytes.
   void PlaneInto(uint32_t z, int ch, std::vector<float>& acc, std::vector<uint32_t>& touched, unsigned char* out,
                  double scale = 0) const;

private:
   void QuantizeFrom(const std::vector<float>& a, const std::vector<uint32_t>& touched, int ch, unsigned char* out,
                     double scale) const;
   Grid g_;
   int nc_, bits_;
   std::vector<uint32_t> start_;   // rows of plane z: [start_[z], start_[z + 1]) of the sorted rows
   std::vector<uint32_t> at_;      // the sorted rows' voxel in their plane
   std::vector<double> w_;         // their weight
   std::vector<uint8_t> ch_;       // their channel
   std::vector<std::vector<float>> acc_;
   std::vector<uint32_t> touched_;   // the filled plane's voxels that hold rows (repeats allowed)
   std::vector<double> max_, sum_, scale_;
};

// One label per plane, channel fastest: "<channel>, " when there are several, then "z = <the slice's centre> µm".
std::vector<std::string> Labels(const Grid& g, const std::vector<std::string>& channels);
// The ImageJ description: a hyperstack (composite) with several channels, unit micron, the z spacing, the origin in
// voxels (Fiji's status bar shows world um), a single 8/16-bit channel's calibration (cf/c0/c1/vunit = unit), min/max,
// then insiliscope.value=<value> per voxel, extra (lines ending in \n), insiliscope.channels, insiliscope.scale (8/16
// bits: unit per step, each channel), insiliscope.origin-um and insiliscope.voxel-um.
std::string Description(const Stack& s, const std::vector<std::string>& channels, const std::string& value,
                        const std::string& unit, const std::string& extra);

// A classic little-endian TIFF around planes stored elsewhere: head = the header, the description, the resolution
// (pixels per um, X and Y), the labels (ImageJ's IJMetadata); the planes at offsets, back to back after head, never
// padded (ImageJ reads an uncompressed stack as one block from the first plane); ifd = an IFD per plane, at first.
// sizes: each plane's stored bytes; sameAs (optional): plane k stored as plane sameAs[k] < k (its size not laid out
// again: an empty plane stored once), -1 its own. compression: 1 none, 8 deflate (zlib), 32773 PackBits. error: the
// file passes a classic TIFF's 4 GB.
struct TiffLayout
{
   std::string head, ifd;
   std::vector<uint64_t> offsets;
   uint64_t first = 0;
   std::string error;
};
TiffLayout LayoutTiff(const std::vector<uint64_t>& sizes, const std::vector<int64_t>* sameAs, uint32_t nx, uint32_t ny,
                      int bits, double pixelsPerUm, const std::string& description,
                      const std::vector<std::string>& labels, int compression);

// PackBits (TIFF compression 32773) of n bytes, appended to out.
void PackBits(const unsigned char* b, size_t n, std::string& out);
// A zlib stream (TIFF compression 8, Adobe deflate) of n bytes, appended to out: runs as matches at distance 1 (up to
// 258 bytes each), the rest literals, one dynamic-Huffman block per 2^20 bytes. A mostly empty plane deflates to
// ~1/1000 of its size (PackBits: 1/64); ImageJ, tifffile and zlib read it.
void Deflate(const unsigned char* b, size_t n, std::string& out);

} // namespace vox
} // namespace sim
