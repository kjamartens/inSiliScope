#include "cytomesh.h"

#include "jsmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
constexpr double CYTO_EDT_INF = 1e20;

// 1D squared Euclidean distance transform (Felzenszwalb-Huttenlocher).
void Edt1d(const double* f, int n, double* d, int* v, double* z)
{
   int k = 0;
   v[0] = 0; z[0] = -CYTO_EDT_INF; z[1] = CYTO_EDT_INF;
   for (int q = 1; q < n; q++) {
      double s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
      while (s <= z[k]) {
         k--;
         s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
      }
      k++;
      v[k] = q; z[k] = s; z[k + 1] = CYTO_EDT_INF;
   }
   k = 0;
   for (int q = 0; q < n; q++) {
      while (z[k + 1] < q) k++;
      d[q] = (double)(q - v[k]) * (q - v[k]) + f[v[k]];
   }
}

// Sorted crossings of the outline polygon with each grid line (axis 0: rows,
// coordinate x; axis 1: columns, coordinate y), half-open in the other one.
std::vector<std::vector<double>> GridCrossings(const std::vector<Pt2>& pts, int N, int half, double g, int axis)
{
   std::vector<std::vector<double>> lines((size_t)N);
   const size_t n = pts.size();
   for (size_t e = 0, f = n - 1; e < n; f = e++) {
      const double ax = axis == 0 ? pts[f].x : pts[f].y, ay = axis == 0 ? pts[f].y : pts[f].x;
      const double bx = axis == 0 ? pts[e].x : pts[e].y, by = axis == 0 ? pts[e].y : pts[e].x;
      if (ay == by) continue;
      const double lo = std::min(ay, by), hi = std::max(ay, by);
      const int j0 = (int)std::max(0.0, std::floor(lo / g + half));
      const int j1 = (int)std::min((double)(N - 1), std::ceil(hi / g + half));
      for (int j = j0; j <= j1; j++) {
         const double y = (j - half) * g;
         if (!(y >= lo && y < hi)) continue;
         lines[(size_t)j].push_back(ax + (y - ay) * (bx - ax) / (by - ay));
      }
   }
   for (auto& l : lines) std::sort(l.begin(), l.end());
   return lines;
}

double CrossFrac(const std::vector<double>& line, double u, int dir, double len)
{
   double best = len;
   for (double cpos : line) {
      const double d = (cpos - u) * dir;
      if (d >= 0 && d < best) best = d;
   }
   return std::max(0.01, best / len);
}
} // namespace

CytoHeightGrid BuildCytoHeightGrid(const Cell& c, const Params& p)
{
   const double g = CYTO_GRID_UM;
   const std::vector<Pt2> pts = CellOutlineLocal(c, CYTO_OUTLINE_N);
   double ext = 0;
   for (const Pt2& q : pts) ext = std::max(ext, std::max(std::fabs(q.x), std::fabs(q.y)));
   const int half = (int)std::ceil(ext / (4 * g)) * 4 + 4;
   const int N = 2 * half + 1;
   const size_t NN = (size_t)N * N;
   const auto rows = GridCrossings(pts, N, half, g, 0), cols = GridCrossings(pts, N, half, g, 1);
   std::vector<uint8_t> inside(NN);
   for (int j = 0; j < N; j++) {
      const std::vector<double>& l = rows[(size_t)j];
      size_t q = 0;
      uint8_t odd = 0;
      for (int i = 0; i < N; i++) {
         const double x = (i - half) * g;
         while (q < l.size() && l[q] < x) { q++; odd ^= 1; }
         inside[(size_t)j * N + i] = odd;
      }
   }
   // dEdge: squared EDT to the outside nodes, columns then rows.
   std::vector<double> d2(NN);
   {
      std::vector<double> f((size_t)N), d((size_t)N), z((size_t)N + 1);
      std::vector<int> v((size_t)N);
      for (int i = 0; i < N; i++) {
         for (int j = 0; j < N; j++) f[(size_t)j] = inside[(size_t)j * N + i] ? CYTO_EDT_INF : 0;
         Edt1d(f.data(), N, d.data(), v.data(), z.data());
         for (int j = 0; j < N; j++) d2[(size_t)j * N + i] = d[(size_t)j];
      }
      for (int j = 0; j < N; j++) {
         for (int i = 0; i < N; i++) f[(size_t)i] = d2[(size_t)j * N + i];
         Edt1d(f.data(), N, d.data(), v.data(), z.data());
         for (int i = 0; i < N; i++) d2[(size_t)j * N + i] = d[(size_t)i];
      }
   }
   const double margin = std::max(0.1, p.nucMargin);
   const double inner = CytoDomeReach(c, p);
   double midDist = std::max(0.0, c.cytoMidDistFrac) * c.rOuter;
   if (p.cytoMaxSlope > 0) midDist = std::max(midDist, 1.5 * std::max(0.0, c.cytoMidHeight - c.cytoRimHeight) / p.cytoMaxSlope);
   const double nucReach = std::max(c.nucLong, c.nucShort) / 2 + inner + midDist + g;
   const double ncr = jsm::cos(-c.nucRot), nsr = jsm::sin(-c.nucRot);
   const double na = std::max(1e-6, c.nucLong / 2), nb = std::max(1e-6, c.nucShort / 2), nrz = c.nucHeight / 2;
   std::vector<double> ht(NN, 0.0), ob(NN, 0.0);
   for (int j = 0; j < N; j++) {
      for (int i = 0; i < N; i++) {
         const size_t v = (size_t)j * N + i;
         if (!inside[v]) continue;
         const double x = (i - half) * g, y = (j - half) * g;
         const double dEdge = std::max(0.0, std::sqrt(d2[v]) * g - 0.5 * g);
         const double dc = jsm::hypot(x - c.nucOffX, y - c.nucOffY);
         const double dNuc = dc < nucReach ? NucleusSignedDistLocal(c, x, y) : dc;
         ht[v] = CytoHeightAt(c, p, dEdge, dNuc);
         const double ex = x - c.nucOffX, ey = y - c.nucOffY;
         const double ux = (ex * ncr - ey * nsr) / na, uy = (ex * nsr + ey * ncr) / nb;
         const double q = 1 - ux * ux - uy * uy;
         if (q > 0) ob[v] = c.nucZ + nrz * std::sqrt(q) + margin;
      }
   }
   std::vector<double> h(NN, 0.0);
   const double ell = std::max(0.0, p.cytoRelaxUm);
   if (!(ell > 0)) {
      h = ht;
   } else {
      for (int lev = 0; lev < 3; lev++) {
         const int s = 4 >> lev;
         const double gs = g * s, a = (ell * ell) / (gs * gs);
         const int M = (N - 1) / s + 1;
         const size_t MM = (size_t)M * M;
         std::vector<double> wL(MM), wR(MM), wD(MM), wU(MM), diag(MM);
         std::vector<uint8_t> bL(MM), bR(MM), bD(MM), bU(MM);
         auto node = [&](int I, int J) { return (size_t)(J * s) * N + (size_t)(I * s); };
         for (int J = 1; J < M - 1; J++) {
            for (int I = 1; I < M - 1; I++) {
               const size_t m = (size_t)J * M + I, v = node(I, J);
               if (!inside[v]) continue;
               const double x = (I * s - half) * g, y = (J * s - half) * g;
               const double tL = inside[node(I - 1, J)] ? 1 : CrossFrac(rows[(size_t)(J * s)], x, -1, gs);
               const double tR = inside[node(I + 1, J)] ? 1 : CrossFrac(rows[(size_t)(J * s)], x, 1, gs);
               const double tD = inside[node(I, J - 1)] ? 1 : CrossFrac(cols[(size_t)(I * s)], y, -1, gs);
               const double tU = inside[node(I, J + 1)] ? 1 : CrossFrac(cols[(size_t)(I * s)], y, 1, gs);
               bL[m] = inside[node(I - 1, J)]; bR[m] = inside[node(I + 1, J)];
               bD[m] = inside[node(I, J - 1)]; bU[m] = inside[node(I, J + 1)];
               wL[m] = 2 / ((tL + tR) * tL); wR[m] = 2 / ((tL + tR) * tR);
               wD[m] = 2 / ((tD + tU) * tD); wU[m] = 2 / ((tD + tU) * tU);
               diag[m] = 1 + a * (wL[m] + wR[m] + wD[m] + wU[m]);
            }
         }
         if (lev == 0) {
            for (int J = 0; J < M; J++)
               for (int I = 0; I < M; I++) { const size_t v = node(I, J); h[v] = inside[v] ? ht[v] : 0; }
         } else {
            const int sc = s * 2;
            for (int J = 0; J < M; J++) {
               for (int I = 0; I < M; I++) {
                  const size_t v = node(I, J);
                  if (!inside[v]) { h[v] = 0; continue; }
                  if ((I & 1) == 0 && (J & 1) == 0) continue;
                  const int fi = I * s, fj = J * s;
                  const int i0 = (fi / sc) * sc, j0 = (fj / sc) * sc;
                  const int i1 = std::min(N - 1, i0 + sc), j1 = std::min(N - 1, j0 + sc);
                  const double ti = (double)(fi - i0) / sc, tj = (double)(fj - j0) / sc;
                  const size_t v00 = (size_t)j0 * N + i0, v10 = (size_t)j0 * N + i1;
                  const size_t v01 = (size_t)j1 * N + i0, v11 = (size_t)j1 * N + i1;
                  const double h00 = inside[v00] ? h[v00] : 0, h10 = inside[v10] ? h[v10] : 0;
                  const double h01 = inside[v01] ? h[v01] : 0, h11 = inside[v11] ? h[v11] : 0;
                  h[v] = (h00 * (1 - ti) + h10 * ti) * (1 - tj) + (h01 * (1 - ti) + h11 * ti) * tj;
               }
            }
         }
         for (int sweep = 0; sweep < CYTO_RELAX_SWEEPS[lev]; sweep++) {
            for (int J = 1; J < M - 1; J++) {
               for (int I = 1; I < M - 1; I++) {
                  const size_t m = (size_t)J * M + I, v = node(I, J);
                  if (!inside[v]) continue;
                  const double sL = bL[m] ? h[node(I - 1, J)] : 0, sR = bR[m] ? h[node(I + 1, J)] : 0;
                  const double sD = bD[m] ? h[node(I, J - 1)] : 0, sU = bU[m] ? h[node(I, J + 1)] : 0;
                  const double nbv = wL[m] * sL + wR[m] * sR + wD[m] * sD + wU[m] * sU;
                  const double hv = (ht[v] + a * nbv) / diag[m];
                  h[v] = std::max(hv, ob[v]);
               }
            }
         }
      }
   }
   CytoHeightGrid hg;
   hg.N = N; hg.half = half; hg.g = g;
   hg.h.assign(NN, 0.0f);
   for (size_t v = 0; v < NN; v++) hg.h[v] = inside[v] ? (float)h[v] : 0.0f;
   for (int j = 1; j < N - 1; j++) {
      for (int i = 1; i < N - 1; i++) {
         const size_t v = (size_t)j * N + i;
         if (inside[v]) continue;
         double sum = 0;
         int cnt = 0;
         for (int q = 0; q < 4; q++) {
            const int di = q == 0 ? -1 : q == 1 ? 1 : 0, dj = q == 2 ? -1 : q == 3 ? 1 : 0;
            const size_t u = (size_t)(j + dj) * N + (size_t)(i + di);
            if (!inside[u]) continue;
            const double t = dj == 0 ? CrossFrac(rows[(size_t)j], (i + di - half) * g, -di, g)
                                     : CrossFrac(cols[(size_t)i], (j + dj - half) * g, -dj, g);
            sum += -h[u] * std::min(4.0, (1 - t) / t);
            cnt++;
         }
         if (cnt > 0) hg.h[v] = (float)(sum / cnt);
      }
   }
   return hg;
}

double SampleCytoHeightGrid(const CytoHeightGrid& hg, double x, double y)
{
   const double fi = x / hg.g + hg.half, fj = y / hg.g + hg.half;
   if (!(fi >= 0 && fj >= 0 && fi < hg.N - 1 && fj < hg.N - 1)) return 0;
   const int i0 = (int)std::floor(fi), j0 = (int)std::floor(fj);
   const double ti = fi - i0, tj = fj - j0;
   const size_t N = (size_t)hg.N, v = (size_t)j0 * N + i0;
   const std::vector<float>& h = hg.h;
   const double r = ((double)h[v] * (1 - ti) + (double)h[v + 1] * ti) * (1 - tj)
                  + ((double)h[v + N] * (1 - ti) + (double)h[v + N + 1] * ti) * tj;
   return std::max(0.0, r);
}

CytoMesh BuildCytoMesh(const Cell& c, const Params& p)
{
   const std::vector<Pt2> outline = CellOutlineLocal(c, (int)std::max(8.0, JsRound(p.cytoTheta)));
   CytoMesh m;
   m.rings = (int)std::max(2.0, JsRound(p.cytoRings));
   m.n = (int)outline.size();
   m.hg = BuildCytoHeightGrid(c, p);
   const size_t nv = (size_t)(m.rings + 1) * m.n;
   m.x.resize(nv); m.y.resize(nv); m.h.resize(nv);
   for (int k = 0; k <= m.rings; k++) {
      const double frac = k == m.rings ? 1 : 1 - jsm::pow(1 - (double)k / m.rings, CYTO_RING_BIAS);
      for (int i = 0; i < m.n; i++) {
         const double lx = outline[i].x * frac, ly = outline[i].y * frac;
         const size_t v = (size_t)k * m.n + i;
         m.x[v] = lx; m.y[v] = ly; m.h[v] = frac >= 1 ? 0 : SampleCytoHeightGrid(m.hg, lx, ly);
      }
   }
   return m;
}

double SampleCytoMeshHeight(const Cell&, const CytoMesh& m, double x, double y)
{
   return SampleCytoHeightGrid(m.hg, x, y);
}

} // namespace isc
