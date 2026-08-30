#include "SMLMPatterns.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;

double Jitter(std::mt19937_64& rng, double amount)
{
   std::uniform_real_distribution<double> d(-amount, amount);
   return d(rng);
}

// Picks one of N weighted buckets (weight[i] proportional to that bucket's
// share of e.g. total arc length, so longer curves get proportionally more
// samples) and returns its index.
int WeightedPick(const double* weights, int n, std::mt19937_64& rng)
{
   double total = 0.0;
   for (int i = 0; i < n; ++i)
      total += weights[i];
   std::uniform_real_distribution<double> dist(0.0, total);
   double w = dist(rng);
   double cum = 0.0;
   for (int i = 0; i < n; ++i)
   {
      cum += weights[i];
      if (w <= cum)
         return i;
   }
   return n - 1;
}

// +1 or -1 with equal probability -- which of a pair's two curves a sampled
// point lands on.
double RandomSign(std::mt19937_64& rng)
{
   std::uniform_int_distribution<int> d(0, 1);
   return d(rng) == 0 ? 1.0 : -1.0;
}
} // namespace

std::vector<double> DefaultResolutionSpacingsNm()
{
   return {500.0, 300.0, 200.0, 100.0, 80.0, 50.0, 30.0, 20.0, 10.0};
}

std::string FormatResolutionSpacingsNm(const std::vector<double>& spacingsNm)
{
   std::ostringstream os;
   for (size_t i = 0; i < spacingsNm.size(); ++i)
   {
      if (i > 0)
         os << ",";
      os << spacingsNm[i];
   }
   return os.str();
}

std::vector<double> ParseResolutionSpacingsNm(const std::string& text)
{
   std::vector<double> result;
   std::string token;
   std::istringstream iss(text);
   while (std::getline(iss, token, ','))
   {
      try
      {
         size_t pos = 0;
         double v = std::stod(token, &pos);
         if (v > 0.0)
            result.push_back(v);
      }
      catch (const std::exception&)
      {
         // Skip unparsable tokens (extra commas, stray whitespace-only
         // entries, non-numeric text).
      }
   }
   return result;
}

EmitterSite CirclePattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   double cx = widthUm / 2.0;
   double cy = heightUm / 2.0;
   double maxRadius = 0.47 * std::min(widthUm, heightUm);
   double minRadius = 0.12 * maxRadius;

   int n = static_cast<int>(spacingsNm_.size());
   // Ring selection weighted by radius: circumference = 2*pi*radius, so this
   // is exactly proportional to arc length -- uniform line density.
   std::vector<double> radii(n);
   std::vector<double> weights(n);
   for (int i = 0; i < n; ++i)
   {
      radii[i] = (n == 1) ? maxRadius : minRadius + (maxRadius - minRadius) * i / (n - 1);
      weights[i] = radii[i];
   }
   int ringIdx = WeightedPick(weights.data(), n, rng);

   // Smallest ring (index 0) gets the FIRST spacing in the list, largest
   // ring gets the LAST -- spacingsNm_ read in reverse so the default
   // (500..10 nm, easy-to-hard) puts the smallest gap on the smallest ring.
   double gapUm = spacingsNm_[n - 1 - ringIdx] / 1000.0;
   double radius = radii[ringIdx] + RandomSign(rng) * gapUm / 2.0;

   std::uniform_real_distribution<double> angleDist(0.0, 2.0 * kPi);
   double angle = angleDist(rng);
   return {cx + radius * std::cos(angle), cy + radius * std::sin(angle)};
}

EmitterSite LinesPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   // Parametrized port of webSMLM's buildStructure(): a small number of
   // sine-wiggle lines spanning the field of view.
   const int numLines = 3;
   std::uniform_int_distribution<int> linePick(0, numLines - 1);
   int f = linePick(rng);

   double y0 = heightUm * (0.25 + 0.25 * f);
   double amplitude = (0.02 + 0.01 * f) * heightUm;
   double freq = (0.09 + 0.02 * f) / (widthUm > 0 ? widthUm / 100.0 : 1.0);
   double phase = f * 2.0;

   std::uniform_real_distribution<double> xDist(0.0, widthUm);
   double x = xDist(rng);
   double y = y0 + amplitude * std::sin(freq * x + phase);
   return {x, y + Jitter(rng, 0.01 * heightUm)};
}

EmitterSite GridPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   // Fixed lattice density: with continuous per-call sampling there's no
   // external "how many total sites" hint to derive this from, so pick a
   // resolution that reads as a clear grid at typical FOV sizes.
   const int cols = 30;
   const int rows = 30;
   double stepX = widthUm / (cols + 1);
   double stepY = heightUm / (rows + 1);
   double jitterAmount = 0.15 * std::min(stepX, stepY);

   std::uniform_int_distribution<int> colPick(1, cols);
   std::uniform_int_distribution<int> rowPick(1, rows);
   double x = colPick(rng) * stepX + Jitter(rng, jitterAmount);
   double y = rowPick(rng) * stepY + Jitter(rng, jitterAmount);
   return {x, y};
}

EmitterSite RandomPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   std::uniform_real_distribution<double> dx(0.0, widthUm);
   std::uniform_real_distribution<double> dy(0.0, heightUm);
   return {dx(rng), dy(rng)};
}

EmitterSite SpiralPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   double cx = widthUm / 2.0;
   double cy = heightUm / 2.0;
   double maxRadius = 0.47 * std::min(widthUm, heightUm);
   const double turns = 4.5;
   int n = static_cast<int>(spacingsNm_.size());
   double angularSweepPerSeg = turns * 2.0 * kPi / n;

   // Segment selection weighted by approximate arc length (average radius *
   // angular sweep -- the radial term is negligible at this pitch).
   std::vector<double> weights(n);
   for (int i = 0; i < n; ++i)
   {
      double tStart = static_cast<double>(i) / n;
      double tEnd = static_cast<double>(i + 1) / n;
      double avgRadius = maxRadius * (tStart + tEnd) / 2.0;
      weights[i] = avgRadius * angularSweepPerSeg;
   }
   int seg = WeightedPick(weights.data(), n, rng);

   // Innermost arc (seg 0) gets the FIRST spacing in the list, outermost the
   // LAST -- spacingsNm_ read in reverse (see CirclePattern).
   double gapUm = spacingsNm_[n - 1 - seg] / 1000.0;
   double tStart = static_cast<double>(seg) / n;
   double tEnd = static_cast<double>(seg + 1) / n;
   std::uniform_real_distribution<double> tDist(tStart, tEnd);
   double t = tDist(rng);

   double theta = turns * 2.0 * kPi * t;
   double r = maxRadius * t;
   double x = cx + r * std::cos(theta);
   double y = cy + r * std::sin(theta);

   // Analytic tangent of the Archimedean spiral r(t)=maxRadius*t,
   // theta(t)=turns*2*pi*t, to find the exact local normal direction.
   double dThetaDt = turns * 2.0 * kPi;
   double dx = maxRadius * std::cos(theta) - r * std::sin(theta) * dThetaDt;
   double dy = maxRadius * std::sin(theta) + r * std::cos(theta) * dThetaDt;
   double len = std::sqrt(dx * dx + dy * dy);
   double nx = 0.0, ny = 0.0;
   if (len > 1e-12)
   {
      nx = -dy / len;
      ny = dx / len;
   }

   double half = gapUm / 2.0 * RandomSign(rng);
   return {x + nx * half, y + ny * half};
}

EmitterSite StarPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   double cx = widthUm / 2.0;
   double cy = heightUm / 2.0;
   double maxOuterRadius = 0.44 * std::min(widthUm, heightUm);
   const int numPoints = 5;
   int n = static_cast<int>(spacingsNm_.size());

   // Star selection weighted by scale: perimeter scales exactly linearly
   // with a uniform scale factor, so weight == scale is exact, not an
   // approximation.
   std::vector<double> weights(n);
   for (int i = 0; i < n; ++i)
      weights[i] = (i + 1.0) / n;
   int starIdx = WeightedPick(weights.data(), n, rng);

   double scale = (starIdx + 1.0) / n;
   double outerRadius = maxOuterRadius * scale;
   double innerRadius = outerRadius * 0.382; // classic five-pointed-star ratio
   // Smallest star (index 0) gets the FIRST spacing in the list, largest
   // gets the LAST -- spacingsNm_ read in reverse (see CirclePattern).
   double gapUm = spacingsNm_[n - 1 - starIdx] / 1000.0;

   EmitterSite vertices[numPoints * 2];
   for (int i = 0; i < numPoints * 2; ++i)
   {
      double angle = kPi / 2.0 + i * kPi / numPoints; // start pointing up
      double r = (i % 2 == 0) ? outerRadius : innerRadius;
      vertices[i] = {cx + r * std::cos(angle), cy - r * std::sin(angle)};
   }

   std::uniform_int_distribution<int> edgePick(0, numPoints * 2 - 1);
   int e = edgePick(rng);
   const EmitterSite& a = vertices[e];
   const EmitterSite& b = vertices[(e + 1) % (numPoints * 2)];

   std::uniform_real_distribution<double> tDist(0.0, 1.0);
   double t = tDist(rng);
   double x = a.xUm + (b.xUm - a.xUm) * t;
   double y = a.yUm + (b.yUm - a.yUm) * t;

   double ex = b.xUm - a.xUm;
   double ey = b.yUm - a.yUm;
   double len = std::sqrt(ex * ex + ey * ey);
   double nx = 0.0, ny = 0.0;
   if (len > 1e-12)
   {
      nx = -ey / len;
      ny = ex / len;
   }

   double half = gapUm / 2.0 * RandomSign(rng);
   return {x + nx * half, y + ny * half};
}

EmitterSite HeartPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   double cx = widthUm / 2.0;
   double cy = heightUm / 2.0;
   // Classic parametric heart curve: x in [-16,16], y in [-17,13] (the
   // bottom cusp extends further than the top lobes). Bound the outermost
   // heart's scale so BOTH axes -- independently, since width and height
   // may differ -- stay within a margin of the FOV.
   const double kHeartMaxXAbs = 16.0;
   const double kHeartMaxYAbs = 17.0;
   const double kFillFraction = 0.85; // leave a margin so the two-curve offset never spills over the edge either
   double maxScale = std::min(kFillFraction * (widthUm / 2.0) / kHeartMaxXAbs,
                               kFillFraction * (heightUm / 2.0) / kHeartMaxYAbs);
   int n = static_cast<int>(spacingsNm_.size());

   // Heart selection weighted by scale: perimeter scales exactly linearly
   // with a uniform scale factor, so weight == scale is exact.
   std::vector<double> weights(n);
   for (int i = 0; i < n; ++i)
      weights[i] = (i + 1.0) / n;
   int heartIdx = WeightedPick(weights.data(), n, rng);

   double scale = maxScale * (heartIdx + 1.0) / n;
   // Smallest heart (index 0) gets the FIRST spacing in the list, largest
   // gets the LAST -- spacingsNm_ read in reverse (see CirclePattern).
   double gapUm = spacingsNm_[n - 1 - heartIdx] / 1000.0;

   std::uniform_real_distribution<double> tDist(0.0, 2.0 * kPi);
   double t = tDist(rng);
   double hx = 16.0 * std::pow(std::sin(t), 3.0);
   double hy = 13.0 * std::cos(t) - 5.0 * std::cos(2.0 * t) - 2.0 * std::cos(3.0 * t) - std::cos(4.0 * t);
   // Flip y: the heart curve points up in math coordinates, image y grows
   // downward.
   double x = cx + scale * hx;
   double y = cy - scale * hy;

   // Analytic derivative of the parametric curve, to find the exact local
   // normal direction (also flipped in y to match).
   double dhx = 48.0 * std::sin(t) * std::sin(t) * std::cos(t);
   double dhy = -13.0 * std::sin(t) + 10.0 * std::sin(2.0 * t) + 6.0 * std::sin(3.0 * t) + 4.0 * std::sin(4.0 * t);
   double dx = scale * dhx;
   double dy = -scale * dhy;
   double len = std::sqrt(dx * dx + dy * dy);
   double nx = 0.0, ny = 0.0;
   if (len > 1e-12)
   {
      nx = -dy / len;
      ny = dx / len;
   }

   double half = gapUm / 2.0 * RandomSign(rng);
   return {x + nx * half, y + ny * half};
}

EmitterSite ResolutionTargetPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   // A cols x rows grid of sub-FOVs (as close to square as possible for
   // however many spacings are configured); cell idx (row-major) gets
   // spacingsNm_[idx] as its line spacing and draws up to 5 continuous
   // vertical lines at that spacing, centered in the cell. If the cell is
   // too narrow to fit 5 lines at its spacing (a small FOV), fewer are
   // drawn -- never wider than the cell itself.
   const int linesPerCellTarget = 5;
   const double marginFrac = 0.06;   // outer margin, fraction of FOV
   const double cellGapFrac = 0.025; // gap between cells, fraction of FOV

   int numCells = static_cast<int>(spacingsNm_.size());
   int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(numCells))));
   int rows = static_cast<int>(std::ceil(static_cast<double>(numCells) / cols));

   double originX = widthUm * marginFrac;
   double originY = heightUm * marginFrac;
   double usableW = widthUm * (1.0 - 2.0 * marginFrac);
   double usableH = heightUm * (1.0 - 2.0 * marginFrac);
   double cellGapX = widthUm * cellGapFrac;
   double cellGapY = heightUm * cellGapFrac;
   double cellW = (usableW - (cols - 1) * cellGapX) / cols;
   double cellH = (usableH - (rows - 1) * cellGapY) / rows;

   // Weight cell selection by its actual total line length (lineCount *
   // cellH), so a cell clamped to fewer than 5 lines (tiny FOV) is
   // proportionally under-represented rather than sampled as if it had the
   // full 5.
   std::vector<double> weights(numCells);
   std::vector<int> lineCounts(numCells);
   for (int idx = 0; idx < numCells; ++idx)
   {
      double spacingUm = spacingsNm_[idx] / 1000.0;
      int maxFit = std::max(1, static_cast<int>(cellW / spacingUm) + 1);
      int n = std::min(linesPerCellTarget, maxFit);
      lineCounts[idx] = n;
      weights[idx] = static_cast<double>(n) * cellH;
   }
   int cellIdx = WeightedPick(weights.data(), numCells, rng);

   int row = cellIdx / cols;
   int col = cellIdx % cols;
   double spacingUm = spacingsNm_[cellIdx] / 1000.0;
   int n = lineCounts[cellIdx];
   double cellX0 = originX + col * (cellW + cellGapX);
   double cellY0 = originY + row * (cellH + cellGapY);
   double usedWidth = (n - 1) * spacingUm;
   double startX = cellX0 + (cellW - usedWidth) / 2.0;

   std::uniform_int_distribution<int> linePick(0, n - 1);
   double x = startX + linePick(rng) * spacingUm;

   double yTop = cellY0 + cellH * 0.12;
   double yBottom = cellY0 + cellH * 0.88;
   std::uniform_real_distribution<double> yDist(yTop, yBottom);
   double y = yDist(rng);
   return {x, y};
}

EmitterSite CustomPointsPattern::SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const
{
   if (!loaded_)
   {
      cachedNormalizedPoints_.clear();
      std::ifstream in(filePath_);
      if (in.is_open())
      {
         std::string line;
         while (std::getline(in, line))
         {
            if (line.empty())
               continue;
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream iss(line);
            double xNorm, yNorm;
            if (iss >> xNorm >> yNorm)
            {
               xNorm = std::min(std::max(xNorm, 0.0), 1.0);
               yNorm = std::min(std::max(yNorm, 0.0), 1.0);
               cachedNormalizedPoints_.push_back({xNorm, yNorm});
            }
         }
      }
      loaded_ = true;
   }

   if (cachedNormalizedPoints_.empty())
   {
      // Fall back to a circle so the camera still produces something
      // sensible if the file is missing/unreadable/empty.
      static const CirclePattern fallback;
      return fallback.SampleSite(widthUm, heightUm, rng);
   }

   std::uniform_int_distribution<size_t> pick(0, cachedNormalizedPoints_.size() - 1);
   const EmitterSite& n = cachedNormalizedPoints_[pick(rng)];
   return {n.xUm * widthUm, n.yUm * heightUm};
}

std::unique_ptr<IPatternGenerator> CreatePattern(SMLMPatternType type, const std::string& customPointsFile,
                                                  const std::vector<double>& spacingsNm)
{
   switch (type)
   {
      case PATTERN_CIRCLE:
         return std::make_unique<CirclePattern>(spacingsNm);
      case PATTERN_LINES:
         return std::make_unique<LinesPattern>();
      case PATTERN_GRID:
         return std::make_unique<GridPattern>();
      case PATTERN_RANDOM:
         return std::make_unique<RandomPattern>();
      case PATTERN_CUSTOM_POINTS:
         return std::make_unique<CustomPointsPattern>(customPointsFile);
      case PATTERN_SPIRAL:
         return std::make_unique<SpiralPattern>(spacingsNm);
      case PATTERN_STAR:
         return std::make_unique<StarPattern>(spacingsNm);
      case PATTERN_HEART:
         return std::make_unique<HeartPattern>(spacingsNm);
      case PATTERN_RESOLUTION_TARGET:
         return std::make_unique<ResolutionTargetPattern>(spacingsNm);
   }
   return std::make_unique<CirclePattern>(spacingsNm);
}

} // namespace sim
