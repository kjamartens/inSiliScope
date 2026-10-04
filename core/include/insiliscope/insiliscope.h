/* insiliscope core -- C ABI.
 *
 * The only interface consumers (WASM/JS, Micro-Manager adapter, cli) use.
 * Flat buffers and opaque handles: no exceptions, no STL across it.
 *
 * M0: RNG and cell packing. M2: the world (fixed-block packing, cytoplasm,
 * microtubules, dyes) and its window queries sitesInWindow / densityInWindow.
 * M3 (ABI 2): dye blink schedules and the event query, the full cell record
 * and per-cell geometry for the viewer. ABI 3: activation as a rate per dye,
 * non-bleaching (persistent) sites. ABI 4: isc_world_set_dye_cache, isc_world_prefetch.
 * ABI 5: isc_density3d_in_window (z-resolved, per population; WideField).
 * ABI 6: isc_optical_volume_in_window (cytoplasm/nucleus/microtubule volume
 * fractions per voxel; BrightField). ABI 7: isc_world_pack_block /
 * isc_world_set_block (hand a packed block from one world to another, so
 * several worlds of one seed -- the viewer's workers -- pack each block once).
 * ABI 8: isc_world_set_cache_dir / isc_world_flush_cache (packed blocks kept
 * in a small per-user file across runs), isc_world_version. ABI 9:
 * isc_cell_nucleus_rings (the shaped nucleus's surface, for drawing); the
 * optical volume's nucleus chord follows the shaped nucleus.
 *
 * Units: um; z is height above the coverslip. Windows are half-open
 * [x0,x1) x [y0,y1) x [zMin,zMax); pass -INFINITY/INFINITY for no z limit.
 */
#ifndef INSILISCOPE_H
#define INSILISCOPE_H

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

#define ISC_ABI_VERSION 9

ISC_API int32_t isc_abi_version(void);

/* The generator's version: the date spec/golden was last re-frozen. Bump it
 * with every change that moves a cell (packing, RawCandidate): the packed
 * block caches (isc_world_set_cache_dir, the viewer's local storage) are
 * keyed on it, so stale poses are never taken for current ones. */
#define ISC_WORLD_VERSION "2026-10-05"
ISC_API const char* isc_world_version(void);

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
 * labelled fractions of the lattice sites are the `labelEfficiency` param
 * (bleaching dyes, default 0.1) and `labelNonBleaching` (persistent,
 * DNA-PAINT-like sites, default 0; one draw per site decides which).
 * Not thread-safe: use one world per thread. */
typedef struct IscWorld IscWorld;
ISC_API IscWorld* isc_world_new(uint32_t seed, const IscParams* p);
ISC_API void isc_world_free(IscWorld* w);

/* Upper bound of the dye cache, in dyes (ABI 4; default 2e6). Each cached dye
 * costs ~100-200 bytes with its blink schedule; the blocks one query uses are
 * never evicted, so this bounds what is kept beyond the current window
 * (e.g. the rest of the z column, or where the window was before). Caches
 * only: no answer depends on it. Returns 0, or -1 on bad arguments. */
ISC_API int32_t isc_world_set_dye_cache(IscWorld* w, double maxDyes);

/* Cells whose footprint circle intersects the window, ISC_CELL_STRIDE doubles
 * each: cx, cy, x, y, packRot, rOuter, height, nucOffX, nucOffY, nucRot,
 * nucLong, nucShort, nucHeight, nucZ (the prototype's cell fields; nuc* are
 * in the cell's local frame). Writes at most `cap`; returns the total (call
 * again with a larger buffer if it exceeds cap), or -1 on bad arguments. */
#define ISC_CELL_STRIDE 14
ISC_API int32_t isc_cells_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                    double* out, int32_t cap);

/* ABI 7. The packed cells of one 8x8-chunk block (bx, by) -- the chunks
 * [8 bx, 8 bx + 7] x [8 by, 8 by + 7] -- as ISC_CELL_STRIDE rows (the layout
 * of isc_cells_in_window), in the block's order; packs it if needed. Same
 * cap/return convention, -1 on bad arguments. */
ISC_API int32_t isc_world_pack_block(IscWorld* w, int32_t bx, int32_t by, double* out, int32_t cap);

/* ABI 7. Installs the packed cells of block (bx, by) as another world of the
 * same seed and parameters packed them (rows as isc_world_pack_block writes
 * them; only cx, cy, x, y and packRot are read, each cell's shape is
 * recomputed from its address), so this world does not pack that block.
 * Returns 1 when installed, 0 when the block was cached already (nothing
 * changes), -1 when a row is not a present cell of that block or the rows
 * are out of order (nothing changes). A cache only: every answer stays a
 * pure function of (seed, params, window). */
ISC_API int32_t isc_world_set_block(IscWorld* w, int32_t bx, int32_t by, const double* rows, int32_t n);

/* ABI 8. Keeps this world's packed blocks in dir/packed_blocks.bin (the
 * directory is made if needed): blocks found there -- written by an earlier
 * run of the same seed, parameters and ISC_WORLD_VERSION -- are installed as
 * isc_world_set_block would (validated; a bad row costs a repack, never a
 * wrong cell), blocks packed from now on are added and written every 16
 * blocks, at isc_world_flush_cache and when the world is freed. One file per
 * directory, rewritten whole, at most 4096 blocks (a few MB): a world of
 * another key overwrites it. Five numbers per cell (cx, cy, x, y, packRot),
 * nothing else is stored. NULL or "" turns it off. Returns 0, or -1 when the
 * directory cannot be used (no store then; WASM has none). */
ISC_API int32_t isc_world_set_cache_dir(IscWorld* w, const char* dir);
/* ABI 8. Writes the pending blocks now. 0, -1 without a store or on an I/O error. */
ISC_API int32_t isc_world_flush_cache(IscWorld* w);

/* Labelled dyes in the window, ISC_SITE_STRIDE doubles each: x, y, z, id (a
 * per-dye uint32 hash, stable across windows; the seed of its blink
 * schedule). Same cap/return convention as above. */
#define ISC_SITE_STRIDE 4
ISC_API int32_t isc_sites_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                    double zMin, double zMax, double* out, int32_t cap);

/* Blink kinetics, simulated seconds (spec/PORT.md 6.1). A dark dye switches
 * on at activationRatePerSec (per dye; 0 = never). Bleaching dyes: first
 * activation after Exp(1/rate), ON Exp(onSec), then bleach with probability
 * bleachProb (clamped to [0.01, 1]) or dark Exp(offSec) and blink again (at
 * most 1000 blinks). Persistent sites: blinks start as a Poisson process of
 * that rate for ever, ON Exp(onSec). Per-blink brightness log-normal, mean 1,
 * CV photonCV. Defaults 0.01, 0.05, 1, 1, 0.5. Changing them keeps the
 * geometry cached. Returns 0, or -1 on bad arguments. */
ISC_API int32_t isc_world_set_kinetics(IscWorld* w, double activationRatePerSec, double onSec, double offSec,
                                       double bleachProb, double photonCV);

/* Blinks overlapping [t0, t1) (tOn < t1 and tOff > t0) of the labelled dyes
 * in the window, ISC_EVENT_STRIDE doubles each: x, y, z, tOn, tOff,
 * brightness, id. Each dye's whole blink lifetime is a pure function of its
 * address, so the same dye blinks the same way whenever it is queried. Same
 * cap/return convention as above. */
#define ISC_EVENT_STRIDE 7
ISC_API int32_t isc_events_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                     double zMin, double zMax, double t0, double t1, double* out, int32_t cap);

/* Warms the caches for a window (cells, dyes, blink schedules, and the
 * persistent sites' blinks around [t0, t1)) for at most about budgetMs, so a
 * later isc_events_in_window there is fast: e.g. a margin around the current
 * window and the whole z column, in the idle time before the next frame
 * (ABI 4). Changes no answer. Returns 1 when the whole window is cached (a
 * repeat over a window already done returns at once), 0 if the budget ran
 * out first, -1 on bad arguments. */
ISC_API int32_t isc_world_prefetch(IscWorld* w, double x0, double y0, double x1, double y1, double zMin,
                                   double zMax, double t0, double t1, double budgetMs);

/* ---- Per-cell geometry (viewer), cell-local frame (before packRot) ----
 * The cell is addressed by its home chunk (cx, cy); all return -1 if that
 * chunk holds no cell. */
/* Footprint outline: n points as x, y pairs. Returns n. */
ISC_API int32_t isc_cell_outline(IscWorld* w, int32_t cx, int32_t cy, double* out, int32_t capPts);
/* Smoothed cytoplasm mesh: (rings+1) x n vertices as x, y, h triples, index
 * k*n + i; dims[0] = rings, dims[1] = n. Returns the vertex count. */
ISC_API int32_t isc_cell_mesh(IscWorld* w, int32_t cx, int32_t cy, int32_t dims[2], double* out, int32_t capVerts);
/* Microtubule centrelines: all points as x, y, z triples (up to capPts) and
 * the point count of each microtubule in lens (up to capMts). *totalPts
 * receives the total point count. Returns the microtubule count. */
ISC_API int32_t isc_cell_microtubules(IscWorld* w, int32_t cx, int32_t cy, double* xyz, int32_t capPts,
                                      int32_t* lens, int32_t capMts, int32_t* totalPts);
/* Nucleus surface (ABI 9): `slices` horizontal sections of `pts` points each,
 * bottom to top (zeta = -cos(pi i / (slices - 1)): both poles included), as
 * x, y, z triples, index k*pts + i (up to capPts points). Returns
 * slices*pts. */
ISC_API int32_t isc_cell_nucleus_rings(IscWorld* w, int32_t cx, int32_t cy, int32_t slices, int32_t pts,
                                       double* out, int32_t capPts);

/* Labelled-dye counts on an nx x ny grid over the window (row-major, row = y),
 * written to out[nx*ny]. Returns the total count, or -1 on bad arguments. */
ISC_API int32_t isc_density_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                      double zMin, double zMax, int32_t nx, int32_t ny, float* out);

/* Labelled-dye counts on an nx x ny x nz grid (ABI 5), written to
 * out[(k*ny + iy)*nx + ix]; plane k spans [zMin + k*(zMax-zMin)/nz, ...),
 * x/y binned as isc_density_in_window. populations selects which dyes count
 * (ISC_POP_BLEACHING | ISC_POP_PERSISTENT; 0 counts none). The core bins its
 * cached dyes straight into the voxels, so nothing crosses the ABI per dye.
 * zMin/zMax may be infinite only with nz = 1. Returns the total count, or -1
 * on bad arguments. */
#define ISC_POP_BLEACHING 1
#define ISC_POP_PERSISTENT 2
ISC_API int32_t isc_density3d_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                        double zMin, double zMax, int32_t nx, int32_t ny, int32_t nz,
                                        int32_t populations, float* out);

/* Optical volume (ABI 6, BrightField): the volume fractions of cytoplasm
 * (cell body minus nucleus), nucleus and microtubules (12.5 nm tubes) in
 * each voxel of an nx x ny x nz grid over the window, written channel-major
 * to out[3*nx*ny*nz]: out[((ch*nz + k)*ny + iy)*nx + ix], ch = 0 cytoplasm,
 * 1 nucleus, 2 microtubule. Pure geometry (no refractive indices). Each
 * voxel column is sampled at sub x sub points (1..16) across its footprint;
 * z overlaps are exact. zMin/zMax must be finite. Returns the number of
 * cells that reach the window, or -1 on bad arguments. */
#define ISC_OPT_CYTOPLASM 0
#define ISC_OPT_NUCLEUS 1
#define ISC_OPT_MICROTUBULE 2
ISC_API int32_t isc_optical_volume_in_window(IscWorld* w, double x0, double y0, double x1, double y1,
                                             double zMin, double zMax, int32_t nx, int32_t ny, int32_t nz,
                                             int32_t sub, float* out);

#ifdef __cplusplus
}
#endif

#endif /* INSILISCOPE_H */
