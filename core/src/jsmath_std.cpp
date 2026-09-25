// CONTROL ONLY (ISC_STD_MATH=ON): the platform libm instead of the V8 fdlibm
// port, to measure how much bit-exactness depends on jsmath_fdlibm.cpp.
// Never ship a build with this.
#include "jsmath.h"

#include <cmath>

namespace isc {
namespace jsm {

double sin(double x) { return std::sin(x); }
double cos(double x) { return std::cos(x); }
double atan(double x) { return std::atan(x); }
double atan2(double y, double x) { return std::atan2(y, x); }
double exp(double x) { return std::exp(x); }
double log(double x) { return std::log(x); }

} // namespace jsm
} // namespace isc
