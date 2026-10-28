// ctest cli_settings: settings files (settings_file.h, the viewer's Save settings, insiliscope_cli --settings):
//   - every engine option has a part (ScopeOptionPart), the options with names give them;
//   - a file's values become the spec: names (dyes, modes, cameras) as their numbers, the parts asked for only;
//   - two files mix: each part from the file read last with it;
//   - unknown keys, a part in the wrong place and another release are warned about, a file that is not one refused;
//   - the measurement's light history (runs of specs, each the changes from the one before) replayed (LiveScope::
//     Advance, the current sample's keys put in) gives the live session's next frame, bit for bit.
//
//   settings_check     exit code 0 = all checks passed
#include "settings_file.h"

#include "insiliscope/version.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok) g_failures++;
}

ScopeSpec Spec(const std::string& text)
{
   ScopeSpec s;
   std::string err;
   if (!ParseScopeSpec(text, s, err)) { std::printf("FAIL  spec: %s\n", err.c_str()); g_failures++; }
   return s;
}

bool Has(const std::vector<std::string>& v, const char* text)
{
   for (const std::string& s : v)
      if (s.find(text) != std::string::npos)
         return true;
   return false;
}

// A file as the viewer writes one (docs/dev/SAVE_SETTINGS.md), its parts given.
std::string File(const std::string& parts, const char* release = ISC_VERSION)
{
   return std::string("{\"format\":\"insiliscope-settings\",\"version\":1,\"made\":{\"app\":\"viewer\",\"release\":\"") + release +
          "\"},\"parts\":{" + parts + "}}";
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();

   // Every option has a part, and the named ones round-trip through their names.
   bool parts = true, names = true;
   for (const ScopeOption& o : ScopeMovieOptions())
   {
      const char* p = ScopeOptionPart(o.name);
      parts = parts && p && (!std::strcmp(p, "instrument") || !std::strcmp(p, "settings") || !std::strcmp(p, "sample") ||
                             !std::strcmp(p, "measurement") || !std::strcmp(p, "host"));
      if (const std::vector<std::string>* list = ScopeOptionNames(o.name))
         for (size_t i = 0; i < list->size(); ++i)
         {
            double v = -9;
            names = names && ScopeOptionValue(o.name, (*list)[i].c_str(), v) && v == static_cast<double>(i);
         }
   }
   Check(parts, "every engine option has a part (instrument, settings, sample, measurement, host)");
   Check(names, "every name of an option's values reads back as its index");
   Check(!std::strcmp(ScopeOptionPart("p.cellDiamMin"), "sample") && !std::strcmp(ScopeOptionPart("zern.5"), "instrument") &&
         !std::strcmp(ScopeOptionPart("mt-dye.qy"), "sample") && !std::strcmp(ScopeOptionPart("dye2.on-sec"), "sample") &&
         !std::strcmp(ScopeOptionPart("laser-640"), "settings") && !std::strcmp(ScopeOptionPart("na"), "instrument") &&
         !std::strcmp(ScopeOptionPart("x"), "measurement") && ScopeOptionPart("no-such-option") == nullptr,
         "p.* sample, zern.* instrument, dye fields sample, laser-640 settings, na instrument, x measurement");

   // Values, names and parts.
   const std::string a = File(
      "\"instrument\":{\"values\":{\"na\":1.49,\"camera-preset\":\"iXonUltra897\",\"zern.5\":0.05}},"
      "\"settings\":{\"values\":{\"laser-640\":2.5,\"modality\":\"Fluorescence\",\"size\":32}},"
      "\"sample\":{\"values\":{\"mt-mode\":\"DNA-PAINT\",\"mt-dye\":\"ATTO655\",\"npc-dye\":\"None\",\"p.cellDiamMin\":20,\"world-seed\":7}},"
      "\"measurement\":{\"values\":{\"x\":3,\"y\":-2,\"exposure-ms\":100}},"
      "\"view\":{\"values\":{\"tiltDeg\":30}}");
   settings::Loaded la;
   std::string err;
   const bool okA = settings::ReadText(a, {}, la, err);
   const ScopeSpec& s = la.spec;
   auto is = [&](const char* k, double v) { auto it = s.find(k); return it != s.end() && it->second == v; };
   double dye = -9, cam = -9, mode = -9;
   ScopeOptionValue("mt-dye", "ATTO655", dye);
   ScopeOptionValue("camera-preset", "iXonUltra897", cam);
   ScopeOptionValue("mt-mode", "DNA-PAINT", mode);
   Check(okA && is("na", 1.49) && is("camera-preset", cam) && is("zern.5", 0.05) && is("laser-640", 2.5) && is("mt-dye", dye) &&
         is("mt-mode", mode) && is("p.cellDiamMin", 20) && is("x", 3) && is("exposure-ms", 100) && !s.count("tiltDeg") &&
         la.warnings.empty(), "a file's values become the spec (names as their numbers; the view part left to the viewer)");
   settings::Loaded only;
   Check(settings::ReadText(a, { "sample" }, only, err) && only.spec.count("mt-dye") && !only.spec.count("na") &&
         !only.spec.count("x"), "only the parts asked for (sample: no na, no x)");

   // Two files mix: the instrument of one, the rest of the other.
   const std::string b = File("\"instrument\":{\"values\":{\"na\":1.27,\"camera-preset\":\"Kinetix22\"}},"
                              "\"sample\":{\"values\":{\"mt-mode\":\"dSTORM\"}}");
   settings::Loaded mix;
   const bool okMix = settings::ReadText(a, {}, mix, err) && settings::ReadText(b, { "instrument" }, mix, err);
   double kin = -9;
   ScopeOptionValue("camera-preset", "Kinetix22", kin);
   Check(okMix && mix.spec["na"] == 1.27 && mix.spec["camera-preset"] == kin && mix.spec["mt-mode"] == mode &&
         mix.spec["laser-640"] == 2.5, "file a, then file b's instrument: b's NA and camera, a's sample and lasers");

   // Warnings and refusals.
   settings::Loaded w;
   const bool okW = settings::ReadText(File("\"settings\":{\"values\":{\"no-such-option\":1,\"na\":1.3}}", "0.0.1-old"), {}, w, err);
   Check(okW && Has(w.warnings, "no-such-option") && Has(w.warnings, "instrument part") && Has(w.warnings, "0.0.1-old") &&
         w.spec["na"] == 1.3, "warned: an unknown key (left out), a key in another part (kept), another release");
   settings::Loaded bad;
   Check(!settings::ReadText("{\"format\":\"something else\"}", {}, bad, err) && !settings::ReadText("{not json", {}, bad, err) &&
         !settings::ReadText(a, { "nope" }, bad, err), "refused: another format, broken JSON, a part name that is none");
   Check(!settings::ReadText(File("\"sample\":{\"values\":{}}"), { "instrument" }, bad, err) && err.find("no instrument") != std::string::npos,
         "refused: a part the file does not have");
   std::string path;
   std::vector<std::string> ps;
   settings::ParseArg("C:\\x\\a.isc.json:instrument,sample", path, ps);
   const bool arg1 = path == "C:\\x\\a.isc.json" && ps.size() == 2 && ps[1] == "sample";
   settings::ParseArg("C:\\x\\a.isc.json", path, ps);
   Check(arg1 && path == "C:\\x\\a.isc.json" && ps.empty(), "file:part,part, and a drive letter's colon kept in the path");

   // The light history: a live session, its runs written as changes from the run before, read back and replayed.
   const std::string base = "size=32 npc-dye=None mt-mode=dSTORM psf-kernel-half-width-nm=2500 illum-profile=FlatTop ";
   const std::vector<std::pair<std::string, int>> runs = {
      { "x=3 y=-2 z=0.5 exposure-ms=100", 3 }, { "x=8 y=-2 z=0.5 exposure-ms=100", 2 },
      { "x=8 y=-2 z=0.5 exposure-ms=100 laser-640=3", 2 }, { "x=3 y=-2 z=0.5 exposure-ms=50", 2 } };
   LiveScope session;
   for (const auto& r : runs)
      for (int k = 0; k < r.second; k++)
      {
         std::vector<uint16_t> adu;
         LiveFrameInfo li;
         session.Frame(Spec(base + r.first), true, adu, li, err);
      }
   // The sample part holds npc-dye and mt-mode (put in on replay); the records the rest, as the viewer saves them.
   const std::string hist = File(
      "\"sample\":{\"values\":{\"npc-dye\":\"None\",\"mt-mode\":\"dSTORM\"}},"
      "\"measurement\":{\"values\":{\"x\":3,\"y\":-2,\"z\":0.5,\"exposure-ms\":50},\"history\":{\"records\":["
      "{\"n\":3,\"lit\":true,\"set\":{\"size\":32,\"psf-kernel-half-width-nm\":2500,\"illum-profile\":\"FlatTop\",\"x\":3,\"y\":-2,\"z\":0.5,\"exposure-ms\":100}},"
      "{\"n\":2,\"lit\":true,\"set\":{\"x\":8}},"
      "{\"n\":2,\"lit\":true,\"set\":{\"laser-640\":3}},"
      "{\"n\":2,\"lit\":true,\"set\":{\"x\":3,\"laser-640\":null,\"exposure-ms\":50}}]}}");
   settings::Loaded h;
   LiveScope replay;
   const auto tr = std::chrono::steady_clock::now();
   const bool okH = settings::ReadText(hist, {}, h, err) && settings::Replay(h, replay, err);
   const double replayMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr).count();
   std::vector<uint16_t> fa, fb;
   LiveFrameInfo ia, ib;
   const ScopeSpec next = Spec(base + runs.back().first);
   const bool fa_ok = session.Frame(next, true, fa, ia, err), fb_ok = replay.Frame(next, true, fb, ib, err);
   char msg[240];
   std::snprintf(msg, sizeof msg, "a session's light history (4 runs, 9 frames) read back and replayed: the next frame is the "
                 "session's, bit for bit (t %.2f s, lit here %.3f s; replay %.0f ms)", ib.tSec, ib.litSec, replayMs);
   Check(okH && h.history.size() == 4 && fa_ok && fb_ok && fa == fb && ia.tSec == ib.tSec && ia.litSec == ib.litSec, msg);
   if (!okH) std::printf("      (%s)\n", err.c_str());

   std::printf("cli_settings: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
