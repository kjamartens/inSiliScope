///////////////////////////////////////////////////////////////////////////////
// FILE:          Peripherals.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inSiliScope hub's peripherals besides the camera and the
//                stages: the optics (Objective turret, EmissionPath
//                magnifier, FilterCube and its Dichroic/EmissionFilter
//                wheels), the light sources (Lasers, TransmittedLamp: shutters
//                whose state is the imaging), the sample (SampleHolder, the
//                CellField specimen, Fluorophores) and the Renderer. Their
//                properties are the registry's rows (Registry/PropertyTable.cpp);
//                what is device-specific here is the MM device type: a turret
//                position, a shutter state, a magnification.
//
//                Which device owns what, and when to add one: spec/MM_DEVICES.md.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "../Registry/RegistryDevice.h"

#include <string>
#include <vector>

namespace isc {

// ---- generic devices: rows only ----
template <class U>
class GenericPeripheral : public RegistryDevice<U, CGenericBase<U>>
{
public:
   explicit GenericPeripheral(const char* name, const char* description) : name_(name), description_(description)
   {
      this->SetRegistryErrorTexts();
   }
   int Initialize() override
   {
      if (initialized_)
         return DEVICE_OK;
      int ret = this->InitRegistry(name_);
      if (ret != DEVICE_OK)
         return ret;
      this->CreateStringProperty(MM::g_Keyword_Name, name_, true);
      this->CreateStringProperty(MM::g_Keyword_Description, description_, true);
      initialized_ = true;
      return DEVICE_OK;
   }
   int Shutdown() override
   {
      this->ShutdownRegistry();
      initialized_ = false;
      return DEVICE_OK;
   }
   void GetName(char* name) const override { CDeviceUtils::CopyLimitedString(name, name_); }
   bool Busy() override { return false; }

private:
   const char* name_;
   const char* description_;
   bool initialized_ = false;
};

class CellFieldDevice : public GenericPeripheral<CellFieldDevice>
{
public:
   CellFieldDevice();
};

class FluorophoresDevice : public GenericPeripheral<FluorophoresDevice>
{
public:
   FluorophoresDevice();
};

class RendererDevice : public GenericPeripheral<RendererDevice>
{
public:
   RendererDevice();
};

// ---- state devices: a turret or wheel whose position is a setting ----
template <class U>
class StatePeripheral : public RegistryDevice<U, CStateDeviceBase<U>>
{
public:
   StatePeripheral(const char* name, const char* description) : name_(name), description_(description)
   {
      this->SetRegistryErrorTexts();
   }
   int Initialize() override
   {
      if (initialized_)
         return DEVICE_OK;
      int ret = this->InitRegistry(name_);
      if (ret != DEVICE_OK)
         return ret;
      this->CreateStringProperty(MM::g_Keyword_Name, name_, true);
      this->CreateStringProperty(MM::g_Keyword_Description, description_, true);
      labels_ = Labels();
      ret = this->CreateIntegerProperty(MM::g_Keyword_State, Position(), false,
                                        new FnAction([this](MM::PropertyBase* p, MM::ActionType a) {
                                           return OnState(p, a);
                                        }));
      if (ret != DEVICE_OK)
         return ret;
      this->SetPropertyLimits(MM::g_Keyword_State, 0, static_cast<double>(labels_.size()) - 1);
      ret = this->CreateStringProperty(MM::g_Keyword_Label, "", false,
                                       new FnAction([this](MM::PropertyBase* p, MM::ActionType a) {
                                          return this->OnLabel(p, a);
                                       }));
      if (ret != DEVICE_OK)
         return ret;
      for (size_t i = 0; i < labels_.size(); ++i)
         this->SetPositionLabel(static_cast<long>(i), labels_[i].c_str());
      initialized_ = true;
      return DEVICE_OK;
   }
   int Shutdown() override
   {
      this->ShutdownRegistry();
      initialized_ = false;
      return DEVICE_OK;
   }
   void GetName(char* name) const override { CDeviceUtils::CopyLimitedString(name, name_); }
   bool Busy() override { return false; }
   unsigned long GetNumberOfPositions() const override { return static_cast<unsigned long>(labels_.size()); }

   void KeyChanged(const std::string& key) override
   {
      RegistryDevice<U, CStateDeviceBase<U>>::KeyChanged(key);
      for (const std::string& k : WatchedKeys())
         if (k == key && initialized_)
            this->OnStateChanged(Position());
   }

protected:
   virtual std::vector<std::string> Labels() = 0;
   // The position the settings are at (a derived position: the first match).
   virtual long Position() = 0;
   // Moves to a position: changes the settings, notifies the hub.
   virtual void MoveTo(long pos) = 0;
   // The settings keys the position follows.
   virtual std::vector<std::string> WatchedKeys() = 0;
   std::vector<std::string> labels_;

private:
   int OnState(MM::PropertyBase* pProp, MM::ActionType eAct)
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(Position());
      else if (eAct == MM::AfterSet)
      {
         long pos = 0;
         pProp->Get(pos);
         if (pos < 0 || pos >= static_cast<long>(labels_.size()))
            return DEVICE_UNKNOWN_POSITION;
         MoveTo(pos);
         this->Hub()->Changed(Invalidate::All);
      }
      return DEVICE_OK;
   }

   const char* name_;
   const char* description_;
   bool initialized_ = false;
};

class ObjectiveDevice : public StatePeripheral<ObjectiveDevice>
{
public:
   ObjectiveDevice();

protected:
   std::vector<std::string> Labels() override;
   long Position() override;
   void MoveTo(long pos) override;
   std::vector<std::string> WatchedKeys() override { return { "objective" }; }
};

// The filter cube: (dichroic, emission filter) pairs of the light presets.
class FilterCubeDevice : public StatePeripheral<FilterCubeDevice>
{
public:
   FilterCubeDevice();

protected:
   std::vector<std::string> Labels() override;
   long Position() override;
   void MoveTo(long pos) override;
   std::vector<std::string> WatchedKeys() override { return { "dichroic", "em-filter" }; }
};

// A filter wheel holding the library's filters of one kind ("dichroic" or "em-filter").
class FilterWheelDevice : public StatePeripheral<FilterWheelDevice>
{
public:
   FilterWheelDevice(const char* name, const char* description, const char* option);

protected:
   std::vector<std::string> Labels() override;
   long Position() override;
   void MoveTo(long pos) override;
   std::vector<std::string> WatchedKeys() override { return { option_ }; }

private:
   std::string option_;
};

// The mounted specimen (CellField; later others, spec/MM_DEVICES.md).
class SampleHolderDevice : public StatePeripheral<SampleHolderDevice>
{
public:
   SampleHolderDevice();

protected:
   std::vector<std::string> Labels() override;
   long Position() override { return 0; }
   void MoveTo(long) override {}
   std::vector<std::string> WatchedKeys() override { return {}; }
};

// ---- light sources: shutters whose state is part of every frame ----
template <class U>
class LightSource : public RegistryDevice<U, CShutterBase<U>>
{
public:
   LightSource(const char* name, const char* description, bool epi) : name_(name), description_(description), epi_(epi)
   {
      this->SetRegistryErrorTexts();
   }
   int Initialize() override
   {
      if (initialized_)
         return DEVICE_OK;
      int ret = this->InitRegistry(name_);
      if (ret != DEVICE_OK)
         return ret;
      this->CreateStringProperty(MM::g_Keyword_Name, name_, true);
      this->CreateStringProperty(MM::g_Keyword_Description, description_, true);
      // A loaded light source starts closed (MM's autoshutter opens it for
      // snaps and live; or open it by hand).
      this->Hub()->SetLight(epi_, false);
      ret = this->CreateIntegerProperty(MM::g_Keyword_State, 0, false,
                                        new FnAction([this](MM::PropertyBase* p, MM::ActionType a) {
                                           if (a == MM::BeforeGet)
                                              p->Set(Open() ? 1L : 0L);
                                           else if (a == MM::AfterSet)
                                           {
                                              long v = 0;
                                              p->Get(v);
                                              this->Hub()->SetLight(epi_, v != 0);
                                              this->GetCoreCallback()->OnShutterOpenChanged(this, v != 0);
                                           }
                                           return DEVICE_OK;
                                        }));
      if (ret != DEVICE_OK)
         return ret;
      this->AddAllowedValue(MM::g_Keyword_State, "0");
      this->AddAllowedValue(MM::g_Keyword_State, "1");
      initialized_ = true;
      return DEVICE_OK;
   }
   int Shutdown() override
   {
      // Unloaded: the light keeps its default (the epi light on, the lamp off).
      if (this->Hub())
         this->Hub()->SetLight(epi_, epi_);
      this->ShutdownRegistry();
      initialized_ = false;
      return DEVICE_OK;
   }
   void GetName(char* name) const override { CDeviceUtils::CopyLimitedString(name, name_); }
   bool Busy() override { return false; }
   int SetOpen(bool open = true) override { return this->SetProperty(MM::g_Keyword_State, open ? "1" : "0"); }
   int GetOpen(bool& open) override
   {
      open = Open();
      return DEVICE_OK;
   }
   int Fire(double) override { return DEVICE_UNSUPPORTED_COMMAND; }

private:
   bool Open() const
   {
      return this->Hub() && (epi_ ? this->Hub()->State().epiOpen.load() : this->Hub()->State().transOpen.load());
   }
   const char* name_;
   const char* description_;
   bool epi_;
   bool initialized_ = false;
};

class LasersDevice : public LightSource<LasersDevice>
{
public:
   LasersDevice();
};

class TransmittedLampDevice : public LightSource<TransmittedLampDevice>
{
public:
   TransmittedLampDevice();
};

// ---- the emission path: a magnifier (MM divides the pixel size by it) ----
class EmissionPathDevice : public RegistryDevice<EmissionPathDevice, CMagnifierBase<EmissionPathDevice>>
{
public:
   EmissionPathDevice();
   int Initialize() override;
   int Shutdown() override;
   void GetName(char* name) const override;
   bool Busy() override { return false; }
   double GetMagnification() override;
   void KeyChanged(const std::string& key) override;

private:
   bool initialized_ = false;
};

} // namespace isc
