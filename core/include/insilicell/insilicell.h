/* insilicell core -- C ABI.
 *
 * The only interface consumers (WASM/JS, Micro-Manager adapter, cli) use.
 * Flat buffers and opaque handles: no exceptions, no STL across it.
 *
 * M0 (feasibility spike) surface: RNG and cell packing only. The window
 * queries of the brief (sitesInWindow, densityInWindow, excitationAt)
 * arrive with M2+.
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

#define ISC_ABI_VERSION 0

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

#ifdef __cplusplus
}
#endif

#endif /* INSILICELL_H */
