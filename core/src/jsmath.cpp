// Non-fdlibm parts of jsmath.h. The fdlibm functions are generated into
// jsmath_fdlibm.cpp by tools/gen_jsmath.py.
#include "jsmath.h"

#include <cmath>
#include <limits>

namespace isc {
namespace jsm {

// V8's two-argument Math.hypot (src/builtins/math.tq, FastMathHypot).
double hypot(double x, double y)
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

double sqrt(double x) { return std::sqrt(x); }

} // namespace jsm
} // namespace isc
