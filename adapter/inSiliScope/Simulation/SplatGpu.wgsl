// SPDX-License-Identifier: BSD-3-Clause
// The blink splat + camera noise, fused: the WGSL twin of the adapter's HLSL kernel (GpuSimD3D11.cpp kShaderSource),
// run by SplatGpuHost (SplatGpu.cpp) on the viewer's WebGPU (cli/gpu_js_backend.cpp, web/splat_gpu.js).
// One invocation per output pixel gathers its frame's emitters from the kernels' block sums (PsfKernelCache::BlockSums,
// row-major per plane: the numbers the CPU splat reads, with SplatSetup's indices and weights), adds the frame's other
// light (Extra: what the CPU renders besides the splatted blinks: the background, the continuous populations, binned
// blinks), and applies the sCMOS or EMCCD noise chain with the counter-based pcg4d stream of SMLMCounterRng.h, mirrored
// line for line (change one, change all three). Frames equal the CPU's up to float32 rounding.
//
// Bindings (seven storage buffers: within WebGPU's default limit of eight):
//   0  P     the parameters (64 bytes)
//   1-4 S0..S3 the kernels' block sums (one buffer per distinct kernel: a default one is ~200 MB)
//   5  Stat  per pixel: offset ADU, gain photons/ADU, read noise e-, background photons before the fade (f32 bits);
//            then the halo cut's spans (columns [lo, hi] per (plane, dy)) from word P.spanAt
//   6  Dyn   words 0..31: the kernel table (KGeo[4] = camera radius, oversampling, block-sum width, offset; KMisc[4] =
//            plane stride, spans on, first span, buffer); then 8 words per frame (emitter start, count, frame, bgScale,
//            extra start or 0xffffffff, tile table start); then the emitters (16 words each, GpuSplatEmitter); then per
//            frame its extra image and its tile table (per 16 x 16 tile, row-major, the word position of its list, one
//            more for the end; then the lists: emitter numbers)
//   7  Out   the frames' ADU, frame k at k * W * H

//// kernel splat_frames
struct Params
{
   W: u32, H: u32, NFrames: u32, Seed: u32,
   Emccd: u32, Qe: f32, Dark: f32, Cic: f32,
   EmGain: f32, MaxAdu: f32, PixBase: u32, SpanAt: u32,
   EmAt: u32, Pad0: u32, Pad1: u32, Pad2: u32,
};

struct Em
{
   x0: i32, y0: i32, bx: i32, by: i32,
   plane: i32, nt: i32, photons: f32, cutR2: i32,
   wx: vec4<f32>, wy: vec4<f32>,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read> S0: array<f32>;
@group(0) @binding(2) var<storage, read> S1: array<f32>;
@group(0) @binding(3) var<storage, read> S2: array<f32>;
@group(0) @binding(4) var<storage, read> S3: array<f32>;
@group(0) @binding(5) var<storage, read> Stat: array<u32>;
@group(0) @binding(6) var<storage, read> Dyn: array<u32>;
@group(0) @binding(7) var<storage, read_write> Out: array<u32>;

// One emitter's taps from block-sum plane `base` (one function per kernel buffer: WGSL passes no storage pointers).
fn taps0(base: u32, r0: i32, c0: i32, bw: i32, em: Em) -> f32
{
   var s = 0.0;
   for (var j = 0; j < em.nt; j++)
   {
      let r = r0 + j;
      if (r < 0 || r >= bw) { continue; }
      var row = 0.0;
      for (var ii = 0; ii < em.nt; ii++)
      {
         let c = c0 + ii;
         if (c >= 0 && c < bw) { row += em.wx[ii] * S0[base + u32(r * bw + c)]; }
      }
      s += em.wy[j] * row;
   }
   return s;
}
fn taps1(base: u32, r0: i32, c0: i32, bw: i32, em: Em) -> f32
{
   var s = 0.0;
   for (var j = 0; j < em.nt; j++)
   {
      let r = r0 + j;
      if (r < 0 || r >= bw) { continue; }
      var row = 0.0;
      for (var ii = 0; ii < em.nt; ii++)
      {
         let c = c0 + ii;
         if (c >= 0 && c < bw) { row += em.wx[ii] * S1[base + u32(r * bw + c)]; }
      }
      s += em.wy[j] * row;
   }
   return s;
}
fn taps2(base: u32, r0: i32, c0: i32, bw: i32, em: Em) -> f32
{
   var s = 0.0;
   for (var j = 0; j < em.nt; j++)
   {
      let r = r0 + j;
      if (r < 0 || r >= bw) { continue; }
      var row = 0.0;
      for (var ii = 0; ii < em.nt; ii++)
      {
         let c = c0 + ii;
         if (c >= 0 && c < bw) { row += em.wx[ii] * S2[base + u32(r * bw + c)]; }
      }
      s += em.wy[j] * row;
   }
   return s;
}
fn taps3(base: u32, r0: i32, c0: i32, bw: i32, em: Em) -> f32
{
   var s = 0.0;
   for (var j = 0; j < em.nt; j++)
   {
      let r = r0 + j;
      if (r < 0 || r >= bw) { continue; }
      var row = 0.0;
      for (var ii = 0; ii < em.nt; ii++)
      {
         let c = c0 + ii;
         if (c >= 0 && c < bw) { row += em.wx[ii] * S3[base + u32(r * bw + c)]; }
      }
      s += em.wy[j] * row;
   }
   return s;
}

fn pcg4d(v0: vec4<u32>) -> vec4<u32>
{
   var v = v0 * 1664525u + 1013904223u;
   v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
   v ^= v >> vec4<u32>(16u);
   v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
   return v;
}

var<private> gFrame: u32;
var<private> gPix: u32;
var<private> gCtr: u32;
var<private> gBuf: vec4<u32>;
var<private> gK: u32;

fn uniform01() -> f32
{
   if (gK == 4u)
   {
      gBuf = pcg4d(vec4<u32>(P.Seed, gFrame, gPix, gCtr));
      gCtr++;
      gK = 0u;
   }
   let x = gBuf[gK];
   gK++;
   return (f32(x >> 9u) + 0.5) * 1.1920928955078125e-7;
}

fn gauss() -> f32
{
   let r = sqrt(-2.0 * log(uniform01()));
   return r * cos(6.283185307179586 * uniform01());
}

fn poisson(l: f32) -> f32
{
   if (l > 60.0) { return max(0.0, floor(l + sqrt(l) * gauss() + 0.5)); }
   if (!(l > 0.0)) { return 0.0; }
   let L = exp(-l);
   var k = 0;
   var p = 1.0;
   loop
   {
      k++;
      p *= uniform01();
      if (!(p > L && k < 1000)) { break; }
   }
   return f32(k - 1);
}

fn gammaDraw(k: f32) -> f32
{
   let d = k - 1.0 / 3.0;
   let c = 1.0 / sqrt(9.0 * d);
   for (var it = 0; it < 1000; it++)
   {
      let x = gauss();
      let t = 1.0 + c * x;
      if (t <= 0.0) { continue; }
      let v = t * t * t;
      let w = uniform01();
      if (w < 1.0 - 0.0331 * x * x * x * x) { return d * v; }
      if (log(w) < 0.5 * x * x + d * (1.0 - v + log(v))) { return d * v; }
   }
   return d;
}

fn emAt(e: u32) -> Em
{
   let a = P.EmAt + 16u * e;
   var em: Em;
   em.x0 = bitcast<i32>(Dyn[a]);
   em.y0 = bitcast<i32>(Dyn[a + 1u]);
   em.bx = bitcast<i32>(Dyn[a + 2u]);
   em.by = bitcast<i32>(Dyn[a + 3u]);
   em.plane = bitcast<i32>(Dyn[a + 4u]);
   em.nt = bitcast<i32>(Dyn[a + 5u]);
   em.photons = bitcast<f32>(Dyn[a + 6u]);
   em.cutR2 = bitcast<i32>(Dyn[a + 7u]);
   em.wx = vec4<f32>(bitcast<f32>(Dyn[a + 8u]), bitcast<f32>(Dyn[a + 9u]), bitcast<f32>(Dyn[a + 10u]), bitcast<f32>(Dyn[a + 11u]));
   em.wy = vec4<f32>(bitcast<f32>(Dyn[a + 12u]), bitcast<f32>(Dyn[a + 13u]), bitcast<f32>(Dyn[a + 14u]), bitcast<f32>(Dyn[a + 15u]));
   return em;
}

// The group's 16 x 16 pixels (a tile) share the tile's emitter list: the frame's emitters whose footprint (the kernel's
// halo spans, its cut radius) reaches the tile, in the frame's order, made by the host (2026-11-24: every group walked
// the whole frame's list, ~16 ns a pixel on an A40 at a few thousand blinks a frame -- as slow as 32 CPU cores).
// Staged through workgroup memory in chunks of 256 (one record per invocation, 16 KB: WebGPU's default limit), so a
// record is read once per group, not once per pixel. A pixel sums the same emitters in the same order as before.
const CHUNK = 256u;
var<workgroup> sEm: array<Em, 256>;
var<workgroup> sCount: u32;
var<workgroup> sStart: u32;

@compute @workgroup_size(16, 16, 1)
fn splat_frames(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_index) li: u32,
                @builtin(workgroup_id) gid: vec3<u32>)
{
   // Every invocation of the group takes part in the staging (barriers), also those outside the frame; one group is
   // one frame (gid.z < NFrames).
   let inside = id.x < P.W && id.y < P.H;
   let i = id.y * P.W + id.x;
   var px = vec4<f32>(0.0, 0.0, 1.0, 0.0);
   if (inside)
   {
      px = vec4<f32>(bitcast<f32>(Stat[4u * i]), bitcast<f32>(Stat[4u * i + 1u]), bitcast<f32>(Stat[4u * i + 2u]),
                     bitcast<f32>(Stat[4u * i + 3u]));
   }
   let fa = 32u + 8u * gid.z;
   // The tile's list: Dyn[tileAt + t] .. Dyn[tileAt + t + 1] are the word positions of its emitter numbers.
   let tileAt = Dyn[fa + 5u];
   let t = gid.y * ((P.W + 15u) / 16u) + gid.x;
   if (li == 0u)
   {
      sStart = Dyn[tileAt + t];
      sCount = Dyn[tileAt + t + 1u] - Dyn[tileAt + t];
   }
   let emCount = workgroupUniformLoad(&sCount);
   let listStart = workgroupUniformLoad(&sStart);
   let frame = Dyn[fa + 2u];
   let bgScale = bitcast<f32>(Dyn[fa + 3u]);
   let extra = Dyn[fa + 4u];

   // Splat: gather this frame's emitters, in list order, onto this pixel.
   var acc = px.w * bgScale;
   let X = i32(id.x);
   let Y = i32(id.y);
   for (var e0 = 0u; e0 < emCount; e0 += CHUNK)
   {
      let cnt = min(CHUNK, emCount - e0);
      if (li < cnt) { sEm[li] = emAt(Dyn[listStart + e0 + li]); }
      workgroupBarrier();
      for (var k = 0u; k < cnt; k++)
      {
         let em = sEm[k];
         let kk = u32(em.plane) >> 16u;
         let g = vec4<i32>(bitcast<i32>(Dyn[4u * kk]), bitcast<i32>(Dyn[4u * kk + 1u]), bitcast<i32>(Dyn[4u * kk + 2u]),
                           bitcast<i32>(Dyn[4u * kk + 3u]));   // camRad, os, bw, off
         let dx = X - em.x0;
         let dy = Y - em.y0;
         if (!inside || abs(dx) > g.x || abs(dy) > g.x) { continue; }
         if (dx * dx + dy * dy > em.cutR2) { continue; }
         let q = vec4<u32>(Dyn[16u + 4u * kk], Dyn[17u + 4u * kk], Dyn[18u + 4u * kk], Dyn[19u + 4u * kk]);
         let plane = u32(em.plane) & 0xffffu;
         if (q.y != 0u)
         {
            let sa = P.SpanAt + 2u * (q.z + plane * u32(2 * g.x + 1) + u32(dy + g.x));
            if (dx < bitcast<i32>(Stat[sa]) || dx > bitcast<i32>(Stat[sa + 1u])) { continue; }
         }
         let r0 = em.by + dy * g.y + g.w;
         let c0 = em.bx + dx * g.y + g.w;
         let base = plane * q.x;
         var s: f32;
         if (q.w == 0u) { s = taps0(base, r0, c0, g.z, em); }
         else if (q.w == 1u) { s = taps1(base, r0, c0, g.z, em); }
         else if (q.w == 2u) { s = taps2(base, r0, c0, g.z, em); }
         else { s = taps3(base, r0, c0, g.z, em); }
         acc += em.photons * s;
      }
      workgroupBarrier();
   }
   if (!inside) { return; }
   if (extra != 0xffffffffu) { acc += bitcast<f32>(Dyn[extra + i]); }

   // Noise: this pixel's own counter stream (a window's pixels at their place on the sensor).
   gFrame = frame;
   gPix = P.PixBase + i;
   gCtr = 0u;
   gK = 4u;
   gBuf = vec4<u32>(0u);
   let photons = max(acc, 0.0);
   var adu: f32;
   if (P.Emccd != 0u)
   {
      let ne = poisson(photons * P.Qe + P.Dark + P.Cic);
      var o = 0.0;
      if (ne > 0.0) { o = gammaDraw(ne); }
      adu = floor(px.x + (o + (px.z / P.EmGain) * gauss()) / px.y + 0.5);
      adu = clamp(adu, 0.0, P.MaxAdu);
   }
   else
   {
      let ne = poisson(photons * P.Qe + P.Dark);
      adu = px.x + (ne + px.z * gauss()) / px.y;
      adu = floor(clamp(adu, 0.0, 65535.0) + 0.5);
   }
   Out[gid.z * P.W * P.H + i] = u32(adu);
}
