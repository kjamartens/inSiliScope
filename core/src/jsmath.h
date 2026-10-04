// Transcendentals that reproduce V8's Math.* bit for bit.
//
// V8 implements Math.sin/cos/atan/atan2/exp/log with fdlibm (base::ieee754)
// and Math.hypot with a scaled sum. The platform libm (MSVC CRT, glibc,
// musl) can differ from that in the last ulp, and packing/pruning decisions
// are sensitive to such bits (an overlap of +1e-17 vs -1e-17 flips a prune).
// So core never calls std::sin & co. in decision paths; it calls these,
// which are the same code on every compiler and in WASM.
//
// pow is the exception: V8 defaults to --use-std-math-pow, i.e. JS Math.pow
// is the host platform's std::pow and not the same across browsers or OSes.
// jsm::pow is std::pow too, so it is only "near" the JS (bit-exact when the
// host libm is the same one, e.g. glibc under both Node and native Linux).
#pragma once

#include <cmath>
#include <limits>

namespace isc {
namespace jsm {

double sin(double x);
double cos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double cbrt(double x);
double hypot(double x, double y, double z); // V8 Math.hypot, 3 args (Kahan sum)
double pow(double x, double y);             // std::pow: NOT bit-exact with JS, see above

// IEEE sqrt: exact everywhere. Inline (it is called per sample in the hot loops).
inline double sqrt(double x) { return std::sqrt(x); }

// V8's two-argument Math.hypot (src/builtins/math.tq, FastMathHypot). Inline:
// the packing's clearance loop calls it ~34 times per cell pair.
inline double hypot(double x, double y)
{
   const double a = std::fabs(x), b = std::fabs(y);
   const double inf = std::numeric_limits<double>::infinity();
   if (a == inf || b == inf) return inf;
   if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
   const double max = a > b ? a : b;
   if (max == 0) return 0;
   const double an = a / max, bn = b / max;
   return std::sqrt(an * an + bn * bn) * max;
}

constexpr double PI = 3.141592653589793;
constexpr double LN2 = 0.6931471805599453;

} // namespace jsm
} // namespace isc
