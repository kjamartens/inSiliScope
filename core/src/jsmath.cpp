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

// V8's generic Math.hypot (math.tq MathHypot): normalise by the max, then a
// Kahan-compensated sum of squares. For two arguments the compensation is
// always 0, so this equals the 2-arg version above.
double hypot(double x, double y, double z)
{
   const double v[3] = { std::fabs(x), std::fabs(y), std::fabs(z) };
   const double inf = std::numeric_limits<double>::infinity();
   bool nan = false;
   double max = 0;
   for (double a : v) {
      if (std::isnan(a)) nan = true;
      else if (a > max) max = a;
   }
   if (max == inf) return inf;
   if (nan) return std::numeric_limits<double>::quiet_NaN();
   if (max == 0) return 0;
   double sum = 0, compensation = 0;
   for (double a : v) {
      const double n = a / max;
      const double summand = n * n - compensation;
      const double preliminary = sum + summand;
      compensation = (preliminary - sum) - summand;
      sum = preliminary;
   }
   return std::sqrt(sum) * max;
}

double sqrt(double x) { return std::sqrt(x); }

double pow(double x, double y) { return std::pow(x, y); }

} // namespace jsm
} // namespace isc
