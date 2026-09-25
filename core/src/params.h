// Cell-field parameters. Defaults = the JS prototype's UI defaults
// (index.html inputs, after params()'s own min/max normalisation).
#pragma once

namespace isc {

struct Params {
   // Field
   double chunkSize = 26;
   double jitter = 0.8;
   double density = 0.33;   // chunk occupancy
   // Cell
   double cellDiamMin = 25, cellDiamMax = 35;
   double cellElongMin = 0.5, cellElongMax = 1;
   double cellBlob = 1.75;
   double cellHeightMin = 3, cellHeightMax = 6;
   // Nucleus
   double nucLongMin = 8, nucLongMax = 12;
   double nucRatioMin = 0.6, nucRatioMax = 1;
   double nucHeightMin = 0.3, nucHeightMax = 0.5;
   double nucOffsetFrac = 0.1;
   double nucMargin = 0.6;
   // Cytoplasm
   double cytoRimHeightMin = 0.1, cytoRimHeightMax = 0.3;
   double cytoEdgeRiseMin = 0.1, cytoEdgeRiseMax = 0.5;
   double cytoMidHeightMin = 1, cytoMidHeightMax = 2;
   double cytoMidDistanceMin = 0.1, cytoMidDistanceMax = 0.3;
   double cytoMaxSlope = 1;
   double cytoDomeSlope = 3;
   double cytoSmoothPasses = 12;
   double cytoRings = 60;
   double cytoTheta = 128;
   // Packing
   bool enablePacking = true;
   bool allowPackRotation = true;
   double packFrac = 1.0;
   double relaxIters = 80;
   double relaxDamping = 0.55;
};

// params() in the prototype clamps every "max" to at least its "min".
void NormalizeParams(Params& p);

// Set a field by its (JS prototype) name; booleans take 0/1. False if unknown.
bool SetParam(Params& p, const char* name, double value);

} // namespace isc
