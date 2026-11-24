// SPDX-License-Identifier: BSD-3-Clause
// The viewer module's callbacks to its host (scope_wasm.cpp) in a native build: in the browser they are the page's
// Module.onMovieProgress / onGtFile / onMovieFrame / onMovieRows (EM_JS); natively (the remote server,
// remote_server.cpp) these functions, set around one call. Unset: as the browser with none set (no progress, the
// ground truth packed, every frame taken).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct IscModuleHooks
{
   // A movie's progress (stage, fraction, detail JSON or "").
   std::function<void(const char* stage, double frac, const char* detail)> progress;
   // A ground truth's file as it is made: true when taken (nothing packed).
   std::function<bool(const char* name, const char* data, size_t size)> gtFile;
   // A streamed movie's frame f (n samples); false stops the movie.
   std::function<bool(const uint16_t* px, size_t n, long f)> movieFrame;
   // A movie in strips: frame f's sensor rows y0 .. y0 + rows - 1 (n samples); false stops.
   std::function<bool(const uint16_t* px, size_t n, long f, unsigned y0, unsigned rows)> movieRows;
};

inline IscModuleHooks& IscHooks()
{
   static IscModuleHooks h;
   return h;
}
