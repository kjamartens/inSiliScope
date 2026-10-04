///////////////////////////////////////////////////////////////////////////////
// FILE:          CacheDir.h
// PROJECT:       inSiliScope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The per-user cache directory the cli and the adapter hand
//                the core (packed cell blocks, isc_world_set_cache_dir) and
//                the PSF kernel store: $ISC_CACHE_DIR when set, else
//                %LOCALAPPDATA%\inSiliScope\cache on Windows,
//                $XDG_CACHE_HOME/insiliscope or ~/.cache/insiliscope
//                elsewhere. "" (no cache) with ISC_CACHE=0, when no such
//                directory can be named, and always under Emscripten (the
//                viewer keeps its own copy in the browser's local storage).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include <cstdlib>
#include <string>

namespace sim {

inline std::string DefaultCacheDir()
{
#if defined(__EMSCRIPTEN__)
   return std::string();
#else
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)   // getenv
#endif
   auto env = [](const char* name) -> std::string {
      const char* v = std::getenv(name);
      return v ? std::string(v) : std::string();
   };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
   if (env("ISC_CACHE") == "0")
      return std::string();
   const std::string dir = env("ISC_CACHE_DIR");
   if (!dir.empty())
      return dir;
#if defined(_WIN32)
   const std::string local = env("LOCALAPPDATA");
   if (!local.empty())
      return local + "\\inSiliScope\\cache";
#else
   const std::string xdg = env("XDG_CACHE_HOME");
   if (!xdg.empty())
      return xdg + "/insiliscope";
   const std::string home = env("HOME");
   if (!home.empty())
      return home + "/.cache/insiliscope";
#endif
   return std::string();
#endif
}

} // namespace sim
