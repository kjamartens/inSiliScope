///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpExpr.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The BFP light maps' equations (2026-11-19, spec/ALGORITHM.md
//                "BFP light maps"): statements "name = expression", separated
//                by ';' or newlines ('#' to the line's end is a comment),
//                compiled to a small stack program and run per pupil sample.
//                A statement sets a map (BfpMapNames: I, phase, pol, polLab,
//                ellip, tiltX, tiltY, focus, coherent, T, duty, scanX, scanY,
//                scanRot) or a name of the user's own, read by the statements
//                after it. Variables (BfpVarNames): ux, uy (the pupil position,
//                u units), r = |u|, phi = atan2(uy, ux) (radians), nm (the laser
//                line), t (the time in the cycle, 0..1), umax (the pupil edge
//                NA / n_s), uc (1, the critical angle), pi, bfp (illum-bfp), w
//                (the legacy beam's spot: its 1/e^2 radius in u at this line),
//                fwhm (the legacy beam's FWHM in the sample, um), ns, ni, na, f
//                (the objective's focal length, um), sheetT, sheetW (the light
//                sheet's thickness and width, um), unpolarized (a pol value).
//                Operators + - * / ^ (right associative), comparisons and && ||
//                ! (1 / 0), functions exp log sqrt abs sin cos tan asin acos
//                atan atan2 hypot min max pow floor ceil round mod step gauss
//                spot clamp if, and img("file.tif") (a map image, BfpMaps.h).
//                Arguments separate by ',' or ';' (Micro-Manager refuses commas
//                in a property). Pure: no engine state.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

enum BfpVar
{
   BV_UX = 0, BV_UY, BV_R, BV_PHI, BV_NM, BV_T, BV_UMAX, BV_UC, BV_PI, BV_BFP, BV_W, BV_FWHM, BV_NS, BV_NI, BV_NA, BV_F,
   BV_SHEET_T, BV_SHEET_W, BV_UNPOLARIZED, kBfpVars
};
enum BfpMap
{
   BM_I = 0, BM_PHASE, BM_POL, BM_POL_LAB, BM_ELLIP, BM_TILT_X, BM_TILT_Y, BM_FOCUS, BM_COHERENT, BM_T, BM_DUTY, BM_SCAN_X,
   BM_SCAN_Y, BM_SCAN_ROT, kBfpMaps
};
const std::vector<std::string>& BfpVarNames();
const std::vector<std::string>& BfpMapNames();
// A map's value where no statement sets it (I 0, coherent 1, duty 1, pol unpolarized, the rest 0).
double BfpMapDefault(int map);
constexpr double kBfpUnpolarized = -999;   // = kUnpolarizedDeg (SimPattern.h)

// What a value depends on (DependsOn): the pupil position, the time, the line.
enum BfpDep : unsigned { BD_U = 1, BD_T = 2, BD_NM = 4 };

class BfpProgram
{
public:
   // False (err: "line L: ...") on a syntax error, an unknown name or function, an assignment to a variable.
   bool Compile(const std::string& text, std::string& err);
   bool Empty() const { return ops_.empty(); }
   // The map images img("name") names, in their index order.
   const std::vector<std::string>& Images() const { return images_; }
   bool Sets(int map) const { return (setMask_ >> map) & 1u; }
   // What each map depends on (BfpDep bits), given each image's page count (an image of one page: no time).
   unsigned DependsOn(int map, const std::vector<int>& imagePages) const;
   // An image's value at the pupil position and time (BfpMaps.h supplies it).
   struct ImageSampler
   {
      virtual ~ImageSampler() = default;
      virtual double Sample(int image, double ux, double uy, double t) const = 0;
   };
   // Runs the program: vars (kBfpVars, BV_R and BV_PHI from BV_UX / BV_UY are the caller's) -> maps (kBfpMaps).
   // scratch: the program's registers (resized here); not thread-safe per scratch.
   void Run(const double* vars, double* maps, std::vector<double>& scratch, const ImageSampler* images) const;

   enum Op : uint8_t
   {
      OP_CONST, OP_LOAD, OP_STORE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_NEG, OP_NOT, OP_LT, OP_LE, OP_GT, OP_GE,
      OP_EQ, OP_NE, OP_AND, OP_OR, OP_CALL, OP_IMG
   };
   struct Instr
   {
      Op op;
      int32_t a;      // register / function / image index; argument count in b
      int32_t b;
      double c;       // a constant
   };

private:
   std::vector<Instr> ops_;
   int registers_ = 0;   // kBfpVars + kBfpMaps + the user's names
   std::vector<std::string> locals_;
   std::vector<std::string> images_;
   unsigned setMask_ = 0;
};

// The equations on one line (Micro-Manager's property values): comments dropped, statements joined by "; ".
std::string BfpOneLine(const std::string& text);

} // namespace sim
