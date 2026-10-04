#include "params.h"

#include <cstring>

namespace isc {

namespace {
struct Field { const char* name; double Params::* num; bool Params::* flag; };

#define NUM(n) { #n, &Params::n, nullptr }
#define FLAG(n) { #n, nullptr, &Params::n }
const Field kFields[] = {
   NUM(chunkSize), NUM(jitter), NUM(density),
   NUM(cellDiamMin), NUM(cellDiamMax), NUM(cellElongMin), NUM(cellElongMax), NUM(cellBlob), NUM(cellRough), NUM(cellFractalDim),
   NUM(nucLongMin), NUM(nucLongMax), NUM(nucRatioMin), NUM(nucRatioMax),
   NUM(nucHeightMin), NUM(nucHeightMax), NUM(nucOffsetFrac), NUM(nucMargin),
   NUM(nucBaseMin), NUM(nucBaseMax), NUM(nucIrregMin), NUM(nucIrregMax), NUM(nucBendMin), NUM(nucBendMax),
   NUM(nucSmooth), NUM(nucThickIrreg), NUM(nucAsym), NUM(nucWidestMin), NUM(nucWidestMax),
   NUM(cytoRimHeightMin), NUM(cytoRimHeightMax), NUM(cytoEdgeRiseMin), NUM(cytoEdgeRiseMax),
   NUM(cytoMidHeightMin), NUM(cytoMidHeightMax), NUM(cytoMidDistanceMin), NUM(cytoMidDistanceMax),
   NUM(cytoMaxSlope), NUM(cytoDomeSlope), NUM(cytoRelaxUm), NUM(cytoRings), NUM(cytoTheta),
   FLAG(enablePacking), FLAG(allowPackRotation), NUM(packFrac), NUM(relaxIters), NUM(relaxDamping),
   NUM(mtDensity), NUM(mtStartDecayPct), NUM(mtEndDecayPct), NUM(mtDirKappa), NUM(mtWobbleTurn), NUM(mtWobbleFactor),
   NUM(mtStepLen), NUM(mtSmoothLen), NUM(mtMinTurnRadius), NUM(mtMinSeparation), NUM(mtMaxZSlope),
   NUM(labelEfficiency), NUM(labelNonBleaching),
};
#undef NUM
#undef FLAG
} // namespace

uint64_t PackingFingerprint(const Params& p)
{
   uint64_t h = 14695981039346656037ull;
   auto mix = [&](const void* data, size_t n) {
      const unsigned char* b = static_cast<const unsigned char*>(data);
      for (size_t i = 0; i < n; i++) {
         h ^= b[i];
         h *= 1099511628211ull;
      }
   };
   for (const Field& f : kFields) {
      if (std::strncmp(f.name, "mt", 2) == 0 || std::strncmp(f.name, "label", 5) == 0) continue;
      const double v = f.num ? p.*(f.num) : (p.*(f.flag) ? 1.0 : 0.0);
      mix(f.name, std::strlen(f.name));
      mix(&v, sizeof v);
   }
   return h;
}

bool SetParam(Params& p, const char* name, double value)
{
   for (const Field& f : kFields) {
      if (std::strcmp(f.name, name) != 0) continue;
      if (f.num) p.*(f.num) = value;
      else p.*(f.flag) = value != 0;
      return true;
   }
   return false;
}

} // namespace isc
