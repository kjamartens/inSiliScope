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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace sim {

void LiveScope::Reset()
{
   history_.Reset();
   drift_ = DriftWalker();
   haveWorld_ = false;
   tSec_ = 0;
   noiseCounter_ = 0;
}

bool LiveScope::Frame(const ScopeSpec& spec, bool countLight, std::vector<uint16_t>& adu, LiveFrameInfo& info,
                      std::string& err)
{
   return Frames(spec, 1, countLight, adu, info, err);
}

bool LiveScope::Frames(const ScopeSpec& specIn, long n, bool countLight, std::vector<uint16_t>& adu,
                       LiveFrameInfo& info, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   n = std::max(1L, n);
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
   bool epi = false, trans = false;
   ScopeLights(spec, epi, trans);
   const bool lightsSample = epi && ScopeSampleIntensityKwCm2(spec) > 0;
   // One movie of n frames only where the frames are alike: the pose fixed (no drift) and every frame lighting the
   // sample (the movie's dyes run on from the clock frame by frame). Otherwise frame by frame.
   if (n > 1 && (p.drift.On() || !(countLight && lightsSample)))
   {
      adu.clear();
      std::vector<uint16_t> one;
      LiveFrameInfo fi;
      for (long f = 0; f < n; ++f)
      {
         if (!Frames(specIn, 1, countLight, one, fi, err))
            return false;
         adu.insert(adu.end(), one.begin(), one.end());
         if (f == 0)
         {
            info = fi;
            info.blinks = 0;
            info.dyes = 0;
         }
         info.blinks += fi.blinks;
         info.dyes += fi.dyes;
         info.drift = fi.drift;
         info.paths = fi.paths;
      }
      info.renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      return true;
   }
   spec["frames"] = static_cast<double>(n);
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
   info.epi = epi;
   info.trans = trans;
   // The frames' light (the adapter's MakeLightState at the drifted pose: the beam's place, the HILO pivot at the
   // focus); they start at the sample's time now.
   info.lit = countLight && lightsSample;
   LightState light;
   if (!MakeLightState(spec, info.lit, light, err))
      return false;
   spec["start-sec"] = history_.T();
   info.litSec = history_.DoseAt(posX, posY);

   ScopeMovieInfo mi;
   info.paths = "[]";
   const ScopeProgress paths = [&](const char* stage, double, const std::string& detail) {
      // {"frame":0,...,"backends":[...]}: keep the array
      const size_t at = detail.find("\"backends\":");
      if (!std::strcmp(stage, "frames") && at != std::string::npos && detail.size() > at + 12)
         info.paths = detail.substr(at + 11, detail.size() - at - 12);
   };
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
   const size_t px = static_cast<size_t>(R.W) * R.H;
   adu.assign(px * static_cast<size_t>(n), 0);
   std::vector<uint16_t> one;
   long done = 0;
   const bool ok = RenderScopePhotons(spec, [&](long f, const std::vector<float>& photons) {
      if (f < 0 || f >= n || photons.size() != px)
         return false;
      ApplyNoiseChain(photons, one, R.W, R.H, p.Camera(), offsetMap_, gainMap_, readNoiseMap_, liveNoiseSeed,
                      noiseCounter_ + static_cast<uint32_t>(f), true);
      std::copy(one.begin(), one.end(), adu.begin() + static_cast<std::ptrdiff_t>(px * static_cast<size_t>(f)));
      ++done;
      return true;
   }, mi, err, nullptr, true, &paths, &history_, &light);
   if (!ok)
      return false;
   if (done != n)
   {
      err = "no frame rendered";
      return false;
   }
   noiseCounter_ += static_cast<uint32_t>(n);

   // The frames' light goes into the history (the next frame sees it), one frame at a time as live mode adds it; the
   // sample's time runs from its first light on (the adapter's rule: before it every dye is fresh and DNA-PAINT's
   // binding stationary), and the drift moves on.
   info.tSec = tSec_;
   info.drift = d;
   for (long f = 0; f < n; ++f)
   {
      if (info.lit || history_.Lit())
         history_.Advance(R.expSec, light);
      tSec_ += R.expSec;
      if (p.drift.On())
         drift_.Step(R.expSec);
   }
   info.width = R.W;
   info.height = R.H;
   info.blinks = mi.blinks;
   info.dyes = mi.dyes;
   info.renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
   return true;
}

} // namespace sim
