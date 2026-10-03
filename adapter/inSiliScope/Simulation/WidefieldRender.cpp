///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldRender.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldRender.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "WidefieldRender.h"

#include "CellFieldSource.h"
#include "PsfGeneratorBridge.h"

#include "insiliscope/insiliscope.h"

#include "Parallel.h"
#include "Timing.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <limits>

namespace sim {

namespace {

constexpr double kAvogadro = 6.02214076e23;
// Largest dye grid (floats per population) BuildWidefieldDyeGrid allocates.
constexpr double kMaxGridFloats = 400e6;
// The z column dye tiles cover (world um): every cell of the model sits in it.
constexpr double kColumnMinUm = -5.0, kColumnMaxUm = 50.0;
const double kPi = 3.14159265358979323846;

size_t DefaultTileBudget()
{
#if defined(__EMSCRIPTEN__)
   return size_t(256) << 20;
#else
   return size_t(1536) << 20;
#endif
}

size_t DefaultSpectraBudget()
{
#if defined(__EMSCRIPTEN__)
   return size_t(256) << 20;
#else
   return size_t(1024) << 20;
#endif
}

long FloorDiv(long a, long b)
{
   return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Floor of x, snapping values within 1e-9 of an integer onto it (world
// coordinates / pitch that should land on a cell boundary).
long SnapFloor(double x)
{
   const double r = std::round(x);
   if (std::fabs(x - r) < 1e-9)
      return static_cast<long>(r);
   return static_cast<long>(std::floor(x));
}

// Process-wide serial numbers for channel versions and scene geometries: a
// GPU host shared by several scenes (a self-check, the live scene, a
// prefetch) never sees one scene's keys under another's.
unsigned long long NextSerial()
{
   static std::atomic<unsigned long long> next{1};
   return next.fetch_add(1);
}

double Sinc(double x)
{
   return std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
}

} // namespace

// ---- Photophysics ----------------------------------------------------------

double WidefieldPhotophysics::CrossSectionUm2() const
{
   // ln(10) * 1000 * eps / N_A in cm^2, x 1e8 um^2/cm^2.
   return std::log(10.0) * 1000.0 * extinctionCoeff / kAvogadro * 1e8;
}

double WidefieldPhotophysics::EmissionRatePerSec(double I) const
{
   return quantumYield * CrossSectionUm2() * excitationPhotonsPerUm2PerSec * I;
}

double WidefieldPhotophysics::HalfTimeSec(double I) const
{
   const double k = EmissionRatePerSec(I);
   if (!Bleaches() || !(k > 0.0))
      return std::numeric_limits<double>::infinity();
   return photonBudget * std::log(2.0) / k;
}

double WidefieldCollectionEfficiency(double na, double immersionIndex)
{
   const double r = std::min(1.0, std::max(0.0, na / std::max(1e-6, immersionIndex)));
   return 0.5 * (1.0 - std::sqrt(1.0 - r * r));
}

double WidefieldBleachingPhotons(double eta, double budget, double d0, double dD)
{
   if (!(budget > 0.0))
      return eta * dD;
   return eta * budget * std::exp(-d0 / budget) * (1.0 - std::exp(-dD / budget));
}

double WidefieldGaussianSigmaUm(double defocusUm, double lambdaNm, double na, double immersionIndex)
{
   // TODO(human): the emission PSF's width at a defocus. This placeholder is
   // the in-focus sigma (the cli's 0.21 lambda / NA) and ignores z, so the
   // Gaussian WideField image has no out-of-focus haze yet. Candidates:
   //   Gaussian-beam law     sigma0 * sqrt(1 + (dz / zR)^2)
   //   geometric defocus cone sqrt(sigma0^2 + (dz NA / (2 sqrt(n^2 - NA^2)))^2)
   //   or a blend of the two.
   (void)defocusUm;
   (void)immersionIndex;
   return 0.21 * lambdaNm / std::max(0.01, na) / 1000.0;
}

// ---- Dye grid --------------------------------------------------------------

bool BuildWidefieldDyeGrid(CellFieldSource& src, const WidefieldGridSpec& spec, WidefieldDyeGrid& out,
                           std::string& err)
{
   out = WidefieldDyeGrid();
   out.spec = spec;
   const double dz = spec.zPlaneUm;
   if (!(dz > 0.0) || spec.nx == 0 || spec.ny == 0 || !(spec.zMaxUm > spec.zMinUm))
   {
      err = "WideField: bad dye grid";
      return false;
   }
   const long kLo = static_cast<long>(std::floor(spec.zMinUm / dz));
   const long kHi = static_cast<long>(std::ceil(spec.zMaxUm / dz));
   const long nH = kHi - kLo;
   if (nH <= 0 || nH > 4000000)
   {
      err = "WideField: too many z planes (raise the plane thickness or narrow the z range)";
      return false;
   }
   const double x1 = spec.x0Um + spec.nx * spec.pitchUm, y1 = spec.y0Um + spec.ny * spec.pitchUm;
   std::vector<float> hist(static_cast<size_t>(nH));
   const long n = src.Density3d(spec.x0Um, spec.y0Um, x1, y1, kLo * dz, kHi * dz, 1, 1, static_cast<int>(nH),
                                ISC_POP_BLEACHING | ISC_POP_PERSISTENT, hist.data());
   if (n < 0)
   {
      err = "WideField: dye density query failed";
      return false;
   }
   if (n == 0)
      return true;
   long first = 0, last = nH - 1;
   while (first < nH && hist[static_cast<size_t>(first)] == 0.0f)
      ++first;
   while (last > first && hist[static_cast<size_t>(last)] == 0.0f)
      --last;
   // One spare plane each side: a dye on a plane boundary may bin either way.
   first = std::max(0L, first - 1);
   last = std::min(nH - 1, last + 1);
   out.k0 = kLo + first;
   out.nz = static_cast<unsigned>(last - first + 1);
   const double cells = static_cast<double>(spec.nx) * spec.ny * out.nz;
   if (cells > kMaxGridFloats)
   {
      err = "WideField: dye grid too large (lower the upscaling, raise the plane thickness or set a z range)";
      return false;
   }
   const double zA = out.k0 * dz, zB = (out.k0 + static_cast<long>(out.nz)) * dz;
   for (int pop : {ISC_POP_BLEACHING, ISC_POP_PERSISTENT})
   {
      std::vector<float>& g = pop == ISC_POP_BLEACHING ? out.bleaching : out.persistent;
      g.assign(static_cast<size_t>(cells), 0.0f);
      const long c = src.Density3d(spec.x0Um, spec.y0Um, x1, y1, zA, zB, static_cast<int>(spec.nx),
                                   static_cast<int>(spec.ny), static_cast<int>(out.nz), pop, g.data());
      if (c < 0)
      {
         err = "WideField: dye density query failed";
         return false;
      }
      if (c == 0)
         std::vector<float>().swap(g);
      (pop == ISC_POP_BLEACHING ? out.nBleaching : out.nPersistent) = c;
   }
   return true;
}

// ---- PSF providers -----------------------------------------------------------

GaussianWidefieldPsf::GaussianWidefieldPsf(double pitchUm, double lambdaNm, double na, double immersionIndex,
                                           double stepUm)
   : pitch_(pitchUm), lambdaNm_(lambdaNm), na_(na), n_(immersionIndex), step_(stepUm > 0.0 ? stepUm : 0.1)
{
}

double GaussianWidefieldPsf::Sigma(int p) const
{
   return WidefieldGaussianSigmaUm(p * step_, lambdaNm_, na_, n_);
}

int GaussianWidefieldPsf::Radius(int pLo, int pHi) const
{
   double s = std::max(Sigma(pLo), Sigma(pHi));
   if (pLo <= 0 && pHi >= 0)
      s = std::max(s, Sigma(0));
   return std::max(1, static_cast<int>(std::ceil(3.0 * s / pitch_)));
}

void GaussianWidefieldPsf::Kernel(int p, int R, std::vector<float>& out) const
{
   const int D = 2 * R + 1;
   out.assign(static_cast<size_t>(D) * D, 0.0f);
   const double s = Sigma(p) / pitch_, twoS2 = 2.0 * s * s;
   std::vector<double> k(out.size());
   double sum = 0.0;
   for (int y = -R; y <= R; ++y)
      for (int x = -R; x <= R; ++x)
      {
         const double v = std::exp(-(static_cast<double>(x) * x + static_cast<double>(y) * y) / twoS2);
         k[static_cast<size_t>(y + R) * D + (x + R)] = v;
         sum += v;
      }
   for (size_t i = 0; i < k.size(); ++i)
      out[i] = static_cast<float>(k[i] / sum);
}

int KernelWidefieldPsf::ValidUpscale(int oversampling, int requested)
{
   const int os = std::max(1, oversampling);
   for (int u = std::max(1, std::min(requested, os)); u > 1; --u)
      if (os % u == 0)
         return u;
   return 1;
}

KernelWidefieldPsf::KernelWidefieldPsf(const PsfKernelCache& cache, int upscale) : c_(cache)
{
   const int os = std::max(1, cache.oversampling);
   r_ = std::max(1, os / ValidUpscale(os, upscale));
   // As SplatSetup (Nearest) for a dye at a cell centre: tx = kc - r/2 + 0.5,
   // rounded half up.
   const double kc = (cache.sizeOversampled - 1) / 2.0;
   s0_ = static_cast<int>(std::floor(kc - r_ / 2.0 + 1.0));
}

double KernelWidefieldPsf::PlaneCoord(double defocusUm) const
{
   if (c_.nz <= 1 || !(c_.zStepNm > 0.0))
      return 0.0;
   return defocusUm * 1000.0 / c_.zStepNm + (c_.nz - 1) / 2.0;
}

int KernelWidefieldPsf::MaxPlane() const
{
   return std::max(0, c_.nz - 1);
}

int KernelWidefieldPsf::Radius(int, int) const
{
   const int n = c_.sizeOversampled;
   // Grid cells d whose block [s0 + d r, s0 + d r + r - 1] overlaps [0, n).
   const int dMin = static_cast<int>(std::ceil((-s0_ - r_ + 1) / static_cast<double>(r_)));
   const int dMax = static_cast<int>(std::floor((n - 1 - s0_) / static_cast<double>(r_)));
   return std::max(1, std::max(-dMin, dMax));
}

void KernelWidefieldPsf::Kernel(int p, int R, std::vector<float>& out) const
{
   const int D = 2 * R + 1, n = c_.sizeOversampled;
   out.assign(static_cast<size_t>(D) * D, 0.0f);
   if (p < 0 || p >= static_cast<int>(c_.Planes().size()))
      return;
   const std::vector<float>& P = c_.Planes()[static_cast<size_t>(p)];
   for (int dy = -R; dy <= R; ++dy)
   {
      const int ya = s0_ + dy * r_, yb = std::min(n, ya + r_);
      for (int dx = -R; dx <= R; ++dx)
      {
         const int xa = s0_ + dx * r_, xb = std::min(n, xa + r_);
         double acc = 0.0;
         for (int y = std::max(0, ya); y < yb; ++y)
            for (int x = std::max(0, xa); x < xb; ++x)
               acc += P[static_cast<size_t>(y) * n + x];
         out[static_cast<size_t>(dy + R) * D + (dx + R)] = static_cast<float>(acc);
      }
   }
}

// ---- Bleach field ------------------------------------------------------------

void BleachField::Reset(double pitchUm)
{
   tiles_.clear();
   pitch_ = pitchUm;
}

void BleachField::DoseOver(double x0Um, double y0Um, unsigned nx, unsigned ny, std::vector<float>& out) const
{
   out.assign(static_cast<size_t>(nx) * ny, 0.0f);
   if (tiles_.empty() || !(pitch_ > 0.0))
      return;
   const long long cx0 = static_cast<long long>(std::floor(x0Um / pitch_ + 0.5));
   const long long cy0 = static_cast<long long>(std::floor(y0Um / pitch_ + 0.5));
   for (unsigned iy = 0; iy < ny; ++iy)
   {
      const long long cy = cy0 + iy;
      const long long ty = cy >= 0 ? cy / kTile : -((-cy + kTile - 1) / kTile);
      for (unsigned ix = 0; ix < nx; ++ix)
      {
         const long long cx = cx0 + ix;
         const long long tx = cx >= 0 ? cx / kTile : -((-cx + kTile - 1) / kTile);
         auto it = tiles_.find({tx, ty});
         if (it == tiles_.end())
            continue;
         out[static_cast<size_t>(iy) * nx + ix] =
            it->second[static_cast<size_t>(cy - ty * kTile) * kTile + static_cast<size_t>(cx - tx * kTile)];
      }
   }
}

void BleachField::Deposit(const IlluminationPattern& pattern, double axisXUm, double axisYUm, double dDPerUnitI)
{
   if (!(pitch_ > 0.0) || !(dDPerUnitI > 0.0))
      return;
   double sx0, sy0, sx1, sy1;
   pattern.Support(sx0, sy0, sx1, sy1);
   const long long cx0 = static_cast<long long>(std::floor((axisXUm + sx0) / pitch_));
   const long long cx1 = static_cast<long long>(std::floor((axisXUm + sx1) / pitch_));
   const long long cy0 = static_cast<long long>(std::floor((axisYUm + sy0) / pitch_));
   const long long cy1 = static_cast<long long>(std::floor((axisYUm + sy1) / pitch_));
   for (long long cy = cy0; cy <= cy1; ++cy)
   {
      const long long ty = cy >= 0 ? cy / kTile : -((-cy + kTile - 1) / kTile);
      const double dy = (cy + 0.5) * pitch_ - axisYUm;
      for (long long cx = cx0; cx <= cx1; ++cx)
      {
         const double I = pattern.At((cx + 0.5) * pitch_ - axisXUm, dy);
         if (!(I > 0.0))
            continue;
         const long long tx = cx >= 0 ? cx / kTile : -((-cx + kTile - 1) / kTile);
         std::vector<float>& t = tiles_[{tx, ty}];
         if (t.empty())
            t.assign(static_cast<size_t>(kTile) * kTile, 0.0f);
         t[static_cast<size_t>(cy - ty * kTile) * kTile + static_cast<size_t>(cx - tx * kTile)] +=
            static_cast<float>(dDPerUnitI * I);
      }
   }
}

// ---- Sparse dye planes and tiles ---------------------------------------------

WidefieldDyePlanes WidefieldDyePlanes::FromGrid(const WidefieldDyeGrid& g)
{
   WidefieldDyePlanes out;
   out.rect = g.spec;
   const size_t n2 = static_cast<size_t>(g.spec.nx) * g.spec.ny;
   for (unsigned i = 0; i < g.nz; ++i)
   {
      WidefieldSparsePlane pl;
      for (int pop = 0; pop < 2; ++pop)
      {
         const std::vector<float>& src = pop == 0 ? g.bleaching : g.persistent;
         if (src.empty())
            continue;
         for (size_t c = 0; c < n2; ++c)
         {
            const float v = src[i * n2 + c];
            if (v == 0.0f)
               continue;
            pl.cell[pop].push_back(static_cast<uint32_t>(c));
            pl.count[pop].push_back(v);
            (pop == 0 ? out.nBleaching : out.nPersistent) += static_cast<long>(v);
         }
      }
      if (!pl.Empty(0) || !pl.Empty(1))
         out.planes[g.k0 + static_cast<long>(i)] = std::move(pl);
   }
   return out;
}

WidefieldDyeTiles::WidefieldDyeTiles(size_t budgetBytes)
   : budget_(budgetBytes > 0 ? budgetBytes : DefaultTileBudget())
{
}

size_t WidefieldDyeTiles::Bytes() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return bytes_;
}

void WidefieldDyeTiles::Clear()
{
   std::lock_guard<std::mutex> g(mutex_);
   tiles_.clear();
   bytes_ = 0;
}

bool WidefieldDyeTiles::Fill(CellFieldSource& src, long tx, long ty, Tile& t, std::string& err) const
{
   const double p = pitch_, dz = zPlane_;
   const double x0 = static_cast<double>(tx * kTile) * p, x1 = static_cast<double>((tx + 1) * kTile) * p;
   const double y0 = static_cast<double>(ty * kTile) * p, y1 = static_cast<double>((ty + 1) * kTile) * p;
   const long kLo = SnapFloor(zMin_ / dz), kHi = -SnapFloor(-zMax_ / dz);
   const long nH = kHi - kLo;
   if (nH <= 0 || nH > 4000000)
   {
      err = "WideField: too many z planes (raise the plane thickness)";
      return false;
   }
   std::vector<float> hist(static_cast<size_t>(nH));
   const long n = src.Density3d(x0, y0, x1, y1, kLo * dz, kHi * dz, 1, 1, static_cast<int>(nH),
                                ISC_POP_BLEACHING | ISC_POP_PERSISTENT, hist.data());
   if (n < 0)
   {
      err = "WideField: dye density query failed";
      return false;
   }
   t.bytes = sizeof(Tile);
   if (n == 0)
      return true;
   long first = 0, last = nH - 1;
   while (first < nH && hist[static_cast<size_t>(first)] == 0.0f)
      ++first;
   while (last > first && hist[static_cast<size_t>(last)] == 0.0f)
      --last;
   // One spare plane each side: a dye on a plane boundary may bin either way.
   first = std::max(0L, first - 1);
   last = std::min(nH - 1, last + 1);
   const long k0 = kLo + first;
   const unsigned nz = static_cast<unsigned>(last - first + 1);
   const size_t n2 = static_cast<size_t>(kTile) * kTile;
   std::vector<float> g(n2 * nz);
   for (int pop = 0; pop < 2; ++pop)
   {
      std::fill(g.begin(), g.end(), 0.0f);
      const long c = src.Density3d(x0, y0, x1, y1, k0 * dz, (k0 + static_cast<long>(nz)) * dz, kTile, kTile,
                                   static_cast<int>(nz), pop == 0 ? ISC_POP_BLEACHING : ISC_POP_PERSISTENT, g.data());
      if (c < 0)
      {
         err = "WideField: dye density query failed";
         return false;
      }
      (pop == 0 ? t.nBleaching : t.nPersistent) = c;
      if (c == 0)
         continue;
      for (unsigned i = 0; i < nz; ++i)
         for (size_t cell = 0; cell < n2; ++cell)
         {
            const float v = g[i * n2 + cell];
            if (v == 0.0f)
               continue;
            WidefieldSparsePlane& pl = t.planes[k0 + static_cast<long>(i)];
            pl.cell[pop].push_back(static_cast<uint32_t>(cell));
            pl.count[pop].push_back(v);
         }
   }
   for (const auto& kv : t.planes)
      for (int pop = 0; pop < 2; ++pop)
         t.bytes += kv.second.cell[pop].size() * (sizeof(uint32_t) + sizeof(float)) + 64;
   return true;
}

bool WidefieldDyeTiles::Planes(CellFieldSource& src, const WidefieldGridSpec& rect, long world, WidefieldDyePlanes& out,
                               std::string& err)
{
   const long tx0 = FloorDiv(rect.ix0, kTile), tx1 = FloorDiv(rect.ix0 + static_cast<long>(rect.nx) - 1, kTile);
   const long ty0 = FloorDiv(rect.iy0, kTile), ty1 = FloorDiv(rect.iy0 + static_cast<long>(rect.ny) - 1, kTile);
   std::vector<std::pair<std::pair<long, long>, std::shared_ptr<Tile>>> use;
   unsigned long long stamp;
   {
      std::lock_guard<std::mutex> g(mutex_);
      if (pitch_ != rect.pitchUm || zPlane_ != rect.zPlaneUm || zMin_ != rect.zMinUm || zMax_ != rect.zMaxUm ||
          world_ != world)
      {
         tiles_.clear();
         bytes_ = 0;
         pitch_ = rect.pitchUm;
         zPlane_ = rect.zPlaneUm;
         zMin_ = rect.zMinUm;
         zMax_ = rect.zMaxUm;
         world_ = world;
      }
      stamp = ++clock_;
   }
   for (long ty = ty0; ty <= ty1; ++ty)
      for (long tx = tx0; tx <= tx1; ++tx)
      {
         std::shared_ptr<Tile> t;
         {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = tiles_.find({tx, ty});
            if (it != tiles_.end())
            {
               t = it->second;
               t->used = stamp;
            }
         }
         if (!t)
         {
            // Filled outside the lock; a concurrent fill of the same tile
            // gives the same content, the first insert wins.
            auto fresh = std::make_shared<Tile>();
            if (!Fill(src, tx, ty, *fresh, err))
               return false;
            fresh->used = stamp;
            std::lock_guard<std::mutex> g(mutex_);
            if (pitch_ != rect.pitchUm || world_ != world)
            {
               err = "WideField: dye tile cache reconfigured concurrently";
               return false;
            }
            auto ins = tiles_.insert({{tx, ty}, fresh});
            if (ins.second)
               bytes_ += fresh->bytes;
            t = ins.first->second;
         }
         use.push_back({{tx, ty}, t});
      }
   {
      // Least recently used first, never the ones this call uses.
      std::lock_guard<std::mutex> g(mutex_);
      while (bytes_ > budget_)
      {
         auto victim = tiles_.end();
         for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
            if (it->second->used < stamp && (victim == tiles_.end() || it->second->used < victim->second->used))
               victim = it;
         if (victim == tiles_.end())
            break;
         bytes_ -= victim->second->bytes;
         tiles_.erase(victim);
      }
   }

   out = WidefieldDyePlanes();
   out.rect = rect;
   const long gx1 = rect.ix0 + static_cast<long>(rect.nx), gy1 = rect.iy0 + static_cast<long>(rect.ny);
   for (const auto& e : use)
   {
      const long bx = e.first.first * kTile, by = e.first.second * kTile;
      for (const auto& kv : e.second->planes)
      {
         WidefieldSparsePlane* dst = nullptr;
         for (int pop = 0; pop < 2; ++pop)
         {
            const std::vector<uint32_t>& cells = kv.second.cell[pop];
            const std::vector<float>& counts = kv.second.count[pop];
            for (size_t i = 0; i < cells.size(); ++i)
            {
               const long wx = bx + static_cast<long>(cells[i] % kTile), wy = by + static_cast<long>(cells[i] / kTile);
               if (wx < rect.ix0 || wx >= gx1 || wy < rect.iy0 || wy >= gy1)
                  continue;
               if (!dst)
                  dst = &out.planes[kv.first];
               dst->cell[pop].push_back(static_cast<uint32_t>((wy - rect.iy0) * static_cast<long>(rect.nx) + (wx - rect.ix0)));
               dst->count[pop].push_back(counts[i]);
               (pop == 0 ? out.nBleaching : out.nPersistent) += static_cast<long>(counts[i]);
            }
         }
      }
   }
   return true;
}

// ---- Images ------------------------------------------------------------------

void WidefieldImages::Render(const std::vector<double>& a, std::vector<float>& cam) const
{
   const bool useP = !persistent.empty();
   size_t nb = 0;
   for (size_t j = 0; j < bleach.size() && j < a.size(); ++j)
      if (!bleach[j].empty() && a[j] != 0.0)
         nb = j + 1;
   if (!useP && nb == 0)
      return;
   const unsigned u = std::max(1u, upscale), W = cw / u, H = ch / u;
   cam.resize(static_cast<size_t>(W) * H, 0.0f);
   std::vector<float> af(nb);
   for (size_t j = 0; j < nb; ++j)
      af[j] = static_cast<float>(a[j]);
   for (unsigned Y = 0; Y < H; ++Y)
      for (unsigned X = 0; X < W; ++X)
      {
         double acc = 0.0;
         for (unsigned sy = 0; sy < u; ++sy)
         {
            const size_t row = static_cast<size_t>(Y * u + sy) * cw + X * u;
            for (unsigned sx = 0; sx < u; ++sx)
            {
               const size_t i = row + sx;
               float v = useP ? persistent[i] : 0.0f;
               for (size_t j = 0; j < nb; ++j)
                  if (!bleach[j].empty())
                     v += af[j] * bleach[j][i];
               acc += std::max(0.0f, v);
            }
         }
         cam[static_cast<size_t>(Y) * W + X] += static_cast<float>(acc);
      }
}

// ---- Scene -------------------------------------------------------------------

WidefieldScene::WidefieldScene() : tiles_(std::make_shared<WidefieldDyeTiles>()), cacheBudget_(DefaultSpectraBudget())
{
}

WidefieldGridSpec WidefieldScene::GridSpecFor(const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                                             unsigned* fovCellX, unsigned* fovCellY, double* fracX, double* fracY)
{
   const int u = std::max(1, spec.grid.upscale);
   const double pitch = spec.pixelUm / u;
   // Pixel X's centre is at originX + X pixelUm (the SR renderer's
   // convention), so the FOV spans [originX - pixel/2, ...); the axis is its
   // centre.
   const double axisX = spec.originXUm + (spec.width - 1) / 2.0 * spec.pixelUm;
   const double axisY = spec.originYUm + (spec.height - 1) / 2.0 * spec.pixelUm;
   const double fx0 = spec.originXUm - spec.pixelUm / 2, fy0 = spec.originYUm - spec.pixelUm / 2;
   const double fx1 = fx0 + spec.width * spec.pixelUm, fy1 = fy0 + spec.height * spec.pixelUm;
   // Where the pattern still excites beyond the FOV (dyes elsewhere are
   // dark), at most marginUm.
   double sx0, sy0, sx1, sy1;
   pattern.Support(sx0, sy0, sx1, sy1);
   const double m = std::max(0.0, spec.marginUm);
   const double ex0 = std::min(fx0, std::max(axisX + sx0, fx0 - m)), ex1 = std::max(fx1, std::min(axisX + sx1, fx1 + m));
   const double ey0 = std::min(fy0, std::max(axisY + sy0, fy0 - m)), ey1 = std::max(fy1, std::min(axisY + sy1, fy1 + m));

   WidefieldGridSpec g;
   g.pitchUm = pitch;
   // World-anchored cells: [i pitch, (i + 1) pitch). One extra cell past the
   // FOV's far edge holds the sub-cell shift.
   g.ix0 = SnapFloor(ex0 / pitch);
   g.iy0 = SnapFloor(ey0 / pitch);
   const long ix1 = std::max(-SnapFloor(-ex1 / pitch), SnapFloor(fx1 / pitch) + 1);
   const long iy1 = std::max(-SnapFloor(-ey1 / pitch), SnapFloor(fy1 / pitch) + 1);
   g.nx = static_cast<unsigned>(ix1 - g.ix0);
   g.ny = static_cast<unsigned>(iy1 - g.iy0);
   g.x0Um = g.ix0 * pitch;
   g.y0Um = g.iy0 * pitch;
   const long cx = SnapFloor(fx0 / pitch), cy = SnapFloor(fy0 / pitch);
   if (fovCellX)
      *fovCellX = static_cast<unsigned>(cx - g.ix0);
   if (fovCellY)
      *fovCellY = static_cast<unsigned>(cy - g.iy0);
   if (fracX)
      *fracX = std::max(0.0, fx0 / pitch - cx);
   if (fracY)
      *fracY = std::max(0.0, fy0 / pitch - cy);
   g.zPlaneUm = std::max(1e-3, spec.grid.zPlaneNm / 1000.0);
   if (spec.slabHalfUm > 0.0)
   {
      g.zMinUm = spec.slabCentreUm - spec.slabHalfUm;
      g.zMaxUm = spec.slabCentreUm + spec.slabHalfUm;
   }
   else
   {
      g.zMinUm = kColumnMinUm;
      g.zMaxUm = kColumnMaxUm;
   }
   return g;
}

bool WidefieldScene::Update(CellFieldSource& src, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                            const WidefieldPsf& psf, std::string& err)
{
   if (spec.width == 0 || spec.height == 0 || !(spec.pixelUm > 0.0))
   {
      err = "WideField: bad frame";
      return false;
   }
   const WidefieldGridSpec g = GridSpecFor(pattern, spec);
   const bool rectChanged = !haveSpec_ || !g.SameRect(rect_) || spec.worldVersion != spec_.worldVersion;
   if (rectChanged)
   {
      haveSpec_ = false; // a failure below leaves no half-built state
      WidefieldGridSpec column = g;
      column.zMinUm = kColumnMinUm;
      column.zMaxUm = kColumnMaxUm;
      const auto tTiles = TimingClock::now();
      if (!tiles_->Planes(src, column, spec.worldVersion, dyes_, err))
         return false;
      TimingLog("wf.scene.dye-tiles", TimingSince(tTiles));
   }
   rect_ = g;
   const auto tFinish = TimingClock::now();
   const bool ok = Finish(spec, pattern, psf, rectChanged, err);
   TimingLog("wf.scene.finish", TimingSince(tFinish));
   return ok;
}

bool WidefieldScene::UpdateFromGrid(const WidefieldDyeGrid& grid, const IlluminationPattern& pattern,
                                    const WidefieldSceneSpec& spec, const WidefieldPsf& psf, std::string& err)
{
   if (spec.width == 0 || spec.height == 0 || !(spec.pixelUm > 0.0) ||
       !GridSpecFor(pattern, spec).SameRect(grid.spec))
   {
      err = "WideField: dye grid does not match the scene";
      return false;
   }
   haveSpec_ = false;
   dyes_ = WidefieldDyePlanes::FromGrid(grid);
   rect_ = GridSpecFor(pattern, spec);
   return Finish(spec, pattern, psf, true, err);
}

bool WidefieldScene::Finish(const WidefieldSceneSpec& spec, const IlluminationPattern& pattern,
                            const WidefieldPsf& psf, bool rectChanged, std::string& err)
{
   (void)err;
   const WidefieldGridSpec& g = rect_;
   unsigned fx = 0, fy = 0;
   double frx = 0.0, fry = 0.0;
   GridSpecFor(pattern, spec, &fx, &fy, &frx, &fry);
   axisX_ = spec.originXUm + (spec.width - 1) / 2.0 * spec.pixelUm;
   axisY_ = spec.originYUm + (spec.height - 1) / 2.0 * spec.pixelUm;

   // Illumination and the frame dose per grid column.
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   std::vector<float> ill(n2), dD(n2), wp(n2);
   pattern.Sample(g.x0Um - axisX_, g.y0Um - axisY_, g.pitchUm, g.nx, g.ny, ill.data());
   const double dD1 = spec.phot.EmissionRatePerSec(1.0) * spec.exposureSec;
   for (size_t i = 0; i < n2; ++i)
   {
      dD[i] = static_cast<float>(dD1 * ill[i]);
      wp[i] = static_cast<float>(spec.eta * dD[i]);
   }

   const bool samePsf = haveSpec_ && psf_ == &psf && spec.psfVersion == spec_.psfVersion;
   const bool psfChanged = rectChanged || !haveSpec_ || spec.psfVersion != spec_.psfVersion ||
                           spec.kernelCapUm != spec_.kernelCapUm || psf_ != &psf || spec.width != spec_.width ||
                           spec.height != spec_.height || spec.grid.upscale != spec_.grid.upscale;
   const bool moved = !haveSpec_ || fx != fovX0_ || fy != fovY0_ || frx != fracX_ || fry != fracY_;
   const bool focusChanged = !haveSpec_ || spec.focusWorldUm != spec_.focusWorldUm ||
                             spec.slabCentreUm != spec_.slabCentreUm || spec.slabHalfUm != spec_.slabHalfUm;
   spec_ = spec;
   psf_ = &psf;
   fovX0_ = fx;
   fovY0_ = fy;
   fracX_ = frx;
   fracY_ = fry;

   bool fftChanged = false;
   if (psfChanged || moved)
   {
      const unsigned oldX = nx_, oldY = ny_;
      const int oldR = R_;
      const auto tFft = TimingClock::now();
      SetupFft(psf, spec.kernelCapUm, samePsf);
      TimingLog("wf.scene.setup-fft", TimingSince(tFft));
      fftChanged = rectChanged || !samePsf || nx_ != oldX || ny_ != oldY || R_ != oldR ||
                   spec.psfVersion != kernelsVersion_;
      if (fftChanged)
      {
         kernels_.clear();
         kernelsVersion_ = spec.psfVersion;
      }
      if (fftChanged || rectChanged)
         geometry_ = NextSerial();
   }
   const bool weightsChanged = fftChanged || rectChanged || wp != wp_ || dD != dD_;
   illum_.swap(ill);
   dD_.swap(dD);
   if (weightsChanged)
   {
      wp_.swap(wp);
      if (fftChanged || rectChanged)
      {
         cache_.clear();
         cacheBytes_ = 0;
      }
      persistent_.pop = 1;
      persistent_.weight = wp_;
      DropChannel(persistent_);
      persistent_.version = NextSerial();
      for (Channel& c : bleach_)
         DropChannel(c);
      bleach_.clear();
      bleachValid_ = false;
   }
   haveSpec_ = true;
   if (weightsChanged || moved || psfChanged)
      ++imagesVersion_;
   if (weightsChanged || focusChanged || moved || psfChanged)
   {
      const auto tRefocus = TimingClock::now();
      Refocus(psf);
      TimingLog("wf.scene.refocus", TimingSince(tRefocus));
   }
   return true;
}

void WidefieldScene::DropChannel(const Channel& c)
{
   for (auto it = cache_.begin(); it != cache_.end();)
   {
      if (std::get<0>(it->first) == c.version)
      {
         cacheBytes_ -= it->second.spec.size() * sizeof(cfloat);
         it = cache_.erase(it);
      }
      else
         ++it;
   }
}

void WidefieldScene::SetupFft(const WidefieldPsf& psf, double kernelCapUm, bool keepRadius)
{
   const WidefieldGridSpec& g = rect_;
   const int cap = std::max(1, static_cast<int>(std::ceil(kernelCapUm / g.pitchUm - 1e-9)));
   // Natural radius over the planes the column's dyes can map to (constant
   // for the PSFs in use; kept if it would only shrink, so the kernel and
   // plane spectra stay valid across focus changes).
   int pLo = psf.MaxPlane(), pHi = psf.MinPlane();
   if (!dyes_.planes.empty())
   {
      const double dz = g.zPlaneUm;
      const double za = (dyes_.planes.begin()->first + 0.5) * dz, zb = (dyes_.planes.rbegin()->first + 0.5) * dz;
      auto clampP = [&](double t) {
         return static_cast<int>(std::min<double>(psf.MaxPlane(), std::max<double>(psf.MinPlane(), std::floor(t))));
      };
      const int a = clampP(psf.PlaneCoord(za - spec_.focusWorldUm));
      const int b = clampP(psf.PlaneCoord(zb - spec_.focusWorldUm));
      pLo = std::min(a, b);
      pHi = std::min(psf.MaxPlane(), std::max(a, b) + 1);
   }
   const int natural = pLo <= pHi ? psf.Radius(pLo, pHi) : psf.Radius(0, 0);
   const int R = std::max(1, std::min(cap, natural));
   R_ = (keepRadius && R_ >= R && R_ <= cap) ? R_ : R;
   // Linear (not circular) convolution over the FOV cells: the image of the
   // grid spans [-R - 4, n + R + 4) (kernel, cloud-in-cell spread, sub-cell
   // shift), and the wrapped part must miss the FOV cells.
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   auto need = [&](unsigned n, unsigned fov0, unsigned fovN) {
      const unsigned a = fov0 + fovN + static_cast<unsigned>(R_) + 6;
      const unsigned b = n + static_cast<unsigned>(R_) + 6 - std::min(fov0, n);
      const unsigned m = std::max({n + 8, a, b});
      if (gpuMode_)
      {
         unsigned p = 16;
         while (p < m)
            p <<= 1;
         return p;
      }
      return RealFft2d::FastSize(m, 8);
   };
   const unsigned nx = need(g.nx, fovX0_, spec_.width * u), ny = need(g.ny, fovY0_, spec_.height * u);
   if (nx != nx_ || ny != ny_)
   {
      nx_ = nx;
      ny_ = ny;
      levels_ = 1;
      fft_[0] = RealFft2d(nx, ny);
      for (int L = 1; L < kLevels && !gpuMode_; ++L)
      {
         const unsigned f = 1u << L;
         if (nx % (2 * f) != 0 || ny % f != 0 || nx / f < 16 || ny / f < 16)
            break;
         fft_[L] = RealFft2d(nx / f, ny / f);
         levels_ = L + 1;
      }
   }
}

namespace {

// Energy fraction of a kernel spectrum a level-f cloud-in-cell convolution
// gets wrong: out-of-band energy plus the binning's aliases (white source)
// after deconvolution. 1D: sum_m sinc^4(pi (u + m)) = 1 - 2/3 sin^2(pi u).
double BandError(const std::vector<cfloat>& K, unsigned nx, unsigned ny, unsigned f)
{
   const unsigned W = nx / 2 + 1, ncx = nx / f, ncy = ny / f;
   double tot = 0.0, bad = 0.0;
   for (unsigned ky = 0; ky < ny; ++ky)
   {
      const long kys = ky <= ny / 2 ? static_cast<long>(ky) : static_cast<long>(ky) - static_cast<long>(ny);
      for (unsigned kx = 0; kx < W; ++kx)
      {
         const double m = (kx == 0 || kx == nx / 2) ? 1.0 : 2.0;
         const double e = m * std::norm(K[static_cast<size_t>(ky) * W + kx]);
         tot += e;
         const bool in = kx < ncx / 2 && std::labs(kys) < static_cast<long>(ncy / 2);
         if (!in)
         {
            bad += e;
            continue;
         }
         auto ratio = [&](double uu) {
            const double s = std::sin(kPi * uu), p = Sinc(kPi * uu);
            return (1.0 - 2.0 / 3.0 * s * s) / (p * p * p * p);
         };
         bad += e * (ratio(static_cast<double>(kx) / ncx) * ratio(static_cast<double>(kys) / ncy) - 1.0);
      }
   }
   return tot > 0.0 ? bad / tot : 0.0;
}

} // namespace

void WidefieldScene::MakeKernel(const WidefieldPsf& psf, int p, KernelSpec& ks) const
{
   const unsigned nx = nx_, ny = ny_, W = nx / 2 + 1;
   const int R = R_, D = 2 * R + 1;
   std::vector<float> k;
   psf.Kernel(p, R, k);
   std::vector<float> img(static_cast<size_t>(nx) * ny, 0.0f);
   for (int dy = -R; dy <= R; ++dy)
      for (int dx = -R; dx <= R; ++dx)
      {
         const size_t at = static_cast<size_t>((dy + static_cast<int>(ny)) % static_cast<int>(ny)) * nx +
                           static_cast<size_t>((dx + static_cast<int>(nx)) % static_cast<int>(nx));
         img[at] = k[static_cast<size_t>(dy + R) * D + (dx + R)];
      }
   ks.spec[0].resize(fft_[0].SpecSize());
   fft_[0].Forward(img.data(), nx, ny, nx, ks.spec[0].data());
   ks.level = 0;
   for (int L = 1; L < levels_; ++L)
   {
      if (BandError(ks.spec[0], nx, ny, 1u << L) > bandEpsilon_)
         break;
      ks.level = L;
   }
   // Coarse levels: the fine spectrum restricted to the coarse band (coarse
   // Nyquist bins dropped), times the phase of the coarse cells' centres
   // ((f - 1) / 2 fine cells from the fine ones) over the cloud-in-cell
   // transfer function.
   for (int L = 1; L <= ks.level; ++L)
   {
      const unsigned f = 1u << L, ncx = nx / f, ncy = ny / f, Wc = ncx / 2 + 1;
      const double delta = (f - 1) / 2.0;
      std::vector<cfloat>& out = ks.spec[L];
      out.assign(static_cast<size_t>(Wc) * ncy, cfloat(0.0f, 0.0f));
      for (unsigned cy = 0; cy < ncy; ++cy)
      {
         if (cy == ncy / 2)
            continue;
         const long kys = cy < ncy / 2 ? static_cast<long>(cy) : static_cast<long>(cy) - static_cast<long>(ncy);
         const unsigned fy = static_cast<unsigned>(kys >= 0 ? kys : kys + static_cast<long>(ny));
         const double wy = Sinc(kPi * kys / ncy);
         for (unsigned cx = 0; cx + 1 < Wc; ++cx)
         {
            const double wx = Sinc(kPi * static_cast<double>(cx) / ncx);
            const double a = -2.0 * kPi * (cx * delta / nx + kys * delta / ny);
            const std::complex<double> ph = std::polar(1.0 / (wx * wx * wy * wy), a);
            const std::complex<double> v = std::complex<double>(ks.spec[0][static_cast<size_t>(fy) * W + cx]) * ph;
            out[static_cast<size_t>(cy) * Wc + cx] = cfloat(static_cast<float>(v.real()), static_cast<float>(v.imag()));
         }
      }
   }
}

WidefieldScene::FocusPlan WidefieldScene::PlanFocus(const WidefieldPsf& psf, double focusWorldUm)
{
   FocusPlan plan;
   const WidefieldGridSpec& g = rect_;
   const double dz = g.zPlaneUm;
   const long kLo = SnapFloor(g.zMinUm / dz), kHi = -SnapFloor(-g.zMaxUm / dz);
   std::vector<int> missing;
   auto want = [&](int p) {
      if (!kernels_.count(p) && std::find(missing.begin(), missing.end(), p) == missing.end())
         missing.push_back(p);
   };
   struct Raw { long k; int p0; double frac; };
   std::vector<Raw> raw;
   for (auto it = dyes_.planes.lower_bound(kLo); it != dyes_.planes.end() && it->first < kHi; ++it)
   {
      const long k = it->first;
      double t = psf.PlaneCoord((k + 0.5) * dz - focusWorldUm);
      if (t < psf.MinPlane() || t > psf.MaxPlane())
      {
         for (int pop = 0; pop < 2; ++pop)
            for (float c : it->second.count[pop])
               plan.clamped += static_cast<long>(c);
         t = std::min<double>(psf.MaxPlane(), std::max<double>(psf.MinPlane(), t));
      }
      int p0 = static_cast<int>(std::floor(t));
      double frac = t - p0;
      if (p0 >= psf.MaxPlane())
      {
         p0 = psf.MaxPlane();
         frac = 0.0;
      }
      raw.push_back({k, p0, frac});
      want(p0);
      if (frac > 0.0)
         want(p0 + 1);
   }
   if (!missing.empty())
   {
      std::vector<KernelSpec> made(missing.size());
      ParallelFor(static_cast<unsigned>(missing.size()), [&](unsigned i) { MakeKernel(psf, missing[i], made[i]); });
      for (size_t i = 0; i < missing.size(); ++i)
         kernels_[missing[i]] = std::move(made[i]);
   }
   for (const Raw& r : raw)
   {
      FocusPlan::Dep d;
      d.k = r.k;
      d.p0 = r.p0;
      d.w0 = static_cast<float>(1.0 - r.frac);
      d.w1 = static_cast<float>(r.frac);
      d.level = kernels_[r.p0].level;
      if (r.frac > 0.0)
         d.level = std::min(d.level, kernels_[r.p0 + 1].level);
      plan.perLevel[d.level]++;
      plan.deps.push_back(d);
   }
   return plan;
}

void WidefieldScene::PlaneSpectrum(const Channel& c, const WidefieldSparsePlane& pl, int level,
                                   std::vector<cfloat>& out) const
{
   const WidefieldGridSpec& g = rect_;
   const std::vector<uint32_t>& cells = pl.cell[c.pop];
   const std::vector<float>& counts = pl.count[c.pop];
   bool any = false;
   for (size_t i = 0; i < cells.size() && !any; ++i)
      any = counts[i] * c.weight[cells[i]] != 0.0f;
   if (!any)
   {
      out.clear(); // nothing to add
      return;
   }
   const RealFft2d& fft = fft_[level];
   out.resize(fft.SpecSize());
   if (level == 0)
   {
      std::vector<float> buf(static_cast<size_t>(g.nx) * g.ny, 0.0f);
      for (size_t i = 0; i < cells.size(); ++i)
         buf[cells[i]] = counts[i] * c.weight[cells[i]];
      fft.Forward(buf.data(), g.nx, g.ny, g.nx, out.data());
      return;
   }
   // Cloud-in-cell onto the coarse cells (centres (f - 1) / 2 fine cells
   // from their first fine cell); cell -1 wraps (the padding is empty).
   const unsigned f = 1u << level, ncx = fft.Nx(), ncy = fft.Ny();
   const double delta = (f - 1) / 2.0;
   std::vector<float> buf(static_cast<size_t>(ncx) * ncy, 0.0f);
   for (size_t i = 0; i < cells.size(); ++i)
   {
      const float v = counts[i] * c.weight[cells[i]];
      if (v == 0.0f)
         continue;
      const double ux = (static_cast<double>(cells[i] % g.nx) - delta) / f;
      const double uy = (static_cast<double>(cells[i] / g.nx) - delta) / f;
      const long cx = static_cast<long>(std::floor(ux)), cy = static_cast<long>(std::floor(uy));
      const float fx = static_cast<float>(ux - cx), fy = static_cast<float>(uy - cy);
      const size_t x0 = static_cast<size_t>((cx + static_cast<long>(ncx)) % static_cast<long>(ncx));
      const size_t x1 = static_cast<size_t>((cx + 1 + static_cast<long>(ncx)) % static_cast<long>(ncx));
      const size_t y0 = static_cast<size_t>((cy + static_cast<long>(ncy)) % static_cast<long>(ncy));
      const size_t y1 = static_cast<size_t>((cy + 1 + static_cast<long>(ncy)) % static_cast<long>(ncy));
      buf[y0 * ncx + x0] += v * (1.0f - fx) * (1.0f - fy);
      buf[y0 * ncx + x1] += v * fx * (1.0f - fy);
      buf[y1 * ncx + x0] += v * (1.0f - fx) * fy;
      buf[y1 * ncx + x1] += v * fx * fy;
   }
   fft.Forward(buf.data(), ncx, ncy, ncx, out.data());
}

const std::vector<cfloat>* WidefieldScene::CachedSpectrum(const Channel& c, long k, int level) const
{
   auto it = cache_.find(std::make_tuple(c.version, k, level));
   return it == cache_.end() ? nullptr : &it->second.spec;
}

void WidefieldScene::FillSpectra(const std::vector<const Channel*>& chans, const std::vector<const FocusPlan*>& plans)
{
   const unsigned long long stamp = ++cacheClock_;
   struct Job { const Channel* c; long k; int level; };
   std::vector<Job> jobs;
   for (const Channel* c : chans)
      for (const FocusPlan* plan : plans)
         for (const FocusPlan::Dep& d : plan->deps)
         {
            const auto& pl = dyes_.planes.at(d.k);
            if (pl.Empty(c->pop))
               continue;
            auto key = std::make_tuple(c->version, d.k, d.level);
            auto it = cache_.find(key);
            if (it != cache_.end())
            {
               it->second.used = stamp;
               continue;
            }
            bool dup = false;
            for (const Job& j : jobs)
               dup = dup || (j.c == c && j.k == d.k && j.level == d.level);
            if (!dup)
               jobs.push_back({c, d.k, d.level});
         }
   if (jobs.empty())
      return;
   std::vector<std::vector<cfloat>> made(jobs.size());
   ParallelFor(static_cast<unsigned>(jobs.size()), [&](unsigned i) {
      PlaneSpectrum(*jobs[i].c, dyes_.planes.at(jobs[i].k), jobs[i].level, made[i]);
   });
   for (size_t i = 0; i < jobs.size(); ++i)
   {
      CacheEntry& e = cache_[std::make_tuple(jobs[i].c->version, jobs[i].k, jobs[i].level)];
      cacheBytes_ += made[i].size() * sizeof(cfloat);
      e.spec.swap(made[i]);
      e.used = stamp;
   }
   // Least recently used first, never what these plans use.
   while (cacheBytes_ > cacheBudget_)
   {
      auto victim = cache_.end();
      for (auto it = cache_.begin(); it != cache_.end(); ++it)
         if (it->second.used < stamp && (victim == cache_.end() || it->second.used < victim->second.used))
            victim = it;
      if (victim == cache_.end())
         break;
      cacheBytes_ -= victim->second.spec.size() * sizeof(cfloat);
      cache_.erase(victim);
   }
}

void WidefieldScene::ChannelImage(const Channel& c, const FocusPlan& plan, std::vector<float>& img) const
{
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   const unsigned cw = spec_.width * u, ch = spec_.height * u;
   img.assign(static_cast<size_t>(cw) * ch, 0.0f);
   std::vector<cfloat> S[kLevels];
   bool any = false;
   for (int L = 0; L < levels_; ++L)
   {
      std::vector<const FocusPlan::Dep*> deps;
      std::vector<const std::vector<cfloat>*> specs;
      for (const FocusPlan::Dep& d : plan.deps)
      {
         if (d.level != L)
            continue;
         const std::vector<cfloat>* a = CachedSpectrum(c, d.k, L);
         if (!a || a->empty())
            continue;
         deps.push_back(&d);
         specs.push_back(a);
      }
      if (deps.empty())
         continue;
      any = true;
      const RealFft2d& fft = fft_[L];
      const unsigned W = fft.SpecW(), rows = fft.Ny();
      S[L].assign(fft.SpecSize(), cfloat(0.0f, 0.0f));
      // Rows of the spectrum in parallel; planes in plan order per bin.
      const unsigned B = 8;
      ParallelFor((rows + B - 1) / B, [&](unsigned blk) {
         const unsigned r0 = blk * B, r1 = std::min(rows, r0 + B);
         const size_t b0 = static_cast<size_t>(r0) * W, b1 = static_cast<size_t>(r1) * W;
         float* s = reinterpret_cast<float*>(S[L].data());
         for (size_t j = 0; j < deps.size(); ++j)
         {
            const FocusPlan::Dep& d = *deps[j];
            const float* a = reinterpret_cast<const float*>(specs[j]->data());
            const float* k0 = reinterpret_cast<const float*>(kernels_.at(d.p0).spec[L].data());
            const float* k1 = d.w1 > 0.0f ? reinterpret_cast<const float*>(kernels_.at(d.p0 + 1).spec[L].data()) : nullptr;
            const float w0 = d.w0, w1 = d.w1;
            for (size_t i = b0; i < b1; ++i)
            {
               float kr = w0 * k0[2 * i], ki = w0 * k0[2 * i + 1];
               if (k1)
               {
                  kr += w1 * k1[2 * i];
                  ki += w1 * k1[2 * i + 1];
               }
               const float ar = a[2 * i], ai = a[2 * i + 1];
               s[2 * i] += ar * kr - ai * ki;
               s[2 * i + 1] += ar * ki + ai * kr;
            }
         }
      });
   }
   if (!any)
      return;
   const unsigned nx = nx_, ny = ny_, W = nx / 2 + 1;
   if (S[0].empty())
      S[0].assign(fft_[0].SpecSize(), cfloat(0.0f, 0.0f));
   // Embed the coarse bands.
   for (int L = 1; L < levels_; ++L)
   {
      if (S[L].empty())
         continue;
      const unsigned ncy = fft_[L].Ny(), Wc = fft_[L].SpecW();
      for (unsigned cy = 0; cy < ncy; ++cy)
      {
         const long kys = cy < ncy / 2 ? static_cast<long>(cy) : static_cast<long>(cy) - static_cast<long>(ncy);
         const unsigned fy = static_cast<unsigned>(kys >= 0 ? kys : kys + static_cast<long>(ny));
         for (unsigned cx = 0; cx < Wc; ++cx)
            S[0][static_cast<size_t>(fy) * W + cx] += S[L][static_cast<size_t>(cy) * Wc + cx];
      }
   }
   // Sub-cell shift to the camera: image(j + frac) = spectrum x
   // exp(+2 pi i k frac / n) (Nyquist bins: the real part, so the image
   // stays real).
   if (fracX_ != 0.0 || fracY_ != 0.0)
   {
      std::vector<std::complex<double>> px(W), py(ny);
      for (unsigned kx = 0; kx < W; ++kx)
      {
         const double a = 2.0 * kPi * kx * fracX_ / nx;
         px[kx] = (kx == nx / 2) ? std::complex<double>(std::cos(a), 0.0) : std::polar(1.0, a);
      }
      for (unsigned ky = 0; ky < ny; ++ky)
      {
         const long kys = ky <= ny / 2 ? static_cast<long>(ky) : static_cast<long>(ky) - static_cast<long>(ny);
         const double a = 2.0 * kPi * kys * fracY_ / ny;
         py[ky] = (ky == ny / 2) ? std::complex<double>(std::cos(a), 0.0) : std::polar(1.0, a);
      }
      for (unsigned ky = 0; ky < ny; ++ky)
         for (unsigned kx = 0; kx < W; ++kx)
         {
            cfloat& v = S[0][static_cast<size_t>(ky) * W + kx];
            const std::complex<double> r = std::complex<double>(v) * (px[kx] * py[ky]);
            v = cfloat(static_cast<float>(r.real()), static_cast<float>(r.imag()));
         }
   }
   fft_[0].Inverse(S[0].data(), img.data(), fovY0_, ch, fovX0_, cw, cw);
}

void WidefieldScene::ImagesFor(const FocusPlan& plan, WidefieldImages& out) const
{
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   out.cw = spec_.width * u;
   out.ch = spec_.height * u;
   out.upscale = u;
   out.persistent.clear();
   out.bleach.clear();
   if (dyes_.nPersistent > 0)
      ChannelImage(persistent_, plan, out.persistent);
   if (bleachValid_)
   {
      out.bleach.resize(bleach_.size());
      for (size_t j = 0; j < bleach_.size(); ++j)
         ChannelImage(bleach_[j], plan, out.bleach[j]);
   }
}

std::vector<const WidefieldScene::Channel*> WidefieldScene::ActiveChannels() const
{
   std::vector<const Channel*> chans;
   if (dyes_.nPersistent > 0)
      chans.push_back(&persistent_);
   if (bleachValid_)
      for (const Channel& c : bleach_)
         chans.push_back(&c);
   return chans;
}

void WidefieldScene::Refocus(const WidefieldPsf& psf)
{
   plan_ = PlanFocus(psf, spec_.focusWorldUm);
   clamped_ = plan_.clamped;
   for (int L = 0; L < kLevels; ++L)
      planesPerLevel_[L] = plan_.perLevel[L];
   if (defer_)
   {
      images_ = WidefieldImages();
      return;
   }
   if (accel_)
   {
      std::vector<std::vector<float>> imgs;
      if (AccelImages(plan_, ActiveChannels(), imgs))
      {
         SetImages(imgs);
         return;
      }
   }
   ComputeCpuImages();
}

void WidefieldScene::ComputeCpuImages()
{
   FillSpectra(ActiveChannels(), {&plan_});
   ImagesFor(plan_, images_);
}

bool WidefieldScene::JobFor(const FocusPlan& plan, const std::vector<const Channel*>& chans,
                            WidefieldGpuJob& job) const
{
   job = WidefieldGpuJob();
   if (!gpuMode_ || levels_ != 1 || nx_ > WidefieldGpuJob::kMaxGpuFft || ny_ > WidefieldGpuJob::kMaxGpuFft ||
       (nx_ & (nx_ - 1)) != 0 || (ny_ & (ny_ - 1)) != 0)
      return false;
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   job.NX = nx_;
   job.NY = ny_;
   job.nx = rect_.nx;
   job.ny = rect_.ny;
   job.fovX0 = fovX0_;
   job.fovY0 = fovY0_;
   job.cw = spec_.width * u;
   job.ch = spec_.height * u;
   job.fracX = fracX_;
   job.fracY = fracY_;
   job.geometry = geometry_;
   job.hasPersistent = !chans.empty() && chans[0] == &persistent_;
   std::vector<int> ps;
   auto key = [](const Channel& c, long k) {
      return (static_cast<unsigned long long>(c.version) << 32) ^ static_cast<uint32_t>(k + 0x40000000L);
   };
   // Planes whose weighted dyes are all zero (unlit) have no spectrum: no dep.
   std::map<unsigned long long, bool> planeSeen; // key -> has values
   for (const Channel* c : chans)
   {
      std::vector<WidefieldGpuJob::Dep> deps;
      for (const FocusPlan::Dep& d : plan.deps)
      {
         const WidefieldSparsePlane& pl = dyes_.planes.at(d.k);
         if (pl.Empty(c->pop))
            continue;
         const unsigned long long kk = key(*c, d.k);
         auto seen = planeSeen.find(kk);
         if (seen == planeSeen.end())
         {
            bool any = accel_ && accel_->HasPlane(geometry_, kk);
            if (!any)
            {
               WidefieldGpuJob::Plane jp;
               jp.key = kk;
               const std::vector<uint32_t>& cells = pl.cell[c->pop];
               const std::vector<float>& counts = pl.count[c->pop];
               for (size_t i = 0; i < cells.size(); ++i)
               {
                  const float v = counts[i] * c->weight[cells[i]];
                  if (v == 0.0f)
                     continue;
                  jp.cells.push_back(cells[i]);
                  jp.values.push_back(v);
                  jp.absSum += std::fabs(v);
               }
               any = !jp.cells.empty();
               if (any)
                  job.planes.push_back(std::move(jp));
            }
            seen = planeSeen.insert({kk, any}).first;
         }
         if (!seen->second)
            continue;
         const bool two = d.w1 > 0.0f;
         deps.push_back({kk, d.p0, two ? d.p0 + 1 : d.p0, d.w0, two ? d.w1 : 0.0f, two});
         for (int p : {d.p0, d.p0 + 1})
            if ((p == d.p0 || two) && std::find(ps.begin(), ps.end(), p) == ps.end())
               ps.push_back(p);
      }
      job.channels.push_back(std::move(deps));
   }
   std::sort(ps.begin(), ps.end());
   for (int p : ps)
      job.kernels.push_back({p, &kernels_.at(p).spec[0]});
   return true;
}

bool WidefieldScene::MakeGpuJob(WidefieldGpuJob& job) const
{
   return JobFor(plan_, ActiveChannels(), job);
}

bool WidefieldScene::AccelImages(const FocusPlan& plan, const std::vector<const Channel*>& chans,
                                 std::vector<std::vector<float>>& out)
{
   WidefieldGpuJob job;
   if (!JobFor(plan, chans, job))
   {
      gpuError_ = "WideField GPU: this scene cannot run on the GPU (FFT " + std::to_string(nx_) + "x" +
                  std::to_string(ny_) + ")";
      accel_ = nullptr;
      return false;
   }
   std::string err;
   if (!accel_->Images(job, out, err) || out.size() != job.channels.size())
   {
      gpuError_ = "WideField GPU failed: " + err;
      accel_ = nullptr;
      return false;
   }
   return true;
}

bool WidefieldScene::SetImages(std::vector<std::vector<float>>& imgs)
{
   const std::vector<const Channel*> chans = ActiveChannels();
   if (imgs.size() != chans.size())
      return false;
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   images_.cw = spec_.width * u;
   images_.ch = spec_.height * u;
   images_.upscale = u;
   images_.persistent.clear();
   images_.bleach.clear();
   size_t i = 0;
   if (!chans.empty() && chans[0] == &persistent_)
      images_.persistent.swap(imgs[i++]);
   for (; i < imgs.size(); ++i)
      images_.bleach.push_back(std::move(imgs[i]));
   return true;
}

// ---- Bleaching -----------------------------------------------------------------

void WidefieldScene::FreshBleachWeights(double framesBefore, std::vector<float>& wb) const
{
   wb.resize(dD_.size());
   const double B = spec_.phot.photonBudget;
   for (size_t i = 0; i < dD_.size(); ++i)
      wb[i] = static_cast<float>(WidefieldBleachingPhotons(spec_.eta, B, framesBefore * dD_[i], dD_[i]));
}

void WidefieldScene::BleachWeightsFromDose(const std::vector<float>& d0, std::vector<float>& wb) const
{
   wb.resize(dD_.size());
   const double B = spec_.phot.photonBudget;
   for (size_t i = 0; i < dD_.size(); ++i)
      wb[i] = static_cast<float>(WidefieldBleachingPhotons(spec_.eta, B, i < d0.size() ? d0[i] : 0.0f, dD_[i]));
}

namespace {

// Chebyshev coefficients (y in [-1, 1]) of exp(-tau (y + 1) / 2), n terms,
// by interpolation at the Chebyshev nodes.
std::vector<double> ChebExp(double tau, unsigned n)
{
   std::vector<double> c(n, 0.0);
   for (unsigned j = 0; j < n; ++j)
   {
      double s = 0.0;
      for (unsigned k = 0; k < n; ++k)
      {
         const double th = kPi * (k + 0.5) / n;
         s += std::exp(-tau * (std::cos(th) + 1.0) / 2.0) * std::cos(j * th);
      }
      c[j] = (j == 0 ? 1.0 : 2.0) * s / n;
   }
   return c;
}

} // namespace

void WidefieldScene::BuildBleachChannels(const std::vector<float>& wb)
{
   for (Channel& c : bleach_)
      DropChannel(c);
   bleach_.clear();
   groupDose_.clear();
   cheb_ = false;
   doseMax_ = 0.0f;
   wbRef_ = wb;
   if (dyes_.nBleaching == 0)
      return;
   const size_t n = wb.size();
   std::vector<float> doses;
   for (size_t i = 0; i < n; ++i)
   {
      if (wb[i] == 0.0f)
         continue;
      doseMax_ = std::max(doseMax_, dD_[i]);
      if (doses.size() <= kMaxGroups && std::find(doses.begin(), doses.end(), dD_[i]) == doses.end())
         doses.push_back(dD_[i]);
   }
   if (doses.size() <= kMaxGroups)
   {
      std::sort(doses.begin(), doses.end());
      groupDose_ = doses;
      for (float g : doses)
      {
         Channel c;
         c.pop = 0;
         c.weight.assign(n, 0.0f);
         for (size_t i = 0; i < n; ++i)
            if (wb[i] != 0.0f && dD_[i] == g)
               c.weight[i] = wb[i];
         c.version = NextSerial();
         bleach_.push_back(std::move(c));
      }
      return;
   }
   cheb_ = true;
   for (unsigned j = 0; j < kChebTerms; ++j)
   {
      Channel c;
      c.pop = 0;
      c.weight.assign(n, 0.0f);
      c.version = NextSerial();
      bleach_.push_back(std::move(c));
   }
   for (size_t i = 0; i < n; ++i)
   {
      if (wb[i] == 0.0f)
         continue;
      const double y = 2.0 * dD_[i] / doseMax_ - 1.0;
      double t0 = 1.0, t1 = y;
      for (unsigned j = 0; j < kChebTerms; ++j)
      {
         const double tj = j == 0 ? t0 : (j == 1 ? t1 : 0.0);
         double v = tj;
         if (j >= 2)
         {
            v = 2.0 * y * t1 - t0;
            t0 = t1;
            t1 = v;
         }
         bleach_[j].weight[i] = static_cast<float>(wb[i] * v);
      }
   }
}

void WidefieldScene::SetBleachWeights(const std::vector<float>& wb)
{
   ++imagesVersion_;
   BuildBleachChannels(wb);
   bleachValid_ = true;
   std::vector<const Channel*> chans;
   for (const Channel& c : bleach_)
      chans.push_back(&c);
   if (!haveSpec_ || !psf_ || defer_)
      return;
   if (accel_)
   {
      std::vector<std::vector<float>> imgs;
      if (AccelImages(plan_, chans, imgs))
      {
         images_.bleach = std::move(imgs);
         return;
      }
   }
   FillSpectra(chans, {&plan_});
   images_.bleach.resize(bleach_.size());
   for (size_t j = 0; j < bleach_.size(); ++j)
      ChannelImage(bleach_[j], plan_, images_.bleach[j]);
}

bool WidefieldScene::BleachCoefficients(const std::vector<float>& wb, std::vector<double>& a) const
{
   a.clear();
   if (!bleachValid_ || wb.size() != wbRef_.size())
      return false;
   if (bleach_.empty())
      return true; // no bleaching dyes: the weights draw nothing
   const size_t n = wb.size();
   a.assign(bleach_.size(), 0.0);
   if (!cheb_)
   {
      for (size_t g = 0; g < bleach_.size(); ++g)
      {
         const std::vector<float>& phi = bleach_[g].weight;
         size_t iMax = 0;
         for (size_t i = 1; i < n; ++i)
            if (std::fabs(phi[i]) > std::fabs(phi[iMax]))
               iMax = i;
         a[g] = phi[iMax] != 0.0f ? static_cast<double>(wb[iMax]) / phi[iMax] : 0.0;
      }
   }
   else
   {
      // exp(-tau x), x = dD / doseMax, tau from a column at x = 1.
      size_t iRef = n;
      for (size_t i = 0; i < n; ++i)
         if (wbRef_[i] != 0.0f && dD_[i] == doseMax_ && (iRef == n || wbRef_[i] > wbRef_[iRef]))
            iRef = i;
      if (iRef == n)
         return false;
      const double r = static_cast<double>(wb[iRef]) / wbRef_[iRef];
      if (!(r > 0.0))
         return false;
      a = ChebExp(-std::log(r), kChebTerms);
   }
   float wMax = 0.0f;
   for (float v : wb)
      wMax = std::max(wMax, std::fabs(v));
   const double tol = 1e-5 * std::max(static_cast<double>(wMax), 1e-30);
   for (size_t i = 0; i < n; ++i)
   {
      double v = 0.0;
      for (size_t j = 0; j < bleach_.size(); ++j)
         v += a[j] * bleach_[j].weight[i];
      if (std::fabs(wb[i] - v) > tol)
         return false;
   }
   return true;
}

bool WidefieldScene::ScalarOfBleaching(const std::vector<float>& wb, double& c) const
{
   std::vector<double> a;
   if (!BleachCoefficients(wb, a) || a.size() > 1)
      return false;
   c = a.empty() ? 0.0 : a[0];
   return true;
}

std::vector<double> WidefieldScene::AnchorCoefficients() const
{
   std::vector<double> a(bleach_.size(), cheb_ ? 0.0 : 1.0);
   if (cheb_ && !a.empty())
      a[0] = 1.0;
   return a;
}

void WidefieldScene::RenderFrame(const std::vector<float>& wb, std::vector<float>& cam)
{
   std::vector<double> a;
   lastFast_ = true;
   if (!BleachCoefficients(wb, a))
   {
      SetBleachWeights(wb);
      lastFast_ = false;
      a = AnchorCoefficients();
   }
   images_.Render(a, cam);
}

bool WidefieldScene::AdoptFocus(double focusWorldUm, const WidefieldImages& images, unsigned long long version)
{
   if (!haveSpec_ || !psf_ || version != imagesVersion_)
      return false;
   if (spec_.slabHalfUm > 0.0)
   {
      const double shift = focusWorldUm - spec_.focusWorldUm;
      spec_.slabCentreUm += shift;
      rect_.zMinUm += shift;
      rect_.zMaxUm += shift;
   }
   spec_.focusWorldUm = focusWorldUm;
   plan_ = PlanFocus(*psf_, focusWorldUm);
   clamped_ = plan_.clamped;
   for (int L = 0; L < kLevels; ++L)
      planesPerLevel_[L] = plan_.perLevel[L];
   images_ = images;
   return true;
}

bool WidefieldScene::FocusSeries(const std::vector<double>& focusWorldUm, std::vector<WidefieldImages>& out,
                                 std::string& err)
{
   out.clear();
   if (!haveSpec_ || !psf_)
   {
      err = "WideField: scene not set up";
      return false;
   }
   // The slab follows the focus when it is limited, as for Update.
   const WidefieldGridSpec keep = rect_;
   std::vector<FocusPlan> plans(focusWorldUm.size());
   for (size_t i = 0; i < focusWorldUm.size(); ++i)
   {
      if (spec_.slabHalfUm > 0.0)
      {
         const double shift = focusWorldUm[i] - spec_.focusWorldUm;
         rect_.zMinUm = keep.zMinUm + shift;
         rect_.zMaxUm = keep.zMaxUm + shift;
      }
      plans[i] = PlanFocus(*psf_, focusWorldUm[i]);
   }
   rect_ = keep;
   out.resize(plans.size());
   if (accel_)
   {
      // One job per focus; the plane spectra stay resident between them.
      const std::vector<const Channel*> chans = ActiveChannels();
      bool ok = true;
      for (size_t i = 0; i < plans.size() && ok; ++i)
      {
         std::vector<std::vector<float>> imgs;
         ok = AccelImages(plans[i], chans, imgs);
         if (ok)
         {
            const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
            WidefieldImages& o = out[i];
            o.cw = spec_.width * u;
            o.ch = spec_.height * u;
            o.upscale = u;
            size_t j = 0;
            if (!chans.empty() && chans[0] == &persistent_)
               o.persistent.swap(imgs[j++]);
            for (; j < imgs.size(); ++j)
               o.bleach.push_back(std::move(imgs[j]));
         }
      }
      if (ok)
         return true;
   }
   std::vector<const FocusPlan*> pp;
   for (const FocusPlan& p : plans)
      pp.push_back(&p);
   FillSpectra(ActiveChannels(), pp);
   ParallelFor(static_cast<unsigned>(plans.size()), [&](unsigned i) { ImagesFor(plans[i], out[i]); });
   return true;
}

} // namespace sim
