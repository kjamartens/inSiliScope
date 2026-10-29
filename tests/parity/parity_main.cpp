// Parity harness: reads tests/parity cases (see make_cases.mjs), runs them
// through core and prints one line per result (the format of
// spec/golden/ref.txt, frozen from the retired JS prototype). Built natively
// and with Emscripten (run under Node with NODERAWFS), so the same file
// checks native and WASM against the goldens.
//
//   isc_parity <cases.txt> <out.txt>
#include "cells.h"
#include "cytomesh.h"
#include "dyes.h"
#include "jsmath.h"
#include "microtubules.h"
#include "npcs.h"
#include "origami.h"
#include "fiducials.h"
#include "packing.h"
#include "params.h"
#include "rng.h"
#include "world.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace isc;

namespace {

uint32_t ToU32(long long v) { return (uint32_t)(int64_t)v; }   // JS `|0` then reinterpret

uint64_t Bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

// JS String() prints -0 as "0", so the sign of a zero is not compared.
void PrintD(FILE* f, double d) { std::fprintf(f, " %.17g", d == 0 ? 0.0 : d); }

void MathCheck(FILE* f, int n)
{
   // Inputs from the hash so JS and C++ build them identically; four bands
   // exercise the small, medium and large (Payne-Hanek) argument reductions.
   static const double kScale[4] = { 8 * jsm::PI, 200, 1e5, 1e9 };
   for (int i = 0; i < n; i++) {
      const double u1 = HashUnit(7, i, 0, 0), u2 = HashUnit(7, i, 1, 0), u3 = HashUnit(7, i, 2, 0);
      const double s = kScale[i % 4];
      const double x = (u1 - 0.5) * 2 * s;
      const double ay = (u2 - 0.5) * s, ax = (u3 - 0.5) * s;
      std::fprintf(f, "m %d %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
                   " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n", i,
                   Bits(jsm::sin(x)), Bits(jsm::cos(x)), Bits(jsm::atan2(ay, ax)), Bits(jsm::hypot(ay, ax)),
                   Bits(jsm::atan(x)), Bits(jsm::exp((u1 - 0.5) * 1500)), Bits(jsm::log(u2 * s)),
                   Bits(jsm::asin(u1 * 2 - 1)), Bits(jsm::cbrt(x)), Bits(jsm::hypot(ay, ax, x)));
   }
}

// Frames + lattice geometry, as the JS buildMicrotubuleLabelPoints(pts,
// {startUm: 0.5, lenUm: 0.1, phase: 0.3}, () => 0.5) window: every site
// labelled, linker draws all 0.5; first 13 sites.
void MtlLine(FILE* f, const std::string& id, int cx, int cy, size_t i, const std::vector<Pt3>& pts)
{
   const MtFrames fr = BuildMtFrames(pts);
   const double total = fr.Length();
   const double lenUm = std::min(0.1, total);
   const double startUm = std::min(std::max(0.0, 0.5), total - lenUm);
   std::vector<SiteGeom> L;
   for (int k = 0; k < MT_N_PROTOFILAMENTS; k++) {
      const double th = MtProtofilamentTheta(0.3, k);
      const double off = MtProtofilamentOffsetNm(k);
      for (double sNm = off; sNm < lenUm * 1000 - 1e-9; sNm += MT_DIMER_NM) {
         const double S = startUm + sNm * 1e-3;
         L.push_back(MtSiteGeometry(pts, fr, MtSegmentAt(fr, S), S, th, 0.5, 0.5, 0.5));
      }
   }
   std::fprintf(f, "mtl %s %d %d %zu %zu", id.c_str(), cx, cy, i, L.size());
   for (size_t q = 0; q < L.size() && q < 13; q++)
      for (const Pt3* v : { &L[q].att, &L[q].tip, &L[q].dye }) { PrintD(f, v->x); PrintD(f, v->y); PrintD(f, v->z); }
   std::fprintf(f, "\n");
}

// Cytoplasm mesh + microtubules of the first `maxCells` present candidates
// of a chunk window (cx-major), as summaries (a full cell is ~1e5 points).
void CellsCase(FILE* f, const std::string& id, uint32_t seed, const Params& p,
               int cx0, int cy0, int cx1, int cy1, int maxCells)
{
   int done = 0;
   for (int cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (int cy = cy0; cy <= cy1 && done < maxCells; cy++) {
         const Cell c = RawCandidate(seed, cx, cy, p);
         if (!c.present) continue;
         done++;
         const auto t0 = std::chrono::steady_clock::now();
         const MtCellGeom g = BuildMtCellGeom(c, p);
         const std::vector<Microtubule> mts = BuildMicrotubulesForCell(seed, c, p, g);
         const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
         std::fprintf(f, "cell %s %d %d nmt %zu ms %.3f\n", id.c_str(), cx, cy, mts.size(), ms);

         const CytoMesh& m = g.mesh;
         std::fprintf(f, "mesh %s %d %d %d %d", id.c_str(), cx, cy, m.rings, m.n);
         PrintD(f, g.areaUm2);
         for (size_t v = 0; v < m.h.size(); v += 97) PrintD(f, m.h[v]);
         for (int k = 0; k < 32; k++) {
            const double r = HashUnit(99, cx, cy, 2 * k) * c.rOuter * 1.1;
            const double a = HashUnit(99, cx, cy, 2 * k + 1) * jsm::PI * 2;
            PrintD(f, SampleCytoMeshHeight(c, m, r * jsm::cos(a), r * jsm::sin(a)));
         }
         std::fprintf(f, "\n");

         for (size_t i = 0; i < mts.size(); i++) {
            const std::vector<Pt3>& pts = mts[i].pts;
            double sx = 0, sy = 0, sz = 0;
            for (const Pt3& q : pts) { sx += q.x; sy += q.y; sz += q.z; }
            std::fprintf(f, "mtp %s %d %d %zu %zu", id.c_str(), cx, cy, i, pts.size());
            PrintD(f, sx); PrintD(f, sy); PrintD(f, sz);
            for (size_t j = 0; j < pts.size(); j += 37) { PrintD(f, pts[j].x); PrintD(f, pts[j].y); PrintD(f, pts[j].z); }
            if (!pts.empty()) { PrintD(f, pts.back().x); PrintD(f, pts.back().y); PrintD(f, pts.back().z); }
            std::fprintf(f, "\n");
            if (i % 16 == 0 && pts.size() >= 2) MtlLine(f, id, cx, cy, i, pts);
         }
      }
   }
}

// Nuclear pores of the first `maxCells` present candidates ('npcs').
void NpcsCase(FILE* f, const std::string& id, uint32_t seed, const Params& p, int cx0, int cy0, int cx1, int cy1,
              int maxCells)
{
   double sites[NPC_SITES * 3];
   NpcAttachmentPoints(p, sites);
   std::fprintf(f, "npa %s 0 0 0 %d", id.c_str(), NPC_SITES * 3);
   for (double v : sites) PrintD(f, v);
   std::fprintf(f, "\n");
   Linkage flex;
   flex.binderNm = 10; flex.binderFlexNm = 4; flex.linkerMinNm = 1; flex.linkerMaxNm = 6;
   const Linkage links[2] = { NormLinkage(LegacyLinkage(STRUCTURE_NPC)), NormLinkage(flex) };
   int done = 0;
   for (int cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (int cy = cy0; cy <= cy1 && done < maxCells; cy++) {
         const Cell c = RawCandidate(seed, cx, cy, p);
         if (!c.present) continue;
         done++;
         const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
         const std::vector<Npc> pores = BuildNpcsForCell(seed, cx, cy, c, p);
         double s[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
         for (const Npc& q : pores) {
            s[0] += q.c.x; s[1] += q.c.y; s[2] += q.c.z;
            s[3] += q.n.x; s[4] += q.n.y; s[5] += q.n.z;
            s[6] += q.e1.x; s[7] += q.e1.y; s[8] += q.e1.z;
         }
         std::fprintf(f, "npc %s %d %d 0 %zu", id.c_str(), cx, cy, pores.size());
         PrintD(f, M.area); PrintD(f, M.fBot); PrintD(f, M.fTop);
         for (double v : s) PrintD(f, v);
         for (size_t i = 0; i < pores.size(); i += 23) {
            const Npc& q = pores[i];
            std::fprintf(f, " %d %d", q.cand, q.piece);
            for (const Pt3* v : { &q.c, &q.n, &q.e1, &q.e2 }) { PrintD(f, v->x); PrintD(f, v->y); PrintD(f, v->z); }
         }
         std::fprintf(f, "\n");
         std::vector<int32_t> ids;
         for (int32_t i = 0; i < (int32_t)pores.size() && i < 40; i++) ids.push_back(i);
         for (int v = 0; v < 2; v++) {
            std::vector<NpcDye> dyes;
            std::vector<LinkageParts> parts;
            NpcDyesInBlock(seed, cx, cy, pores, ids, sites, 0.7, 0.9, dyes, links[v], &parts);
            double sx = 0, sy = 0, sz = 0;
            for (const NpcDye& e : dyes) { sx += e.pos.x; sy += e.pos.y; sz += e.pos.z; }
            std::fprintf(f, "npd %s %d %d %d %zu", id.c_str(), cx, cy, v, dyes.size());
            PrintD(f, sx); PrintD(f, sy); PrintD(f, sz);
            for (size_t i = 0; i < dyes.size(); i += 7) {
               const NpcDye& e = dyes[i];
               const LinkageParts& P = parts[i];
               std::fprintf(f, " %d %d %d %u", e.pore, e.k, e.n, e.id);
               for (const Pt3* q : { &e.pos, &P.site, &P.rigid, &P.tip }) { PrintD(f, q->x); PrintD(f, q->y); PrintD(f, q->z); }
            }
            std::fprintf(f, "\n");
         }
      }
   }
}

// The nuclear lamina of the first `maxCells` present candidates ('lamin').
void LaminCase(FILE* f, const std::string& id, uint32_t seed, const Params& p, int cx0, int cy0, int cx1, int cy1,
               int maxCells)
{
   Linkage flex;
   flex.binderNm = 19.3; flex.binderFlexNm = 15.3; flex.linkerMinNm = 0.5; flex.linkerMaxNm = 2;
   const Linkage links[2] = { NormLinkage(LegacyLinkage(STRUCTURE_LAMIN)), NormLinkage(flex) };
   int done = 0;
   for (int cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (int cy = cy0; cy <= cy1 && done < maxCells; cy++) {
         const Cell c = RawCandidate(seed, cx, cy, p);
         if (!c.present) continue;
         done++;
         const NucSurfaceMesh M = BuildNucSurfaceMesh(c);
         const std::vector<LaminPoint> pts = BuildLaminForCell(seed, cx, cy, c, p, BuildNpcsForCell(seed, cx, cy, c, p));
         double s[6] = { 0, 0, 0, 0, 0, 0 };
         for (const LaminPoint& q : pts) {
            s[0] += q.c.x; s[1] += q.c.y; s[2] += q.c.z;
            s[3] += q.n.x; s[4] += q.n.y; s[5] += q.n.z;
         }
         std::fprintf(f, "lam %s %d %d 0 %zu", id.c_str(), cx, cy, pts.size());
         PrintD(f, M.area);
         for (double v : s) PrintD(f, v);
         for (size_t i = 0; i < pts.size(); i += 23) {
            const LaminPoint& q = pts[i];
            std::fprintf(f, " %d %d", q.cand, q.piece);
            for (const Pt3* v : { &q.c, &q.n }) { PrintD(f, v->x); PrintD(f, v->y); PrintD(f, v->z); }
         }
         std::fprintf(f, "\n");
         std::vector<int32_t> ids;
         for (int32_t i = 0; i < (int32_t)pts.size() && i < 200; i++) ids.push_back(i);
         for (int v = 0; v < 2; v++) {
            std::vector<NpcDye> dyes;
            std::vector<LinkageParts> parts;
            LaminDyesInBlock(seed, cx, cy, pts, ids, 0.7, 0.9, dyes, links[v], &parts);
            double sx = 0, sy = 0, sz = 0;
            for (const NpcDye& e : dyes) { sx += e.pos.x; sy += e.pos.y; sz += e.pos.z; }
            std::fprintf(f, "lmd %s %d %d %d %zu", id.c_str(), cx, cy, v, dyes.size());
            PrintD(f, sx); PrintD(f, sy); PrintD(f, sz);
            for (size_t i = 0; i < dyes.size(); i += 7) {
               const NpcDye& e = dyes[i];
               const LinkageParts& P = parts[i];
               std::fprintf(f, " %d %d %d %u", e.pore, e.k, e.n, e.id);
               for (const Pt3* q : { &e.pos, &P.site, &P.rigid, &P.tip }) { PrintD(f, q->x); PrintD(f, q->y); PrintD(f, q->z); }
            }
            std::fprintf(f, "\n");
         }
      }
   }
}

// The molecules of a window of a nanoruler field ('origami').
void OrigamiCase(FILE* f, const std::string& id, uint32_t seed, double x0, double y0, double x1, double y1, Params p)
{
   p.specimen = SPECIMEN_NANORULERS;
   OrigamiField F(seed, p);
   std::vector<OrigamiMolecule> mols;
   F.MoleculesInRect(x0, y0, x1, y1, mols);
   for (const OrigamiMolecule& m : mols) {
      const OrigamiShape S = F.Shape(m);
      std::fprintf(f, "ori %s %d %d %d %d", id.c_str(), m.tx, m.ty, m.i, S.fold ? 1 : 0);
      PrintD(f, m.x); PrintD(f, m.y); PrintD(f, m.yaw); PrintD(f, OrigamiMoleculeRoll(F.Design(), F.P(), m.h1));
      PrintD(f, S.endAngle);
      std::fprintf(f, "\n");
      const std::vector<double> o = OrigamiShapeOutline(S, 16);
      double sum = 0;
      for (double v : o) sum += v;
      std::fprintf(f, "oro %s %d %d %d %zu", id.c_str(), m.tx, m.ty, m.i, o.size() / 2);
      PrintD(f, sum);
      for (size_t j = 0; j < o.size(); j += 16) { PrintD(f, o[j]); PrintD(f, o[j + 1]); }
      std::fprintf(f, "\n");
      std::vector<OrigamiSite> st;
      OrigamiMoleculeSites(F.Design(), F.P(), m, S, 1, 1, st, true);
      std::fprintf(f, "ors %s %d %d %d %zu", id.c_str(), m.tx, m.ty, m.i, st.size());
      for (const OrigamiSite& q : st) {
         std::fprintf(f, " %d %d %u", q.k, q.n, q.id);
         PrintD(f, q.x); PrintD(f, q.y); PrintD(f, q.z);
         PrintD(f, q.rest[0]); PrintD(f, q.rest[1]); PrintD(f, q.rest[2]); PrintD(f, q.bent[0]); PrintD(f, q.bent[1]);
      }
      std::fprintf(f, "\n");
   }
}

// The beads of a window of a fiducial field on the step surface ('fiducials').
void FiducialsCase(FILE* f, const std::string& id, uint32_t seed, double x0, double y0, double x1, double y1,
                   const Params& p)
{
   FiducialField F(seed, p, [](double x, double y) { return x + y > 0 ? 1.5 : 0.0; });
   std::vector<FiducialBead> beads;
   F.InRect(x0, y0, x1, y1, beads);
   std::fprintf(f, "fic %s 0 0 0 %zu\n", id.c_str(), beads.size());
   for (const FiducialBead& b : beads) {
      std::fprintf(f, "fid %s %d %d %d", id.c_str(), b.tx, b.ty, b.i);
      PrintD(f, b.x); PrintD(f, b.y); PrintD(f, b.z); PrintD(f, b.d);
      std::fprintf(f, "\n");
   }
}

} // namespace

int main(int argc, char** argv)
{
   if (argc < 3) { std::fprintf(stderr, "usage: isc_parity <cases.txt> <out.txt>\n"); return 2; }
   std::ifstream in(argv[1]);
   if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
   FILE* f = std::fopen(argv[2], "wb");   // LF on every platform
   if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }

   std::map<std::string, Params> paramSets;
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream ss(line);
      std::string kind;
      ss >> kind;
      if (kind == "params") {
         std::string name, kv;
         ss >> name;
         Params p;
         while (ss >> kv) {
            const size_t eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            if (!SetParam(p, key.c_str(), std::stod(kv.substr(eq + 1)))) {
               std::fprintf(stderr, "unknown param %s\n", key.c_str());
               return 2;
            }
         }
         NormalizeParams(p);
         paramSets[name] = p;
      } else if (kind == "rng") {
         long long seed, cx, cy, k;
         ss >> seed >> cx >> cy >> k;
         const Pcg4dOut r = Pcg4d(ToU32(seed), ToU32(cx), ToU32(cy), ToU32(k));
         std::fprintf(f, "rng %lld %lld %lld %lld %u %u %u %u", seed, cx, cy, k, r.a, r.b, r.c, r.d);
         PrintD(f, HashUnit(ToU32(seed), (int32_t)ToU32(cx), (int32_t)ToU32(cy), ToU32(k)));
         std::fprintf(f, "\n");
      } else if (kind == "stream") {
         long long seed, cx, cy, base; int count;
         ss >> seed >> cx >> cy >> base >> count;
         HashStream hs(ToU32(seed), (int32_t)ToU32(cx), (int32_t)ToU32(cy), ToU32(base));
         std::fprintf(f, "stream %lld %lld %lld %lld", seed, cx, cy, base);
         for (int i = 0; i < count; i++) PrintD(f, hs.Next());
         std::fprintf(f, "\n");
      } else if (kind == "math") {
         int n; ss >> n;
         MathCheck(f, n);
      } else if (kind == "cells") {
         std::string id, pname; long long seed; int cx0, cy0, cx1, cy1, maxCells;
         ss >> id >> seed >> pname >> cx0 >> cy0 >> cx1 >> cy1 >> maxCells;
         CellsCase(f, id, ToU32(seed), paramSets.at(pname), cx0, cy0, cx1, cy1, maxCells);
      } else if (kind == "npcs") {
         std::string id, pname; long long seed; int cx0, cy0, cx1, cy1, maxCells;
         ss >> id >> seed >> pname >> cx0 >> cy0 >> cx1 >> cy1 >> maxCells;
         NpcsCase(f, id, ToU32(seed), paramSets.at(pname), cx0, cy0, cx1, cy1, maxCells);
      } else if (kind == "lamin") {
         std::string id, pname; long long seed; int cx0, cy0, cx1, cy1, maxCells;
         ss >> id >> seed >> pname >> cx0 >> cy0 >> cx1 >> cy1 >> maxCells;
         LaminCase(f, id, ToU32(seed), paramSets.at(pname), cx0, cy0, cx1, cy1, maxCells);
      } else if (kind == "origami") {
         std::string id, kv; long long seed; double x0, y0, x1, y1;
         ss >> id >> seed >> x0 >> y0 >> x1 >> y1;
         Params p;
         while (ss >> kv) {
            const size_t eq = kv.find('=');
            if (!SetParam(p, kv.substr(0, eq).c_str(), std::stod(kv.substr(eq + 1)))) {
               std::fprintf(stderr, "unknown param %s\n", kv.c_str());
               return 2;
            }
         }
         OrigamiCase(f, id, ToU32(seed), x0, y0, x1, y1, p);
      } else if (kind == "fiducials") {
         std::string id, kv; long long seed; double x0, y0, x1, y1;
         ss >> id >> seed >> x0 >> y0 >> x1 >> y1;
         Params p;
         while (ss >> kv) {
            const size_t eq = kv.find('=');
            if (!SetParam(p, kv.substr(0, eq).c_str(), std::stod(kv.substr(eq + 1)))) {
               std::fprintf(stderr, "unknown param %s\n", kv.c_str());
               return 2;
            }
         }
         FiducialsCase(f, id, ToU32(seed), x0, y0, x1, y1, p);
      } else if (kind == "case") {
         std::string id, pname; long long seed; int cx0, cy0, cx1, cy1;
         ss >> id >> seed >> pname >> cx0 >> cy0 >> cx1 >> cy1;
         const Params& p = paramSets.at(pname);
         const uint32_t s = ToU32(seed);

         const auto t0 = std::chrono::steady_clock::now();
         CandidateMap m = BuildCandidateMap(s, cx0, cy0, cx1, cy1, p);
         std::string rawLines;
         for (const Cell& c : m.cells) {
            char buf[512];
            std::snprintf(buf, sizeof buf, "raw %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
                          c.cx, c.cy, c.x, c.y, c.semiMajor, c.semiMinor, c.rot, c.height, c.modFloor, c.nucZ, c.rOuter, c.priority);
            rawLines += buf;
         }
         const int removed = PackMap(m, p);
         const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

         std::fprintf(f, "case %s ncand %zu removed %d ms %.3f\n", id.c_str(), m.cells.size(), removed, ms);
         std::fputs(rawLines.c_str(), f);
         for (size_t i = 0; i < m.cells.size(); i++) {
            if (!m.alive[i]) continue;
            const Cell& c = m.cells[i];
            std::fprintf(f, "pk %d %d", c.cx, c.cy);
            PrintD(f, c.x); PrintD(f, c.y); PrintD(f, c.packRot);
            std::fprintf(f, "\n");
         }
      }
   }
   std::fclose(f);
   return 0;
}
