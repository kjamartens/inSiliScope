#include "SMLMZernike.h"

#include <sstream>

namespace sim {

ZernikeCoefficients ZeroZernikeCoefficients()
{
   ZernikeCoefficients z{};
   z.fill(0.0);
   return z;
}

std::string FormatZernikeCoefficients(const ZernikeCoefficients& coeffs)
{
   std::ostringstream oss;
   for (size_t i = 0; i < coeffs.size(); ++i)
   {
      if (i > 0)
         oss << ",";
      oss << coeffs[i];
   }
   return oss.str();
}

ZernikeCoefficients ParseZernikeCoefficients(const std::string& text, bool& outOk)
{
   ZernikeCoefficients result = ZeroZernikeCoefficients();
   std::vector<std::string> tokens;
   std::string token;
   std::istringstream iss(text);
   while (std::getline(iss, token, ','))
      tokens.push_back(token);

   if (tokens.size() != result.size())
   {
      outOk = false;
      return ZeroZernikeCoefficients();
   }

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

// OSA index -> coefficient value, applied to an otherwise-zero array.
ZernikeCoefficients OneMode(int osaIndex, double value)
{
   ZernikeCoefficients z = ZeroZernikeCoefficients();
   z[static_cast<size_t>(osaIndex)] = value;
   return z;
}

// OSA indices used below (see SMLMZernike.h's ZernikeCoefficients doc
// comment for the full 0-14 mode list):
constexpr int kVerticalAstigmatism = 5;
constexpr int kHorizontalComa = 8;
constexpr int kVerticalTrefoil = 9;
constexpr int kPrimarySpherical = 12;

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
   };
   return names;
}

ZernikeCoefficients ZernikePresetCoefficients(const std::string& presetName)
{
   if (presetName == "AstigmatismWeak")
      return OneMode(kVerticalAstigmatism, 0.075);
   if (presetName == "AstigmatismModerate")
      return OneMode(kVerticalAstigmatism, 0.15);
   if (presetName == "AstigmatismStrong")
      return OneMode(kVerticalAstigmatism, 0.30);
   if (presetName == "ComaWeak")
      return OneMode(kHorizontalComa, 0.05);
   if (presetName == "ComaStrong")
      return OneMode(kHorizontalComa, 0.25);
   if (presetName == "SphericalWeak")
      return OneMode(kPrimarySpherical, 0.075);
   if (presetName == "SphericalStrong")
      return OneMode(kPrimarySpherical, 0.20);
   if (presetName == "TrefoilModerate")
      return OneMode(kVerticalTrefoil, 0.15);
   if (presetName == "MixedRealisticObjective")
   {
      // A plausible "ordinary, not perfectly aligned" objective: a bit of
      // everything rather than one pure mode -- astigmatism (the most
      // common low-order aberration from small mount tilt/decentration),
      // a touch of coma (asymmetric alignment), and a hint of spherical
      // (residual immersion/coverslip index mismatch even near best-case
      // matching). Magnitudes are the "Weak"/mid presets above, combined.
      ZernikeCoefficients z = ZeroZernikeCoefficients();
      z[kVerticalAstigmatism] = 0.10;
      z[kHorizontalComa] = 0.08;
      z[kPrimarySpherical] = 0.05;
      return z;
   }
   // "None" and any unrecognized name.
   return ZeroZernikeCoefficients();
}

} // namespace sim
