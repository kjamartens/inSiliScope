// SPDX-License-Identifier: BSD-3-Clause
// Settings files (<name>.isc.json, docs/dev/SAVE_SETTINGS.md): what the viewer's Save settings writes, read by
// insiliscope_cli --settings. A file holds any of five parts -- instrument, settings, sample, measurement (with the
// sample's light history: the live frames that lit it, as runs of one spec) and view (the viewer's own) -- each part's
// values by engine option name (ScopeOptionPart), enumerations by name. Several files mix: each part from the file
// that is read last with it.
#pragma once

#include "LiveScope.h"
#include "ScopeMovie.h"

#include <string>
#include <vector>

namespace settings {

// One run of the live session's frames: n frames of the spec (its sample keys are the current sample's on replay).
struct Record
{
   sim::ScopeSpec spec;
   long n = 1;
   bool lit = true;
};

struct Loaded
{
   sim::ScopeSpec spec;                  // the values of the parts read, in the order read
   std::vector<Record> history;          // the measurement's light history (of the last file read with a measurement)
   // A Record's file (the viewer's Save of a recording): history[recordFrom..] are the recorded runs (each one
   // LiveScope::Frames call, as the viewer rendered them), the ones before it the light before the Record; -1: none.
   long recordFrom = -1;
   // The measurement's record object (a Record's file): "fresh" (a movie at its start-sec) or "live"; "" for none.
   // A recorded movie's bleach basis is its own (fresh-basis), as the viewer made it.
   std::string recordMode;
   bool haveMeasurement = false;
   std::vector<std::string> warnings;    // keys this engine does not know, another release, ...
};

// The part names in a file, the engine's four and the viewer's own.
const std::vector<std::string>& PartNames();
// "file.isc.json" or "file.isc.json:part,part" (a Windows drive letter's colon is part of the path).
void ParseArg(const std::string& arg, std::string& path, std::vector<std::string>& parts);
// The parts of a file's text (parts empty: all but view) into out, on top of what it holds; false (err) on a file that
// is not a settings file or names a part it does not have.
bool ReadText(const std::string& text, const std::vector<std::string>& parts, Loaded& out, std::string& err);
bool Read(const std::string& path, const std::vector<std::string>& parts, Loaded& out, std::string& err);
// History record i's spec with the file's sample keys put in (the runs hold none).
sim::ScopeSpec RunSpec(const Loaded& s, size_t i);
// The sample's light history replayed on live (from a fresh sample): each record with the spec's sample keys put in;
// only the first `count` records (a Record's file: the light before the Record).
bool Replay(const Loaded& s, sim::LiveScope& live, std::string& err, size_t count = static_cast<size_t>(-1));

} // namespace settings
