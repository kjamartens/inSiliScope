///////////////////////////////////////////////////////////////////////////////
// FILE:          Peripherals.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inSiliScope peripherals (Peripherals.h): the turrets'
//                and wheels' positions, the emission path's magnification.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Peripherals.h"

#include "../Simulation/DyeLibrary.h"

#include <cmath>

namespace isc {

// ---------------------------------------------------------------------------
// Generic devices
// ---------------------------------------------------------------------------

CellFieldDevice::CellFieldDevice()
   : GenericPeripheral("CellField", "Specimen: a field of cells with their labelled structures")
{
}

FluorophoresDevice::FluorophoresDevice()
   : GenericPeripheral("Fluorophores", "The dyes: label mode, photophysics readouts, custom dyes")
{
}

RendererDevice::RendererDevice()
   : GenericPeripheral("Renderer", "How the images are computed: quality, GPU, caches, numerics")
{
}

// ---------------------------------------------------------------------------
// Objective turret
// ---------------------------------------------------------------------------

namespace {

// Typical catalogue objectives (estimate: NA, immersion and magnification as
// engraved on common SMLM/BrightField objectives, no specific product). The
// first is the default (NA 1.40 oil, n 1.518: the PSF's reference settings).
struct ObjectiveSpec
{
   const char* label;
   double mag, na, immersion;
};
const ObjectiveSpec kObjectives[] = {
   { "100x/1.40 Oil", 100, 1.40, 1.518 },
   { "100x/1.49 Oil TIRF", 100, 1.49, 1.518 },
   { "60x/1.42 Oil", 60, 1.42, 1.518 },
   { "60x/1.20 Water", 60, 1.20, 1.333 },
   // Hidden for now (2026-10-08): a 20x field asks too much of the renderer
   // (many cells and dyes per frame, slow live BrightField focusing). Put it
   // back here; tools/gen_mm_configs.py picks the positions up from the DLL.
   // { "20x/0.75 Air", 20, 0.75, 1.0 },
};
constexpr int kObjectiveCount = static_cast<int>(sizeof(kObjectives) / sizeof(kObjectives[0]));

} // namespace

ObjectiveDevice::ObjectiveDevice()
   : StatePeripheral("Objective", "Objective turret: NA, immersion, magnification, aberrations")
{
}

std::vector<std::string> ObjectiveDevice::Labels()
{
   std::vector<std::string> v;
   for (const ObjectiveSpec& o : kObjectives)
      v.push_back(o.label);
   v.push_back("Custom");
   return v;
}

long ObjectiveDevice::Position()
{
   const SceneState& s = Hub()->State();
   for (int i = 0; i < kObjectiveCount; ++i)
      if (s.objectiveMag.load() == kObjectives[i].mag && s.na.load() == kObjectives[i].na &&
          s.immersionIndex.load() == kObjectives[i].immersion)
         return i;
   return kObjectiveCount;   // Custom: the values as set
}

void ObjectiveDevice::MoveTo(long pos)
{
   if (pos >= kObjectiveCount)
      return;   // Custom keeps the values as they are
   SceneState& s = Hub()->State();
   const ObjectiveSpec& o = kObjectives[pos];
   s.objectiveMag = o.mag;
   s.na = o.na;
   s.immersionIndex = o.immersion;
   Hub()->Notify("na");
   Hub()->Notify("immersion-index");
   Hub()->Notify("objective-mag");
   Hub()->Notify("pixel-nm");
}

// ---------------------------------------------------------------------------
// Filter cube and wheels
// ---------------------------------------------------------------------------

namespace {

struct CubeSpec
{
   std::string label;
   int excitation, dichroic, emission;
};

// The (excitation filter, dichroic, emission filter) triples of the light presets, in preset order. A cube without an
// excitation filter is labelled "<dichroic>+<emission filter>", one with "<excitation>+<dichroic>+<emission>".
const std::vector<CubeSpec>& Cubes()
{
   static const std::vector<CubeSpec> cubes = [] {
      std::vector<CubeSpec> v;
      for (const std::string& id : sim::LightPresetIds())
      {
         const sim::LightPresetData* p = sim::FindLightPreset(id);
         if (!p)
            continue;
         const int x = sim::IndexOf(sim::ExcitationFilterIds(), p->excitationFilter);
         const int d = sim::IndexOf(sim::DichroicIds(), p->dichroic);
         const int e = sim::IndexOf(sim::EmissionFilterIds(), p->emissionFilter);
         bool seen = false;
         for (const CubeSpec& c : v)
            seen = seen || (c.excitation == x && c.dichroic == d && c.emission == e);
         const std::string prefix = std::string(p->excitationFilter) == "None" ? "" : std::string(p->excitationFilter) + "+";
         if (!seen && x >= 0 && d >= 0 && e >= 0)
            v.push_back({ prefix + p->dichroic + "+" + p->emissionFilter, x, d, e });
      }
      return v;
   }();
   return cubes;
}

} // namespace

FilterCubeDevice::FilterCubeDevice()
   : StatePeripheral("FilterCube", "Filter cube: excitation filter, dichroic and emission filter as one choice")
{
}

std::vector<std::string> FilterCubeDevice::Labels()
{
   std::vector<std::string> v;
   for (const CubeSpec& c : Cubes())
      v.push_back(c.label);
   v.push_back("Custom");
   return v;
}

long FilterCubeDevice::Position()
{
   const SceneState& s = Hub()->State();
   const int x = static_cast<int>(s.Option("ex-filter"));
   const int d = static_cast<int>(s.Option("dichroic")), e = static_cast<int>(s.Option("em-filter"));
   for (size_t i = 0; i < Cubes().size(); ++i)
      if (Cubes()[i].excitation == x && Cubes()[i].dichroic == d && Cubes()[i].emission == e)
         return static_cast<long>(i);
   return static_cast<long>(Cubes().size());   // Custom: the wheels as set
}

void FilterCubeDevice::MoveTo(long pos)
{
   if (pos >= static_cast<long>(Cubes().size()))
      return;
   const CubeSpec& c = Cubes()[static_cast<size_t>(pos)];
   SceneState& s = Hub()->State();
   const bool changed = s.Option("ex-filter") != c.excitation || s.Option("dichroic") != c.dichroic ||
                        s.Option("em-filter") != c.emission;
   s.SetOption("ex-filter", c.excitation);
   s.SetOption("dichroic", c.dichroic);
   s.SetOption("em-filter", c.emission);
   Hub()->Notify("ex-filter");
   Hub()->Notify("dichroic");
   Hub()->Notify("em-filter");
   if (changed)
      Hub()->ClearLightPreset();
}

FilterWheelDevice::FilterWheelDevice(const char* name, const char* description, const char* option)
   : StatePeripheral(name, description), option_(option)
{
}

std::vector<std::string> FilterWheelDevice::Labels()
{
   return option_ == "ex-filter" ? sim::ExcitationFilterIds()
        : option_ == "dichroic"  ? sim::DichroicIds()
                                 : sim::EmissionFilterIds();
}

long FilterWheelDevice::Position()
{
   return static_cast<long>(Hub()->State().Option(option_));
}

void FilterWheelDevice::MoveTo(long pos)
{
   SceneState& s = Hub()->State();
   const bool changed = s.Option(option_) != pos;
   s.SetOption(option_, pos);
   Hub()->Notify(option_);
   if (changed)
      Hub()->ClearLightPreset();
}

// ---------------------------------------------------------------------------
// Sample holder
// ---------------------------------------------------------------------------

SampleHolderDevice::SampleHolderDevice()
   : StatePeripheral("SampleHolder", "The mounted specimen; drift and background of the sample")
{
}

std::vector<std::string> SampleHolderDevice::Labels()
{
   return { "CellField" };
}

// ---------------------------------------------------------------------------
// Light sources
// ---------------------------------------------------------------------------

LasersDevice::LasersDevice()
   : LightSource("Lasers", "Laser engine and its shutter: line powers, beam profile, geometry", true)
{
}

TransmittedLampDevice::TransmittedLampDevice()
   : LightSource("TransmittedLamp", "Transmitted-light lamp and its shutter (BrightField)", false)
{
}

// ---------------------------------------------------------------------------
// Emission path
// ---------------------------------------------------------------------------

EmissionPathDevice::EmissionPathDevice()
{
   SetRegistryErrorTexts();
}

int EmissionPathDevice::Initialize()
{
   if (initialized_)
      return DEVICE_OK;
   int ret = InitRegistry("EmissionPath");
   if (ret != DEVICE_OK)
      return ret;
   CreateStringProperty(MM::g_Keyword_Name, "EmissionPath", true);
   CreateStringProperty(MM::g_Keyword_Description,
                        "Magnification between objective and camera (sets the pixel size)", true);
   initialized_ = true;
   return DEVICE_OK;
}

int EmissionPathDevice::Shutdown()
{
   ShutdownRegistry();
   initialized_ = false;
   return DEVICE_OK;
}

void EmissionPathDevice::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, "EmissionPath");
}

double EmissionPathDevice::GetMagnification()
{
   return Hub() ? Hub()->State().emissionMag.load() : 1.0;
}

void EmissionPathDevice::KeyChanged(const std::string& key)
{
   RegistryDevice::KeyChanged(key);
   if (key == "emission-mag" && initialized_)
      OnMagnifierChanged();
}

} // namespace isc
