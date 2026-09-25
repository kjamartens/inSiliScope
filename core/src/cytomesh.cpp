#include "cytomesh.h"

#include "jsmath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace isc {

namespace {
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

double Smoothstep(double t)
{
   t = std::max(0.0, std::min(1.0, t));
   return t * t * (3 - 2 * t);
}

double CytoSlopeCeiling(const Cell& c, const Params& p, double dEdge)
{
   const double s = p.cytoMaxSlope, Hc = 2 * c.cytoMidHeight;
   return Hc * (1 - jsm::exp(-s * dEdge / Hc));
}
} // namespace

double JsRound(double x)
{
   const double r = std::floor(x);
   return x - r >= 0.5 ? r + 1 : r;
}

std::vector<Pt2> CellOutlineLocal(const Cell& c, int n)
{
   std::vector<Pt2> pts((size_t)n);
   for (int i = 0; i < n; i++) {
      const double th = ((double)i / n) * jsm::PI * 2;
      const double r = CellRadiusAt(c, th);
      pts[i] = { jsm::cos(th) * r, jsm::sin(th) * r };
   }
   return pts;
}

double NearestDistToOutline(const std::vector<Pt2>& pts, double x, double y)
{
   double best = std::numeric_limits<double>::infinity();
   const size_t n = pts.size();
   for (size_t i = 0, j = n - 1; i < n; j = i++) {
      const double ax = pts[j].x, ay = pts[j].y, bx = pts[i].x, by = pts[i].y;
      const double dx = bx - ax, dy = by - ay;
      const double len2 = dx * dx + dy * dy;
      double t = len2 > 1e-12 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0;
      t = std::max(0.0, std::min(1.0, t));
      const double px = ax + dx * t, py = ay + dy * t;
      const double d = jsm::hypot(x - px, y - py);
      if (d < best) best = d;
   }
   return best;
}

// Eberly's method, Newton safeguarded by bisection (see the JS comment).
Pt2 NearestPointOnEllipse(double a, double b, double x, double y)
{
   bool swapped = false;
   if (a < b) { std::swap(a, b); std::swap(x, y); swapped = true; }
   const double sx = x < 0 ? -1 : 1, sy = y < 0 ? -1 : 1;
   const double x0 = std::fabs(x), y0 = std::fabs(y);
   double ex, ey;
   if (y0 > 1e-9) {
      if (x0 > 1e-9) {
         double lo = -b * b, hi = a * x0 + b * y0;
         double t = (lo + hi) / 2;
         for (int it = 0; it < 80; it++) {
            const double ta = t + a * a, tb = t + b * b;
            const double rx = (a * x0) / ta, ry = (b * y0) / tb;
            const double f = rx * rx + ry * ry - 1;
            if (f > 0) lo = t; else hi = t;
            if (std::fabs(f) < 1e-12) break;
            const double df = -2 * ((a * a * x0 * x0) / (ta * ta * ta) + (b * b * y0 * y0) / (tb * tb * tb));
            double tNext = df != 0 ? t - f / df : (lo + hi) / 2;
            if (!(tNext > lo && tNext < hi)) tNext = (lo + hi) / 2;
            t = tNext;
         }
         ex = (a * a * x0) / (t + a * a);
         ey = (b * b * y0) / (t + b * b);
      } else {
         ex = 0; ey = b;
      }
   } else {
      const double numer = a * x0, denom = a * a - b * b;
      if (denom > 1e-9 && numer < denom) {
         const double xde = numer / denom;
         ex = a * xde; ey = b * jsm::sqrt(std::max(0.0, 1 - xde * xde));
      } else {
         ex = a; ey = 0;
      }
   }
   ex *= sx; ey *= sy;
   if (swapped) std::swap(ex, ey);
   return { ex, ey };
}

double NucleusSignedDistLocal(const Cell& c, double lx0, double ly0)
{
   const double dx = lx0 - c.nucOffX, dy = ly0 - c.nucOffY;
   const double cr = jsm::cos(-c.nucRot), sr = jsm::sin(-c.nucRot);
   const double lx = dx * cr - dy * sr, ly = dx * sr + dy * cr;
   const double a = c.nucLong / 2, b = c.nucShort / 2;
   const Pt2 e = NearestPointOnEllipse(a, b, lx, ly);
   const double dist = jsm::hypot(lx - e.x, ly - e.y);
   const bool inside = (lx * lx) / (a * a) + (ly * ly) / (b * b) < 1;
   return inside ? -dist : dist;
}

double CytoHeightAt(const Cell& c, const Params& p, double dEdge, double dNuc)
{
   const double riseScale = std::max(1e-6, c.cytoEdgeRise);
   const bool slopeOn = p.cytoMaxSlope > 0;
   const double inner = CytoDomeReach(c, p);
   double midDist = std::max(0.0, c.cytoMidDistFrac) * c.rOuter;
   if (slopeOn) midDist = std::max(midDist, 1.5 * std::max(0.0, c.cytoMidHeight - c.cytoRimHeight) / p.cytoMaxSlope);
   const double hEdge = c.cytoRimHeight * (1 - jsm::exp(-dEdge / riseScale));
   double hEnv = 0;
   if (dNuc <= 0) hEnv = c.height;
   else if (dNuc < inner) hEnv = lerp(c.cytoMidHeight, c.height, Smoothstep(1 - dNuc / inner));
   double hCyto;
   if (dNuc < inner) hCyto = c.cytoMidHeight;
   else if (dNuc < inner + midDist) hCyto = lerp(c.cytoRimHeight, c.cytoMidHeight, Smoothstep(1 - (dNuc - inner) / std::max(1e-6, midDist)));
   else hCyto = c.cytoRimHeight;
   hCyto = std::max(hEdge, hCyto);
   if (slopeOn) hCyto = std::min(hCyto, CytoSlopeCeiling(c, p, dEdge));
   const double h = std::min(c.height, std::max(hCyto, hEnv));
   const double edgeCeiling = c.height * (1 - jsm::exp(-dEdge / riseScale));
   return std::min(h, edgeCeiling);
}

namespace {
// 3x3 binomial passes over rings 1..rings-1; theta wraps, rings 0 and
// `rings` stay (the edge must stay pinned at 0).
void SmoothCytoGrid(CytoMesh& m, double passes)
{
   const int rings = m.rings, n = m.n;
   std::vector<double> next((size_t)(rings + 1) * n);
   for (int pass = 0; pass < passes; pass++) {
      for (int k = 1; k < rings; k++) {
         const int km = k - 1, kp = k + 1;
         for (int i = 0; i < n; i++) {
            const int im = (i - 1 + n) % n, ip = (i + 1) % n;
            const double sum = m.H(k, i) * 4
               + (m.H(k, im) + m.H(k, ip) + m.H(km, i) + m.H(kp, i)) * 2
               + m.H(km, im) + m.H(km, ip) + m.H(kp, im) + m.H(kp, ip);
            next[(size_t)k * n + i] = sum / 16;
         }
      }
      for (int k = 1; k < rings; k++)
         for (int i = 0; i < n; i++) m.h[(size_t)k * n + i] = next[(size_t)k * n + i];
   }
}
} // namespace

CytoMesh BuildCytoMesh(const Cell& c, const Params& p)
{
   const std::vector<Pt2> outline = CellOutlineLocal(c, (int)std::max(8.0, JsRound(p.cytoTheta)));
   CytoMesh m;
   m.rings = (int)std::max(2.0, JsRound(p.cytoRings));
   m.n = (int)outline.size();
   const size_t nv = (size_t)(m.rings + 1) * m.n;
   m.x.resize(nv); m.y.resize(nv); m.h.resize(nv);
   for (int k = 0; k <= m.rings; k++) {
      const double frac = k == m.rings ? 1 : 1 - jsm::pow(1 - (double)k / m.rings, CYTO_RING_BIAS);
      for (int i = 0; i < m.n; i++) {
         const double lx = outline[i].x * frac, ly = outline[i].y * frac;
         const double dEdge = frac >= 1 ? 0 : NearestDistToOutline(outline, lx, ly);
         const double dNuc = NucleusSignedDistLocal(c, lx, ly);
         const size_t v = (size_t)k * m.n + i;
         m.x[v] = lx; m.y[v] = ly; m.h[v] = CytoHeightAt(c, p, dEdge, dNuc);
      }
   }
   SmoothCytoGrid(m, p.cytoSmoothPasses);
   return m;
}

double SampleCytoMeshHeight(const Cell& c, const CytoMesh& m, double x, double y)
{
   const int rings = m.rings, n = m.n;
   const double theta = jsm::atan2(y, x);
   const double thetaNorm = std::fmod(std::fmod(theta / (jsm::PI * 2), 1.0) + 1, 1.0);
   const double iF = thetaNorm * n;
   const int i0 = (int)std::floor(iF) % n, i1 = (i0 + 1) % n;
   const double ti = iF - std::floor(iF);
   const double rc = CellRadiusAt(c, theta);
   const double f = std::min(1.0, std::max(0.0, rc > 1e-9 ? jsm::hypot(x, y) / rc : 0));
   const double kF = std::min((double)rings, rings * (1 - jsm::pow(1 - f, 1 / CYTO_RING_BIAS)));
   const int k0 = std::min(rings - 1, (int)std::floor(kF)), k1 = k0 + 1;
   const double tk = kF - k0;
   const double h00 = m.H(k0, i0), h01 = m.H(k0, i1), h10 = m.H(k1, i0), h11 = m.H(k1, i1);
   return (h00 * (1 - ti) + h01 * ti) * (1 - tk) + (h10 * (1 - ti) + h11 * ti) * tk;
}

} // namespace isc
