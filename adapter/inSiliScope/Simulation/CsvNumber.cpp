// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "CsvNumber.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace sim {

void AppendCsvNumber(std::string& out, double v, const char* fmt)
{
   if (!std::isfinite(v))
      return;
   // fmt is "%.<digit>f" or "%.<digit>g": std::to_chars with that precision, which the standard defines as printf's
   // conversion in the "C" locale (ctest csv_numbers compares it with snprintf, natively and in WASM); a printf into
   // 48 bytes kept at most 47 characters.
   char b[400];
   const int prec = fmt[2] - '0';
   const std::chars_format f = fmt[3] == 'g' ? std::chars_format::general : std::chars_format::fixed;
   const std::to_chars_result r = std::to_chars(b, b + sizeof b, v, f, prec);
   if (r.ec != std::errc())
   {
      char c[48];
      std::snprintf(c, sizeof c, fmt, v);
      out += c;
      return;
   }
   out.append(b, std::min<size_t>(static_cast<size_t>(r.ptr - b), 47));
}

} // namespace sim
