///////////////////////////////////////////////////////////////////////////////
// FILE:          GroundTruth.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The ground-truth tables and the emitters' footprints
//                (GroundTruth.h; docs/ground-truth.md).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "GroundTruth.h"

#include "PsfGeneratorBridge.h"
#include "SMLMSimulation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace sim {

namespace {

thread_local GroundTruth* g_active = nullptr;

const char* StateName(int state)
{
   switch (state)
   {
   case 0: return "blink";
   case 1: return "pre";
   case 2: return "initial-on";
   case 3: return "always-on";
   default: return "?";
   }
}

// A CSV field: as is, or quoted when it holds a comma or a quote.
std::string Field(const std::string& s)
{
   if (s.find_first_of(",\"\n") == std::string::npos)
      return s;
   std::string q = "\"";
   for (char c : s)
      q += c == '"' ? std::string("\"\"") : std::string(1, c);
   return q + "\"";
}

// Appends a number (NaN as an empty field).
void Num(std::string& out, double v, const char* fmt)
{
   out += ',';
   if (!std::isfinite(v))
      return;
   char b[48];
   std::snprintf(b, sizeof b, fmt, v);
   out += b;
}

} // namespace

GroundTruth::Emitter::Emitter()
{
   const double nan = std::numeric_limits<double>::quiet_NaN();
   siteXUm = siteYUm = siteZUm = polarDeg = azimuthDeg = nan;
}

int GroundTruth::LabelOf(const Label& l)
{
   for (size_t i = 0; i < labels.size(); ++i)
   {
      const Label& o = labels[i];
      if (o.structure == l.structure && o.pre == l.pre && o.dye == l.dye && o.mode == l.mode &&
          o.wavelengthNm == l.wavelengthNm)
         return static_cast<int>(i);
   }
   labels.push_back(l);
   return static_cast<int>(labels.size() - 1);
}

void GroundTruth::AddDye(const Dye& d)
{
   const uint64_t key = static_cast<uint64_t>(d.id) | (static_cast<uint64_t>(d.structure & 0xFF) << 32) |
                        (static_cast<uint64_t>(d.state & 0xFF) << 40);
   std::vector<size_t>& at = dyeIndex_[key];
   for (size_t i : at)
   {
      Dye& o = dyes_[i];
      if (o.xUm == d.xUm && o.yUm == d.yUm && o.zUm == d.zUm)
      {
         // seen again (a later run of a recording): its latest light, the photons of both runs
         const double det = o.detectedTotal + d.detectedTotal, emi = o.emittedTotal + d.emittedTotal;
         o = d;
         o.detectedTotal = det;
         o.emittedTotal = emi;
         return;
      }
   }
   at.push_back(dyes_.size());
   dyes_.push_back(d);
}

void GroundTruth::GrowBox(double x0, double y0, double x1, double y1, double z0, double z1)
{
   if (!haveBox)
   {
      boxX0 = x0; boxY0 = y0; boxX1 = x1; boxY1 = y1; boxZ0 = z0; boxZ1 = z1;
      haveBox = true;
      return;
   }
   boxX0 = std::min(boxX0, x0); boxY0 = std::min(boxY0, y0); boxZ0 = std::min(boxZ0, z0);
   boxX1 = std::max(boxX1, x1); boxY1 = std::max(boxY1, y1); boxZ1 = std::max(boxZ1, z1);
}

bool GroundTruth::NeedsDetails(size_t emitterFrom, double box[6], std::vector<double>& ids) const
{
   bool any = false;
   std::unordered_map<uint32_t, char> seen;
   const auto grow = [&](uint32_t id, double x, double y, double z) {
      if (seen.emplace(id, 0).second)
         ids.push_back(id);
      if (!any)
      {
         box[0] = box[2] = x; box[1] = box[3] = y; box[4] = box[5] = z;
         any = true;
         return;
      }
      box[0] = std::min(box[0], x); box[2] = std::max(box[2], x);
      box[1] = std::min(box[1], y); box[3] = std::max(box[3], y);
      box[4] = std::min(box[4], z); box[5] = std::max(box[5], z);
   };
   for (size_t i = emitterFrom; i < emitters.size(); ++i)
      if (emitters[i].kind != Fiducial && std::isnan(emitters[i].siteXUm))
         grow(emitters[i].id, emitters[i].xUm, emitters[i].yUm, emitters[i].zUm);
   for (const Dye& d : dyes_)
      if (std::isnan(d.siteXUm))
         grow(d.id, d.xUm, d.yUm, d.zUm);
   if (any)
   {
      // (half-open windows; the rows' positions went through the host's frame and back)
      const double pad = 1e-3;
      box[0] -= pad; box[1] -= pad; box[4] -= pad;
      box[2] += pad; box[3] += pad; box[5] += pad;
   }
   return any;
}

void GroundTruth::ApplyDetails(const double* rows, size_t n, int stride, size_t emitterFrom)
{
   std::unordered_map<uint64_t, std::vector<size_t>> at;
   at.reserve(n);
   for (size_t i = 0; i < n; ++i)
   {
      const double* r = rows + i * static_cast<size_t>(stride);
      at[static_cast<uint64_t>(r[3]) | (static_cast<uint64_t>(r[4]) << 32)].push_back(i);
   }
   const auto fill = [&](uint32_t id, int structure, double x, double y, double z, double& sx, double& sy, double& sz,
                         double& polar, double& azimuth) {
      const auto it = at.find(static_cast<uint64_t>(id) | (static_cast<uint64_t>(structure & 0xFF) << 32));
      if (it == at.end())
         return;
      for (size_t i : it->second)
      {
         const double* r = rows + i * static_cast<size_t>(stride);
         if (!(std::fabs(r[0] - x) < 1e-6 && std::fabs(r[1] - y) < 1e-6 && std::fabs(r[2] - z) < 1e-6))
            continue;
         sx = r[5]; sy = r[6]; sz = r[7];
         const double dx = r[8], dy = r[9], dz = r[10];
         const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
         if (len > 0)
         {
            const double rad = 180.0 / 3.14159265358979323846;
            polar = std::acos(std::max(-1.0, std::min(1.0, dz / len))) * rad;
            azimuth = std::atan2(dy, dx) * rad;
         }
         return;
      }
   };
   for (size_t i = emitterFrom; i < emitters.size(); ++i)
   {
      Emitter& e = emitters[i];
      if (e.kind != Fiducial && std::isnan(e.siteXUm))
         fill(e.id, e.structure, e.xUm, e.yUm, e.zUm, e.siteXUm, e.siteYUm, e.siteZUm, e.polarDeg, e.azimuthDeg);
   }
   for (Dye& d : dyes_)
      if (std::isnan(d.siteXUm))
         fill(d.id, d.structure, d.xUm, d.yUm, d.zUm, d.siteXUm, d.siteYUm, d.siteZUm, d.polarDeg, d.azimuthDeg);
}

bool GroundTruth::Blinking() const
{
   for (const Label& l : labels)
      if (!l.pre && l.mode != "WideField")
         return true;
   return false;
}

std::string GroundTruth::FramesCsv() const
{
   bool pattern = false;
   for (const Frame& f : frames)
      pattern = pattern || f.patternAngle >= 0;
   std::string out = "frame,t_start_s,exposure_s,stage_x_um,stage_y_um,focus_um,focus_world_um,drift_x_nm,drift_y_nm,drift_z_nm";
   out += pattern ? ",pattern_angle,pattern_phase\n" : "\n";
   for (const Frame& f : frames)
   {
      out += std::to_string(f.frame);
      Num(out, f.tSec, "%.6f");
      Num(out, f.expSec, "%.6g");
      Num(out, f.stageXUm, "%.6f");
      Num(out, f.stageYUm, "%.6f");
      Num(out, f.focusUm, "%.6f");
      Num(out, f.focusWorldUm, "%.6f");
      Num(out, f.driftXNm, "%.4f");
      Num(out, f.driftYNm, "%.4f");
      Num(out, f.driftZNm, "%.4f");
      if (pattern)
      {
         out += ',' + std::to_string(f.patternAngle) + ',' + std::to_string(f.patternPhase);
      }
      out += '\n';
   }
   return out;
}

std::string GroundTruth::EmittersCsv(double minOnCamera, bool fiducials) const
{
   // ThunderSTORM's names where it has one (frame, x [nm], y [nm], z [nm], intensity [photon], offset [photon]).
   std::string out = "frame,t_s,kind,id,target,dye,mode,state,x [nm],y [nm],z [nm],x_world_um,y_world_um,z_world_um,"
                     "site_x_world_um,site_y_world_um,site_z_world_um,on_fraction,photons_detected,intensity [photon],"
                     "photons_emitted,offset [photon],bg_centre_photons,wavelength_nm,excitation,regime,bead_diameter_nm,"
                     "orient_polar_deg,orient_azimuth_deg\n";
   out.reserve(out.size() + emitters.size() * 260);
   static const char* regimes[] = { "splat", "binned", "bead" };
   for (const Emitter& e : emitters)
   {
      if (!(e.onCamera >= minOnCamera))
         continue;
      if (e.kind == Fiducial && !fiducials)
         continue;
      const Label* L = e.label >= 0 && e.label < static_cast<int>(labels.size()) ? &labels[static_cast<size_t>(e.label)] : nullptr;
      out += std::to_string(e.frame);
      const double t = e.frame >= 0 && e.frame < static_cast<long>(frames.size()) ? frames[static_cast<size_t>(e.frame)].tSec
                                                                                   : std::numeric_limits<double>::quiet_NaN();
      Num(out, t, "%.6f");
      out += e.kind == Fiducial ? ",fiducial" : ",emitter";
      out += ',' + (e.kind == Fiducial ? std::string() : std::to_string(e.id));
      out += ',' + Field(L ? L->target : std::string(e.kind == Fiducial ? "bead" : ""));
      out += ',' + Field(L ? L->dye : std::string());
      out += ',' + Field(L ? L->mode : std::string());
      out += ',' + std::string(e.kind == Fiducial ? "" : StateName(e.state));
      Num(out, (e.xPx + 0.5) * pixelNm, "%.3f");
      Num(out, (e.yPx + 0.5) * pixelNm, "%.3f");
      Num(out, e.zFocusNm, "%.3f");
      Num(out, e.xUm, "%.6f");
      Num(out, e.yUm, "%.6f");
      Num(out, e.zUm, "%.6f");
      Num(out, e.siteXUm, "%.6f");
      Num(out, e.siteYUm, "%.6f");
      Num(out, e.siteZUm, "%.6f");
      Num(out, e.onFraction, "%.6g");
      Num(out, e.detected, "%.4f");
      Num(out, e.onCamera, "%.4f");
      Num(out, e.emitted, "%.4f");
      Num(out, e.bg, "%.4f");
      Num(out, e.bgCentre, "%.4f");
      Num(out, L ? L->wavelengthNm : std::numeric_limits<double>::quiet_NaN(), "%.4g");
      Num(out, e.excite, "%.6g");
      out += ',' + std::string(regimes[std::min<int>(e.regime, 2)]);
      Num(out, e.kind == Fiducial ? e.beadNm : std::numeric_limits<double>::quiet_NaN(), "%.4g");
      Num(out, e.polarDeg, "%.4f");
      Num(out, e.azimuthDeg, "%.4f");
      out += '\n';
   }
   return out;
}

std::string GroundTruth::FiducialsCsv() const
{
   std::string out = "frame,t_s,x [nm],y [nm],z [nm],x_world_um,y_world_um,z_world_um,bead_diameter_nm,photons_detected,"
                     "intensity [photon],offset [photon],bg_centre_photons\n";
   for (const Emitter& e : emitters)
   {
      if (e.kind != Fiducial)
         continue;
      out += std::to_string(e.frame);
      const double t = e.frame >= 0 && e.frame < static_cast<long>(frames.size()) ? frames[static_cast<size_t>(e.frame)].tSec
                                                                                   : std::numeric_limits<double>::quiet_NaN();
      Num(out, t, "%.6f");
      Num(out, (e.xPx + 0.5) * pixelNm, "%.3f");
      Num(out, (e.yPx + 0.5) * pixelNm, "%.3f");
      Num(out, e.zFocusNm, "%.3f");
      Num(out, e.xUm, "%.6f");
      Num(out, e.yUm, "%.6f");
      Num(out, e.zUm, "%.6f");
      Num(out, e.beadNm, "%.4g");
      Num(out, e.detected, "%.4f");
      Num(out, e.onCamera, "%.4f");
      Num(out, e.bg, "%.4f");
      Num(out, e.bgCentre, "%.4f");
      out += '\n';
   }
   return out;
}

std::string GroundTruth::DyesCsv() const
{
   std::string out = "id,target,dye,mode,state,x_world_um,y_world_um,z_world_um,site_x_world_um,site_y_world_um,"
                     "site_z_world_um,t_on_s,t_off_s,photons_per_s,photons_detected_total,photons_emitted_total,"
                     "orient_polar_deg,orient_azimuth_deg\n";
   out.reserve(out.size() + dyes_.size() * 120);
   for (const Dye& d : dyes_)
   {
      const Label* L = d.label >= 0 && d.label < static_cast<int>(labels.size()) ? &labels[static_cast<size_t>(d.label)] : nullptr;
      out += std::to_string(d.id);
      out += ',' + Field(L ? L->target : std::string());
      out += ',' + Field(L ? L->dye : std::string());
      out += ',' + Field(L ? L->mode : std::string());
      out += ',' + std::string(StateName(d.state));
      Num(out, d.xUm, "%.6f");
      Num(out, d.yUm, "%.6f");
      Num(out, d.zUm, "%.6f");
      Num(out, d.siteXUm, "%.6f");
      Num(out, d.siteYUm, "%.6f");
      Num(out, d.siteZUm, "%.6f");
      Num(out, d.tOnSec, "%.6f");
      Num(out, d.tOffSec, "%.6f");
      Num(out, d.photonsPerSec, "%.6g");
      Num(out, d.detectedTotal, "%.6g");
      Num(out, d.emittedTotal, "%.6g");
      Num(out, d.polarDeg, "%.4f");
      Num(out, d.azimuthDeg, "%.4f");
      out += '\n';
   }
   return out;
}

std::vector<std::pair<std::string, std::string>> GroundTruth::Files(const std::vector<double>& thresholds) const
{
   std::vector<std::pair<std::string, std::string>> out;
   out.emplace_back("frames.csv", FramesCsv());
   bool beads = false;
   for (const Emitter& e : emitters)
      beads = beads || e.kind == Fiducial;
   const bool blinking = Blinking();
   if (blinking)
      for (double t : thresholds)
      {
         char name[64];
         std::snprintf(name, sizeof name, "localizations_min%g.csv", t);
         out.emplace_back(name, EmittersCsv(t, true));
      }
   else if (beads)
      out.emplace_back("fiducials.csv", FiducialsCsv());
   if (!dyes_.empty())
      out.emplace_back("dyes.csv", DyesCsv());
   return out;
}

GroundTruth* ActiveGroundTruth() { return g_active; }

GroundTruthScope::GroundTruthScope(GroundTruth* gt) : prev_(g_active) { g_active = gt; }
GroundTruthScope::~GroundTruthScope() { g_active = prev_; }

EmitterFootprint FootprintOf(std::vector<float>& scratch, unsigned W, unsigned H, const PsfKernelCache* kernel,
                             int zIndex, double sigmaPx, double xPx, double yPx, const std::vector<float>& bg)
{
   EmitterFootprint fp;
   const size_t n = static_cast<size_t>(W) * H;
   if (scratch.size() != n)
      scratch.assign(n, 0.0f);
   int cx, cy, r;
   if (kernel && kernel->valid)
   {
      SplatPlan plan;
      if (!PlanSplat(*kernel, zIndex, xPx, yPx, 1.0, kernel->interpMode, plan))
         return fp;
      SplatRows(scratch, W, H, 0, static_cast<int>(H), *kernel, plan, 1.0);
      cx = plan.st.x0;
      cy = plan.st.y0;
      r = kernel->halfWidthOversampled / std::max(1, kernel->oversampling);
   }
   else
   {
      RenderGaussianPSF(scratch, W, H, xPx, yPx, sigmaPx, 1.0);
      cx = static_cast<int>(std::lround(xPx));
      cy = static_cast<int>(std::lround(yPx));
      r = static_cast<int>(std::ceil(3.0 * sigmaPx));
   }
   const int x0 = std::max(0, cx - r), x1 = std::min(static_cast<int>(W) - 1, cx + r);
   const int y0 = std::max(0, cy - r), y1 = std::min(static_cast<int>(H) - 1, cy + r);
   double sum = 0, dot = 0;
   for (int y = y0; y <= y1; ++y)
   {
      float* row = scratch.data() + static_cast<size_t>(y) * W;
      const float* b = bg.data() + static_cast<size_t>(y) * W;
      for (int x = x0; x <= x1; ++x)
         if (row[x] != 0.0f)
         {
            sum += row[x];
            dot += static_cast<double>(row[x]) * b[x];
            row[x] = 0.0f;
         }
   }
   fp.onCamera = sum;
   fp.bg = sum > 0 ? dot / sum : std::numeric_limits<double>::quiet_NaN();
   const long px = std::lround(xPx), py = std::lround(yPx);
   fp.bgCentre = px >= 0 && py >= 0 && px < static_cast<long>(W) && py < static_cast<long>(H)
                    ? bg[static_cast<size_t>(py) * W + static_cast<size_t>(px)]
                    : std::numeric_limits<double>::quiet_NaN();
   return fp;
}

SparseFootprint SparseFootprintOf(const std::vector<float>& image, unsigned W, unsigned H, double xPx, double yPx,
                                  double cut)
{
   SparseFootprint fp;
   double sum = 0;
   for (float v : image)
      sum += v;
   fp.onCamera = sum;
   const float lim = static_cast<float>(cut * sum);
   for (size_t i = 0; i < image.size(); ++i)
      if (image[i] > 0 && image[i] >= lim)
      {
         fp.idx.push_back(static_cast<uint32_t>(i));
         fp.w.push_back(image[i]);
      }
   const long px = std::lround(xPx), py = std::lround(yPx);
   if (px >= 0 && py >= 0 && px < static_cast<long>(W) && py < static_cast<long>(H))
      fp.centre = py * static_cast<long>(W) + px;
   return fp;
}

EmitterFootprint ApplyFootprint(const SparseFootprint& fp, const std::vector<float>& bg)
{
   EmitterFootprint out;
   out.onCamera = fp.onCamera;
   double sw = 0, dot = 0;
   for (size_t k = 0; k < fp.idx.size(); ++k)
   {
      sw += fp.w[k];
      dot += static_cast<double>(fp.w[k]) * bg[fp.idx[k]];
   }
   out.bg = sw > 0 ? dot / sw : std::numeric_limits<double>::quiet_NaN();
   out.bgCentre = fp.centre >= 0 ? bg[static_cast<size_t>(fp.centre)] : std::numeric_limits<double>::quiet_NaN();
   return out;
}

} // namespace sim
