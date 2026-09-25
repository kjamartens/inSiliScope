#include "insilicell/insilicell.h"

#include "packing.h"
#include "params.h"
#include "rng.h"

#include <new>

struct IscParams { isc::Params p; };

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

} // extern "C"
