///////////////////////////////////////////////////////////////////////////////
// FILE:          LiveScope.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Live imaging of one sample from ScopeMovie specs (LiveScope.h).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "LiveScope.h"

#include "ScopeResolved.h"

#include <chrono>
#include <cstring>
#include <random>

namespace sim {

void LiveScope::LitRect(double xUm, double yUm, double wUm, double hUm, double& x0, double& y0, double& x1,
                        double& y1)
{
   const double m = 2.0 + 0.5;   // the FOV query's margin (kCellFieldMarginUm) + 0.5 um
   x0 = xUm - wUm / 2.0 - m;
   y0 = yUm - hUm / 2.0 - m;
   x1 = xUm + wUm / 2.0 + m;
   y1 = yUm + hUm / 2.0 + m;
}

void LiveScope::Reset()
{
   history_.Reset();
   drift_ = DriftWalker();
   haveWorld_ = false;
   tSec_ = 0;
   noiseCounter_ = 0;
}

bool LiveScope::Frame(const ScopeSpec& specIn, bool countLight, std::vector<uint16_t>& adu, LiveFrameInfo& info,
                      std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   info = LiveFrameInfo();
   ScopeSpec spec = specIn;
   spec["frames"] = 1;
   ScopeResolved R;
   if (!ScopeResolve(spec, R, err))
      return false;
   const SimulationParams& p = R.p;
   // A new world (seed, cell or specimen parameters) is a fresh sample, as the adapter's SyncHistoryWorld.
   if (!haveWorld_ || !R.cf.SameWorld(world_))
   {
      Reset();
      world_ = R.cf;
      haveWorld_ = true;
      drift_ = DriftWalker(R.seed, p.drift, 0);
   }
   drift_.SetSettings(p.drift);
   const DriftNm d = drift_.Position();
   // The FOV over the moved sample, the focal plane dz lower in it (the adapter's live pose); the movie's own drift
   // is off (its frame 0 is undrifted anyway).
   const double x = ScopeSpecGet(spec, "x"), y = ScopeSpecGet(spec, "y"), z = ScopeSpecGet(spec, "z");
   const double posX = x - d.x / 1000.0, posY = y - d.y / 1000.0;
   spec["x"] = posX;
   spec["y"] = posY;
   spec["z"] = z - d.z / 1000.0;
   for (const char* k : { "drift-xy-nm-per-sqrt-sec", "drift-z-nm-per-sqrt-sec", "drift-xy-speed-nm-per-sec",
                          "drift-z-speed-nm-per-sec" })
      spec[k] = 0;
   ScopeLights(spec, info.epi, info.trans);
   const uint32_t epoch = history_.RegisterEpoch(ScopeKineticEnv(spec));
   const bool lightsSample = info.epi && ScopeSampleIntensityKwCm2(spec) > 0;
   const double um = p.pixelSizeNm / 1000.0;
   double lx0, ly0, lx1, ly1;
   LitRect(posX, posY, R.W * um, R.H * um, lx0, ly0, lx1, ly1);
   const ClockSnapshot clock = history_.Snapshot(lx0, ly0, lx1, ly1, epoch);
   info.litSec = history_.ClockAt(posX, posY);

   std::vector<float> photons;
   ScopeMovieInfo mi;
   info.paths = "[]";
   const ScopeProgress paths = [&](const char* stage, double, const std::string& detail) {
      // {"frame":0,...,"backends":[...]}: keep the array
      const size_t at = detail.find("\"backends\":");
      if (!std::strcmp(stage, "frames") && at != std::string::npos && detail.size() > at + 12)
         info.paths = detail.substr(at + 11, detail.size() - at - 12);
   };
   const bool ok = RenderScopePhotons(spec, [&](long, const std::vector<float>& ph) {
      photons = ph;
      return true;
   }, mi, err, &clock, true, &paths);
   if (!ok)
      return false;
   if (photons.size() != static_cast<size_t>(R.W) * R.H)
   {
      err = "no frame rendered";
      return false;
   }
   // The camera's fixed pattern from the seed (the stack's NoiseSetup order), its live noise on a seed of its own
   // with a frame counter (the adapter's liveNoiseSeed).
   const std::vector<double> key = { static_cast<double>(R.W), static_cast<double>(R.H), static_cast<double>(R.seed),
                                     p.offsetAdu, p.offsetStdAdu, p.gainPhotonsPerAdu, p.pixelGainStdFraction,
                                     p.readNoiseElectrons, p.pixelReadNoiseStdFraction };
   if (key != noiseKey_)
   {
      noiseKey_ = key;
      std::mt19937_64 rng(static_cast<uint64_t>(R.seed));
      offsetMap_.Generate(R.W, R.H, p.offsetAdu, p.offsetStdAdu, rng);
      gainMap_.Generate(R.W, R.H, p.gainPhotonsPerAdu, p.pixelGainStdFraction, rng);
      readNoiseMap_.Generate(R.W, R.H, p.readNoiseElectrons, p.pixelReadNoiseStdFraction, rng);
   }
   const uint32_t liveNoiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(R.seed) ^ 0x4C4E4F49ULL);
   ApplyNoiseChain(photons, adu, R.W, R.H, p.Camera(), offsetMap_, gainMap_, readNoiseMap_, liveNoiseSeed,
                   noiseCounter_++, true);

   // The frame's light goes into the history (the next frame sees it); the sample's time and drift move on.
   info.lit = countLight && lightsSample;
   if (info.lit)
      history_.Advance(lx0, ly0, lx1, ly1, R.expSec, {}, epoch);
   info.tSec = tSec_;
   info.drift = d;
   tSec_ += R.expSec;
   if (p.drift.On())
      drift_.Step(R.expSec);
   info.width = R.W;
   info.height = R.H;
   info.blinks = mi.blinks;
   info.dyes = mi.dyes;
   info.renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
   return true;
}

} // namespace sim
