#include "insilicell/insilicell.h"

#include "packing.h"
#include "params.h"
#include "rng.h"
#include "world.h"

#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

struct IscParams { isc::Params p; };
struct IscWorld { isc::World w; std::vector<isc::WorldDye> scratch; };

extern "C" {

int32_t isc_abi_version(void) { return ISC_ABI_VERSION; }

void isc_pcg4d(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t out[4])
{
   const isc::Pcg4dOut r = isc::Pcg4d(a, b, c, d);
   out[0] = r.a; out[1] = r.b; out[2] = r.c; out[3] = r.d;
}

double isc_hash_unit(uint32_t seed, int32_t cx, int32_t cy, uint32_t k)
{
   return isc::HashUnit(seed, cx, cy, k);
}

IscParams* isc_params_new(void) { return new (std::nothrow) IscParams(); }
void isc_params_free(IscParams* p) { delete p; }

int32_t isc_params_set(IscParams* p, const char* name, double value)
{
   if (!p || !name) return -1;
   return isc::SetParam(p->p, name, value) ? 0 : -1;
}

int32_t isc_pack_window(uint32_t seed, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
                        const IscParams* ip, double* out, int32_t cap, int32_t* removed)
{
   if (!ip || cx1 < cx0 || cy1 < cy0 || (cap > 0 && !out)) return -1;
   try {
      isc::Params p = ip->p;
      isc::NormalizeParams(p);
      isc::CandidateMap m = isc::BuildCandidateMap(seed, cx0, cy0, cx1, cy1, p);
      const int r = isc::PackMap(m, p);
      if (removed) *removed = r;
      int32_t n = 0;
      for (size_t i = 0; i < m.cells.size(); i++) {
         if (!m.alive[i]) continue;
         if (n < cap) {
            const isc::Cell& c = m.cells[i];
            double* o = out + (size_t)n * ISC_PACKED_STRIDE;
            o[0] = c.cx; o[1] = c.cy; o[2] = c.x; o[3] = c.y; o[4] = c.packRot;
         }
         n++;
      }
      return n;
   } catch (...) {
      return -1;
   }
}

IscWorld* isc_world_new(uint32_t seed, const IscParams* p)
{
   if (!p) return nullptr;
   try {
      return new IscWorld{ isc::World(seed, p->p), {} };
   } catch (...) {
      return nullptr;
   }
}

void isc_world_free(IscWorld* w) { delete w; }

namespace {
bool BadRect(double x0, double y0, double x1, double y1)
{
   return !(x1 > x0) || !(y1 > y0) || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1);
}
} // namespace

int32_t isc_cells_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::Cell> cells;
      w->w.CellsInRect(x0, y0, x1, y1, cells);
      for (size_t i = 0; i < cells.size() && (int32_t)i < cap; i++) {
         const isc::Cell& c = cells[i];
         double* o = out + i * ISC_CELL_STRIDE;
         o[0] = c.cx; o[1] = c.cy; o[2] = c.x; o[3] = c.y; o[4] = c.packRot; o[5] = c.rOuter;
      }
      return (int32_t)cells.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_sites_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                            double zMin, double zMax, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::WorldDye>& d = w->scratch;
      d.clear();
      w->w.SitesInWindow(x0, y0, x1, y1, zMin, zMax, d);
      if (d.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < d.size() && (int32_t)i < cap; i++) {
         double* o = out + i * ISC_SITE_STRIDE;
         o[0] = d[i].x; o[1] = d[i].y; o[2] = d[i].z; o[3] = d[i].id;
      }
      return (int32_t)d.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_density_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                              double zMin, double zMax, int32_t nx, int32_t ny, float* out)
{
   if (!w || BadRect(x0, y0, x1, y1) || nx <= 0 || ny <= 0 || !out) return -1;
   try {
      const long n = w->w.DensityInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, out);
      return n > INT32_MAX ? -1 : (int32_t)n;
   } catch (...) {
      return -1;
   }
}

} // extern "C"
