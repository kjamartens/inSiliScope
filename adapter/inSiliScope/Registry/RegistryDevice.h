///////////////////////////////////////////////////////////////////////////////
// FILE:          RegistryDevice.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The base of every inSiliScope peripheral: finds the hub,
//                creates the device's rows of the property table
//                (PropertyTable.h) that the session's Detail shows, serves
//                them with one generic handler bound to the hub's settings,
//                and refreshes them when a coupling on another device changes
//                their setting. Base is the MM device base (CGenericBase<U>,
//                CShutterBase<U>, ...), U the device class.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "../InSiliScopeHub.h"
#include "PropertyTable.h"

#include <functional>
#include <string>
#include <vector>

namespace isc {

class RowHandler
{
public:
   virtual ~RowHandler() {}
   virtual int HandleRow(size_t row, MM::PropertyBase* pProp, MM::ActionType eAct) = 0;
};

// The action functor of one row.
class RowAction : public MM::ActionFunctor
{
public:
   RowAction(RowHandler* h, size_t row) : h_(h), row_(row) {}
   int Execute(MM::PropertyBase* pProp, MM::ActionType eAct) override { return h_->HandleRow(row_, pProp, eAct); }

private:
   RowHandler* h_;
   size_t row_;
};

// An action functor from a function: a device's own (non-registry) property.
class FnAction : public MM::ActionFunctor
{
public:
   explicit FnAction(std::function<int(MM::PropertyBase*, MM::ActionType)> f) : f_(std::move(f)) {}
   int Execute(MM::PropertyBase* pProp, MM::ActionType eAct) override { return f_(pProp, eAct); }

private:
   std::function<int(MM::PropertyBase*, MM::ActionType)> f_;
};

template <class U, class Base>
class RegistryDevice : public Base, public RowSink, public RowHandler
{
public:
   RegistryDevice() { this->CreateHubIDProperty(); }

   InSiliScopeHub* Hub() const { return hub_; }

   int HandleRow(size_t row, MM::PropertyBase* pProp, MM::ActionType eAct) override
   {
      if (!hub_)
         return DEVICE_ERR;
      const PropDef& d = PropertyTable()[row];
      if (eAct == MM::BeforeGet)
      {
         if (d.kind == PropKind::Text)
            pProp->Set(d.getText(*hub_).c_str());
         else if (d.kind == PropKind::Integer)
            pProp->Set(static_cast<long>(d.get(*hub_)));
         else
            pProp->Set(d.get(*hub_));
      }
      else if (eAct == MM::AfterSet && !d.readOnly)
      {
         if (d.kind == PropKind::Text)
         {
            std::string s;
            pProp->Get(s);
            if (!d.setText(*hub_, s))
            {
               pProp->Set(d.getText(*hub_).c_str());
               return DEVICE_INVALID_PROPERTY_VALUE;
            }
         }
         else
         {
            double v = 0;
            pProp->Get(v);
            d.set(*hub_, v);
         }
         hub_->Changed(d.invalidate);
      }
      return DEVICE_OK;
   }

   void KeyChanged(const std::string& key) override
   {
      for (size_t row : rows_)
      {
         const PropDef& d = PropertyTable()[row];
         if (d.key == key)
            this->OnPropertyChanged(d.name.c_str(), d.kind == PropKind::Text ? d.getText(*hub_).c_str()
                                                                              : FormatNumber(d.get(*hub_)).c_str());
      }
   }

   void ChoicesChanged(const std::string& key) override
   {
      bool any = false;
      for (size_t row : rows_)
      {
         const PropDef& d = PropertyTable()[row];
         if (d.key == key && d.choices)
         {
            std::vector<std::string> values = d.choices(*hub_);
            this->SetAllowedValues(d.name.c_str(), values);
            any = true;
         }
      }
      if (any)
         this->OnPropertiesChanged();
   }

protected:
   // Finds the hub (the parent) and creates this device's rows. Call first in Initialize().
   int InitRegistry(const char* device)
   {
      hub_ = this->template AssignToHub<InSiliScopeHub>();
      if (!hub_)
         return ERR_NO_HUB;
      rows_.clear();
      const std::vector<PropDef>& table = PropertyTable();
      for (size_t i = 0; i < table.size(); ++i)
      {
         const PropDef& d = table[i];
         if (std::string(d.device) != device || !hub_->Shows(d.tier))
            continue;
         int ret = CreateRow(i, d);
         if (ret != DEVICE_OK)
            return ret;
         rows_.push_back(i);
      }
      hub_->AddSink(this);
      return DEVICE_OK;
   }

   void ShutdownRegistry()
   {
      if (hub_)
         hub_->RemoveSink(this);
   }

   // Shared error codes of the peripherals.
   static constexpr int ERR_NO_HUB = 10101;
   void SetRegistryErrorTexts()
   {
      this->InitializeDefaultErrorMessages();
      this->SetErrorText(ERR_NO_HUB, "No inSiliScope hub: load the 'inSiliScope' hub device and make it this "
                                     "device's parent (the Hardware Configuration Wizard does both).");
   }

private:
   int CreateRow(size_t i, const PropDef& d)
   {
      auto* act = new RowAction(this, i);
      int ret;
      if (d.kind == PropKind::Text)
         ret = this->CreateStringProperty(d.name.c_str(), d.getText(*hub_).c_str(), d.readOnly, act);
      else if (d.kind == PropKind::Integer)
         ret = this->CreateIntegerProperty(d.name.c_str(), static_cast<long>(d.get(*hub_)), d.readOnly, act);
      else
         ret = this->CreateFloatProperty(d.name.c_str(), d.get(*hub_), d.readOnly, act);
      if (ret != DEVICE_OK)
         return ret;
      if (d.kind == PropKind::Text && d.choices)
      {
         std::vector<std::string> values = d.choices(*hub_);
         if (!values.empty())
            this->SetAllowedValues(d.name.c_str(), values);
      }
      else if (d.kind != PropKind::Text && d.lo < d.hi && !d.readOnly)
         this->SetPropertyLimits(d.name.c_str(), d.lo, d.hi);
      return DEVICE_OK;
   }

   InSiliScopeHub* hub_ = nullptr;
   std::vector<size_t> rows_;
};

} // namespace isc
