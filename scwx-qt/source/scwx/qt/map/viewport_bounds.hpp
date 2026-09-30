#pragma once

#include <QMapLibre/Types>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace scwx::qt::map
{

// A geographic rectangle in degrees, used to build only the part of a
// CONUS-sized grid layer (wind barbs, hodographs) that is actually on
// screen. `limited == false` means "everything" -- culling disabled.
struct GeoBounds
{
   double south {-90.0};
   double west {-180.0};
   double north {90.0};
   double east {180.0};
   bool   limited {false};

   [[nodiscard]] bool Contains(double latitude, double longitude) const noexcept
   {
      return !limited || (latitude >= south && latitude <= north &&
                          longitude >= west && longitude <= east);
   }

   [[nodiscard]] bool ContainsBounds(const GeoBounds& inner) const noexcept
   {
      if (!limited)
      {
         return true;
      }
      if (!inner.limited)
      {
         return false;
      }
      return inner.south >= south && inner.north <= north &&
             inner.west >= west && inner.east <= east;
   }

   bool operator==(const GeoBounds&) const = default;
};

// The lat/lon rectangle covered by the map view, grown by `scale` about its
// center (1.0 = exactly what's visible; 2.0 = twice the radius, so the view
// can pan by about half its own size before leaving it). Sized from the
// view's half-diagonal, which covers it at any bearing. Falls back to
// unlimited (no culling) for a tilted view, where far-away terrain is
// on screen and a simple rectangle would cut it off, or before the view has
// a size.
[[nodiscard]] inline GeoBounds
VisibleBounds(const QMapLibre::CustomLayerRenderParameters& params, double scale)
{
   if (params.width <= 0.0 || params.height <= 0.0 || params.pitch > 1.0)
   {
      return {};
   }

   constexpr double kPi       = std::numbers::pi;
   constexpr double kTileSize = 512.0; // MapLibre's logical tile size

   const double worldPixels = kTileSize * std::pow(2.0, params.zoom);
   const double halfExtent =
      0.5 * std::hypot(params.width, params.height) * scale / worldPixels;

   // Web Mercator, normalized to [0, 1] with y = 0 at the north edge.
   const double latitude =
      std::clamp(params.latitude, -85.0511, 85.0511) * kPi / 180.0;
   const double centerX = (params.longitude + 180.0) / 360.0;
   const double centerY =
      0.5 - std::log(std::tan(kPi / 4.0 + latitude / 2.0)) / (2.0 * kPi);

   const auto toLatitude = [kPi](double y)
   {
      y = std::clamp(y, 0.0, 1.0);
      return std::atan(std::sinh(kPi * (1.0 - 2.0 * y))) * 180.0 / kPi;
   };

   GeoBounds bounds;
   bounds.limited = true;
   bounds.west    = (centerX - halfExtent) * 360.0 - 180.0;
   bounds.east    = (centerX + halfExtent) * 360.0 - 180.0;
   bounds.north   = toLatitude(centerY - halfExtent);
   bounds.south   = toLatitude(centerY + halfExtent);
   return bounds;
}

} // namespace scwx::qt::map
