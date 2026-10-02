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

// pcg4d(...)[0] only (the common case).
export function pcgA(a, b, c, d) { return pcg4d(a, b, c, d)[0]; }

// ((a >> 9) + 0.5) * 2^-23: a (0, 1) uniform from one word.
export function unit(a) { return ((a >>> 9) + 0.5) * 1.1920928955078125e-7; }

export function hashUnit(seed, cx, cy, k) { return unit(pcgA(seed >>> 0, cx >>> 0, cy >>> 0, k >>> 0)); }

// ---- counter-based camera noise (SMLMCounterRng.h) ----
export class CounterRng {
  constructor(seed, frame) { this.seed = seed >>> 0; this.frame = frame >>> 0; this.pix = 0; this.ctr = 0; this.k = 4; this.buf = null; }
  pixel(p) { this.pix = p >>> 0; this.ctr = 0; this.k = 4; }
  uniform() {
    if (this.k === 4) {
      this.buf = pcg4d(this.seed, this.frame, this.pix, this.ctr);
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
const M64 = (1n << 64n) - 1n;
export class Mt19937_64 {
  constructor(seed) {
    this.mt = new Array(312);
    this.mt[0] = BigInt.asUintN(64, BigInt(seed));
    for (let i = 1; i < 312; i++) {
      const p = this.mt[i - 1];
      this.mt[i] = (6364136223846793005n * (p ^ (p >> 62n)) + BigInt(i)) & M64;
    }
    this.idx = 312;
  }
  twist() {
    const mt = this.mt;
    for (let i = 0; i < 312; i++) {
      const x = (mt[i] & 0xFFFFFFFF80000000n) | (mt[(i + 1) % 312] & 0x7FFFFFFFn);
      let xA = x >> 1n;
      if (x & 1n) xA ^= 0xB5026F5AA96619E9n;
      mt[i] = mt[(i + 156) % 312] ^ xA;
    }
    this.idx = 0;
  }
  next() {
    if (this.idx >= 312) this.twist();
    let x = this.mt[this.idx++];
    x ^= (x >> 29n) & 0x5555555555555555n;
    x ^= (x << 17n) & 0x71D67FFFEDA60000n;
    x ^= (x << 37n) & 0xFFF7EEE000000000n;
    x ^= x >> 43n;
    return x & M64;
  }
  // generate_canonical<double, 53>: double(x) / 2^64 (round to nearest), kept below 1.
  uniform() {
    const u = Number(this.next()) / 18446744073709551616;
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
