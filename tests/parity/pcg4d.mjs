// Standalone copy of the prototype's pcg4d, only for building test inputs.
export function pcg4d(a, b, c, d) {
  a = Math.imul(a, 1664525) + 1013904223 | 0;
  b = Math.imul(b, 1664525) + 1013904223 | 0;
  c = Math.imul(c, 1664525) + 1013904223 | 0;
  d = Math.imul(d, 1664525) + 1013904223 | 0;
  a = a + Math.imul(b, d) | 0;
  b = b + Math.imul(c, a) | 0;
  c = c + Math.imul(a, b) | 0;
  d = d + Math.imul(b, c) | 0;
  a ^= a >>> 16; b ^= b >>> 16; c ^= c >>> 16; d ^= d >>> 16;
  a = a + Math.imul(b, d) | 0;
  b = b + Math.imul(c, a) | 0;
  c = c + Math.imul(a, b) | 0;
  d = d + Math.imul(b, c) | 0;
  return [a >>> 0, b >>> 0, c >>> 0, d >>> 0];
}
