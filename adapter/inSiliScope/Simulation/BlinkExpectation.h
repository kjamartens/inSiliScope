///////////////////////////////////////////////////////////////////////////////
// FILE:          BlinkExpectation.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The expected time one dye of a blinking label spends ON (in a
//                blink of its main state) within [t0, t1) of its clock, under
//                piecewise-constant kinetics (a rate history's segments): the
//                mean of what the core's schedules draw (core/src/dyes.cpp),
//                for the blink mean-field regime (spec/ALGORITHM.md "Blink
//                render regimes").
//                  dSTORM / PALM (DyeSchedule, DyeScheduleHistory,
//                LabelSchedule): a Markov chain initial ON (dSTORM, mean
//                initialOnSec) -> first dark (rate activation) -> ON (mean
//                onSec) -> bleached (p) | OFF (1 - p, mean offSec) -> ON; the
//                ON time integral per segment by the matrix exponential of the
//                generator with an integral row (scaling and squaring, Taylor;
//                +, -, x, / only).
//                  DNA-PAINT (PersistentGen): per 1 s bin a Poisson number of
//                blinks of rate activation, starting uniformly in the bin,
//                lasting min(Exp(onSec), 20 onSec): the exact integral.
//                Not modelled: the 1000-blink cap and the normal approximation
//                of a bin's count above 30 (their means are the same to
//                ~1e-3 and below).
//                JS twin: web/prototype/scope/blink_expectation.js.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <vector>

namespace sim {

// One segment of a dye's kinetics from clock tStart on (the first starts at
// 0): the ISC_KIN_* values of its structure.
struct BlinkKineticsSegment
{
   double tStart = 0;
   double activationRatePerSec = 0, onSec = 0, offSec = 0, bleachProb = 1, initialOnSec = 0;
};

// Expected ON seconds of one dye in [t0, t1) (mode: ISC_MODE_*; 0 for
// WideField). segs: ascending tStart, the first 0; empty = never ON.
double ExpectedBlinkOnSeconds(int mode, const std::vector<BlinkKineticsSegment>& segs, double t0, double t1);

} // namespace sim
