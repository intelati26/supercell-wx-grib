// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace scwx::qt::map::hodograph_zoom
{

// Every decimation stride TierForZoom() can return -- the only grid
// rows/columns HodographLayer ever reads, so the only ones it keeps in memory.
inline constexpr std::array<long, 3> kTierStrides {30, 14, 6};

// How big a hodograph looks on screen: its 30 m/s (~60 kt) vector is about this
// many pixels long at every zoom level. A hodograph is drawn in geographic
// metres, so a fixed metres-per-(m/s) grows with the map -- tiny specks zoomed
// out, enormous zoomed in. Sizing by pixels instead keeps it readable
// throughout; zooming in adds more hodographs rather than bigger ones.
inline constexpr double kGlyphPixels     = 64.0;
inline constexpr double kReferenceWindMs = 30.0;
// Latitude the metres-per-pixel conversion assumes. The true value changes as
// the view pans, and following it would rebuild every hodograph on each pan for
// a +-20% change in size across the CONUS.
inline constexpr double kReferenceLatitudeDegrees = 38.0;

// Decimation stride and geographic scale for one zoom band.
//
// stride: which grid rows/columns get a hodograph -- sparser zoomed out, denser
// zoomed in, tuned by eye so neighbours stay well apart at the glyph size
// above (RRFS's grid is 3 km: stride 30 is ~90 km, stride 6 ~18 km).
//
// metersPerMs: metres of on-map offset per 1 m/s of wind, chosen so the
// reference vector is kGlyphPixels long (see MetersPerMsForZoom()).
//
// visible: below zoom 6 (CONUS/regional) neighbouring hodographs would be
// closer than their own size, so none are drawn at all.
struct ZoomTier
{
   long   stride;
   double metersPerMs;
   bool   visible;

   bool operator==(const ZoomTier&) const = default;
};

// Metres per 1 m/s of wind that makes the reference vector kGlyphPixels long
// at `zoom` (MapLibre's 512 px tiles). Steps every quarter of a zoom level
// rather than varying continuously, so a smooth zoom does not rebuild the
// geometry on every frame (the size stays within ~19% of the target).
inline double MetersPerMsForZoom(double zoom)
{
   constexpr double kPi             = 3.14159265358979323846;
   constexpr double kEquatorMeters  = 40075016.686;
   constexpr double kTileSizePixels = 512.0;

   const double stepped = std::floor(zoom * 4.0) / 4.0;
   const double pixelsPerMeter =
      kTileSizePixels * std::exp2(stepped) /
      (kEquatorMeters * std::cos(kReferenceLatitudeDegrees * kPi / 180.0));

   return kGlyphPixels / (kReferenceWindMs * pixelsPerMeter);
}

inline ZoomTier TierForZoom(double zoom)
{
   if (zoom < 6.0)
   {
      return {0, 0.0, false};
   }

   const double metersPerMs = MetersPerMsForZoom(zoom);

   if (zoom < 7.5)
   {
      return {kTierStrides[0], metersPerMs, true};
   }
   if (zoom < 9.0)
   {
      return {kTierStrides[1], metersPerMs, true};
   }
   return {kTierStrides[2], metersPerMs, true};
}

} // namespace scwx::qt::map::hodograph_zoom
