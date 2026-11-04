///////////////////////////////////////////////////////////////////////////////
// FILE:          GroundTruth.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The ground truth of a fluorescence movie (docs/ground-truth.md): what
//                FluorescenceMovie::Render drew, written as tables -- one row
//                per frame (time, stage, focus, drift, the pattern's state),
//                one per emitter per frame (every drawn blink, and the beads in
//                a movie with a blinking label: world and camera position,
//                expected photons detected, on the camera, emitted, the
//                non-blink light under its PSF), one per continuous dye
//                (WideField, dSTORM initial ON, PALM pre state: position, its
//                window, photons per second). A host makes a GroundTruth and
//                sets it as the thread's sink (GroundTruthScope) around the
//                movies it renders (a movie, LiveScope's runs one after
//                another): every FluorescenceMovie on that thread fills it.
//                The frames' pixels are the same with a sink as without.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim {

struct PsfKernelCache;

class GroundTruth
{
public:
   // Emitter rows with fewer expected photons on the camera than this in their frame are left out.
   double minPhotons = 5;
   // The continuous dyes' list (Dye rows; the volumes need it), and whether Files writes it as dyes.csv (only without a
   // blinking label: a movie's ground truth is its localizations or its dye list).
   bool dyeList = true;
   bool writeDyeCsv = true;

   // A label as drawn: a structure's dye in its mode, the main state or the PALM pre state.
   struct Label
   {
      int structure = 0;
      bool pre = false;
      std::string target, dye, mode;
      double wavelengthNm = 0;          // the detected light's (the PSF's) wavelength
      double emittedPerDetected = 0;    // emitted photons per detected one (1 / (collection x path x QE))
   };
   struct Frame
   {
      long frame = 0;
      double tSec = 0, expSec = 0;        // the sample's time at the frame's start (s), the exposure
      double stageXUm = 0, stageYUm = 0;  // the FOV centre (world um)
      double focusUm = 0;                 // the focus (z stage) as set, um above the focus reference
      double focusWorldUm = 0;            // the focal plane's world height (um above the coverslip), drift in
      double driftXNm = 0, driftYNm = 0, driftZNm = 0;
      int patternAngle = -1, patternPhase = -1;   // a structured pattern's state (-1: none)
   };
   enum Kind : uint8_t { Emitter_ = 0, Fiducial = 1 };
   enum Regime : uint8_t { Splat = 0, Binned = 1, Bead = 2 };
   struct Emitter
   {
      long frame = 0;
      uint8_t kind = Emitter_, regime = Splat;
      int label = -1;                     // index into labels (-1: a bead)
      uint32_t id = 0;                    // the dye's id (the core's per-dye hash)
      int structure = -1, state = 0;
      double xUm = 0, yUm = 0, zUm = 0;   // world um, undrifted (z above the coverslip)
      double xPx = 0, yPx = 0;            // camera px, drift in (an integer: a pixel's centre)
      double zFocusNm = 0;                // height above the focal plane (nm), drift in
      double onFraction = 1;              // the part of the frame it was on
      double detected = 0, onCamera = 0, emitted = 0;   // expected photons (before shot noise)
      double bg = 0, bgCentre = 0;        // the non-blink light per pixel under its PSF (PSF-weighted), at its centre
      double excite = 1;                  // the excitation factor at its site
      double beadNm = 0;                  // a bead's diameter
      // The dye's target site (world um) and mean dipole (degrees: polar from z, azimuth from x; NaN: free) -- filled
      // by the host's details pass (ApplyDetails); NaN until then.
      double siteXUm, siteYUm, siteZUm, polarDeg, azimuthDeg;
      Emitter();
   };
   struct Dye
   {
      int label = -1;
      uint32_t id = 0;
      int structure = 0, state = 0;
      uint32_t run = 0;                   // the movie of the recording that added it (EndMovie's merge)
      double xUm = 0, yUm = 0, zUm = 0;   // world um
      double tOnSec = 0, tOffSec = 0;     // its window in the sample's time (s; tOff: bleached / switched off)
      double photonsPerSec = 0;           // detected, at its excitation (0: no light reaches it)
      // Over the recorded frames (every movie that saw it: its window's overlap with each movie's time x its rate):
      // the photons it gave, detected and emitted (the density volume's weight).
      double detectedTotal = 0, emittedTotal = 0;
      // As an Emitter's (ApplyDetails).
      double siteXUm = NaN(), siteYUm = NaN(), siteZUm = NaN(), polarDeg = NaN(), azimuthDeg = NaN();
   };
   static double NaN() { return std::numeric_limits<double>::quiet_NaN(); }

   std::vector<Label> labels;
   std::vector<Frame> frames;
   std::vector<Emitter> emitters;
   unsigned width = 0, height = 0;
   double pixelNm = 0;
   // The world box the movies queried (um; their FOV, margin and z range): the density volume's extent.
   double boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0, boxZ0 = 0, boxZ1 = 0;
   bool haveBox = false;
   // Frames already in (the next movie's frame 0 is this).
   long frameBase = 0;

   // A label's index (added when new).
   int LabelOf(const Label& l);
   // A continuous dye's row. The same dye seen again by a later movie of the recording is merged at that movie's
   // end (EndMovie: the later row, the totals summed).
   void AddDye(const Dye& d);
   const std::vector<Dye>& Dyes() const { return dyes_; }
   void GrowBox(double x0, double y0, double x1, double y1, double z0, double z1);
   // Whether any label blinks (dSTORM, PALM, DNA-PAINT): the beads then go into the emitter tables.
   bool Blinking() const;
   // A movie's end. Merges the dyes this movie saw again (sorted in place: a WideField field by a nucleus holds ~4 M
   // dyes, and no index beside them), then gives the rows still without a site -- the emitter rows from emitterFrom
   // on, and the continuous dyes when dyes.csv will be written (writeDyeCsv, no blinking label: the volumes need only
   // positions) -- their site and orientation: per chunk of at most chunk rows along x, query(box: x0, y0, x1, y1, z0,
   // z1 um padded; their ids, each once; rows) gives the core's rows (isc_dye_details_in_window, stride doubles each:
   // x, y, z, id, structure, site xyz, dipole xyz), matched by id, structure and position. Returns the rows asked for.
   using DetailQuery = std::function<bool(const double box[6], const std::vector<double>& ids, std::vector<double>& rows)>;
   size_t EndMovie(size_t emitterFrom, const DetailQuery& query, int stride, size_t chunk = size_t(1) << 18);

   // The tables (CSV, a header line; docs/ground-truth.md). Emitters: rows with onCamera >= minOnCamera, by frame;
   // fiducials: whether the beads go in (else FiducialsCsv has them).
   std::string FramesCsv() const;
   std::string EmittersCsv(double minOnCamera, bool fiducials) const;
   std::string FiducialsCsv() const;
   std::string DyesCsv() const;
   // The continuous dyes as density stacks (Voxelize.h, the converter page's code): per target and state, the
   // detected photons over the recorded frames (photons_detected_total) summed per voxel of lateralNm x lateralNm x
   // axialNm, every stack on one grid (the box of all their dyes: they overlap in Fiji and napari), 8 bits scaled to
   // the stack's largest voxel (the scale in the description, Fiji's calibration), deflated planes, a label per slice
   // (its z), an ImageJ TIFF (the voxel size in X/YResolution and the spacing, unit micron; the world position of
   // voxel (0, 0, 0)'s corner as insiliscope.origin-um and ImageJ's origin). Above capBytes in all (uncompressed), the
   // voxels grow (note says so). Name, file bytes, and per stack a line for the README.
   struct Volume
   {
      std::string name, bytes, line;
   };
   std::vector<Volume> Volumes(double lateralNm, double axialNm, double capBytes, std::string* note) const;
   // Every file (name, contents): frames.csv; with a blinking label, a localizations_min<N>.csv per threshold N
   // (photons on the camera; the beads in them too); else fiducials.csv for beads; dyes.csv and the volumes
   // (volumeLateralNm 0: none) with continuous dyes; README.txt describing them.
   std::vector<std::pair<std::string, std::string>> Files(const std::vector<double>& thresholds,
                                                          double volumeLateralNm = 10, double volumeAxialNm = 10,
                                                          double volumeCapBytes = 1024.0 * 1024 * 1024) const;
   // The same files handed to sink one at a time as each is made (a WideField ground truth's dyes.csv is ~150 bytes
   // a dye: the viewer packs each and lets it go, never holding every file and the packed copy at once).
   using FileSink = std::function<void(const std::string& name, std::string&& bytes)>;
   void Files(const FileSink& sink, const std::vector<double>& thresholds, double volumeLateralNm = 10,
              double volumeAxialNm = 10, double volumeCapBytes = 1024.0 * 1024 * 1024) const;

private:
   std::vector<Dye> dyes_;
   size_t runFrom_ = 0;   // the dyes before this movie's
   uint32_t run_ = 0;
};

// The ground truth the movies on this thread fill (nullptr: none).
GroundTruth* ActiveGroundTruth();
class GroundTruthScope
{
public:
   explicit GroundTruthScope(GroundTruth* gt);
   ~GroundTruthScope();
   GroundTruthScope(const GroundTruthScope&) = delete;
   GroundTruthScope& operator=(const GroundTruthScope&) = delete;

private:
   GroundTruth* prev_;
};

// One emitter's PSF on the camera: of one photon at camera position (xPx, yPx) on kernel plane zIndex (kernel
// nullptr or invalid: the Gaussian of sigmaPx), the part that lands on the camera (as the splat draws it), the
// mean of bg (W x H) weighted by it, and bg at the pixel under its centre (NaN off the camera). scratch: W x H
// zeros, left zero.
struct EmitterFootprint
{
   double onCamera = 0, bg = 0, bgCentre = 0;
};
EmitterFootprint FootprintOf(std::vector<float>& scratch, unsigned W, unsigned H, const PsfKernelCache* kernel,
                             int zIndex, double sigmaPx, double xPx, double yPx, const std::vector<float>& bg);
// The same from an image of the emitter alone (one photon over the whole camera: a bead and its halo), kept for
// the frames at the same focus and drift: its sum on the camera, and the pixels holding at least cut of it (they
// weigh the background; a bead's halo reaches past the FOV).
struct SparseFootprint
{
   double onCamera = 0;
   std::vector<uint32_t> idx;
   std::vector<float> w;
   long centre = -1;
};
SparseFootprint SparseFootprintOf(const std::vector<float>& image, unsigned W, unsigned H, double xPx, double yPx,
                                  double cut = 1e-4);
EmitterFootprint ApplyFootprint(const SparseFootprint& fp, const std::vector<float>& bg);

} // namespace sim
