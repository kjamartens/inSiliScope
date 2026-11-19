///////////////////////////////////////////////////////////////////////////////
// FILE:          MemoryBudget.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See MemoryBudget.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "MemoryBudget.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

namespace sim {

double PhysicalMemoryBytes()
{
#if defined(_WIN32)
   MEMORYSTATUSEX m;
   m.dwLength = sizeof m;
   return GlobalMemoryStatusEx(&m) ? static_cast<double>(m.ullTotalPhys) : 0.0;
#elif defined(__EMSCRIPTEN__)
   return 0.0;
#else
   const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
   return pages > 0 && size > 0 ? static_cast<double>(pages) * static_cast<double>(size) : 0.0;
#endif
}

double MemoryBudgetBytes()
{
   if (const char* e = std::getenv("ISC_MEMORY_BUDGET_GB"))
   {
      const double gb = std::atof(e);
      if (gb > 0)
         return gb * 1073741824.0;
   }
   return kMemoryBudgetShare * PhysicalMemoryBytes();
}

} // namespace sim
