///////////////////////////////////////////////////////////////////////////////
// FILE:          SceneState.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The simulated microscope's settings (SceneState.h): defaults
//                and the locked option/string accessors.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "SceneState.h"

#include "../Simulation/DyeLibrary.h"
#include "../Simulation/ScopeMovie.h"

#include <cmath>

namespace isc {

const char* g_CellFieldCoreParam[CF_COUNT] = {
   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
   "nucBaseMin",
   "nucBaseMax",
   "nucIrregMin",
   "nucIrregMax",
   "nucBendMin",
   "nucBendMax",
   "nucSmooth",
   "nucThickIrreg",
   "nucAsym",
   "nucWidestMin",
   "nucWidestMax",
   "mtStartDecayPct",
   "mtEndDecayPct",
   "mtDirKappa",
};

SceneState::SceneState()
{
   // CellField: the prototype's geometry (spec/PORT.md 4.2), a 7 um z slab
   // around the focal plane (0 = every dye), no focus offset (ZStage = 0 puts
   // the coverslip in focus), then the nucleus shape and microtubule start/end
   // at the core's defaults (core/src/params.h).
   const double cellFieldDefaults[CF_COUNT] = { 26.0, 0.33, 25.0, 35.0, 0.9, 0.0, 7.0,
      0.4, 0.9, 0.03, 0.2, 0.0, 0.3, 2.5, 0.1, 0.5, 0.2, 0.4, 1.6, 20.0, 1.5 };
   for (int i = 0; i < CF_COUNT; ++i)
      cellField[i] = cellFieldDefaults[i];
   // Mean-field grid: 1 cell/pixel, 25 nm dye planes (the cli/viewer's wf-upscale / wf-plane-nm).
   const double wideFieldDefaults[WF_COUNT] = { 1.0, 25.0 };
   for (int i = 0; i < WF_COUNT; ++i)
      wideField[i] = wideFieldDefaults[i];
   // Directed drift: off (speeds 0), random direction per seed, no wander, 60 s correlation time.
   const double directedDriftDefaults[DD_COUNT] = { 0.0, 0.0, -1.0, 0.0, 0.0, 60.0 };
   for (int i = 0; i < DD_COUNT; ++i)
      directedDrift[i] = directedDriftDefaults[i];
   // BrightField: quality 3 (sources/upscaling/samples/slice 0 / -1 = from the
   // quality), condenser NA 0.4, 550 nm, 80000 photons/pixel/s, the PSF's
   // aberrations, the indices of medium / cytoplasm / nucleus / microtubule
   // (spec/BRIGHTFIELD.md), no absorption (unstained).
   const double brightFieldDefaults[BF_COUNT] = { 3, 0, 0, 0, -1, 0.4, 550, 80000, 1, 1.337, 1.35, 1.35, 1.48, 0 };
   for (int i = 0; i < BF_COUNT; ++i)
      brightField[i] = brightFieldDefaults[i];
   zernikeCoefficients_ = sim::FormatZernikeCoefficients(sim::ZernikePresetCoefficients(zernikePreset_));

   // The scope options with adapter defaults of their own: the labelling at
   // the default dye's mode suggestion, the QE curve of the default camera.
   using namespace sim;
   const int mtDye = static_cast<int>(ScopeSpecGet(ScopeSpec(), "mt-dye"));
   const int mode = IndexOf(DyeModeNames(), DyeAt(mtDye).defaultMode);
   options_["mt-label-pct"] = SuggestedLabelingPct(mode);
   options_["qe-curve"] = IndexOf(CameraIds(), DefaultCamera());
   cameraPreset = IndexOf(CameraIds(), DefaultCamera());
   lastMtMode = mode;
}

double SceneState::EmGain() const
{
   const int p = cameraPreset.load();
   const double preamp = p >= 0 && p < static_cast<int>(sim::CameraIds().size()) ? sim::CameraPreamp(sim::CameraAt(p))
                                                                                  : sim::kDefaultPreampElectronsPerAdu;
   return sim::EmGainFromGain(preamp, gainElectronsPerAdu.load());
}

std::string SceneState::ZernikeCoefficients() const
{
   std::lock_guard<std::mutex> g(textMutex_);
   return zernikeCoefficients_;
}

void SceneState::SetZernikeCoefficients(const std::string& s)
{
   bool ok = false;
   const sim::ZernikeCoefficients z = sim::ParseZernikeCoefficients(s, ok);
   if (!ok)
      return;   // a malformed list leaves the previous (valid) one
   std::lock_guard<std::mutex> g(textMutex_);
   zernikeCoefficients_ = sim::FormatZernikeCoefficients(z);
}

std::string SceneState::ZernikePreset() const
{
   std::lock_guard<std::mutex> g(textMutex_);
   return zernikePreset_;
}

void SceneState::SetZernikePreset(const std::string& s)
{
   std::lock_guard<std::mutex> g(textMutex_);
   zernikePreset_ = s;
}

std::string SceneState::JavaHome() const
{
   std::lock_guard<std::mutex> g(textMutex_);
   return javaHome_;
}

void SceneState::SetJavaHome(const std::string& s)
{
   std::lock_guard<std::mutex> g(textMutex_);
   javaHome_ = s;
}

std::string SceneState::LightPreset() const
{
   std::lock_guard<std::mutex> g(textMutex_);
   return lightPreset_;
}

void SceneState::SetLightPreset(const std::string& s)
{
   std::lock_guard<std::mutex> g(textMutex_);
   lightPreset_ = s;
}

std::string SceneState::GpuStatus() const
{
   std::lock_guard<std::mutex> g(textMutex_);
   return gpuStatus_;
}

void SceneState::SetGpuStatus(const std::string& s)
{
   std::lock_guard<std::mutex> g(textMutex_);
   gpuStatus_ = s;
}

double SceneState::Option(const std::string& option) const
{
   std::lock_guard<std::mutex> g(optionMutex_);
   auto it = options_.find(option);
   return it != options_.end() ? it->second : sim::ScopeSpecGet(sim::ScopeSpec(), option.c_str());
}

void SceneState::SetOption(const std::string& option, double v)
{
   std::lock_guard<std::mutex> g(optionMutex_);
   options_[option] = v;
}

bool SceneState::DyeEdit(const std::string& key, double& v) const
{
   std::lock_guard<std::mutex> g(optionMutex_);
   auto it = dyeEdits_.find(key);
   if (it == dyeEdits_.end())
      return false;
   v = it->second;
   return true;
}

void SceneState::SetDyeEdit(const std::string& key, double v)
{
   std::lock_guard<std::mutex> g(optionMutex_);
   dyeEdits_[key] = v;
}

void SceneState::ClearDyeEdit(const std::string& key)
{
   std::lock_guard<std::mutex> g(optionMutex_);
   dyeEdits_.erase(key);
}

void SceneState::ClearDyeEditsWithPrefix(const std::string& prefix)
{
   std::lock_guard<std::mutex> g(optionMutex_);
   for (auto it = dyeEdits_.begin(); it != dyeEdits_.end();)
      it = it->first.compare(0, prefix.size(), prefix) == 0 ? dyeEdits_.erase(it) : std::next(it);
}

std::map<std::string, double> SceneState::Options() const
{
   std::lock_guard<std::mutex> g(optionMutex_);
   return options_;
}

std::map<std::string, double> SceneState::DyeEdits() const
{
   std::lock_guard<std::mutex> g(optionMutex_);
   return dyeEdits_;
}

} // namespace isc
