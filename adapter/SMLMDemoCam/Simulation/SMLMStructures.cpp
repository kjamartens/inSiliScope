#include "SMLMStructures.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Ported from webSMLM's area-scaled site count (targets ~2 camera-px
// neighbor spacing regardless of FOV size, so a large FOV doesn't degrade
// into "a few spots repeated a lot", capped so a huge FOV can't allocate
// unboundedly). webSMLM works in camera px directly; this project's
// structure builders work in physical micrometers instead
// (BuildStructurePattern is never told the camera's pixel size), so the
// target spacing below is expressed as a physical distance (200nm -- what
// "2 px" means at this project's own 100nm/px default) rather than as a
// px count.
constexpr double kAreaTargetSpacingUm = 0.2;
constexpr size_t kAreaSitesMin = 200;
constexpr size_t kAreaSitesMax = 50000;
constexpr size_t kShellSites = 2000; // fixed, not area-scaled -- matches webSMLM's Shell
constexpr double kMarginFraction = 0.03; // fraction of FOV kept clear at the edges

// webSMLM's own NUP corner-arc angles (degrees, measured from the purely
// radial-outward direction) -- the 4 Nup96 sites per corner sit on a
// half-circle arc bulging outward, not a 2x2 square.
constexpr double kNupCornerArcAnglesDeg[4] = {-67.5, -22.5, 22.5, 67.5};
constexpr int kNupCorners = 8;
constexpr int kNupRings = 2;

size_t AreaStructureSiteCount(double areaUm2)
{
   double n = areaUm2 / (kAreaTargetSpacingUm * kAreaTargetSpacingUm);
   size_t count = static_cast<size_t>(std::max(1.0, n));
   return std::min(kAreaSitesMax, std::max(kAreaSitesMin, count));
}

std::vector<EmitterSite> BuildTiltedPlaneSites(double widthUm, double heightUm,
                                                 const StructureParams& sp, std::mt19937_64& rng)
{
   size_t n = AreaStructureSiteCount(widthUm * heightUm);
   std::vector<EmitterSite> sites;
   sites.reserve(n);

   double mx = widthUm * kMarginFraction, my = heightUm * kMarginFraction;
   std::uniform_real_distribution<double> xDist(mx, std::max(mx, widthUm - mx));
   std::uniform_real_distribution<double> yDist(my, std::max(my, heightUm - my));
   double cx = widthUm / 2.0;
   double halfW = std::max(widthUm / 2.0, 1e-9);

   for (size_t i = 0; i < n; ++i)
   {
      double x = xDist(rng);
      double y = yDist(rng);
      // z ramps linearly with x, +/-zRangeNm end to end -- webSMLM's own
      // tiltedPlane structure.
      double z = (x - cx) / halfW * sp.zRangeNm;
      sites.push_back({x, y, z});
   }
   return sites;
}

std::vector<EmitterSite> BuildUniform3DSites(double widthUm, double heightUm,
                                               const StructureParams& sp, std::mt19937_64& rng)
{
   size_t n = AreaStructureSiteCount(widthUm * heightUm);
   std::vector<EmitterSite> sites;
   sites.reserve(n);

   double mx = widthUm * kMarginFraction, my = heightUm * kMarginFraction;
   std::uniform_real_distribution<double> xDist(mx, std::max(mx, widthUm - mx));
   std::uniform_real_distribution<double> yDist(my, std::max(my, heightUm - my));
   std::uniform_real_distribution<double> zDist(-sp.zRangeNm, sp.zRangeNm);

   for (size_t i = 0; i < n; ++i)
      sites.push_back({xDist(rng), yDist(rng), zDist(rng)});
   return sites;
}

std::vector<EmitterSite> BuildShellSites(double widthUm, double heightUm,
                                           const StructureParams& sp, std::mt19937_64& rng)
{
   // Fixed 2000 sites regardless of FOV/area -- the shell's z extent is its
   // own radius (structureSizeNm), NOT structure.zRangeNm; see
   // StructureZExtentNm below.
   double cx = widthUm / 2.0, cy = heightUm / 2.0;
   double radiusUm = sp.structureSizeNm / 1000.0;
   std::normal_distribution<double> g(0.0, 1.0);

   std::vector<EmitterSite> sites;
   sites.reserve(kShellSites);
   for (size_t i = 0; i < kShellSites; ++i)
   {
      // Marsaglia: a normalized Gaussian 3-vector is uniform on the unit
      // sphere -- uniform coverage of the shell's surface.
      double vx = g(rng), vy = g(rng), vz = g(rng);
      double len = std::sqrt(vx * vx + vy * vy + vz * vz);
      if (len < 1e-12)
      {
         vx = 0.0;
         vy = 0.0;
         vz = 1.0;
         len = 1.0;
      }
      double ux = vx / len, uy = vy / len, uz = vz / len;
      sites.push_back({cx + radiusUm * ux, cy + radiusUm * uy, sp.structureSizeNm * uz});
   }
   return sites;
}

// Uniform-in-volume displacement magnitude in [minNm, maxNm] with a
// uniform-on-sphere direction -- webSMLM's displaceByLinker(). Uniform in
// volume (not in radius) means the cube-root of a uniform draw over the
// shell [minNm^3, maxNm^3], so points aren't artificially concentrated
// near minNm the way a plain uniform-radius draw would.
void SampleLinkerOffset(double minNm, double maxNm, std::mt19937_64& rng,
                         double& outDx, double& outDy, double& outDz)
{
   std::uniform_real_distribution<double> u01(0.0, 1.0);
   double minCube = minNm * minNm * minNm;
   double maxCube = maxNm * maxNm * maxNm;
   double r = std::cbrt(minCube + (maxCube - minCube) * u01(rng));
   double cosTheta = 2.0 * u01(rng) - 1.0; // uniform in [-1,1]
   double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
   double phi = 2.0 * kPi * u01(rng);
   outDx = r * sinTheta * std::cos(phi);
   outDy = r * sinTheta * std::sin(phi);
   outDz = r * cosTheta;
}

// One Nup96 NPC's 64 attachment points (2 rings x 8 corners x 4 arc
// points), in the pore's OWN local frame: (a,b) the ring plane, c the
// pore's axial offset -- before the pore's random azimuthal rotation,
// membrane orientation mapping, or center placement, all applied by the
// caller. Port of webSMLM's buildNupAttachmentPoints()+displaceByLinker().
std::vector<std::array<double, 3>> BuildNupLocalPoints(const StructureParams& sp, std::mt19937_64& rng)
{
   std::vector<std::array<double, 3>> points;
   points.reserve(static_cast<size_t>(kNupRings) * kNupCorners * 4);
   double arcRadiusNm = sp.nupCornerSpreadNm / 2.0;

   for (int ring = 0; ring < kNupRings; ++ring)
   {
      double c = (ring == 0) ? sp.nupRingSeparationNm / 2.0 : -sp.nupRingSeparationNm / 2.0;
      for (int corner = 0; corner < kNupCorners; ++corner)
      {
         double cornerAngle = corner * (2.0 * kPi / kNupCorners);
         double cosC = std::cos(cornerAngle), sinC = std::sin(cornerAngle);
         for (double phiDeg : kNupCornerArcAnglesDeg)
         {
            double phi = phiDeg * kPi / 180.0;
            // Local (radial, tangential) offset from the ring point at this
            // corner: u bulges outward (radial), v is tangential.
            double u = arcRadiusNm * std::cos(phi);
            double v = arcRadiusNm * std::sin(phi);
            double a = (sp.nupRadiusNm + u) * cosC - v * sinC;
            double b = (sp.nupRadiusNm + u) * sinC + v * cosC;

            double dx, dy, dz;
            SampleLinkerOffset(sp.nupLinkerMinNm, sp.nupLinkerMaxNm, rng, dx, dy, dz);
            points.push_back({a + dx, b + dy, c + dz});
         }
      }
   }
   return points;
}

std::vector<EmitterSite> BuildNupSites(double widthUm, double heightUm,
                                         const StructureParams& sp, std::mt19937_64& rng)
{
   std::vector<EmitterSite> sites;
   int count = std::max(1, sp.nupCount);

   // Rejection-sample NPC centers so no two are closer than
   // nupMinSpacingNm, with a margin large enough that a pore's own
   // attachment points (radius + corner spread + max linker) can't spill
   // past the FOV edge.
   double marginUm = (sp.nupRadiusNm + sp.nupCornerSpreadNm / 2.0 + sp.nupLinkerMaxNm) / 1000.0 + 0.05;
   double minSpacingUm = sp.nupMinSpacingNm / 1000.0;
   double xLo = marginUm, xHi = std::max(marginUm, widthUm - marginUm);
   double yLo = marginUm, yHi = std::max(marginUm, heightUm - marginUm);
   std::uniform_real_distribution<double> xDist(xLo, xHi);
   std::uniform_real_distribution<double> yDist(yLo, yHi);
   std::uniform_real_distribution<double> angleDist(0.0, 2.0 * kPi);

   std::vector<std::array<double, 2>> centers;
   centers.reserve(static_cast<size_t>(count));
   int maxTries = count * 200;
   int tries = 0;
   double minSpacingSq = minSpacingUm * minSpacingUm;
   while (static_cast<int>(centers.size()) < count && tries < maxTries)
   {
      ++tries;
      double cxCand = xDist(rng), cyCand = yDist(rng);
      bool ok = true;
      for (const auto& c : centers)
      {
         double dx = cxCand - c[0], dy = cyCand - c[1];
         if (dx * dx + dy * dy < minSpacingSq)
         {
            ok = false;
            break;
         }
      }
      if (ok)
         centers.push_back({cxCand, cyCand});
   }
   // Rejection sampling can legitimately fall short of the requested count
   // when NupCount/NupMinSpacingNm don't fit the FOV -- this is reported
   // via outTotalSites vs. nupCount*64 by the caller (BuildStructurePattern
   // doesn't itself log; see SMLMImageGeneration.cpp's structure-build call
   // site), matching webSMLM's own silent-shortfall behavior but with a
   // corelog warning added on top.

   double maxR2 = std::pow(std::min(widthUm, heightUm) / 2.0, 2.0);
   sites.reserve(centers.size() * kNupRings * kNupCorners * 4);
   for (const auto& center : centers)
   {
      std::vector<std::array<double, 3>> local = BuildNupLocalPoints(sp, rng);
      double azimuth = angleDist(rng);
      double cosA = std::cos(azimuth), sinA = std::sin(azimuth);

      double dxFromCenter = center[0] - widthUm / 2.0;
      double dyFromCenter = center[1] - heightUm / 2.0;
      // Mean-subtracted bowl curvature: centers the curved patch on z=0
      // rather than offsetting the whole structure by curveAmp/2.
      double curveNm = sp.nupCurvatureNm *
                        ((dxFromCenter * dxFromCenter + dyFromCenter * dyFromCenter) / std::max(maxR2, 1e-9) - 0.5);

      for (const auto& p : local)
      {
         double a = p[0], b = p[1], c = p[2];
         double aRot = a * cosA - b * sinA;
         double bRot = a * sinA + b * cosA;

         if (sp.nupMembrane == MembraneOrientation::TopDown)
         {
            // Pore axis parallel to the optical Z axis: the ring plane maps
            // straight to x,y; the axial offset (+ curvature) maps to z.
            double x = center[0] + aRot / 1000.0;
            double y = center[1] + bRot / 1000.0;
            double z = c + curveNm;
            sites.push_back({x, y, z});
         }
         else
         {
            // Sideways: pore axis parallel to image Y. Ring axis 'a' stays
            // x; ring axis 'b' (previously tangential-in-plane) becomes
            // depth; the axial offset (+curvature) becomes the in-plane Y.
            double x = center[0] + aRot / 1000.0;
            double y = center[1] + (c + curveNm) / 1000.0;
            double z = bRot;
            sites.push_back({x, y, z});
         }
      }
   }
   return sites;
}

// The fraction of physical structure sites that ever carry a functional
// label at all (real SMLM labels -- antibodies, SNAP/Halo, FP fusions --
// never reach 100% of their target). Applied ONCE here, as a keep/drop
// over the candidate list, BEFORE the Poisson arrival process ever picks a
// site: an unlabeled site can never light up, at any frame -- not "less
// often". At pct>=100 this makes zero rng draws, so the default (100)
// leaves every site-list structure's random stream unaffected by this
// filter's mere existence.
void ApplyLabelingEfficiency(std::vector<EmitterSite>& sites, double pct, std::mt19937_64& rng)
{
   if (pct >= 100.0 || sites.empty())
      return;
   double frac = std::max(0.0, pct) / 100.0;
   std::uniform_real_distribution<double> u01(0.0, 1.0);
   std::vector<EmitterSite> kept;
   kept.reserve(sites.size());
   for (const EmitterSite& s : sites)
   {
      if (u01(rng) < frac)
         kept.push_back(s);
   }
   sites = std::move(kept);
}

} // namespace

SiteListPattern::SiteListPattern(std::vector<EmitterSite> sites, const char* name)
   : sites_(std::move(sites)), name_(name)
{
}

EmitterSite SiteListPattern::SampleSite(double /*widthUm*/, double /*heightUm*/, std::mt19937_64& rng) const
{
   if (sites_.empty())
      return EmitterSite();
   std::uniform_int_distribution<size_t> pick(0, sites_.size() - 1);
   return sites_[pick(rng)];
}

std::unique_ptr<IPatternGenerator> BuildStructurePattern(SMLMPatternType type, double widthUm, double heightUm,
                                                           const StructureParams& sp, uint64_t structureSeed,
                                                           size_t* outKeptSites, size_t* outTotalSites)
{
   std::mt19937_64 structureRng(structureSeed);
   std::vector<EmitterSite> sites;
   const char* name = "Structure";

   switch (type)
   {
      case PATTERN_TILTED_PLANE:
         sites = BuildTiltedPlaneSites(widthUm, heightUm, sp, structureRng);
         name = "TiltedPlane";
         break;
      case PATTERN_UNIFORM_3D:
         sites = BuildUniform3DSites(widthUm, heightUm, sp, structureRng);
         name = "Uniform3D";
         break;
      case PATTERN_SHELL:
         sites = BuildShellSites(widthUm, heightUm, sp, structureRng);
         name = "Shell";
         break;
      case PATTERN_NUP:
         sites = BuildNupSites(widthUm, heightUm, sp, structureRng);
         name = "NUP";
         break;
      default:
         return nullptr;
   }

   if (outTotalSites)
      *outTotalSites = sites.size();

   ApplyLabelingEfficiency(sites, sp.labelingEfficiencyPct, structureRng);

   if (outKeptSites)
      *outKeptSites = sites.size();

   if (sites.empty())
   {
      // LabelingEfficiencyPct=0 (allowed by the property limits) or NUP
      // rejection sampling finding no valid center would otherwise hand
      // SiteListPattern an empty list, which would UB on
      // uniform_int_distribution(0, -1) in SampleSite. Fall back to a
      // single centre-of-FOV site so the camera still produces something
      // rather than crashing.
      sites.push_back({widthUm / 2.0, heightUm / 2.0, 0.0});
   }

   return std::make_unique<SiteListPattern>(std::move(sites), name);
}

double StructureZExtentNm(SMLMPatternType type, const StructureParams& sp)
{
   switch (type)
   {
      case PATTERN_TILTED_PLANE:
      case PATTERN_UNIFORM_3D:
         return sp.zRangeNm;
      case PATTERN_SHELL:
         return sp.structureSizeNm;
      case PATTERN_NUP:
         return sp.nupRingSeparationNm / 2.0 + sp.nupCurvatureNm / 2.0;
      default:
         return 0.0;
   }
}

} // namespace sim
