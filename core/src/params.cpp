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
   NUM(cellHeightMin), NUM(cellHeightMax),
   NUM(nucLongMin), NUM(nucLongMax), NUM(nucRatioMin), NUM(nucRatioMax),
   NUM(nucHeightMin), NUM(nucHeightMax), NUM(nucOffsetFrac), NUM(nucMargin),
   NUM(cytoRimHeightMin), NUM(cytoRimHeightMax), NUM(cytoEdgeRiseMin), NUM(cytoEdgeRiseMax),
   NUM(cytoMidHeightMin), NUM(cytoMidHeightMax), NUM(cytoMidDistanceMin), NUM(cytoMidDistanceMax),
   NUM(cytoMaxSlope), NUM(cytoDomeSlope), NUM(cytoRelaxUm), NUM(cytoRings), NUM(cytoTheta),
   FLAG(enablePacking), FLAG(allowPackRotation), NUM(packFrac), NUM(relaxIters), NUM(relaxDamping),
   NUM(mtDensity), NUM(mtStartFracMin), NUM(mtStartFracMax), NUM(mtStartOffsetXY),
   NUM(mtEndFracMin), NUM(mtEndFracMax), NUM(mtEndJitterDeg), NUM(mtWobbleTurn), NUM(mtWobbleFactor),
   NUM(mtStepLen), NUM(mtSmoothLen), NUM(mtMinTurnRadius), NUM(mtMinSeparation), NUM(mtMaxZSlope),
   NUM(labelEfficiency), NUM(labelNonBleaching),
};
#undef NUM
#undef FLAG
} // namespace

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
