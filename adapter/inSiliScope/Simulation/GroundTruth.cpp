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
#include "Voxelize.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <tuple>

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

std::string Fmt(const char* f, double v)
{
   char b[64];
   std::snprintf(b, sizeof b, f, v);
   return b;
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
   dyes_.push_back(d);
   dyes_.back().run = run_;
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

size_t GroundTruth::EndMovie(size_t emitterFrom, const DetailQuery& query, int stride, size_t chunk)
{
   // The dyes this movie saw again: sorted by (id, structure, state, position, movie), each run of one dye merged into
   // its last row (its latest light) with the totals summed. Only when an earlier movie added dyes.
   if (runFrom_ > 0 && dyes_.size() > runFrom_)
   {
      const auto key = [](const Dye& d) {
         return std::make_tuple(d.id, d.structure, d.state, d.xUm, d.yUm, d.zUm, d.run);
      };
      std::sort(dyes_.begin(), dyes_.end(), [&](const Dye& a, const Dye& b) { return key(a) < key(b); });
      size_t w = 0;
      for (size_t i = 0; i < dyes_.size(); ++i)
      {
         const Dye& d = dyes_[i];
         if (w > 0)
         {
            Dye& o = dyes_[w - 1];
            if (o.id == d.id && o.structure == d.structure && o.state == d.state && o.xUm == d.xUm && o.yUm == d.yUm &&
                o.zUm == d.zUm)
            {
               const double det = o.detectedTotal + d.detectedTotal, emi = o.emittedTotal + d.emittedTotal;
               o = d;
               o.detectedTotal = det;
               o.emittedTotal = emi;
               continue;
            }
         }
         dyes_[w++] = d;
      }
      dyes_.resize(w);
   }
   runFrom_ = dyes_.size();
   run_++;

   // The rows still without a site, along x (16 bytes each), in chunks: each one core query over its box.
   struct Pending
   {
      double x;
      uint32_t i;
      bool dye;
   };
   std::vector<Pending> todo;
   for (size_t i = emitterFrom; i < emitters.size(); ++i)
      if (emitters[i].kind != Fiducial && std::isnan(emitters[i].siteXUm))
         todo.push_back({ emitters[i].xUm, static_cast<uint32_t>(i), false });
   if (writeDyeCsv && !Blinking())
      for (size_t i = 0; i < dyes_.size(); ++i)
         if (std::isnan(dyes_[i].siteXUm))
            todo.push_back({ dyes_[i].xUm, static_cast<uint32_t>(i), true });
   std::sort(todo.begin(), todo.end(), [](const Pending& a, const Pending& b) { return a.x < b.x || (a.x == b.x && a.i < b.i); });
   const auto rowKey = [](const double* r) { return static_cast<uint64_t>(r[3]) | (static_cast<uint64_t>(r[4]) << 32); };
   std::vector<double> ids, rows;
   std::vector<uint32_t> order;
   for (size_t a = 0; a < todo.size(); a += chunk)
   {
      const size_t b = std::min(todo.size(), a + chunk);
      double box[6];
      ids.clear();
      for (size_t k = a; k < b; ++k)
      {
         const Pending& p = todo[k];
         double x, y, z;
         uint32_t id;
         if (p.dye) { const Dye& d = dyes_[p.i]; x = d.xUm; y = d.yUm; z = d.zUm; id = d.id; }
         else { const Emitter& e = emitters[p.i]; x = e.xUm; y = e.yUm; z = e.zUm; id = e.id; }
         if (k == a) { box[0] = box[2] = x; box[1] = box[3] = y; box[4] = box[5] = z; }
         box[0] = std::min(box[0], x); box[2] = std::max(box[2], x);
         box[1] = std::min(box[1], y); box[3] = std::max(box[3], y);
         box[4] = std::min(box[4], z); box[5] = std::max(box[5], z);
         ids.push_back(id);
      }
      // (half-open windows; the rows' positions went through the host's frame and back)
      const double pad = 1e-3;
      box[0] -= pad; box[1] -= pad; box[4] -= pad;
      box[2] += pad; box[3] += pad; box[5] += pad;
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      rows.clear();
      if (!query(box, ids, rows))
         continue;
      const size_t n = rows.size() / static_cast<size_t>(stride);
      order.resize(n);
      for (size_t i = 0; i < n; ++i)
         order[i] = static_cast<uint32_t>(i);
      std::sort(order.begin(), order.end(), [&](uint32_t u, uint32_t v) {
         return rowKey(&rows[u * static_cast<size_t>(stride)]) < rowKey(&rows[v * static_cast<size_t>(stride)]);
      });
      const auto fill = [&](uint32_t id, int structure, double x, double y, double z, double& sx, double& sy,
                            double& sz, double& polar, double& azimuth) {
         const uint64_t k = static_cast<uint64_t>(id) | (static_cast<uint64_t>(structure & 0xFF) << 32);
         auto it = std::lower_bound(order.begin(), order.end(), k, [&](uint32_t u, uint64_t kk) {
            return rowKey(&rows[u * static_cast<size_t>(stride)]) < kk;
         });
         for (; it != order.end() && rowKey(&rows[*it * static_cast<size_t>(stride)]) == k; ++it)
         {
            const double* r = &rows[*it * static_cast<size_t>(stride)];
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
      for (size_t k = a; k < b; ++k)
      {
         const Pending& p = todo[k];
         if (p.dye)
         {
            Dye& d = dyes_[p.i];
            fill(d.id, d.structure, d.xUm, d.yUm, d.zUm, d.siteXUm, d.siteYUm, d.siteZUm, d.polarDeg, d.azimuthDeg);
         }
         else
         {
            Emitter& e = emitters[p.i];
            fill(e.id, e.structure, e.xUm, e.yUm, e.zUm, e.siteXUm, e.siteYUm, e.siteZUm, e.polarDeg, e.azimuthDeg);
         }
      }
   }
   return todo.size();
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
   out.reserve(out.size() + dyes_.size() * 170);   // (~155 bytes a row: one allocation)
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

namespace {

std::string SafeName(const std::string& s)
{
   std::string o;
   for (char c : s)
      o += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
   return o.empty() ? std::string("dyes") : o;
}

} // namespace

std::vector<GroundTruth::Volume> GroundTruth::Volumes(double lateralNm, double axialNm, double capBytes,
                                                      std::string* note) const
{
   std::vector<Volume> out;
   if (!(lateralNm > 0) || !(axialNm > 0) || dyes_.empty())
      return out;
   // The groups: (label, state), each its dyes that sent light to the camera; one grid for all of them (the box of
   // every group's dyes), so the stacks lie on top of each other in Fiji and napari.
   struct Group
   {
      int label, state;
      std::vector<double> x, y, z, w;
   };
   std::vector<Group> groups;
   vox::Box box;
   for (const Dye& d : dyes_)
   {
      if (!(d.detectedTotal > 0))
         continue;
      Group* g = nullptr;
      for (Group& o : groups)
         if (o.label == d.label && o.state == d.state)
            g = &o;
      if (!g)
      {
         groups.push_back({ d.label, d.state, {}, {}, {}, {} });
         g = &groups.back();
      }
      g->x.push_back(d.xUm);
      g->y.push_back(d.yUm);
      g->z.push_back(d.zUm);
      g->w.push_back(d.detectedTotal);
      box.Add(d.xUm, d.yUm, d.zUm);
   }
   if (groups.empty())
      return out;
   // The voxel: the asked one, grown until every stack (8 bits) fits the cap uncompressed.
   double f = 1;
   const vox::Grid grid = vox::GridWithin(box, lateralNm / 1000.0, axialNm / 1000.0, static_cast<double>(groups.size()),
                                          capBytes, &f);
   if (note && f > 1)
   {
      char b[300];
      std::snprintf(b, sizeof b,
                    "The density stacks' voxels are %g x %g x %g nm: the %g x %g x %g nm asked would pass %.0f MB "
                    "uncompressed in all.", grid.vx * 1000, grid.vx * 1000, grid.vz * 1000, lateralNm, lateralNm,
                    axialNm, capBytes / 1048576.0);
      *note = b;
   }
   for (const Group& g : groups)
   {
      const Label* L = g.label >= 0 && g.label < static_cast<int>(labels.size()) ? &labels[static_cast<size_t>(g.label)] : nullptr;
      const std::string target = L ? L->target : std::string("?");
      const std::string channel = g.state != 3 ? target + " (" + StateName(g.state) + ")" : target;
      std::string name = "density_" + SafeName(L ? L->target : std::string());
      if (g.state != 3)
         name += std::string("_") + StateName(g.state);
      name += ".tif";
      vox::Stack st(grid, 1, 8, g.x.data(), g.y.data(), g.z.data(), g.w.data(), nullptr, g.x.size());
      st.Maxima();
      const std::vector<std::string> chans{ channel };
      const std::string desc = vox::Description(
         st, chans, "photons_detected_total per voxel (the detected photons of its dyes over the recorded frames)",
         "photons",
         "insiliscope.target=" + target + "\ninsiliscope.dye=" + (L ? L->dye : std::string()) +
            "\ninsiliscope.state=" + StateName(g.state) + "\n");
      // the planes, deflated each (zlib); an empty plane stored once
      std::vector<std::string> planes(grid.nz);
      std::vector<uint64_t> sizes(grid.nz);
      std::vector<int64_t> sameAs(grid.nz, -1);
      std::vector<unsigned char> q(st.PlaneBytes());
      int64_t empty = -1;
      for (uint32_t z = 0; z < grid.nz; ++z)
      {
         if (st.Empty(z) && empty >= 0)
         {
            sameAs[z] = empty;
            sizes[z] = sizes[static_cast<size_t>(empty)];
            continue;
         }
         st.Fill(z);
         st.Quantize(0, q.data());
         vox::Deflate(q.data(), q.size(), planes[z]);
         sizes[z] = planes[z].size();
         if (st.Empty(z))
            empty = z;
      }
      const vox::TiffLayout lay = vox::LayoutTiff(sizes, &sameAs, grid.nx, grid.ny, 8, 1.0 / grid.vx, desc,
                                                  vox::Labels(grid, chans), 8);
      Volume vol;
      vol.name = name;
      vol.bytes = lay.head;
      for (uint32_t z = 0; z < grid.nz; ++z)
         if (sameAs[z] < 0)
            vol.bytes += planes[z];
      vol.bytes.resize(static_cast<size_t>(lay.first), '\0');
      vol.bytes += lay.ifd;
      char line[700];
      std::snprintf(line, sizeof line,
                    "%s: %s (%s, %s), %u x %u x %u voxels of %g x %g x %g nm, 8 bits: the detected photons of its dyes "
                    "over the recorded frames per voxel, %.6g photons per step (%.6g in all, %zu dyes). Every density "
                    "stack lies on the same grid: voxel (0, 0, 0)'s corner is at world (%.4f, %.4f, %.4f) um, so voxel "
                    "(i, j, k)'s centre at origin + ((i, j, k) + 0.5) x voxel; Fiji reads the voxel size, the origin "
                    "(its status bar shows world x, y, z) and a label per slice (its z), napari the voxel size.",
                    name.c_str(), target.c_str(), L ? L->dye.c_str() : "?", StateName(g.state), grid.nx, grid.ny,
                    grid.nz, grid.vx * 1000, grid.vx * 1000, grid.vz * 1000, st.Scales()[0], st.Sums()[0], g.x.size(),
                    grid.Ox(), grid.Oy(), grid.Oz());
      vol.line = line;
      out.push_back(std::move(vol));
   }
   return out;
}

std::vector<std::pair<std::string, std::string>> GroundTruth::Files(const std::vector<double>& thresholds,
                                                                    double volumeLateralNm, double volumeAxialNm,
                                                                    double volumeCapBytes) const
{
   std::vector<std::pair<std::string, std::string>> out;
   Files([&](const std::string& name, std::string&& bytes) { out.emplace_back(name, std::move(bytes)); }, thresholds,
         volumeLateralNm, volumeAxialNm, volumeCapBytes);
   return out;
}

void GroundTruth::Files(const FileSink& sink, const std::vector<double>& thresholds, double volumeLateralNm,
                        double volumeAxialNm, double volumeCapBytes) const
{
   std::string readme =
      "inSiliScope ground truth. The columns, units and caveats: https://kjamartens.github.io/inSiliScope/docs/ground-truth/\n"
      "Photons are expected values (before shot noise); world coordinates are um, z above the coverslip; camera x, y\n"
      "are nm from the image's top-left corner, drift included; z [nm] is the height above the focal plane.\n\n";
   sink("frames.csv", FramesCsv());
   readme += "frames.csv: one row per image: its time, stage position, focus, drift (and pattern state under SIM).\n";
   bool beads = false;
   for (const Emitter& e : emitters)
      beads = beads || e.kind == Fiducial;
   const bool blinking = Blinking();
   if (blinking)
      for (double t : thresholds)
      {
         char name[64];
         std::snprintf(name, sizeof name, "localizations_min%g.csv", t);
         sink(name, EmittersCsv(t, true));
         readme += std::string(name) + ": every blink drawn" + (beads ? " and every bead (kind fiducial)" : "") +
                   " with at least " + Fmt("%g", t) +
                   " expected photons landing on the camera in a frame, one row per frame. A blink the Quality's\n"
                   "  blink-min-photons cut left out of the image is not in it either.\n";
      }
   else if (beads)
   {
      sink("fiducials.csv", FiducialsCsv());
      readme += "fiducials.csv: every bead per frame (no blinking label in this movie).\n";
   }
   if (!dyes_.empty())
   {
      if (writeDyeCsv && !blinking)   // a movie's ground truth is its localizations or its dye list, never both
      {
         sink("dyes.csv", DyesCsv());
         readme += "dyes.csv: every dye in the queried box: its window, its detected rate now and its photons over the\n"
                   "  recorded frames.\n";
      }
      std::string note;
      for (Volume& v : Volumes(volumeLateralNm, volumeAxialNm, volumeCapBytes, &note))
      {
         readme += v.line + "\n";
         sink(v.name, std::move(v.bytes));
      }
      if (!note.empty())
         readme += note + "\n";
   }
   readme += "\nThe orientation columns give each dye's mean emission dipole (polar angle from z, azimuth from x);\n"
             "the image does not use it yet (the PSF is isotropic). Empty: a freely rotating dye.\n";
   sink("README.txt", std::move(readme));
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
