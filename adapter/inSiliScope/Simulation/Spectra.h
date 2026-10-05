///////////////////////////////////////////////////////////////////////////////
// FILE:          Spectra.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Spectra and the light path's physics (issue 16): sampled
//                spectra on one grid, ideal filters, absorption cross
//                section, laser photon flux, detected fraction and effective
//                emission wavelength. The twin of
//                web/prototype/scope/spectra.js (the JS reference): same
//                operations in the same order. Pure functions, no state.
//
//                Grid: 300..900 nm in 1 nm steps (data/dyes/
//                fpbase_spectra.json). Dye excitation/emission are
//                normalised to peak 1 (FPbase); filter transmission and
//                camera QE are fractions.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <vector>

namespace sim {

constexpr double kSpectraGridMinNm = 300.0, kSpectraGridMaxNm = 900.0, kSpectraGridStepNm = 1.0;
constexpr int kSpectraGridN = 601;
inline double SpectraGridNm(int i) { return kSpectraGridMinNm + i * kSpectraGridStepNm; }
constexpr double kAvogadro = 6.02214076e23;

using Spectrum = std::vector<double>;   // kSpectraGridN samples

// Value at nm (linear between grid points, 0 outside).
double SampleAt(const double* values, double nm);
inline double SampleAt(const Spectrum& s, double nm) { return SampleAt(s.data(), nm); }

// Skewed Gaussian, peak 1 at peakNm, FWHM-like spreads on the short / long
// side (Custom dyes: no FPbase data).
Spectrum SkewedGaussian(double peakNm, double leftWidthNm, double rightWidthNm);
Spectrum ParametricExcitation(double peakNm, double widthNm);   // long blue tail
Spectrum ParametricEmission(double peakNm, double widthNm);     // long red tail

enum class FilterType { None, LongPass, ShortPass, BandPass, Notch };
struct IdealFilterSpec
{
   FilterType type;
   double edgeNm, loNm, hiNm;
   int nBands;            // Notch: reflected bands
   double bands[8][2];
};
// Transmission of an ideal filter (hard edges on the grid).
Spectrum IdealTransmission(const IdealFilterSpec& spec);

// Absorption cross section, um^2, at nm: ln(10) 1000 eps(nm) / N_A cm^2 x 1e8,
// eps(nm) = epsPeak x the normalised excitation spectrum there.
double CrossSectionUm2(double epsPeak, const Spectrum& ex, double nm);
// Photon flux of a laser line, photons / um^2 / s, at kW/cm^2.
double PhotonFluxPerUm2(double kWPerCm2, double nm);
// Fraction of the solid angle an objective of NA collects in index n.
double CollectionEfficiency(double na, double n);

// Detection of an emission spectrum through transmission curves: fraction =
// sum(E T1 T2 ...) / sum(E) and the mean detected wavelength (NaN if none).
struct Detection { double fraction, lambdaNm; };
Detection Detect(const Spectrum& em, const std::vector<const Spectrum*>& curves);

} // namespace sim
