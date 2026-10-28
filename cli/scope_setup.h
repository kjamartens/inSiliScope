// scope_setup -- the resolved setup of a movie as JSON, shared by insiliscope_cli's --setup-json (scope_probes) and
// the viewer's module (isc_scope_setup: the light-path plot, the BFP readout, the data layers' labels).
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include "DyeLibrary.h"
#include "ScopeMovie.h"
#include "Spectra.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace probes {

// A small JSON writer (numbers %.<digits>g; NaN and infinities as null, or with exactInf infinities as +-1e999,
// which JSON.parse reads as Infinity).
struct Json
{
   std::string s;
   bool first = true;
   int digits = 9;
   bool exactInf = false;
   void Sep()
   {
      if (!first)
         s += ',';
      first = false;
   }
   void Key(const char* k)
   {
      Sep();
      s += '"';
      s += k;
      s += "\":";
      first = true;
   }
   void Num(double v)
   {
      Sep();
      if (!std::isfinite(v))
      {
         s += exactInf && std::isinf(v) ? (v > 0 ? "1e999" : "-1e999") : "null";
         return;
      }
      char b[40];
      std::snprintf(b, sizeof b, "%.*g", digits, v);
      s += b;
   }
   void Str(const std::string& v)
   {
      Sep();
      s += '"';
      for (char c : v)
      {
         if (c == '"' || c == '\\')
            s += '\\';
         s += c;
      }
      s += '"';
   }
   void Bool(bool v)
   {
      Sep();
      s += v ? "true" : "false";
   }
   void Null()
   {
      Sep();
      s += "null";
   }
   void Open(char c)
   {
      Sep();
      s += c;
      first = true;
   }
   void Close(char c)
   {
      s += c;
      first = false;
   }
   template <class T> void Array(const T* v, size_t n)
   {
      Open('[');
      for (size_t i = 0; i < n; ++i)
         Num(static_cast<double>(v[i]));
      Close(']');
   }
   void KNum(const char* k, double v) { Key(k); Num(v); }
   void KStr(const char* k, const std::string& v) { Key(k); Str(v); }
   void KBool(const char* k, bool v) { Key(k); Bool(v); }
};

void SpectrumKey(Json& j, const char* key, const sim::Spectrum& v);
// A state's photophysics under the light path, and its spectra (ex, em, detected) when it emits.
void StateJson(Json& j, const sim::StateData& sd, const sim::StatePhysics& st, const sim::LightPath& lp);

// What the viewer draws of a movie's setup (exact numbers: %.17g, infinities kept, NaN null):
//   light_path: lasers [{nm, kw_per_cm2}] (after the excitation filter), the filters', dichroic's and QE curve's names
//     and curves on the spectra grid (300..900 nm, 1 nm);
//   excitation: regime, u, u_max, field_factor, depth_nm per laser line ({"405": ..}; ScopeExcitation);
//   labels (one per structure): off, mode, dye_id, dye_name (empty when off), generic (no data for the dye in its mode), label_vector
//     (isc_world_set_label), on_s_now, initial_on_s, excitation_scale, k_act_per_s, imager_background_per_px_per_s (at
//     the spec's pixel size), main / pre (the photophysics, null when dark or absent), dye_main / dye_pre (the dye's
//     states: present, is_dark, ex, em: the spectra when it has them);
//   legacy_linkage: per target id the built-in linkage numbers (LegacyLinkageNm: binder, flex, linker min, max).
bool ViewerSetupJson(const sim::ScopeSpec& spec, std::string& out, std::string& err);

} // namespace probes
