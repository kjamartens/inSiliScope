// Random numbers of the imaging path, mirroring the C++:
//   pcg4d / unit           core/src/rng.h, core/src/dyes.cpp Unit (address-based, bit-exact)
//   CounterRng & draws     adapter/inSiliScope/Simulation/SMLMCounterRng.h (camera noise)
//   Mt19937_64, gaussianRng std::mt19937_64 + SMLMNoise.cpp GaussianRng (static per-pixel maps)
// Part of the JS reference for imaging (web/prototype/scope/README.md).

// Pcg4d on uint32 words; returns [a, b, c, d] (uint32).
export function pcg4d(a, b, c, d) {
  a = (Math.imul(a, 1664525) + 1013904223) >>> 0;
  b = (Math.imul(b, 1664525) + 1013904223) >>> 0;
  c = (Math.imul(c, 1664525) + 1013904223) >>> 0;
  d = (Math.imul(d, 1664525) + 1013904223) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0;
  b = (b + Math.imul(c, a)) >>> 0;
  c = (c + Math.imul(a, b)) >>> 0;
  d = (d + Math.imul(b, c)) >>> 0;
  a = (a ^ (a >>> 16)) >>> 0; b = (b ^ (b >>> 16)) >>> 0; c = (c ^ (c >>> 16)) >>> 0; d = (d ^ (d >>> 16)) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0;
  b = (b + Math.imul(c, a)) >>> 0;
  c = (c + Math.imul(a, b)) >>> 0;
  d = (d + Math.imul(b, c)) >>> 0;
  return [a, b, c, d];
}

// pcg4d into a caller's Uint32Array(4) (CounterRng's buffer: no array per 4 uniforms).
export function pcg4dInto(out, a, b, c, d) {
  a = (Math.imul(a, 1664525) + 1013904223) >>> 0;
  b = (Math.imul(b, 1664525) + 1013904223) >>> 0;
  c = (Math.imul(c, 1664525) + 1013904223) >>> 0;
  d = (Math.imul(d, 1664525) + 1013904223) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0;
  b = (b + Math.imul(c, a)) >>> 0;
  c = (c + Math.imul(a, b)) >>> 0;
  d = (d + Math.imul(b, c)) >>> 0;
  a = (a ^ (a >>> 16)) >>> 0; b = (b ^ (b >>> 16)) >>> 0; c = (c ^ (c >>> 16)) >>> 0; d = (d ^ (d >>> 16)) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0;
  b = (b + Math.imul(c, a)) >>> 0;
  c = (c + Math.imul(a, b)) >>> 0;
  d = (d + Math.imul(b, c)) >>> 0;
  out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}

// pcg4d(...)[0] only (the common case): lane a's operations alone, in order (b, c, d
// enter it only through the products kept here), no array.
export function pcgA(a, b, c, d) {
  a = (Math.imul(a, 1664525) + 1013904223) >>> 0;
  b = (Math.imul(b, 1664525) + 1013904223) >>> 0;
  c = (Math.imul(c, 1664525) + 1013904223) >>> 0;
  d = (Math.imul(d, 1664525) + 1013904223) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0;
  b = (b + Math.imul(c, a)) >>> 0;
  c = (c + Math.imul(a, b)) >>> 0;
  d = (d + Math.imul(b, c)) >>> 0;
  a = (a ^ (a >>> 16)) >>> 0; b = (b ^ (b >>> 16)) >>> 0; d = (d ^ (d >>> 16)) >>> 0;
  return (a + Math.imul(b, d)) >>> 0;
}

// ((a >> 9) + 0.5) * 2^-23: a (0, 1) uniform from one word.
export function unit(a) { return ((a >>> 9) + 0.5) * 1.1920928955078125e-7; }

export function hashUnit(seed, cx, cy, k) { return unit(pcgA(seed >>> 0, cx >>> 0, cy >>> 0, k >>> 0)); }

// ---- counter-based camera noise (SMLMCounterRng.h) ----
export class CounterRng {
  constructor(seed, frame) { this.seed = seed >>> 0; this.frame = frame >>> 0; this.pix = 0; this.ctr = 0; this.k = 4; this.buf = new Uint32Array(4); }
  pixel(p) { this.pix = p >>> 0; this.ctr = 0; this.k = 4; }
  uniform() {
    if (this.k === 4) {
      pcg4dInto(this.buf, this.seed, this.frame, this.pix, this.ctr);
      this.ctr = (this.ctr + 1) >>> 0;
      this.k = 0;
    }
    return unit(this.buf[this.k++]);
  }
}

export function counterGauss(u) {
  const r = Math.sqrt(-2.0 * Math.log(u.uniform()));
  return r * Math.cos(6.283185307179586 * u.uniform());
}

export function counterPoisson(l, u) {
  if (l > 60.0) return Math.max(0.0, Math.floor(l + Math.sqrt(l) * counterGauss(u) + 0.5));
  if (!(l > 0.0)) return 0.0;
  const L = Math.exp(-l);
  let k = 0, p = 1.0;
  do { ++k; p *= u.uniform(); } while (p > L && k < 1000);
  return k - 1;
}

export function counterGamma(k, u) {
  const d = k - 1.0 / 3.0, c = 1.0 / Math.sqrt(9.0 * d);
  for (let it = 0; it < 1000; ++it) {
    const x = counterGauss(u), t = 1.0 + c * x;
    if (t <= 0.0) continue;
    const v = t * t * t, w = u.uniform();
    if (w < 1.0 - 0.0331 * x * x * x * x) return d * v;
    if (Math.log(w) < 0.5 * x * x + d * (1.0 - v + Math.log(v))) return d * v;
  }
  return d;
}

// ---- std::mt19937_64 + std::uniform_real_distribution<double>(0, 1) ----
// 64-bit words as (hi, lo) uint32 pairs (no BigInt: the per-pixel maps draw
// millions of these). Every step is the generator's own integer arithmetic.
// (hi, lo) of a * b mod 2^64, a and b as (hi, lo).
function mul64(ahi, alo, bhi, blo, out) {
  const a0 = alo & 0xffff, a1 = alo >>> 16, b0 = blo & 0xffff, b1 = blo >>> 16;
  const p00 = a0 * b0, mid = a0 * b1 + a1 * b0, p11 = a1 * b1;   // exact (< 2^33)
  const midLo = mid % 65536, midHi = Math.floor(mid / 65536);
  const lo = p00 + midLo * 65536;                                 // < 2^33, exact
  const carry = Math.floor(lo / 4294967296);
  out[1] = lo >>> 0;
  out[0] = (p11 + midHi + carry + Math.imul(ahi, blo) + Math.imul(alo, bhi)) >>> 0;
}
export class Mt19937_64 {
  constructor(seed) {
    const n = 312, hi = new Uint32Array(n), lo = new Uint32Array(n);
    // seed as uint64 (two's complement for a negative seed), |seed| < 2^53.
    hi[0] = Math.floor(seed / 4294967296) >>> 0;
    lo[0] = seed >>> 0;
    const prod = new Uint32Array(2);
    for (let i = 1; i < n; i++) {
      // p ^ (p >> 62): the top two bits of p land in lo's bits 0..1.
      const phi = hi[i - 1], plo = lo[i - 1];
      const xhi = phi, xlo = (plo ^ (phi >>> 30)) >>> 0;
      mul64(0x5851F42D, 0x4C957F2D, xhi, xlo, prod);           // 6364136223846793005
      const sum = prod[1] + i;                                     // + i, with carry into hi
      lo[i] = sum >>> 0;
      hi[i] = (prod[0] + Math.floor(sum / 4294967296)) >>> 0;
    }
    this.hi = hi; this.lo = lo; this.idx = n;
  }
  twist() {
    const hi = this.hi, lo = this.lo;
    for (let i = 0; i < 312; i++) {
      const j = (i + 1) % 312, k = (i + 156) % 312;
      // x = (mt[i] & 0xFFFFFFFF80000000) | (mt[i+1] & 0x7FFFFFFF)
      const xhi = hi[i], xlo = ((lo[i] & 0x80000000) | (lo[j] & 0x7FFFFFFF)) >>> 0;
      // xA = x >> 1, then ^= 0xB5026F5AA96619E9 if x is odd
      let ahi = xhi >>> 1, alo = ((xlo >>> 1) | ((xhi & 1) << 31)) >>> 0;
      if (xlo & 1) { ahi = (ahi ^ 0xB5026F5A) >>> 0; alo = (alo ^ 0xA96619E9) >>> 0; }
      hi[i] = (hi[k] ^ ahi) >>> 0;
      lo[i] = (lo[k] ^ alo) >>> 0;
    }
    this.idx = 0;
  }
  // The tempered 64-bit output as (hi, lo) in this.outHi / this.outLo.
  next() {
    if (this.idx >= 312) this.twist();
    let xhi = this.hi[this.idx], xlo = this.lo[this.idx];
    this.idx++;
    // x ^= (x >> 29) & 0x5555555555555555
    let shi = xhi >>> 29, slo = ((xlo >>> 29) | (xhi << 3)) >>> 0;
    xhi = (xhi ^ (shi & 0x55555555)) >>> 0; xlo = (xlo ^ (slo & 0x55555555)) >>> 0;
    // x ^= (x << 17) & 0x71D67FFFEDA60000
    shi = ((xhi << 17) | (xlo >>> 15)) >>> 0; slo = (xlo << 17) >>> 0;
    xhi = (xhi ^ (shi & 0x71D67FFF)) >>> 0; xlo = (xlo ^ (slo & 0xEDA60000)) >>> 0;
    // x ^= (x << 37) & 0xFFF7EEE000000000 (the low word of the shifted value is 0)
    shi = (xlo << 5) >>> 0;
    xhi = (xhi ^ (shi & 0xFFF7EEE0)) >>> 0;
    // x ^= x >> 43 (lands in the low word only)
    xlo = (xlo ^ (xhi >>> 11)) >>> 0;
    this.outHi = xhi; this.outLo = xlo;
  }
  // generate_canonical<double, 53>: double(x) / 2^64 (round to nearest), kept below 1.
  // hi * 2^32 is exact and the one rounding of + lo is the rounding of the integer.
  uniform() {
    this.next();
    const u = (this.outHi * 4294967296 + this.outLo) / 18446744073709551616;
    return u >= 1 ? 1 - 2 ** -53 : u;
  }
}

// SMLMNoise.cpp GaussianRng: Box-Muller, u1 then u2.
export function gaussianRng(rng, mean, std) {
  let u1 = rng.uniform();
  if (u1 < 1e-300) u1 = 1e-300;
  const u2 = rng.uniform();
  const z0 = Math.sqrt(-2.0 * Math.log(u1)) * Math.cos(2.0 * Math.PI * u2);
  return mean + std * z0;
}
