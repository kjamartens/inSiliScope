// Nuclear pore complexes: see npcs.h. Every expression keeps the prototype's operand order (web/prototype/index.html).
#include "npcs.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace isc {

namespace {
inline double NpcUnit(uint32_t a) { return ((double)(a >> 9) + 0.5) * 1.1920928955078125e-7; }

// Outward unit normal at surface parameters (central differences, clamped to the piece's range): side dP/dth x
// dP/dalpha, bottom cap dP/dth x dP/ds, top cap dP/ds x dP/dth. False where it degenerates.
bool NucSurfaceNormal(const Cell& c, const NucSurfaceMesh& M, int piece, double th, double w, Pt3& n)
{
   const double hi = piece == NPC_PIECE_SIDE ? jsm::PI : piece == NPC_PIECE_BOTTOM ? M.fBot : M.fTop;
   const double wa = std::max(0.0, w - NPC_DIFF), wb = std::min(hi, w + NPC_DIFF);
   const Pt3 pt0 = NucSurfaceLocal(c, piece, th - NPC_DIFF, w), pt1 = NucSurfaceLocal(c, piece, th + NPC_DIFF, w);
   const Pt3 pw0 = NucSurfaceLocal(c, piece, th, wa), pw1 = NucSurfaceLocal(c, piece, th, wb);
   const double tx = pt1.x - pt0.x, ty = pt1.y - pt0.y, tz = pt1.z - pt0.z;
   const double wx = pw1.x - pw0.x, wy = pw1.y - pw0.y, wz = pw1.z - pw0.z;
   double nx, ny, nz;
   if (piece == NPC_PIECE_TOP) { nx = wy * tz - wz * ty; ny = wz * tx - wx * tz; nz = wx * ty - wy * tx; }
   else { nx = ty * wz - tz * wy; ny = tz * wx - tx * wz; nz = tx * wy - ty * wx; }
   const double n2 = nx * nx + ny * ny + nz * nz;
   if (!(n2 > 1e-40)) return false;
   const double inv = 1 / std::sqrt(n2);
   n = { nx * inv, ny * inv, nz * inv };
   return true;
}

struct GridKey {
   int64_t x, y, z;
   bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridKeyHash {
   size_t operator()(const GridKey& k) const
   {
      uint64_t h = (uint64_t)k.x * 0x9E3779B97F4A7C15ull;
      h ^= (uint64_t)k.y * 0xBF58476D1CE4E5B9ull + (h << 6) + (h >> 2);
      h ^= (uint64_t)k.z * 0x94D049BB133111EBull + (h << 6) + (h >> 2);
      return (size_t)(h ^ (h >> 31));
   }
};

// The surface point of uniforms (u0, u1, u2) on the mesh M: u0 picks a triangle by its area (the CDF), (u1, u2) a
// uniform point on it (barycentric, sqrt(u1)), mapped back to the parameters (an apex triangle: the polar image of the
// flat point) and evaluated on the true surface (JS nucSamplePoint).
struct SurfaceSample {
   int piece;
   double th, w;
   Pt3 pt;
   const NucSurfaceMesh::Tri* tri;
};
SurfaceSample NucSamplePoint(const Cell& c, const NucSurfaceMesh& M, double u0, double u1, double u2)
{
   const int nT = (int)M.tris.size();
   const double total = M.cdf[(size_t)nT - 1];
   const double target = u0 * total;
   int lo = 0, hi = nT - 1;
   while (lo < hi) {
      const int mid = (int)(((unsigned)lo + (unsigned)hi) >> 1);
      if (M.cdf[(size_t)mid] > target) hi = mid; else lo = mid + 1;
   }
   const NucSurfaceMesh::Tri& tr = M.tris[(size_t)lo];
   const NucSurfaceMesh::Vert& v1 = M.verts[(size_t)tr.v1];
   const NucSurfaceMesh::Vert& v2 = M.verts[(size_t)tr.v2];
   const NucSurfaceMesh::Vert& v3 = M.verts[(size_t)tr.v3];
   const double sq = std::sqrt(u1);
   const double b1 = 1 - sq, b2 = sq * (1 - u2), b3 = sq * u2;
   double th, w;
   if (tr.apex) {   // apex at v1: the polar image of the flat point
      const double b23 = b2 + b3;
      th = b23 > 0 ? v2.th + (v3.th - v2.th) * (b3 / b23) : v2.th;
      w = b1 * v1.w + b23 * v2.w;
   } else {
      th = b1 * v1.th + b2 * v2.th + b3 * v3.th;
      w = b1 * v1.w + b2 * v2.w + b3 * v3.w;
   }
   return { tr.piece, th, w, NucSurfaceLocal(c, tr.piece, th, w), &tr };
}

// The outward unit normal at a NucSamplePoint: NucSurfaceNormal, or where that degenerates the triangle's face normal
// turned away from the nucleus centre; false if that degenerates too (JS nucPointNormal).
bool NucPointNormal(const Cell& c, const NucSurfaceMesh& M, const SurfaceSample& s, Pt3& n)
{
   if (NucSurfaceNormal(c, M, s.piece, s.th, s.w, n)) return true;
   const double x = s.pt.x, y = s.pt.y, z = s.pt.z;
   const Pt3 &p1 = M.verts[(size_t)s.tri->v1].p, &p2 = M.verts[(size_t)s.tri->v2].p, &p3 = M.verts[(size_t)s.tri->v3].p;
   const double ux = p2.x - p1.x, uy = p2.y - p1.y, uz = p2.z - p1.z;
   const double vx = p3.x - p1.x, vy = p3.y - p1.y, vz = p3.z - p1.z;
   double fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
   const double f2 = fx * fx + fy * fy + fz * fz;
   if (!(f2 > 0)) return false;
   const double inv = 1 / std::sqrt(f2);
   fx *= inv; fy *= inv; fz *= inv;
   if (fx * (x - c.nucOffX) + fy * (y - c.nucOffY) + fz * (z - c.nucZ) < 0) { fx = -fx; fy = -fy; fz = -fz; }
   n = { fx, fy, fz };
   return true;
}
} // namespace

Pt3 NucSurfaceLocal(const Cell& c, int piece, double th, double w)
{
   const double C = jsm::cos(th), S = jsm::sin(th);
   if (piece != NPC_PIECE_SIDE) return NucMapLocal(c, w, C, S, piece == NPC_PIECE_BOTTOM ? -1 : 1);
   const double zeta = -jsm::cos(w);
   if (!c.nucShaped) {
      const double s = std::sqrt(std::max(0.0, 1 - zeta * zeta));
      const double lx = C * (c.nucLong / 2) * s, ly = S * (c.nucShort / 2) * s;
      const double cr = jsm::cos(c.nucRot), sr = jsm::sin(c.nucRot);
      return { c.nucOffX + lx * cr - ly * sr, c.nucOffY + lx * sr + ly * cr, c.nucZ + zeta * c.nucHeight / 2 };
   }
   return NucMapLocal(c, NucSectionW(zeta, zeta < 0 ? c.nucFBot : c.nucFTop), C, S, zeta);
}

NucSurfaceMesh BuildNucSurfaceMesh(const Cell& c)
{
   NucSurfaceMesh M;
   M.fBot = c.nucShaped ? c.nucFBot : 0;
   M.fTop = c.nucShaped ? c.nucFTop : 0;
   const int NT = NPC_MESH_NT;
   auto ring = [&](int piece, double w) {
      const int first = (int)M.verts.size();
      for (int i = 0; i <= NT; i++) {
         const double th = ((double)i / NT) * jsm::PI * 2;
         M.verts.push_back({ th, w, NucSurfaceLocal(c, piece, th, w) });
      }
      return first;
   };
   // Triangles between ring a (lower / inner) and ring b; apexA / apexB: that ring is one point (a pole, a cap centre).
   auto band = [&](int piece, int a, int b, bool apexA, bool apexB) {
      for (int i = 0; i < NT; i++) {
         if (apexA) M.tris.push_back({ piece, true, a + i, b + i, b + i + 1 });
         else if (apexB) M.tris.push_back({ piece, true, b + i, a + i + 1, a + i });
         else {
            M.tris.push_back({ piece, false, a + i, a + i + 1, b + i + 1 });
            M.tris.push_back({ piece, false, a + i, b + i + 1, b + i });
         }
      }
   };
   auto cap = [&](int piece, double f) {
      int prev = ring(piece, 0);
      for (int k = 1; k <= NPC_MESH_NC; k++) {
         const int cur = ring(piece, f * ((double)k / NPC_MESH_NC));
         band(piece, prev, cur, k == 1, false);
         prev = cur;
      }
   };
   if (M.fBot > 0) cap(NPC_PIECE_BOTTOM, M.fBot);
   int prev = ring(NPC_PIECE_SIDE, 0);
   for (int j = 1; j <= NPC_MESH_NA; j++) {
      const int cur = ring(NPC_PIECE_SIDE, jsm::PI * ((double)j / NPC_MESH_NA));
      band(NPC_PIECE_SIDE, prev, cur, j == 1 && !(M.fBot > 0), j == NPC_MESH_NA && !(M.fTop > 0));
      prev = cur;
   }
   if (M.fTop > 0) cap(NPC_PIECE_TOP, M.fTop);
   M.cdf.resize(M.tris.size());
   double A = 0;
   for (size_t t = 0; t < M.tris.size(); t++) {
      const Pt3& p1 = M.verts[(size_t)M.tris[t].v1].p;
      const Pt3& p2 = M.verts[(size_t)M.tris[t].v2].p;
      const Pt3& p3 = M.verts[(size_t)M.tris[t].v3].p;
      const double ux = p2.x - p1.x, uy = p2.y - p1.y, uz = p2.z - p1.z;
      const double vx = p3.x - p1.x, vy = p3.y - p1.y, vz = p3.z - p1.z;
      const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
      A += 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
      M.cdf[t] = A;
   }
   M.area = A;
   return M;
}

std::vector<Npc> BuildNpcsForCell(uint32_t seed, int32_t cx, int32_t cy, const Cell& c, const Params& p)
{
   std::vector<Npc> out;
   const double density = p.npcDensity;
   if (!(density > 0)) return out;
   const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
   const double wantD = std::floor(density * M.area + 0.5);
   if (!(wantD > 0)) return out;
   const long want = (long)wantD;
   const long tries = want * NPC_TRIES_PER_PORE;
   const double dMin = std::max(0.0, p.npcMinDistNm) * 1e-3, d2 = dMin * dMin;
   // A grid of cells >= dMin over the accepted centres (speed only: the answer of testing every accepted pore).
   const double G = dMin > 0 ? dMin : 1;
   std::unordered_map<GridKey, std::vector<size_t>, GridKeyHash> grid;
   auto key = [&](double x, double y, double z) {
      return GridKey{ (int64_t)std::floor(x / G), (int64_t)std::floor(y / G), (int64_t)std::floor(z / G) };
   };
   const uint32_t salt = seed ^ NPC_POS_SALT;
   for (long i = 0; i < tries && (long)out.size() < want; i++) {
      const Pcg4dOut q = Pcg4d(salt, (uint32_t)cx, (uint32_t)cy, (uint32_t)i);
      const double u0 = NpcUnit(q.a), u1 = NpcUnit(q.b), u2 = NpcUnit(q.c), u3 = NpcUnit(q.d);
      const SurfaceSample s = NucSamplePoint(c, M, u0, u1, u2);
      const int piece = s.piece;
      const double x = s.pt.x, y = s.pt.y, z = s.pt.z;
      if (dMin > 0) {
         const GridKey k0 = key(x, y, z);
         bool clash = false;
         for (int dx = -1; dx <= 1 && !clash; dx++)
            for (int dy = -1; dy <= 1 && !clash; dy++)
               for (int dz = -1; dz <= 1 && !clash; dz++) {
                  auto it = grid.find(GridKey{ k0.x + dx, k0.y + dy, k0.z + dz });
                  if (it == grid.end()) continue;
                  for (size_t o : it->second) {
                     const double ex = x - out[o].c.x, ey = y - out[o].c.y, ez = z - out[o].c.z;
                     if (ex * ex + ey * ey + ez * ez < d2) { clash = true; break; }
                  }
               }
         if (clash) continue;
      }
      Pt3 n;
      if (!NucPointNormal(c, M, s, n)) continue;
      // Orthonormal basis about n (Duff et al. 2017), turned by 2 pi u3 about it.
      const double nx = n.x, ny = n.y, nz = n.z;
      const double sg = nz >= 0 ? 1 : -1;
      const double a = -1 / (sg + nz), b = nx * ny * a;
      const double f1x = 1 + sg * nx * nx * a, f1y = sg * b, f1z = -sg * nx;
      const double f2x = b, f2y = sg + ny * ny * a, f2z = -ny;
      const double phi = 2 * jsm::PI * u3, cp = jsm::cos(phi), sp = jsm::sin(phi);
      Npc pore;
      pore.c = { x, y, z };
      pore.n = n;
      pore.e1 = { cp * f1x + sp * f2x, cp * f1y + sp * f2y, cp * f1z + sp * f2z };
      pore.e2 = { cp * f2x - sp * f1x, cp * f2y - sp * f1y, cp * f2z - sp * f1z };
      pore.cand = (int32_t)i;
      pore.piece = piece;
      out.push_back(pore);
      if (dMin > 0) grid[key(x, y, z)].push_back(out.size() - 1);
   }
   return out;
}

std::vector<LaminPoint> BuildLaminForCell(uint32_t seed, int32_t cx, int32_t cy, const Cell& c, const Params& p,
                                          const std::vector<Npc>& pores)
{
   std::vector<LaminPoint> out;
   const double density = p.laminDensity;
   if (!(density > 0)) return out;
   const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
   const double wantD = std::floor(density * M.area + 0.5);
   if (!(wantD > 0)) return out;
   const long want = (long)wantD;
   const long tries = want * NPC_TRIES_PER_PORE;
   const double rEx = std::max(0.0, p.laminNpcExclNm) / 2 * 1e-3, r2 = rEx * rEx;
   // The pore centres on a grid of cells >= rEx (speed only: the answer of testing every pore).
   const double G = rEx > 0 ? rEx : 1;
   std::unordered_map<GridKey, std::vector<size_t>, GridKeyHash> grid;
   auto key = [&](double x, double y, double z) {
      return GridKey{ (int64_t)std::floor(x / G), (int64_t)std::floor(y / G), (int64_t)std::floor(z / G) };
   };
   if (rEx > 0)
      for (size_t o = 0; o < pores.size(); o++) grid[key(pores[o].c.x, pores[o].c.y, pores[o].c.z)].push_back(o);
   const uint32_t salt = seed ^ LAMIN_POS_SALT;
   for (long i = 0; i < tries && (long)out.size() < want; i++) {
      const Pcg4dOut q = Pcg4d(salt, (uint32_t)cx, (uint32_t)cy, (uint32_t)i);
      const SurfaceSample s = NucSamplePoint(c, M, NpcUnit(q.a), NpcUnit(q.b), NpcUnit(q.c));
      const double x = s.pt.x, y = s.pt.y, z = s.pt.z;
      if (!grid.empty()) {
         const GridKey k0 = key(x, y, z);
         bool inPore = false;
         for (int dx = -1; dx <= 1 && !inPore; dx++)
            for (int dy = -1; dy <= 1 && !inPore; dy++)
               for (int dz = -1; dz <= 1 && !inPore; dz++) {
                  auto it = grid.find(GridKey{ k0.x + dx, k0.y + dy, k0.z + dz });
                  if (it == grid.end()) continue;
                  for (size_t o : it->second) {
                     const double ex = x - pores[o].c.x, ey = y - pores[o].c.y, ez = z - pores[o].c.z;
                     if (ex * ex + ey * ey + ez * ez < r2) { inPore = true; break; }
                  }
               }
         if (inPore) continue;
      }
      Pt3 n;
      if (!NucPointNormal(c, M, s, n)) continue;
      out.push_back(LaminPoint{ { x, y, z }, n, (int32_t)i, s.piece });
   }
   return out;
}

void NpcAttachmentPoints(const Params& p, double out[NPC_SITES * 3])
{
   static const double ARC_DEG[4] = { -67.5, -22.5, 22.5, 67.5 };
   const double R = p.npcRadiusNm, arcR = p.npcCornerSpreadNm / 2, halfSep = p.npcRingSepNm / 2;
   int k = 0;
   for (int ring = 0; ring < 2; ring++) {
      const double cz = ring == 0 ? -halfSep : halfSep;
      for (int corner = 0; corner < 8; corner++) {
         const double th = corner * jsm::PI / 4, ct = jsm::cos(th), st = jsm::sin(th);
         for (int j = 0; j < 4; j++) {
            const double phi = ARC_DEG[j] * jsm::PI / 180;
            const double u = jsm::cos(phi) * arcR, v = jsm::sin(phi) * arcR;
            out[3 * k] = R * ct + (u * ct - v * st);
            out[3 * k + 1] = R * st + (u * st + v * ct);
            out[3 * k + 2] = cz;
            k++;
         }
      }
   }
}

double NpcSiteReachUm(const Params& p)
{
   return jsm::hypot(p.npcRadiusNm + p.npcCornerSpreadNm / 2, p.npcRingSepNm / 2) * 1e-3;
}

} // namespace isc
