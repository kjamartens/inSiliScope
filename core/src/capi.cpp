#include "insiliscope/insiliscope.h"

#include "cytomesh.h"
#include "packing.h"
#include "params.h"
#include "rng.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

struct IscParams { isc::Params p; };
struct IscWorld { isc::World w; std::vector<isc::WorldDye> scratch; std::vector<isc::WorldEvent> events; };

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
      return new IscWorld{ isc::World(seed, p->p), {}, {} };
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
         o[6] = c.height; o[7] = c.nucOffX; o[8] = c.nucOffY; o[9] = c.nucRot;
         o[10] = c.nucLong; o[11] = c.nucShort; o[12] = c.nucHeight; o[13] = c.nucZ;
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

int32_t isc_world_set_dye_cache(IscWorld* w, double maxDyes)
{
   if (!w || !(maxDyes >= 0) || !std::isfinite(maxDyes)) return -1;
   w->w.SetDyeCacheCap((size_t)std::min(maxDyes, 1e15));
   return 0;
}

int32_t isc_world_prefetch(IscWorld* w, double x0, double y0, double x1, double y1, double zMin, double zMax,
                           double t0, double t1, double budgetMs)
{
   if (!w || BadRect(x0, y0, x1, y1) || !(t1 >= t0) || !(budgetMs >= 0)) return -1;
   try {
      return w->w.Prefetch(x0, y0, x1, y1, zMin, zMax, t0, t1, budgetMs) ? 1 : 0;
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_set_kinetics(IscWorld* w, double activationRatePerSec, double onSec, double offSec,
                               double bleachProb, double photonCV)
{
   if (!w || !(activationRatePerSec >= 0) || !(onSec > 0) || !(offSec >= 0) || !(bleachProb > 0) || !(photonCV >= 0) ||
       !std::isfinite(activationRatePerSec) || !std::isfinite(onSec) || !std::isfinite(offSec) || !std::isfinite(photonCV))
      return -1;
   isc::Kinetics k;
   k.activationRatePerSec = activationRatePerSec; k.onSec = onSec; k.offSec = offSec;
   k.bleachProb = bleachProb; k.photonCV = photonCV;
   w->w.SetKinetics(k);
   return 0;
}

int32_t isc_events_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                             double zMin, double zMax, double t0, double t1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || !(t1 >= t0) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::WorldEvent>& e = w->events;
      e.clear();
      w->w.EventsInWindow(x0, y0, x1, y1, zMin, zMax, t0, t1, e);
      if (e.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < e.size() && (int32_t)i < cap; i++) {
         double* o = out + i * ISC_EVENT_STRIDE;
         o[0] = e[i].x; o[1] = e[i].y; o[2] = e[i].z;
         o[3] = e[i].tOn; o[4] = e[i].tOff; o[5] = e[i].brightness; o[6] = e[i].id;
      }
      return (int32_t)e.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_cell_outline(IscWorld* w, int32_t cx, int32_t cy, double* out, int32_t capPts)
{
   if (!w || (capPts > 0 && !out)) return -1;
   try {
      isc::Cell c;
      if (!w->w.FindCell(cx, cy, c)) return -1;
      const isc::Params& p = w->w.GetParams();
      const std::vector<isc::Pt2> o = isc::CellOutlineLocal(c, (int)std::max(8.0, isc::JsRound(p.cytoTheta)));
      for (size_t i = 0; i < o.size() && (int32_t)i < capPts; i++) { out[2 * i] = o[i].x; out[2 * i + 1] = o[i].y; }
      return (int32_t)o.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_cell_mesh(IscWorld* w, int32_t cx, int32_t cy, int32_t dims[2], double* out, int32_t capVerts)
{
   if (!w || !dims || (capVerts > 0 && !out)) return -1;
   try {
      isc::Cell c;
      if (!w->w.FindCell(cx, cy, c)) return -1;
      const isc::CytoMesh& m = w->w.Assets(c).geom.mesh;
      dims[0] = m.rings; dims[1] = m.n;
      const size_t nv = m.h.size();
      for (size_t v = 0; v < nv && (int32_t)v < capVerts; v++) {
         out[3 * v] = m.x[v]; out[3 * v + 1] = m.y[v]; out[3 * v + 2] = m.h[v];
      }
      return (int32_t)nv;
   } catch (...) {
      return -1;
   }
}

int32_t isc_cell_microtubules(IscWorld* w, int32_t cx, int32_t cy, double* xyz, int32_t capPts,
                              int32_t* lens, int32_t capMts, int32_t* totalPts)
{
   if (!w || (capPts > 0 && !xyz) || (capMts > 0 && !lens)) return -1;
   try {
      isc::Cell c;
      if (!w->w.FindCell(cx, cy, c)) return -1;
      const std::vector<isc::Microtubule>& mts = w->w.Assets(c).mts;
      int32_t o = 0;
      for (size_t i = 0; i < mts.size(); i++) {
         if ((int32_t)i < capMts) lens[i] = (int32_t)mts[i].pts.size();
         for (const isc::Pt3& q : mts[i].pts) {
            if (o < capPts) { xyz[3 * o] = q.x; xyz[3 * o + 1] = q.y; xyz[3 * o + 2] = q.z; }
            o++;
         }
      }
      if (totalPts) *totalPts = o;
      return (int32_t)mts.size();
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

int32_t isc_density3d_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double zMin, double zMax,
                                int32_t nx, int32_t ny, int32_t nz, int32_t populations, float* out)
{
   if (!w || BadRect(x0, y0, x1, y1) || nx <= 0 || ny <= 0 || nz <= 0 || !out || populations < 0 ||
       (populations & ~(ISC_POP_BLEACHING | ISC_POP_PERSISTENT)) != 0 || !(zMax > zMin) ||
       (nz > 1 && (!std::isfinite(zMin) || !std::isfinite(zMax))))
      return -1;
   if ((double)nx * ny * nz > 2.0e9) return -1;
   try {
      const long n = w->w.Density3dInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, nz, (unsigned)populations, out);
      return n > INT32_MAX ? -1 : (int32_t)n;
   } catch (...) {
      return -1;
   }
}

int32_t isc_optical_volume_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double zMin,
                                     double zMax, int32_t nx, int32_t ny, int32_t nz, int32_t sub, float* out)
{
   if (!w || BadRect(x0, y0, x1, y1) || nx <= 0 || ny <= 0 || nz <= 0 || !out || sub < 1 || sub > 16 ||
       !std::isfinite(zMin) || !std::isfinite(zMax) || !(zMax > zMin))
      return -1;
   if (3.0 * nx * ny * nz > 2.0e9) return -1;
   try {
      const long n = w->w.OpticalVolumeInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, nz, sub, out);
      return n > INT32_MAX ? -1 : (int32_t)n;
   } catch (...) {
      return -1;
   }
}

} // extern "C"
