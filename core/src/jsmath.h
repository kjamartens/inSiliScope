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
double hypot(double x, double y);          // V8 Math.hypot, 2 args
double hypot(double x, double y, double z); // V8 Math.hypot, 3 args (Kahan sum)
double pow(double x, double y);             // std::pow: NOT bit-exact with JS, see above
double sqrt(double x);                      // IEEE, exact everywhere

constexpr double PI = 3.141592653589793;
constexpr double LN2 = 0.6931471805599453;

} // namespace jsm
} // namespace isc
