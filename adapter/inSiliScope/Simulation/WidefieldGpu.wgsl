// WidefieldGpu.wgsl -- the WideField GPU kernels (spec/PORT.md 13), ONE source
// for both GPU paths: WebGPU in the viewer (web/wf_gpu.js) and Direct3D 11 in
// the adapter (Simulation/WidefieldGpuD3D11.cpp), whose HLSL is generated from
// this file by naga (tools/gen_wf_gpu.mjs; never hand-edit the generated
// WidefieldGpuHlsl.inc).
//
// The file is a set of modules: "//// common" is prepended to every
// "//// kernel <name>" section, and each section is compiled on its own (its
// bindings, group 0, are its own). The entry point is named like the section.
//
// Pipeline (WidefieldScene::MakeGpuJob describes the work):
//   wf_clear, wf_scatter    two dye planes' weighted counts -> one complex grid
//                            (plane A real, plane B imaginary)
//   wf_fft (rows, columns)  radix-2 in workgroup memory, n <= 2048
//   wf_split                Hermitian split into the two planes' half spectra,
//                            stored as fp16 pairs scaled by 1 / sum|values|
//   wf_mac                  per channel: sum over its dye planes of
//                            A_k (w0 K_p0 + w1 K_p1)   (the focus re-pairing)
//   wf_expand               two channels -> one full complex spectrum with the
//                            sub-cell phase ramp (Nyquist bins: cosine)
//   wf_fft (inverse)        rows, columns
//   wf_crop                 the FOV cells of both channels (1 / (nx ny))
//   wf_frame                bin(max(0, P + sum_j a_j B_j)) + background, then
//                            the camera noise chain -- the counter-based pcg4d
//                            stream of SMLMCounterRng.h, mirrored LINE FOR LINE
//                            (change one, change all three: this, the HLSL of
//                            GpuSimD3D11.cpp, SMLMCounterRng.h)

//// common

const kNone: u32 = 0xffffffffu;

fn cmul(a: vec2<f32>, b: vec2<f32>) -> vec2<f32> {
   return vec2<f32>(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

fn conj2(a: vec2<f32>) -> vec2<f32> {
   return vec2<f32>(a.x, -a.y);
}

//// kernel wf_clear

struct ClearParams { count: u32, rowLen: u32, p2: u32, p3: u32 }
@group(0) @binding(0) var<uniform> P: ClearParams;
@group(0) @binding(1) var<storage, read_write> data: array<vec2<f32>>;

@compute @workgroup_size(256)
fn wf_clear(@builtin(global_invocation_id) g: vec3<u32>) {
   let i = g.x + g.y * P.rowLen;
   if (i < P.count) {
      data[i] = vec2<f32>(0.0, 0.0);
   }
}

//// kernel wf_scatter

// entries: (grid slot, cell = y * nx + x, bitcast value, component 0 re / 1 im)
struct ScatterParams { count: u32, nx: u32, NX: u32, planeStride: u32, rowLen: u32, p1: u32, p2: u32, p3: u32 }
@group(0) @binding(0) var<uniform> P: ScatterParams;
@group(0) @binding(1) var<storage, read> entries: array<vec4<u32>>;
@group(0) @binding(2) var<storage, read_write> data: array<vec2<f32>>;

@compute @workgroup_size(256)
fn wf_scatter(@builtin(global_invocation_id) g: vec3<u32>) {
   let i = g.x + g.y * P.rowLen;
   if (i >= P.count) {
      return;
   }
   let e = entries[i];
   let x = e.y % P.nx;
   let y = e.y / P.nx;
   let idx = e.x * P.planeStride + y * P.NX + x;
   let v = bitcast<f32>(e.z);
   if (e.w == 0u) {
      data[idx].x = v;
   } else {
      data[idx].y = v;
   }
}

//// kernel wf_fft

// Transform t (workgroup x) of plane z: elements base + i * elemStride,
// base = z * planeStride + t * xfStride. Iterative radix-2 in workgroup
// memory (bit-reversed load); twiddles exp(-2 pi i k / n) at twOff.
struct FftParams { n: u32, logn: u32, elemStride: u32, xfStride: u32, planeStride: u32, inverse: u32, twOff: u32, p7: u32 }
@group(0) @binding(0) var<uniform> P: FftParams;
@group(0) @binding(1) var<storage, read_write> data: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> tw: array<vec2<f32>>;

var<workgroup> buf: array<vec2<f32>, 2048>;

@compute @workgroup_size(256)
fn wf_fft(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {
   let base = wid.z * P.planeStride + wid.x * P.xfStride;
   let shift = 32u - P.logn;
   for (var i = lid.x; i < P.n; i += 256u) {
      buf[reverseBits(i) >> shift] = data[base + i * P.elemStride];
   }
   workgroupBarrier();
   var half = 1u;
   for (var s = 0u; s < P.logn; s++) {
      let step = P.n / (2u * half);
      for (var j = lid.x; j < P.n / 2u; j += 256u) {
         let k = j % half;
         let i0 = (j / half) * 2u * half + k;
         let i1 = i0 + half;
         var w = tw[P.twOff + k * step];
         if (P.inverse != 0u) {
            w.y = -w.y;
         }
         let a = buf[i0];
         let b = cmul(buf[i1], w);
         buf[i0] = a + b;
         buf[i1] = a - b;
      }
      workgroupBarrier();
      half = half * 2u;
   }
   for (var i = lid.x; i < P.n; i += 256u) {
      data[base + i * P.elemStride] = buf[i];
   }
}

//// kernel wf_split

// Grid pair z holds Z = F(a + i b); A = (Z[k] + conj Z[-k]) / 2,
// B = (Z[k] - conj Z[-k]) / 2i, stored (half spectrum, kx <= NX/2) as fp16
// pairs times invScale. slots[z] = (slot A, slot B or none, invScale A,
// invScale B as bits).
struct SplitParams { NX: u32, NY: u32, W: u32, SS: u32, PS: u32, pairs: u32, p6: u32, p7: u32 }
@group(0) @binding(0) var<uniform> P: SplitParams;
@group(0) @binding(1) var<storage, read> data: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> slots: array<vec4<u32>>;
@group(0) @binding(3) var<storage, read_write> spec: array<u32>;

@compute @workgroup_size(16, 16)
fn wf_split(@builtin(global_invocation_id) g: vec3<u32>) {
   let kx = g.x;
   let ky = g.y;
   if (kx >= P.W || ky >= P.NY) {
      return;
   }
   let z = g.z;
   let base = z * P.PS;
   let zk = data[base + ky * P.NX + kx];
   let zm = conj2(data[base + ((P.NY - ky) % P.NY) * P.NX + (P.NX - kx) % P.NX]);
   let s = slots[z];
   let bin = ky * P.W + kx;
   let a = 0.5 * (zk + zm);
   spec[s.x * P.SS + bin] = pack2x16float(a * bitcast<f32>(s.z));
   if (s.y != kNone) {
      let d = 0.5 * (zk - zm);
      // (Zk - conj Zm) / 2i
      spec[s.y * P.SS + bin] = pack2x16float(vec2<f32>(d.y, -d.x) * bitcast<f32>(s.w));
   }
}

//// kernel wf_mac

// Channel z: bins of sum over deps[chan.x .. chan.x + chan.y) of
// A_plane * scale (w0 K_k0 + w1 K_k1). depI = (plane slot, k0, k1 or none,
// scale bits), depW = (w0, w1).
struct MacParams { SS: u32, nChan: u32, rowLen: u32, p3: u32 }
@group(0) @binding(0) var<uniform> P: MacParams;
@group(0) @binding(1) var<storage, read> spec: array<u32>;
@group(0) @binding(2) var<storage, read> kspec: array<vec2<f32>>;
@group(0) @binding(3) var<storage, read> depI: array<vec4<u32>>;
@group(0) @binding(4) var<storage, read> depW: array<vec2<f32>>;
@group(0) @binding(5) var<storage, read> chans: array<vec2<u32>>;
@group(0) @binding(6) var<storage, read_write> out: array<vec2<f32>>;

@compute @workgroup_size(256)
fn wf_mac(@builtin(global_invocation_id) g: vec3<u32>) {
   let bin = g.x + g.y * P.rowLen;
   if (bin >= P.SS) {
      return;
   }
   let c = chans[g.z];
   var acc = vec2<f32>(0.0, 0.0);
   for (var d = c.x; d < c.x + c.y; d++) {
      let di = depI[d];
      let dw = depW[d];
      let a = unpack2x16float(spec[di.x * P.SS + bin]) * bitcast<f32>(di.w);
      var k = dw.x * kspec[di.y * P.SS + bin];
      if (di.z != kNone) {
         k = k + dw.y * kspec[di.z * P.SS + bin];
      }
      acc = acc + cmul(a, k);
   }
   out[g.z * P.SS + bin] = acc;
}

//// kernel wf_expand

// Pair z = (channel c1, channel c2 or none): Z = S1 ph + i S2 ph over the
// whole grid (the other half from Hermitian symmetry); ph = exp(+2 pi i kx
// fracX / NX) exp(+2 pi i ky fracY / NY), each factor's cosine on its axis's
// Nyquist bin (so the shifted spectrum stays Hermitian).
struct ExpandParams { NX: u32, NY: u32, W: u32, SS: u32, PS: u32, pairs: u32, fracX: f32, fracY: f32 }
@group(0) @binding(0) var<uniform> P: ExpandParams;
@group(0) @binding(1) var<storage, read> chan: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> pairs: array<vec2<u32>>;
@group(0) @binding(3) var<storage, read_write> data: array<vec2<f32>>;

fn halfBin(c: u32, x: u32, y: u32) -> vec2<f32> {
   if (x < P.W) {
      return chan[c * P.SS + y * P.W + x];
   }
   return conj2(chan[c * P.SS + ((P.NY - y) % P.NY) * P.W + (P.NX - x)]);
}

@compute @workgroup_size(16, 16)
fn wf_expand(@builtin(global_invocation_id) g: vec3<u32>) {
   let x = g.x;
   let y = g.y;
   if (x >= P.NX || y >= P.NY) {
      return;
   }
   let pr = pairs[g.z];
   var kx = f32(x);
   if (x > P.NX / 2u) {
      kx = kx - f32(P.NX);
   }
   var ky = f32(y);
   if (y > P.NY / 2u) {
      ky = ky - f32(P.NY);
   }
   // Separable per axis, the cosine on an axis's Nyquist bin (as the CPU).
   let ax = 6.283185307179586 * kx * P.fracX / f32(P.NX);
   let ay = 6.283185307179586 * ky * P.fracY / f32(P.NY);
   var px = vec2<f32>(cos(ax), sin(ax));
   if (x == P.NX / 2u) {
      px.y = 0.0;
   }
   var py = vec2<f32>(cos(ay), sin(ay));
   if (y == P.NY / 2u) {
      py.y = 0.0;
   }
   let ph = cmul(px, py);
   let s1 = cmul(halfBin(pr.x, x, y), ph);
   var z = s1;
   if (pr.y != kNone) {
      let s2 = cmul(halfBin(pr.y, x, y), ph);
      z = vec2<f32>(s1.x - s2.y, s1.y + s2.x);
   }
   data[g.z * P.PS + y * P.NX + x] = z;
}

//// kernel wf_crop

// Pair z's FOV cells: real part -> image of c1, imaginary -> image of c2
// (images are cw x ch, channel c at c * cw * ch), times 1 / (NX NY).
struct CropParams { NX: u32, PS: u32, fovX0: u32, fovY0: u32, cw: u32, ch: u32, scale: f32, p7: u32 }
@group(0) @binding(0) var<uniform> P: CropParams;
@group(0) @binding(1) var<storage, read> data: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> pairs: array<vec2<u32>>;
@group(0) @binding(3) var<storage, read_write> imgs: array<f32>;

@compute @workgroup_size(16, 16)
fn wf_crop(@builtin(global_invocation_id) g: vec3<u32>) {
   if (g.x >= P.cw || g.y >= P.ch) {
      return;
   }
   let pr = pairs[g.z];
   let v = data[g.z * P.PS + (P.fovY0 + g.y) * P.NX + P.fovX0 + g.x] * P.scale;
   let i = g.y * P.cw + g.x;
   let cs = P.cw * P.ch;
   imgs[pr.x * cs + i] = v.x;
   if (pr.y != kNone) {
      imgs[pr.y * cs + i] = v.y;
   }
}

//// kernel wf_frame

// Camera pixel (x, y) of batch frame z: the dyes (images: [persistent],
// bleach maps; coef: nB per frame) binned over upscale^2 cells, plus the
// background, then the sCMOS or EMCCD noise chain (ApplyNoiseChain).
struct FrameParams {
   W: u32, H: u32, cw: u32, u: u32,
   nB: u32, hasP: u32, nFrames: u32, seed: u32,
   emccd: u32, qe: f32, dark: f32, cic: f32,
   emGain: f32, maxAdu: f32, p14: u32, p15: u32,
}
@group(0) @binding(0) var<uniform> P: FrameParams;
@group(0) @binding(1) var<storage, read> imgs: array<f32>;
@group(0) @binding(2) var<storage, read> coef: array<f32>;
@group(0) @binding(3) var<storage, read> frames: array<vec2<u32>>; // frame id, background scale bits
@group(0) @binding(4) var<storage, read> pix: array<vec4<f32>>;   // offset ADU, gain photons/ADU, read noise e-, background
@group(0) @binding(5) var<storage, read_write> out: array<u32>;

var<private> gFrame: u32;
var<private> gPix: u32;
var<private> gCtr: u32;
var<private> gBuf: vec4<u32>;
var<private> gK: u32;

fn pcg4d(vin: vec4<u32>) -> vec4<u32> {
   var v = vin * 1664525u + vec4<u32>(1013904223u);
   v.x += v.y * v.w;
   v.y += v.z * v.x;
   v.z += v.x * v.y;
   v.w += v.y * v.z;
   v = v ^ (v >> vec4<u32>(16u));
   v.x += v.y * v.w;
   v.y += v.z * v.x;
   v.z += v.x * v.y;
   v.w += v.y * v.z;
   return v;
}

fn uniform01() -> f32 {
   if (gK == 4u) {
      gBuf = pcg4d(vec4<u32>(P.seed, gFrame, gPix, gCtr));
      gCtr = gCtr + 1u;
      gK = 0u;
   }
   let x = gBuf[gK];
   gK = gK + 1u;
   return (f32(x >> 9u) + 0.5) * 1.1920928955078125e-7;
}

fn gauss() -> f32 {
   let r = sqrt(-2.0 * log(uniform01()));
   return r * cos(6.283185307179586 * uniform01());
}

fn poisson(l: f32) -> f32 {
   if (l > 60.0) {
      return max(0.0, floor(l + sqrt(l) * gauss() + 0.5));
   }
   if (!(l > 0.0)) {
      return 0.0;
   }
   let L = exp(-l);
   var k = 0;
   var p = 1.0;
   loop {
      k = k + 1;
      p = p * uniform01();
      if (!(p > L && k < 1000)) {
         break;
      }
   }
   return f32(k - 1);
}

fn gammaDraw(k: f32) -> f32 {
   let d = k - 1.0 / 3.0;
   let c = 1.0 / sqrt(9.0 * d);
   for (var it = 0; it < 1000; it++) {
      let x = gauss();
      let t = 1.0 + c * x;
      if (t <= 0.0) {
         continue;
      }
      let v = t * t * t;
      let w = uniform01();
      if (w < 1.0 - 0.0331 * x * x * x * x) {
         return d * v;
      }
      if (log(w) < 0.5 * x * x + d * (1.0 - v + log(v))) {
         return d * v;
      }
   }
   return d;
}

@compute @workgroup_size(16, 16)
fn wf_frame(@builtin(global_invocation_id) g: vec3<u32>) {
   if (g.x >= P.W || g.y >= P.H || g.z >= P.nFrames) {
      return;
   }
   let i = g.y * P.W + g.x;
   let px = pix[i];
   let fr = frames[g.z];
   let cs = P.cw * (P.H * P.u);
   var dyes = 0.0;
   for (var sy = 0u; sy < P.u; sy++) {
      for (var sx = 0u; sx < P.u; sx++) {
         let c = (g.y * P.u + sy) * P.cw + g.x * P.u + sx;
         var v = 0.0;
         if (P.hasP != 0u) {
            v = imgs[c];
         }
         for (var j = 0u; j < P.nB; j++) {
            v = v + coef[g.z * P.nB + j] * imgs[(P.hasP + j) * cs + c];
         }
         dyes = dyes + max(0.0, v);
      }
   }
   let photons = max(px.w * bitcast<f32>(fr.y) + dyes, 0.0);

   gFrame = fr.x;
   gPix = i;
   gCtr = 0u;
   gK = 4u;
   gBuf = vec4<u32>(0u, 0u, 0u, 0u);
   var adu: f32;
   if (P.emccd != 0u) {
      let ne = poisson(photons * P.qe + P.dark + P.cic);
      var o = 0.0;
      if (ne > 0.0) {
         o = gammaDraw(ne);
      }
      adu = floor(px.x + (o + (px.z / P.emGain) * gauss()) / px.y + 0.5);
      adu = clamp(adu, 0.0, P.maxAdu);
   } else {
      let ne = poisson(photons * P.qe + P.dark);
      adu = px.x + (ne + px.z * gauss()) / px.y;
      adu = floor(clamp(adu, 0.0, 65535.0) + 0.5);
   }
   out[g.z * P.W * P.H + i] = u32(adu);
}
