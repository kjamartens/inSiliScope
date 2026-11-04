// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// The ground truth tables' numbers (GroundTruth.h): no other engine code, so ctest csv_numbers also runs in WASM.
#pragma once

#include <string>

namespace sim {

// A table's number: v as printf's fmt ("%.<digit>f" or "%.<digit>g") would write it into 48 bytes (std::to_chars), appended
// to out; nothing for NaN or an infinity (an empty field).
void AppendCsvNumber(std::string& out, double v, const char* fmt);

} // namespace sim
