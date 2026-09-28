///////////////////////////////////////////////////////////////////////////////
// FILE:          Illumination.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Excitation pattern in the sample plane, modality-neutral.
//                Anchored to the objective: stage moves slide the sample
//                under it. Coordinates are um relative to the optical axis
//                (= the FOV centre); values are relative excitation, peak 1.
//                WideField reads its emission rate and deposits its bleach
//                dose from it, so a new pattern (a beam, a TIRF footprint, a
//                structured pattern) needs no renderer change. SR keeps its
//                FluoParam_IllumProfile field for now.
//
// LICENSE:       BSD (see license.txt)

#pragma once

namespace sim {

class IlluminationPattern
{
public:
   virtual ~IlluminationPattern() = default;
   // Relative excitation at (dx, dy) um from the axis, peak 1.
   virtual double At(double dxUm, double dyUm) const = 0;
   // Half-open rect [x0, x1) x [y0, y1) (um from the axis) outside which At is 0.
   virtual void Support(double& x0, double& y0, double& x1, double& y1) const = 0;
   // Cell-centre samples on an nx x ny grid (row-major, y outer) whose cell
   // (0, 0) starts at (x0Um, y0Um) relative to the axis. Override for exact
   // area averages.
   virtual void Sample(double x0Um, double y0Um, double pitchUm, unsigned nx, unsigned ny, float* out) const;
};

// 1 inside a w x h rect centred on the axis, 0 outside (the FOV square).
class SquareIllumination : public IlluminationPattern
{
public:
   SquareIllumination(double widthUm, double heightUm) : w_(widthUm), h_(heightUm) {}
   double At(double dxUm, double dyUm) const override;
   void Support(double& x0, double& y0, double& x1, double& y1) const override;

private:
   double w_, h_;
};

} // namespace sim
