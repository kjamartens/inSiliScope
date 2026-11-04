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
#include <cctype>
#include <cstdio>
#include <cstring>
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

namespace {

void Put16(std::string& s, uint32_t v)
{
   s.push_back(static_cast<char>(v & 255));
   s.push_back(static_cast<char>((v >> 8) & 255));
}
void Put32(std::string& s, uint32_t v)
{
   Put16(s, v & 0xFFFF);
   Put16(s, v >> 16);
}

// A float32 ImageJ stack (classic little-endian TIFF): the description, one rational (pixels per micron, X and Y),
// the planes back to back, then an IFD per plane (the description in the first).
std::string FloatTiff(const std::vector<float>& v, uint32_t nx, uint32_t ny, uint32_t nz, double pixelsPerUm,
                      const std::string& description)
{
   std::string s = "II";
   Put16(s, 42);
   Put32(s, 0);   // the first IFD's offset, below
   const uint32_t descAt = static_cast<uint32_t>(s.size());
   s += description;
   s.push_back('\0');
   if (s.size() & 1)
      s.push_back('\0');
   const uint32_t ratAt = static_cast<uint32_t>(s.size());
   const double den = 1000.0;
   Put32(s, static_cast<uint32_t>(std::lround(pixelsPerUm * den)));
   Put32(s, static_cast<uint32_t>(den));
   const uint32_t dataAt = static_cast<uint32_t>(s.size());
   const uint32_t plane = nx * ny * 4u;
   const size_t start = s.size();
   s.resize(start + v.size() * 4);
   for (size_t i = 0; i < v.size(); ++i)
   {
      uint32_t u;
      std::memcpy(&u, &v[i], 4);
      s[start + 4 * i] = static_cast<char>(u & 255);
      s[start + 4 * i + 1] = static_cast<char>((u >> 8) & 255);
      s[start + 4 * i + 2] = static_cast<char>((u >> 16) & 255);
      s[start + 4 * i + 3] = static_cast<char>(u >> 24);
   }
   if (s.size() & 1)
      s.push_back('\0');
   const uint32_t first = static_cast<uint32_t>(s.size());
   s[4] = static_cast<char>(first & 255);
   s[5] = static_cast<char>((first >> 8) & 255);
   s[6] = static_cast<char>((first >> 16) & 255);
   s[7] = static_cast<char>(first >> 24);
   const uint32_t descLen = static_cast<uint32_t>(description.size() + 1);
   for (uint32_t k = 0; k < nz; ++k)
   {
      const bool desc = k == 0;
      const uint16_t n = desc ? 15 : 14;
      Put16(s, n);
      const auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
         Put16(s, tag);
         Put16(s, type);
         Put32(s, count);
         Put32(s, value);
      };
      entry(254, 4, 1, 0);
      entry(256, 4, 1, nx);
      entry(257, 4, 1, ny);
      entry(258, 3, 1, 32);
      entry(259, 3, 1, 1);
      entry(262, 3, 1, 1);
      if (desc)
         entry(270, 2, descLen, descAt);
      entry(273, 4, 1, dataAt + k * plane);
      entry(277, 3, 1, 1);
      entry(278, 4, 1, ny);
      entry(279, 4, 1, plane);
      entry(282, 5, 1, ratAt);
      entry(283, 5, 1, ratAt);
      entry(296, 3, 1, 1);
      entry(339, 3, 1, 3);
      Put32(s, 0);
      if (k + 1 < nz)
      {
         // the next IFD follows at once
         const uint32_t next = static_cast<uint32_t>(s.size());
         s[s.size() - 4] = static_cast<char>(next & 255);
         s[s.size() - 3] = static_cast<char>((next >> 8) & 255);
         s[s.size() - 2] = static_cast<char>((next >> 16) & 255);
         s[s.size() - 1] = static_cast<char>(next >> 24);
      }
   }
   return s;
}

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
   // The groups: (label, state), each its dyes' box.
   struct Group
   {
      int label, state;
      double x0, y0, z0, x1, y1, z1;
      std::vector<size_t> dyes;
   };
   std::vector<Group> groups;
   for (size_t i = 0; i < dyes_.size(); ++i)
   {
      const Dye& d = dyes_[i];
      if (!(d.emittedTotal > 0))
         continue;
      Group* g = nullptr;
      for (Group& o : groups)
         if (o.label == d.label && o.state == d.state)
            g = &o;
      if (!g)
      {
         groups.push_back({ d.label, d.state, d.xUm, d.yUm, d.zUm, d.xUm, d.yUm, d.zUm, {} });
         g = &groups.back();
      }
      g->x0 = std::min(g->x0, d.xUm); g->x1 = std::max(g->x1, d.xUm);
      g->y0 = std::min(g->y0, d.yUm); g->y1 = std::max(g->y1, d.yUm);
      g->z0 = std::min(g->z0, d.zUm); g->z1 = std::max(g->z1, d.zUm);
      g->dyes.push_back(i);
   }
   // The voxel size: the asked one, grown by whole factors until every volume fits the cap.
   const auto sizeAt = [&](double f, size_t gi, uint32_t* n) {
      const Group& g = groups[gi];
      const double p = lateralNm * f / 1000.0, q = axialNm * f / 1000.0;
      const double nx = std::floor(g.x1 / p) - std::floor(g.x0 / p) + 1, ny = std::floor(g.y1 / p) - std::floor(g.y0 / p) + 1,
                   nz = std::floor(g.z1 / q) - std::floor(g.z0 / q) + 1;
      if (n) { n[0] = static_cast<uint32_t>(nx); n[1] = static_cast<uint32_t>(ny); n[2] = static_cast<uint32_t>(nz); }
      return nx * ny * nz * 4.0;
   };
   double f = 1;
   for (;; f += 1)
   {
      double total = 0;
      for (size_t gi = 0; gi < groups.size(); ++gi)
         total += sizeAt(f, gi, nullptr);
      if (total <= capBytes || f >= 1000)
         break;
   }
   if (note && f > 1)
   {
      char b[200];
      std::snprintf(b, sizeof b, "The volumes' voxels are %g x %g x %g nm, %g times the %g x %g x %g nm asked, to stay under %.0f MB.",
                    lateralNm * f, lateralNm * f, axialNm * f, f, lateralNm, lateralNm, axialNm, capBytes / 1048576.0);
      *note = b;
   }
   const double p = lateralNm * f / 1000.0, q = axialNm * f / 1000.0;
   for (size_t gi = 0; gi < groups.size(); ++gi)
   {
      const Group& g = groups[gi];
      uint32_t n[3];
      sizeAt(f, gi, n);
      const double ox = std::floor(g.x0 / p) * p, oy = std::floor(g.y0 / p) * p, oz = std::floor(g.z0 / q) * q;
      std::vector<float> v(static_cast<size_t>(n[0]) * n[1] * n[2], 0.0f);
      double sum = 0;
      for (size_t i : g.dyes)
      {
         const Dye& d = dyes_[i];
         const long ix = std::min<long>(n[0] - 1, std::max<long>(0, static_cast<long>(std::floor(d.xUm / p) - std::floor(g.x0 / p))));
         const long iy = std::min<long>(n[1] - 1, std::max<long>(0, static_cast<long>(std::floor(d.yUm / p) - std::floor(g.y0 / p))));
         const long iz = std::min<long>(n[2] - 1, std::max<long>(0, static_cast<long>(std::floor(d.zUm / q) - std::floor(g.z0 / q))));
         v[(static_cast<size_t>(iz) * n[1] + static_cast<size_t>(iy)) * n[0] + static_cast<size_t>(ix)] += static_cast<float>(d.emittedTotal);
         sum += d.emittedTotal;
      }
      float mx = 0;
      for (float x : v)
         mx = std::max(mx, x);
      const Label* L = g.label >= 0 && g.label < static_cast<int>(labels.size()) ? &labels[static_cast<size_t>(g.label)] : nullptr;
      std::string name = "density_" + SafeName(L ? L->target : std::string());
      if (g.state != 3)
         name += std::string("_") + StateName(g.state);
      name += ".tif";
      char d[600];
      std::snprintf(d, sizeof d,
                    "ImageJ=1.11a\nimages=%u\nslices=%u\nunit=micron\nspacing=%.9g\nloop=false\nmin=0\nmax=%.9g\n"
                    "insiliscope.value=emitted photons per voxel over the recorded frames\n"
                    "insiliscope.target=%s\ninsiliscope.dye=%s\ninsiliscope.state=%s\n"
                    "insiliscope.origin-um=%.6f,%.6f,%.6f\ninsiliscope.voxel-um=%.9g,%.9g,%.9g\n",
                    n[2], n[2], q, static_cast<double>(mx), L ? L->target.c_str() : "", L ? L->dye.c_str() : "",
                    StateName(g.state), ox, oy, oz, p, p, q);
      Volume vol;
      vol.name = name;
      vol.bytes = FloatTiff(v, n[0], n[1], n[2], 1.0 / p, d);
      char line[600];
      std::snprintf(line, sizeof line,
                    "%s: %s (%s, %s), %u x %u x %u voxels of %g x %g x %g nm; the emitted photons of its dyes over the "
                    "recorded frames per voxel (%.6g in all, %zu dyes). Voxel (0, 0, 0)'s corner is at world (%.4f, "
                    "%.4f, %.4f) um, so voxel (i, j, k)'s centre at origin + ((i, j, k) + 0.5) x voxel; Fiji and "
                    "napari read the voxel size, not the origin.",
                    name.c_str(), L ? L->target.c_str() : "?", L ? L->dye.c_str() : "?", StateName(g.state), n[0], n[1],
                    n[2], p * 1000, p * 1000, q * 1000, sum, g.dyes.size(), ox, oy, oz);
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
   std::string readme =
      "inSiliScope ground truth. The columns, units and caveats: https://kjamartens.github.io/inSiliScope/docs/ground-truth/\n"
      "Photons are expected values (before shot noise); world coordinates are um, z above the coverslip; camera x, y\n"
      "are nm from the image's top-left corner, drift included; z [nm] is the height above the focal plane.\n\n";
   out.emplace_back("frames.csv", FramesCsv());
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
         out.emplace_back(name, EmittersCsv(t, true));
         readme += std::string(name) + ": every blink drawn" + (beads ? " and every bead (kind fiducial)" : "") +
                   " with at least " + std::to_string(static_cast<long>(t)) +
                   " expected photons landing on the camera in a frame, one row per frame. A blink the Quality's\n"
                   "  blink-min-photons cut left out of the image is not in it either.\n";
      }
   else if (beads)
   {
      out.emplace_back("fiducials.csv", FiducialsCsv());
      readme += "fiducials.csv: every bead per frame (no blinking label in this movie).\n";
   }
   if (!dyes_.empty())
   {
      if (writeDyeCsv && !blinking)   // a movie's ground truth is its localizations or its dye list, never both
      {
         out.emplace_back("dyes.csv", DyesCsv());
         readme += "dyes.csv: every dye in the queried box: its window, its detected rate now and its photons over the\n"
                   "  recorded frames.\n";
      }
      std::string note;
      for (Volume& v : Volumes(volumeLateralNm, volumeAxialNm, volumeCapBytes, &note))
      {
         readme += v.line + "\n";
         out.emplace_back(v.name, std::move(v.bytes));
      }
      if (!note.empty())
         readme += note + "\n";
   }
   readme += "\nThe orientation columns give each dye's mean emission dipole (polar angle from z, azimuth from x);\n"
             "the image does not use it yet (the PSF is isotropic). Empty: a freely rotating dye.\n";
   out.emplace_back("README.txt", readme);
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
