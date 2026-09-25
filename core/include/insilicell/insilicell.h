/* insilicell core -- C ABI.
 *
 * The only interface consumers (WASM/JS, Micro-Manager adapter, cli) use.
 * Flat buffers and opaque handles: no exceptions, no STL across it.
 *
 * M0: RNG and cell packing. M2: the world (fixed-block packing, cytoplasm,
 * microtubules, dyes) and its window queries sitesInWindow / densityInWindow.
 * excitationAt arrives with M5.
 *
 * Units: um; z is height above the coverslip. Windows are half-open
 * [x0,x1) x [y0,y1) x [zMin,zMax); pass -INFINITY/INFINITY for no z limit.
 */
#ifndef INSILICELL_H
#define INSILICELL_H

#include <stdint.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define ISC_API EMSCRIPTEN_KEEPALIVE
#else
#define ISC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ISC_ABI_VERSION 1

ISC_API int32_t isc_abi_version(void);

/* ---- RNG (bit-exact with the JS prototype) ---- */
ISC_API void isc_pcg4d(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t out[4]);
ISC_API double isc_hash_unit(uint32_t seed, int32_t cx, int32_t cy, uint32_t k);

/* ---- Parameters (opaque; defaults = prototype UI defaults) ---- */
typedef struct IscParams IscParams;
ISC_API IscParams* isc_params_new(void);
ISC_API void isc_params_free(IscParams* p);
/* Field names as in the prototype's params(); booleans take 0/1.
 * Returns 0 on success, -1 for an unknown name. */
ISC_API int32_t isc_params_set(IscParams* p, const char* name, double value);

/* ---- Packing ----
 * Builds candidates for chunks [cx0..cx1] x [cy0..cy1], relaxes and prunes
 * them (JS packMap), and writes the surviving cells as ISC_PACKED_STRIDE
 * doubles each: cx, cy, x, y, packRot (um, radians). Writes at most `cap`
 * cells; returns the total number of surviving cells (call again with a
 * larger buffer if it exceeds cap), or -1 on bad arguments. `removed`
 * (nullable) receives the number of cells prune() dropped. */
#define ISC_PACKED_STRIDE 5
ISC_API int32_t isc_pack_window(uint32_t seed, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
                                const IscParams* p, double* out, int32_t cap, int32_t* removed);

/* ---- World (M2) ----
 * A world = (seed, params) plus caches. Every answer is a pure function of
 * (seed, params, window): cells are packed on fixed 8x8-chunk blocks (or not
 * at all with enablePacking=0), so moving the window away and back gives the
 * same cells and dyes. Params are copied (and normalised) at creation; the
 * dye labelling efficiency is the `labelEfficiency` param (default 0.1).
 * Not thread-safe: use one world per thread. */
typedef struct IscWorld IscWorld;
ISC_API IscWorld* isc_world_new(uint32_t seed, const IscParams* p);
ISC_API void isc_world_free(IscWorld* w);

/* Cells whose footprint circle intersects the window, ISC_CELL_STRIDE doubles
 * each: cx, cy, x, y, packRot, rOuter. Writes at most `cap`; returns the total
 * (call again with a larger buffer if it exceeds cap), or -1 on bad arguments. */
#define ISC_CELL_STRIDE 6
ISC_API int32_t isc_cells_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                    double* out, int32_t cap);

/* Labelled dyes in the window, ISC_SITE_STRIDE doubles each: x, y, z, id (a
 * per-dye uint32 hash, stable across windows; the seed of its blink
 * schedule). Same cap/return convention as above. */
#define ISC_SITE_STRIDE 4
ISC_API int32_t isc_sites_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                    double zMin, double zMax, double* out, int32_t cap);

/* Labelled-dye counts on an nx x ny grid over the window (row-major, row = y),
 * written to out[nx*ny]. Returns the total count, or -1 on bad arguments. */
ISC_API int32_t isc_density_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                      double zMin, double zMax, int32_t nx, int32_t ny, float* out);

#ifdef __cplusplus
}
#endif

#endif /* INSILICELL_H */
