#include "SMLMZernike.h"

#include <initializer_list>
#include <sstream>
#include <utility>

namespace sim {

ZernikeCoefficients ZeroZernikeCoefficients()
{
   ZernikeCoefficients z{};
   z.fill(0.0);
   return z;
}

std::string FormatZernikeCoefficients(const ZernikeCoefficients& coeffs, char separator)
{
   std::ostringstream oss;
   for (size_t i = 0; i < coeffs.size(); ++i)
   {
      if (i > 0)
         oss << separator;
      oss << coeffs[i];
   }
   return oss.str();
}

ZernikeCoefficients ParseZernikeCoefficients(const std::string& text, bool& outOk)
{
   ZernikeCoefficients result = ZeroZernikeCoefficients();
   // Commas, semicolons and whitespace all separate values (see
   // SMLMZernike.h -- MMCore rejects a comma in any property value SET
   // through it). Empty fields ("0,,0") are an error, not skipped, so a
   // missing value can't silently shift every later mode down by one.
   std::vector<std::string> tokens;
   std::string token;
   bool pendingSeparator = false;
   for (char ch : text)
   {
      bool sep = (ch == ',' || ch == ';');
      bool space = (ch == ' ' || ch == '\t');
      if (sep || space)
      {
         if (!token.empty())
         {
            tokens.push_back(token);
            token.clear();
            pendingSeparator = false;
         }
         if (sep)
         {
            if (pendingSeparator)
            {
               outOk = false;
               return ZeroZernikeCoefficients();
            }
            pendingSeparator = true;
         }
         continue;
      }
      token += ch;
   }
   if (!token.empty())
      tokens.push_back(token);

   if (tokens.size() != 15 && tokens.size() != result.size())
   {
      outOk = false;
      return ZeroZernikeCoefficients();
   }

   // A 15-value list fills indices 0-14; 15-27 stay zero (padding).
   for (size_t i = 0; i < tokens.size(); ++i)
   {
      try
      {
         size_t pos = 0;
         result[i] = std::stod(tokens[i], &pos);
      }
      catch (const std::exception&)
      {
         outOk = false;
         return ZeroZernikeCoefficients();
      }
   }

   outOk = true;
   return result;
}

namespace {

ZernikeCoefficients Modes(std::initializer_list<std::pair<int, double>> modes)
{
   ZernikeCoefficients z = ZeroZernikeCoefficients();
   for (const auto& m : modes)
      z[static_cast<size_t>(m.first)] = m.second;
   return z;
}

// OSA indices used below (see SMLMZernike.h's ZernikeCoefficients doc
// comment for the full 0-27 mode list):
constexpr int kDefocus = 4;
constexpr int kVerticalAstigmatism = 5;
constexpr int kObliqueTrefoil = 6;
constexpr int kVerticalComa = 7;
constexpr int kPrimarySpherical = 12;
constexpr int kVerticalSecondaryAstigmatism = 13;
constexpr int kVerticalTertiaryAstigmatism = 25;

} // namespace

const std::vector<std::string>& ZernikePresetNames()
{
   static const std::vector<std::string> names = {
      "None",
      "AstigmatismWeak",
      "AstigmatismModerate",
      "AstigmatismStrong",
      "ComaWeak",
      "ComaStrong",
      "SphericalWeak",
      "SphericalStrong",
      "TrefoilModerate",
      "MixedRealisticObjective",
      "SaddlePoint",
      "ExtendedRange",
      "ExtendedRangeStrong",
   };
   return names;
}

// Values: webSMLM's PSF_ZERNIKE_PRESETS, one-to-one (webSMLM name in the
// trailing comment) -- see SMLMZernike.h.
ZernikeCoefficients ZernikePresetCoefficients(const std::string& presetName)
{
   if (presetName == "AstigmatismWeak") // astigWeak
      return Modes({{kVerticalAstigmatism, 0.07}});
   if (presetName == "AstigmatismModerate") // astigModerate
      return Modes({{kVerticalAstigmatism, 0.15}});
   if (presetName == "AstigmatismStrong") // astigStrong
      return Modes({{kVerticalAstigmatism, 0.30}});
   if (presetName == "ComaWeak") // comaWeak
      return Modes({{kVerticalComa, 0.10}});
   if (presetName == "ComaStrong") // comaStrong
      return Modes({{kVerticalComa, 0.25}});
   if (presetName == "SphericalWeak") // sphericalWeak
      return Modes({{kPrimarySpherical, 0.10}});
   if (presetName == "SphericalStrong") // sphericalStrong
      return Modes({{kPrimarySpherical, 0.25}});
   if (presetName == "TrefoilModerate") // trefoilModerate
      return Modes({{kObliqueTrefoil, 0.15}});
   if (presetName == "MixedRealisticObjective") // mixedRealistic
   {
      // A plausible "ordinary, not perfectly aligned" objective: a bit of
      // everything rather than one pure mode -- a little defocus,
      // astigmatism (the most common low-order aberration from small mount
      // tilt/decentration), a touch of coma (asymmetric alignment), and a
      // hint of spherical (residual immersion/coverslip index mismatch).
      return Modes({{kDefocus, 0.05}, {kVerticalAstigmatism, 0.08}, {kVerticalComa, 0.06}, {kPrimarySpherical, 0.07}});
   }
   if (presetName == "SaddlePoint") // saddlePoint
      return Modes({{kVerticalAstigmatism, 0.6}, {kVerticalSecondaryAstigmatism, 0.2}});
   if (presetName == "ExtendedRange") // extendedRange
      return Modes({{kVerticalAstigmatism, 1.0}, {kVerticalSecondaryAstigmatism, 0.4}});
   if (presetName == "ExtendedRangeStrong") // extendedRangeStrong
      return Modes({{kVerticalAstigmatism, 1.8}, {kVerticalSecondaryAstigmatism, 0.8}, {kVerticalTertiaryAstigmatism, 0.3}});
   // "None" and any unrecognized name.
   return ZeroZernikeCoefficients();
}

} // namespace sim
