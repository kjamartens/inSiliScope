// Nuclear pore complexes (issue 15; spec/ALGORITHM.md "Nuclear pores"): pores on each cell's nuclear envelope at
// npcDensity per um^2 of it, uniform by area, each along the local outward normal (the middle plane on the
// membrane), no two closer than npcMinDistNm. Ported from the retired JS prototype (nucSurfaceLocal /
// nucSurfaceMesh / nucSurfaceNormal / buildNpcsForCell / npcAttachmentPoints) bit for bit (jsm math, the JS
// operand order), which keeps the frozen goldens.
//
// Area-uniform sampling: a fixed mesh of the surface's parameters (its flat triangles chosen by area, a uniform point
// on the triangle, mapped back to the parameters and evaluated on the true surface). Pieces: the side, alpha in
// [0, pi] with zeta = -cos(alpha) and s = the section width (the vertices of NucleusRingsLocal), and the flat caps of
// a widened half (zeta = -1 / +1, s in [0, f]). Candidate i is Pcg4d(seed ^ NPC_POS_SALT, cx, cy, i): address-based;
// a candidate closer than npcMinDistNm to an accepted pore is rejected and the next one tried (round(density x area)
// pores, or NPC_TRIES_PER_PORE x that many candidates).
//
// The nuclear lamina (2026-10-27; spec/ALGORITHM.md "Nuclear lamina"): points on the same envelope at laminDensity per
// um^2, sampled on the same mesh (LAMIN_POS_SALT), none within laminNpcExclNm / 2 of a pore centre. Port of the
// prototype's buildLaminForCell.
#pragma once

#include "cells.h"
#include "params.h"

#include <cstdint>
#include <vector>

namespace isc {

constexpr uint32_t NPC_POS_SALT = 0x4E504331u;   // "NPC1"
constexpr int NPC_MESH_NT = 128;                 // angles
constexpr int NPC_MESH_NA = 64;                  // side bands (alpha)
constexpr int NPC_MESH_NC = 16;                  // cap rings
constexpr double NPC_DIFF = 1e-5;                // central-difference step of the normal (parameter units)
constexpr int NPC_TRIES_PER_PORE = 50;
constexpr int NPC_PIECE_SIDE = 0, NPC_PIECE_BOTTOM = 1, NPC_PIECE_TOP = 2;
constexpr int NPC_SITES = 64;                    // attachment points per pore (2 rings x 8 corners x 4)
constexpr double NPC_BLOCK_UM = 1.0;             // dye blocks: the pores whose centre lies in one cell-local cube
constexpr uint32_t LAMIN_POS_SALT = 0x4C414D31u; // "LAM1"

// One pore, cell-local um: centre on the membrane, outward unit normal n (the pore axis), the ring plane's e1 (corner
// 0's direction) and e2, its candidate index and surface piece.
struct Npc {
   Pt3 c, n, e1, e2;
   int32_t cand;
   int piece;
};

// One point of the nuclear lamina, cell-local um: on the membrane, the outward unit normal n, its candidate index and
// surface piece.
struct LaminPoint {
   Pt3 c, n;
   int32_t cand;
   int piece;
};

// Surface point of piece `piece` at angle th and radial parameter w (side: alpha; caps: s), cell-local.
Pt3 NucSurfaceLocal(const Cell& c, int piece, double th, double w);

// The flat-triangle mesh of the envelope's parameters and its running area (um^2).
struct NucSurfaceMesh {
   struct Vert { double th, w; Pt3 p; };
   struct Tri { int piece; bool apex; int v1, v2, v3; };
   std::vector<Vert> verts;
   std::vector<Tri> tris;
   std::vector<double> cdf;   // running flat area
   double area = 0, fBot = 0, fTop = 0;
};
NucSurfaceMesh BuildNucSurfaceMesh(const Cell& c);

// The pores of a cell, in candidate order (= acceptance order). A pure function of (seed, cell address, params).
std::vector<Npc> BuildNpcsForCell(uint32_t seed, int32_t cx, int32_t cy, const Cell& c, const Params& p);

// The lamina points of a cell, in candidate order (= acceptance order): uniform by area at laminDensity per um^2, none
// within laminNpcExclNm / 2 of a centre of `pores` (BuildNpcsForCell of the same cell; 3D), no spacing between points
// (random; a patterned network replaces this function). Candidate i is Pcg4d(seed ^ LAMIN_POS_SALT, cx, cy, i) (its
// fourth word unused); round(density x area) points or NPC_TRIES_PER_PORE x that many candidates. A pure function of
// (seed, cell address, params).
std::vector<LaminPoint> BuildLaminForCell(uint32_t seed, int32_t cx, int32_t cy, const Cell& c, const Params& p,
                                          const std::vector<Npc>& pores);

// The 64 attachment points of one pore in its own frame (nm): webSMLM's Nup96 model (Thevathasan et al. 2019):
// 2 rings at -sep/2 (k 0-31, the nucleoplasmic ring, inside the nucleus) and +sep/2 (k 32-63, cytoplasmic) along the
// pore axis n, 8 corners at 45 deg, at each a 4-point arc (phi = -67.5, -22.5, 22.5, 67.5 deg) of radius spread/2
// centred on the ring point; a along e1, b along e2. Site k = ring x 32 + corner x 4 + arc. out: 64 x (a, b, c).
void NpcAttachmentPoints(const Params& p, double out[NPC_SITES * 3]);

// The farthest an attachment point is from its pore's centre (um).
double NpcSiteReachUm(const Params& p);

} // namespace isc
