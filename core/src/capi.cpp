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

static_assert(ISC_STRUCT_COUNT == isc::STRUCTURE_COUNT && ISC_STRUCT_MICROTUBULE == isc::STRUCTURE_MT &&
                 ISC_STRUCT_NPC == isc::STRUCTURE_NPC && ISC_NPC_STRIDE == 9 &&
                 ISC_STRUCT_ORIGAMI_SITES == isc::STRUCTURE_ORIGAMI, "structures");
static_assert(ISC_SPECIMEN_CELLFIELD == isc::SPECIMEN_CELLFIELD && ISC_SPECIMEN_NANORULERS == isc::SPECIMEN_NANORULERS,
              "specimens");
static_assert(ISC_STATE_BLINK == isc::STATE_BLINK && ISC_STATE_PRE == isc::STATE_PRE &&
                 ISC_STATE_INITIAL_ON == isc::STATE_INITIAL_ON && ISC_STATE_ALWAYS_ON == isc::STATE_ALWAYS_ON, "states");
static_assert(ISC_MODE_DNA_PAINT == (int)isc::LabelMode::DnaPaint && ISC_MODE_WIDEFIELD == (int)isc::LabelMode::WideField &&
                 ISC_ORIENT_RANDOM == (int)isc::OrientationMode::Random, "modes");

struct IscParams { isc::Params p; };
struct IscWorld { isc::World w; std::vector<isc::WorldDye> scratch; std::vector<isc::WorldEvent> events; };

namespace {
// The clock map's flat buffer (isc_world_set_clock_map). No exceptions (the WASM builds abort on a throw): a short
// buffer or a bad count sets bad, reads 0 (counts their least), and the read gives up.
struct MapReader {
   const double* v;
   size_t n, i = 0;
   bool bad = false;
   double D()
   {
      if (i >= n) {
         bad = true;
         return 0.0;
      }
      return v[i++];
   }
   long Int(long lo, long hi)
   {
      const double d = D();
      if (!(d == std::floor(d) && d >= (double)lo && d <= (double)hi)) {
         bad = true;
         return lo;
      }
      return (long)d;
   }
   long Count(long lo) { return Int(lo, (long)(n - i)); }   // at most one value per item left
   isc::ClockField Field()
   {
      isc::ClockField f((size_t)Count(0));
      for (isc::ClockTerm& t : f) {
         t.grid = (int32_t)Int(-1, INT32_MAX);
         t.depth = (int32_t)Int(-1, INT32_MAX);
         t.ox = D(); t.oy = D(); t.zp = D(); t.sx = D(); t.sy = D(); t.scale = D();
         if (bad) break;
      }
      return f;
   }
};

bool ReadClockMap(const double* v, size_t n, isc::ClockMap& m)
{
   MapReader r{ v, n };
   r.Int(ISC_CLOCK_MAP_FORMAT, ISC_CLOCK_MAP_FORMAT);
   m.grids.resize((size_t)r.Count(0));
   m.depths.resize((size_t)r.Count(0));
   if (r.bad) return false;
   for (isc::ClockGrid& g : m.grids) {
      g.x0 = r.D(); g.y0 = r.D(); g.h = r.D();
      g.nx = (int32_t)r.Count(1);
      g.ny = (int32_t)r.Count(1);
      if (r.bad || (double)g.nx * (double)g.ny > (double)(n - r.i)) return false;
      g.v.assign(v + r.i, v + r.i + (size_t)g.nx * (size_t)g.ny);
      r.i += g.v.size();
   }
   for (isc::ClockDepth& d : m.depths) {
      d.z0 = r.D(); d.dz = r.D();
      const long k = r.Count(0);
      if (r.bad) return false;
      d.v.assign(v + r.i, v + r.i + k);
      r.i += (size_t)k;
   }
   m.s.resize(ISC_STRUCT_COUNT);
   for (isc::StructClock& c : m.s) {
      c.mode = (int)r.Int(ISC_CLOCK_GLOBAL, ISC_CLOCK_ACTIVATION);
      const long ne = r.Count(1);
      for (long e = 0; e < ne && !r.bad; e++) {
         c.hist.tStart.push_back(r.D());
         double k[ISC_KIN_COUNT];
         for (double& x : k) x = r.D();
         isc::Kinetics q;
         q.activationRatePerSec = k[ISC_KIN_ACTIVATION_RATE];
         q.onSec = k[ISC_KIN_ON_SEC];
         q.offSec = k[ISC_KIN_OFF_SEC];
         q.bleachProb = k[ISC_KIN_BLEACH_PROB];
         q.photonCV = k[ISC_KIN_PHOTON_CV];
         q.initialOnSec = k[ISC_KIN_INITIAL_ON_SEC];
         c.hist.kin.push_back(q);
      }
      c.bound.resize((size_t)r.Count(0));
      for (isc::ClockField& b : c.bound) b = r.Field();
      c.total = r.Field();
      c.totalC = r.D();
      c.rate = r.Field();
      c.rateC = r.D();
      c.tOpen = r.D();
      c.steps.resize((size_t)r.Count(0));
      for (isc::ClockStep& st : c.steps) {
         st.t0 = r.D(); st.t1 = r.D(); st.c = r.D();
         st.field = r.Field();
         if (r.bad) break;
      }
      c.bleachTotal = r.Field();
      c.bleachRate = r.Field();
      if (r.bad) return false;
   }
   return !r.bad && r.i == n;   // (no trailing values)
}
} // namespace

extern "C" {

int32_t isc_abi_version(void) { return ISC_ABI_VERSION; }
const char* isc_world_version(void) { return ISC_WORLD_VERSION; }

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

// One ISC_CELL_STRIDE row.
void WriteCellRow(const isc::Cell& c, double* o)
{
   o[0] = c.cx; o[1] = c.cy; o[2] = c.x; o[3] = c.y; o[4] = c.packRot; o[5] = c.rOuter;
   o[6] = c.height; o[7] = c.nucOffX; o[8] = c.nucOffY; o[9] = c.nucRot;
   o[10] = c.nucLong; o[11] = c.nucShort; o[12] = c.nucHeight; o[13] = c.nucZ;
}
} // namespace

int32_t isc_cells_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::Cell> cells;
      w->w.CellsInRect(x0, y0, x1, y1, cells);
      for (size_t i = 0; i < cells.size() && (int32_t)i < cap; i++)
         WriteCellRow(cells[i], out + i * ISC_CELL_STRIDE);
      return (int32_t)cells.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_pack_block(IscWorld* w, int32_t bx, int32_t by, double* out, int32_t cap)
{
   if (!w || (cap > 0 && !out)) return -1;
   try {
      const std::vector<isc::Cell>& cells = w->w.BlockCells(bx, by);
      for (size_t i = 0; i < cells.size() && (int32_t)i < cap; i++)
         WriteCellRow(cells[i], out + i * ISC_CELL_STRIDE);
      return (int32_t)cells.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_set_block(IscWorld* w, int32_t bx, int32_t by, const double* rows, int32_t n)
{
   if (!w || n < 0 || (n > 0 && !rows)) return -1;
   try {
      bool skipped = false;
      if (!w->w.SetPackedBlock(bx, by, rows, n, ISC_CELL_STRIDE, skipped)) return -1;
      return skipped ? 0 : 1;
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_set_cache_dir(IscWorld* w, const char* dir)
{
   if (!w) return -1;
   try {
      return w->w.SetCacheDir(dir ? std::string(dir) : std::string()) ? 0 : -1;
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_flush_cache(IscWorld* w)
{
   if (!w) return -1;
   try {
      return w->w.FlushCache() ? 0 : -1;
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
         o[0] = d[i].x; o[1] = d[i].y; o[2] = d[i].z; o[3] = d[i].id; o[4] = d[i].structure;
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

int32_t isc_world_set_kinetics_history(IscWorld* w, const double* rows, int32_t nSeg)
{
   if (!w || nSeg < 0 || nSeg > 1000000 || (nSeg > 0 && !rows)) return -1;
   std::vector<double> tStart((size_t)nSeg);
   std::vector<isc::Kinetics> kin((size_t)nSeg * ISC_STRUCT_COUNT);
   for (int32_t i = 0; i < nSeg; i++) {
      const double* r = rows + (size_t)i * ISC_KIN_ROW;
      tStart[(size_t)i] = r[0];
      for (int s = 0; s < ISC_STRUCT_COUNT; s++) {
         const double* k = r + 1 + s * ISC_KIN_COUNT;
         isc::Kinetics& q = kin[(size_t)i * ISC_STRUCT_COUNT + (size_t)s];
         q.activationRatePerSec = k[ISC_KIN_ACTIVATION_RATE];
         q.onSec = k[ISC_KIN_ON_SEC];
         q.offSec = k[ISC_KIN_OFF_SEC];
         q.bleachProb = k[ISC_KIN_BLEACH_PROB];
         q.photonCV = k[ISC_KIN_PHOTON_CV];
         q.initialOnSec = k[ISC_KIN_INITIAL_ON_SEC];
      }
   }
   try {
      return w->w.SetKineticsHistory(tStart, kin) ? 0 : -1;
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_set_label(IscWorld* w, int32_t structure, const double* v, int32_t n)
{
   if (!w || structure < 0 || structure >= ISC_STRUCT_COUNT || n < 0 || n > ISC_LABEL_COUNT || (n > 0 && !v)) return -1;
   double x[ISC_LABEL_COUNT];
   const isc::Label def;
   x[ISC_LABEL_DENSITY] = def.density;
   x[ISC_LABEL_FLUORESCENT_FRACTION] = def.fluorescentFraction;
   x[ISC_LABEL_MODE] = (double)(int)def.mode;
   x[ISC_LABEL_ACTIVATION_RATE] = def.kin.activationRatePerSec;
   x[ISC_LABEL_ON_SEC] = def.kin.onSec;
   x[ISC_LABEL_OFF_SEC] = def.kin.offSec;
   x[ISC_LABEL_BLEACH_PROB] = def.kin.bleachProb;
   x[ISC_LABEL_PHOTON_CV] = def.kin.photonCV;
   x[ISC_LABEL_INITIAL_ON_SEC] = def.kin.initialOnSec;
   x[ISC_LABEL_PRE_STATE] = def.preState ? 1 : 0;
   x[ISC_LABEL_ORIENT_MODE] = (double)(int)def.orientation.mode;
   x[ISC_LABEL_ORIENT_POLAR_DEG] = def.orientation.polarDeg;
   x[ISC_LABEL_ORIENT_AZIMUTH_DEG] = def.orientation.azimuthDeg;
   x[ISC_LABEL_WOBBLE_DEG] = def.orientation.wobbleDeg;
   x[ISC_LABEL_MOTION] = def.motion;
   x[ISC_LABEL_OFFTARGET_COUNT] = def.offTargetCount;
   for (int32_t i = ISC_LABEL_BINDER_NM; i <= ISC_LABEL_LINKER_MAX_NM; i++) x[i] = NAN;   // the structure's linkage
   for (int32_t i = 0; i < n; i++) x[i] = v[i];
   bool linkage = true;
   for (int32_t i = 0; i < ISC_LABEL_COUNT; i++) {
      if (i >= ISC_LABEL_BINDER_NM && i <= ISC_LABEL_LINKER_MAX_NM) {
         if (std::isnan(x[i])) linkage = false;
         else if (!std::isfinite(x[i])) return -1;
      } else if (!std::isfinite(x[i])) {
         return -1;
      }
   }
   auto isInt = [](double d, int lo, int hi) { return d == std::floor(d) && d >= lo && d <= hi; };
   if (!isInt(x[ISC_LABEL_MODE], 0, 3) || !isInt(x[ISC_LABEL_ORIENT_MODE], 0, 2) || !(x[ISC_LABEL_DENSITY] >= 0) ||
       !(x[ISC_LABEL_FLUORESCENT_FRACTION] >= 0) || !(x[ISC_LABEL_ACTIVATION_RATE] >= 0) || !(x[ISC_LABEL_ON_SEC] > 0) ||
       !(x[ISC_LABEL_OFF_SEC] >= 0) || !(x[ISC_LABEL_BLEACH_PROB] > 0) || !(x[ISC_LABEL_PHOTON_CV] >= 0) ||
       !(x[ISC_LABEL_INITIAL_ON_SEC] >= 0) || !(x[ISC_LABEL_WOBBLE_DEG] >= 0))
      return -1;
   isc::Label l;
   l.density = x[ISC_LABEL_DENSITY];
   l.fluorescentFraction = x[ISC_LABEL_FLUORESCENT_FRACTION];
   l.mode = (isc::LabelMode)(int)x[ISC_LABEL_MODE];
   l.kin.activationRatePerSec = x[ISC_LABEL_ACTIVATION_RATE];
   l.kin.onSec = x[ISC_LABEL_ON_SEC];
   l.kin.offSec = x[ISC_LABEL_OFF_SEC];
   l.kin.bleachProb = x[ISC_LABEL_BLEACH_PROB];
   l.kin.photonCV = x[ISC_LABEL_PHOTON_CV];
   l.kin.initialOnSec = x[ISC_LABEL_INITIAL_ON_SEC];
   l.preState = x[ISC_LABEL_PRE_STATE] != 0;
   l.orientation.mode = (isc::OrientationMode)(int)x[ISC_LABEL_ORIENT_MODE];
   l.orientation.polarDeg = x[ISC_LABEL_ORIENT_POLAR_DEG];
   l.orientation.azimuthDeg = x[ISC_LABEL_ORIENT_AZIMUTH_DEG];
   l.orientation.wobbleDeg = x[ISC_LABEL_WOBBLE_DEG];
   // Anything non-zero is "not Static" / "some off-target entries".
   l.motion = x[ISC_LABEL_MOTION] != 0 ? 1 : 0;
   l.offTargetCount = x[ISC_LABEL_OFFTARGET_COUNT] != 0 ? 1 : 0;
   l.hasLinkage = linkage;
   if (linkage) {
      l.linkage.binderNm = x[ISC_LABEL_BINDER_NM];
      l.linkage.binderFlexNm = x[ISC_LABEL_BINDER_FLEX_NM];
      l.linkage.linkerMinNm = x[ISC_LABEL_LINKER_MIN_NM];
      l.linkage.linkerMaxNm = x[ISC_LABEL_LINKER_MAX_NM];
   }
   if (isc::LabelNotImplemented(l)) return -2;
   if (isc::ValidateLabel(l)) return -1;
   try {
      return w->w.SetLabel(structure, l) ? 0 : -1;
   } catch (...) {
      return -1;
   }
}

namespace {
void WriteEventRow(const isc::WorldEvent& e, double* o)
{
   o[0] = e.x; o[1] = e.y; o[2] = e.z; o[3] = e.tOn; o[4] = e.tOff; o[5] = e.brightness; o[6] = e.id;
   o[7] = e.structure; o[8] = e.state; o[9] = e.aux;
}
} // namespace

int32_t isc_events_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                             double zMin, double zMax, double t0, double t1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || !(t1 >= t0) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::WorldEvent>& e = w->events;
      e.clear();
      w->w.EventsInWindow(x0, y0, x1, y1, zMin, zMax, t0, t1, e);
      if (e.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < e.size() && (int32_t)i < cap; i++) WriteEventRow(e[i], out + i * ISC_EVENT_STRIDE);
      return (int32_t)e.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_continuous_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                 double zMin, double zMax, double tMin, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || std::isnan(tMin) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::WorldEvent>& e = w->events;
      e.clear();
      w->w.ContinuousInWindow(x0, y0, x1, y1, zMin, zMax, tMin, e);
      if (e.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < e.size() && (int32_t)i < cap; i++) WriteEventRow(e[i], out + i * ISC_EVENT_STRIDE);
      return (int32_t)e.size();
   } catch (...) {
      return -1;
   }
}


namespace {
void WriteWindowRow(const isc::MapWindow& m, double* o)
{
   WriteEventRow(m.e, o);
   o[10] = m.clock;
   o[11] = m.rate;
}
} // namespace

int32_t isc_world_set_clock_map(IscWorld* w, const double* v, int32_t n)
{
   if (!w || n < 0 || (n > 0 && !v)) return -1;
   try {
      if (n == 0) {
         w->w.ClearClockMap();
         return 0;
      }
      isc::ClockMap m;
      return ReadClockMap(v, (size_t)n, m) && w->w.SetClockMap(m) ? 0 : -1;
   } catch (...) {
      return -1;
   }
}

int32_t isc_events_in_map(IscWorld* w, double x0, double y0, double x1, double y1, double zMin, double zMax, double t0,
                          double t1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || !(t1 >= t0) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::WorldEvent>& e = w->events;
      e.clear();
      w->w.EventsInMap(x0, y0, x1, y1, zMin, zMax, t0, t1, e);
      if (e.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < e.size() && (int32_t)i < cap; i++) WriteEventRow(e[i], out + i * ISC_EVENT_STRIDE);
      return (int32_t)e.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_continuous_in_map(IscWorld* w, double x0, double y0, double x1, double y1, double zMin, double zMax,
                              double tMin, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || std::isnan(tMin) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::MapWindow> m;
      w->w.ContinuousInMap(x0, y0, x1, y1, zMin, zMax, tMin, m);
      if (m.size() > (size_t)INT32_MAX) return -1;
      for (size_t i = 0; i < m.size() && (int32_t)i < cap; i++) WriteWindowRow(m[i], out + i * ISC_MAP_WINDOW_STRIDE);
      return (int32_t)m.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_world_prefetch_map(IscWorld* w, double x0, double y0, double x1, double y1, double zMin, double zMax,
                               double t0, double t1, double budgetMs)
{
   if (!w || BadRect(x0, y0, x1, y1) || !(t1 >= t0) || !(budgetMs >= 0)) return -1;
   try {
      return w->w.PrefetchMap(x0, y0, x1, y1, zMin, zMax, t0, t1, budgetMs) ? 1 : 0;
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
      const isc::CytoMesh& m = w->w.Assets(c, false).geom.mesh;   // the mesh alone: no microtubules
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

int32_t isc_cell_npcs(IscWorld* w, int32_t cx, int32_t cy, double* out, int32_t cap)
{
   if (!w || cap < 0 || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::Npc> pores;
      if (!w->w.CellNpcs(cx, cy, pores)) return -1;
      for (size_t i = 0; i < pores.size() && (int32_t)i < cap; i++) {
         const isc::Npc& q = pores[i];
         double* o = out + ISC_NPC_STRIDE * i;
         o[0] = q.c.x; o[1] = q.c.y; o[2] = q.c.z;
         o[3] = q.n.x; o[4] = q.n.y; o[5] = q.n.z;
         o[6] = q.e1.x; o[7] = q.e1.y; o[8] = q.e1.z;
      }
      return (int32_t)pores.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_cell_nucleus_rings(IscWorld* w, int32_t cx, int32_t cy, int32_t slices, int32_t pts,
                               double* out, int32_t capPts)
{
   if (!w || slices < 2 || pts < 1 || (capPts > 0 && !out)) return -1;
   try {
      isc::Cell c;
      if (!w->w.FindCell(cx, cy, c)) return -1;
      const std::vector<isc::Pt3> r = isc::NucleusRingsLocal(c, slices, pts);
      for (size_t i = 0; i < r.size() && (int32_t)i < capPts; i++) {
         out[3 * i] = r[i].x; out[3 * i + 1] = r[i].y; out[3 * i + 2] = r[i].z;
      }
      return (int32_t)r.size();
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
                                int32_t nx, int32_t ny, int32_t nz, int32_t structureMask, float* out)
{
   if (!w || BadRect(x0, y0, x1, y1) || nx <= 0 || ny <= 0 || nz <= 0 || !out || structureMask < 0 ||
       (structureMask & ~((1 << ISC_STRUCT_COUNT) - 1)) != 0 || !(zMax > zMin) ||
       (nz > 1 && (!std::isfinite(zMin) || !std::isfinite(zMax))))
      return -1;
   if ((double)nx * ny * nz > 2.0e9) return -1;
   try {
      const long n = w->w.Density3dInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, nz, (unsigned)structureMask, out);
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

int32_t isc_origami_design_count(void) { return isc::OrigamiDesignCount(); }

const char* isc_origami_design_id(int32_t i)
{
   return i >= 0 && i < isc::OrigamiDesignCount() ? isc::OrigamiDesignAt(i).id : nullptr;
}

int32_t isc_origami_heights(const IscParams* ip, double out[2])
{
   if (!ip || !out) return -1;
   isc::Params p = ip->p;
   isc::NormalizeOrigamiParams(p);
   const isc::OrigamiDesign& d = isc::OrigamiDesignAt((int)p.origamiDesign);
   out[0] = (p.origamiAttachNm + d.heightNm / 2) / 1000.0;
   out[1] = (p.origamiAttachNm + d.heightNm + p.origamiLinkerNm) / 1000.0;
   return 0;
}

int32_t isc_origami_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || (cap > 0 && !out)) return -1;
   try {
      isc::OrigamiField* f = w->w.Origami();
      if (!f) return 0;
      std::vector<isc::OrigamiMolecule> mols;
      f->MoleculesInRect(x0, y0, x1, y1, mols);
      if (mols.size() > (size_t)INT32_MAX) return -1;
      for (size_t j = 0; j < mols.size() && (int32_t)j < cap; j++) {
         const isc::OrigamiMolecule& m = mols[j];
         const isc::OrigamiShape S = f->Shape(m);
         double* o = out + j * ISC_ORIGAMI_STRIDE;
         o[0] = m.tx; o[1] = m.ty; o[2] = m.i; o[3] = f->P().origamiDesign; o[4] = m.x; o[5] = m.y; o[6] = m.yaw;
         o[7] = isc::OrigamiMoleculeRoll(f->Design(), f->P(), m.h1); o[8] = S.endAngle; o[9] = S.fold ? 1 : 0;
      }
      return (int32_t)mols.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_fiducials_in_window(IscWorld* w, double x0, double y0, double x1, double y1, double* out, int32_t cap)
{
   if (!w || BadRect(x0, y0, x1, y1) || (cap > 0 && !out)) return -1;
   try {
      std::vector<isc::FiducialBead> beads;
      w->w.FiducialsInRect(x0, y0, x1, y1, beads);
      if (beads.size() > (size_t)INT32_MAX) return -1;
      for (size_t j = 0; j < beads.size() && (int32_t)j < cap; j++) {
         const isc::FiducialBead& b = beads[j];
         double* o = out + j * ISC_FIDUCIAL_STRIDE;
         o[0] = b.tx; o[1] = b.ty; o[2] = b.i; o[3] = b.x; o[4] = b.y; o[5] = b.z; o[6] = b.d;
      }
      return (int32_t)beads.size();
   } catch (...) {
      return -1;
   }
}

int32_t isc_origami_outline(IscWorld* w, int32_t tx, int32_t ty, int32_t i, int32_t nPerSide, double* out,
                            int32_t capPts)
{
   if (!w || nPerSide < 2 || nPerSide > 100000 || (capPts > 0 && !out)) return -1;
   try {
      isc::OrigamiField* f = w->w.Origami();
      isc::OrigamiMolecule m;
      if (!f || !f->Find(tx, ty, i, m)) return -1;
      const std::vector<double> o = f->OutlineWorld(m, nPerSide);
      const int32_t n = (int32_t)(o.size() / 2);
      for (int32_t j = 0; j < n && j < capPts; j++) { out[2 * j] = o[2 * (size_t)j]; out[2 * j + 1] = o[2 * (size_t)j + 1]; }
      return n;
   } catch (...) {
      return -1;
   }
}

int32_t isc_origami_sites(IscWorld* w, int32_t tx, int32_t ty, int32_t i, double* out, int32_t cap)
{
   if (!w || (cap > 0 && !out)) return -1;
   try {
      isc::OrigamiField* f = w->w.Origami();
      isc::OrigamiMolecule m;
      if (!f || !f->Find(tx, ty, i, m)) return -1;
      const isc::OrigamiShape S = f->Shape(m);
      const isc::Label& label = w->w.GetLabel(isc::STRUCTURE_ORIGAMI);
      std::vector<isc::OrigamiSite> all, lit;
      isc::OrigamiMoleculeSites(f->Design(), f->P(), m, S, 1, 1, all, true);
      isc::OrigamiMoleculeSites(f->Design(), f->P(), m, S, label.density, label.fluorescentFraction, lit);
      size_t q = 0;
      for (size_t j = 0; j < all.size(); j++) {
         const isc::OrigamiSite& s = all[j];
         const bool on = q < lit.size() && lit[q].k == s.k && lit[q].n == s.n;
         if (on) q++;
         if ((int32_t)j >= cap) continue;
         double* o = out + j * ISC_ORIGAMI_SITE_STRIDE;
         o[0] = s.k; o[1] = s.n; o[2] = s.x; o[3] = s.y; o[4] = s.z; o[5] = s.rest[0]; o[6] = s.rest[1];
         o[7] = s.rest[2]; o[8] = s.bent[0]; o[9] = s.bent[1]; o[10] = on ? 1 : 0;
      }
      return (int32_t)all.size();
   } catch (...) {
      return -1;
   }
}

} // extern "C"
