///////////////////////////////////////////////////////////////////////////////
// FILE:          MemoryBudget.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   How much memory a render may take on this machine (2026-11-18): a share of the physical RAM, so a
//                setup whose estimate (ScopeMovie.h EstimateScopeMemory) exceeds it is refused before it runs
//                instead of taking the host (Micro-Manager) down. ISC_MEMORY_BUDGET_GB overrides it (tests).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

namespace sim {

// The machine's physical RAM, bytes; 0 when not known (WebAssembly).
double PhysicalMemoryBytes();

// The share of the physical RAM a render may use.
constexpr double kMemoryBudgetShare = 0.8;

// The bytes a render may take: $ISC_MEMORY_BUDGET_GB if set (> 0), else kMemoryBudgetShare x the physical RAM;
// 0 = no limit known (nothing is refused).
double MemoryBudgetBytes();

} // namespace sim
