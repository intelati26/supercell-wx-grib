#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace scwx::qt::util
{

// Zoom levels at and above this draw lines at full detail: the tolerance
// below is far smaller than a pixel there, so simplifying gains little.
inline constexpr int kFullDetailZoomTier = 12;

// Whole-number zoom level a line's simplification is computed for -- the
// tier changes (and lines are rebuilt) once per zoom level, not per frame.
[[nodiscard]] inline int SimplificationTier(double zoom)
{
   return std::clamp(static_cast<int>(std::floor(zoom)), 0, kFullDetailZoomTier);
}

// Douglas-Peucker tolerance for a tier, in Web Mercator units normalized to
// the whole world = 1: `pixels` on screen at that zoom (MapLibre's 512px
// tiles). 0 at full detail.
[[nodiscard]] inline double SimplificationTolerance(int tier, double pixels = 2.0)
{
   if (tier >= kFullDetailZoomTier)
   {
      return 0.0;
   }
   return pixels / (512.0 * std::pow(2.0, tier));
}

// A line whose whole extent is smaller than this on screen is too small to
// read -- at low zoom a contour set is full of these specks, which is most of
// what makes it look noisy -- and is dropped until you zoom in far enough for
// it to be visible.
inline constexpr double kMinLineExtentPixels = 5.0;

// True if a polyline (anything with `.latitude_`/`.longitude_` in degrees)
// spans less than `minPixels` on screen at `tier`'s zoom. Never true at full
// detail. Uses the bounding box diagonal in Web Mercator space.
template<typename Element>
[[nodiscard]] bool IsNegligibleAtTier(const std::vector<Element>& points,
                                      int                          tier,
                                      double minPixels = kMinLineExtentPixels)
{
   if (tier >= kFullDetailZoomTier || points.empty())
   {
      return false;
   }

   constexpr double kPi = std::numbers::pi;

   double minX = 1.0;
   double maxX = 0.0;
   double minY = 1.0;
   double maxY = 0.0;
   for (const auto& point : points)
   {
      const double lat = std::clamp(point.latitude_, -85.0511, 85.0511);
      const double x   = (point.longitude_ + 180.0) / 360.0;
      const double y =
         0.5 - std::log(std::tan(kPi / 4.0 + lat * kPi / 360.0)) / (2.0 * kPi);
      minX = std::min(minX, x);
      maxX = std::max(maxX, x);
      minY = std::min(minY, y);
      maxY = std::max(maxY, y);
   }

   return std::hypot(maxX - minX, maxY - minY) * 512.0 * std::pow(2.0, tier) <
          minPixels;
}

// Indices of the points of a polyline (anything with `.latitude_` and
// `.longitude_` in degrees) to keep so the simplified line stays within
// `tolerance` of the original, measured in Web Mercator space so the result
// looks the same on screen at any latitude. First and last points are always
// kept. Returns every index when tolerance <= 0 or there is nothing to drop.
//
// Used to draw a placefile line's noisy, over-detailed geometry more simply
// at low zoom while the line itself (and its hover text) is unchanged.
template<typename Element>
[[nodiscard]] std::vector<std::size_t>
SimplifyLine(const std::vector<Element>& points, double tolerance)
{
   const std::size_t n = points.size();

   std::vector<std::size_t> kept;
   if (n <= 2 || tolerance <= 0.0)
   {
      kept.resize(n);
      for (std::size_t i = 0; i < n; ++i)
      {
         kept[i] = i;
      }
      return kept;
   }

   constexpr double kPi = std::numbers::pi;

   std::vector<std::array<double, 2>> xy(n);
   for (std::size_t i = 0; i < n; ++i)
   {
      const double lat = std::clamp(points[i].latitude_, -85.0511, 85.0511);
      xy[i][0]         = (points[i].longitude_ + 180.0) / 360.0;
      xy[i][1] =
         0.5 - std::log(std::tan(kPi / 4.0 + lat * kPi / 360.0)) / (2.0 * kPi);
   }

   const auto distanceToSegment = [&xy](std::size_t p, std::size_t a, std::size_t b)
   {
      const double dx     = xy[b][0] - xy[a][0];
      const double dy     = xy[b][1] - xy[a][1];
      const double length = dx * dx + dy * dy;

      double t = 0.0;
      if (length > 0.0)
      {
         t = std::clamp(
            ((xy[p][0] - xy[a][0]) * dx + (xy[p][1] - xy[a][1]) * dy) / length,
            0.0,
            1.0);
      }
      return std::hypot(xy[p][0] - (xy[a][0] + t * dx),
                        xy[p][1] - (xy[a][1] + t * dy));
   };

   std::vector<bool> keep(n, false);
   keep[0]     = true;
   keep[n - 1] = true;

   // Iterative rather than recursive: real contours have thousands of points.
   std::vector<std::pair<std::size_t, std::size_t>> stack {{0, n - 1}};
   while (!stack.empty())
   {
      const auto [a, b] = stack.back();
      stack.pop_back();

      double      maxDistance = 0.0;
      std::size_t farthest    = a;
      for (std::size_t i = a + 1; i < b; ++i)
      {
         const double d = distanceToSegment(i, a, b);
         if (d > maxDistance)
         {
            maxDistance = d;
            farthest    = i;
         }
      }

      if (maxDistance > tolerance)
      {
         keep[farthest] = true;
         stack.emplace_back(a, farthest);
         stack.emplace_back(farthest, b);
      }
   }

   for (std::size_t i = 0; i < n; ++i)
   {
      if (keep[i])
      {
         kept.push_back(i);
      }
   }
   return kept;
}

} // namespace scwx::qt::util
