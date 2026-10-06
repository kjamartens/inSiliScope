///////////////////////////////////////////////////////////////////////////////
// FILE:          PropertyTable.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The property registry: one row per Micro-Manager property of
//                every inSiliScope device, the only place a property is
//                defined (device, name, tier, type, limits, the setting it is
//                bound to, what a change invalidates, a one-line help). The
//                devices create their rows (RegistryDevice.h) for the tiers
//                the session shows (the hub's pre-init Detail); a row above it
//                is never created and its setting keeps its default.
//
//                Rules for adding a row (which device, which tier):
//                spec/MM_DEVICES.md.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <functional>
#include <string>
#include <vector>

class InSiliScopeHub;

namespace isc {

// Who sees a property. Basic: what a microscopist changes at the instrument
// during a session. Advanced: physical parameters to match a setup. Expert:
// numerics, caches, model shapes, custom dyes. Test: only automated checks
// (created only with the environment variable ISC_TEST=1, never by Detail).
enum class Tier
{
   Basic = 0,
   Advanced = 1,
   Expert = 2,
   Test = 3,
};
const char* TierName(Tier t);
// "Basic"/"Advanced"/"Expert" -> the tier (Basic for anything else).
Tier TierFromName(const std::string& s);

enum class PropKind
{
   Float,
   Integer,
   Text,    // a string; with allowed values (choices) an enumeration
};

// What a change throws away: nothing, the precomputed stack only (the
// exposure), the live renderer's cached state only (a cache switch), or both.
enum class Invalidate
{
   None,
   Stack,
   Live,
   All,
};

struct PropDef
{
   const char* device = nullptr;   // the device's name, e.g. "Lasers"
   std::string name;               // the MM property name, no group prefix
   Tier tier = Tier::Basic;
   PropKind kind = PropKind::Float;
   double lo = 0, hi = 0;          // limits (when lo < hi)
   bool readOnly = false;
   Invalidate invalidate = Invalidate::All;
   // The setting's key: rows with the same key are refreshed together when a
   // coupling changes that setting (InSiliScopeHub::Notify). The engine
   // option's name where the setting is one.
   std::string key;
   std::string help;
   // Numeric rows: read/write the setting. Text rows: as text.
   std::function<double(InSiliScopeHub&)> get;
   std::function<void(InSiliScopeHub&, double)> set;
   std::function<std::string(InSiliScopeHub&)> getText;
   std::function<bool(InSiliScopeHub&, const std::string&)> setText;   // false: value refused
   // Text rows: the allowed values (empty: free text). Called again when the
   // hub refreshes the row's choices (InSiliScopeHub::RefreshChoices).
   std::function<std::vector<std::string>(InSiliScopeHub&)> choices;
};

// Every row, in creation order (the devices create theirs in this order).
const std::vector<PropDef>& PropertyTable();

// Number formatting for OnPropertyChanged (10 significant digits).
std::string FormatNumber(double v);

} // namespace isc
