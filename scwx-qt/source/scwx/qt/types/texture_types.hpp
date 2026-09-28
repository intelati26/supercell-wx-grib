#pragma once

#include <scwx/util/iterator.hpp>

#include <string>

namespace scwx
{
namespace qt
{
namespace types
{

enum class ImageTexture
{
   CardinalPoint24,
   Compass24,
   Crosshairs24,
   Cursor17,
   Dot3,
   LocationBriefcase,
   LocationBuildingColumns,
   LocationBuilding,
   LocationCaravan,
   LocationCrosshair,
   LocationHouse,
   LocationMarker,
   LocationPin,
   LocationStar,
   LocationTent,
   MapboxLogo,
   MapTilerLogo,
   OpenFreeMapLogo,
   // Wind barb glyphs, one per 5-knot speed bucket 0..100kt (standard
   // meteorological convention: pennants=50kt, full barbs=10kt, half
   // barbs=5kt). Reference orientation is a north wind (shaft pointing
   // up); rotate per-instance via GeoIcons::SetIconAngle to match real
   // wind direction. See grib-helper/src/gen_wind_barbs.cpp, which
   // generated the actual SVGs.
   WindBarb000,
   WindBarb005,
   WindBarb010,
   WindBarb015,
   WindBarb020,
   WindBarb025,
   WindBarb030,
   WindBarb035,
   WindBarb040,
   WindBarb045,
   WindBarb050,
   WindBarb055,
   WindBarb060,
   WindBarb065,
   WindBarb070,
   WindBarb075,
   WindBarb080,
   WindBarb085,
   WindBarb090,
   WindBarb095,
   WindBarb100,
   // Same 21 buckets, red instead of black, identical geometry at each
   // bucket (see gen_wind_barbs.cpp) -- drawn *underneath* the matching
   // WindBarbNNN icon at the gust speed bucket instead of sustained, so
   // only the gust's excess length (gust minus sustained, in whole barb
   // units) shows through in red. The real technique wX itself uses (see
   // NexradLevel3WindBarbs.cpp/NexradWidget.cpp in the wxqt repo -- gust
   // drawn first, sustained drawn second on top), not a bespoke two-tone
   // icon design.
   WindBarbGust000,
   WindBarbGust005,
   WindBarbGust010,
   WindBarbGust015,
   WindBarbGust020,
   WindBarbGust025,
   WindBarbGust030,
   WindBarbGust035,
   WindBarbGust040,
   WindBarbGust045,
   WindBarbGust050,
   WindBarbGust055,
   WindBarbGust060,
   WindBarbGust065,
   WindBarbGust070,
   WindBarbGust075,
   WindBarbGust080,
   WindBarbGust085,
   WindBarbGust090,
   WindBarbGust095,
   WindBarbGust100
};
typedef scwx::util::Iterator<ImageTexture,
                             ImageTexture::CardinalPoint24,
                             ImageTexture::WindBarbGust100>
   ImageTextureIterator;

// Looks up the WindBarbNNN entry for the nearest 5-knot bucket to
// `speedKnots`, clamped to [0, 100]. The one place callers should go
// through instead of hand-rounding themselves.
ImageTexture GetWindBarbTexture(double speedKnots);

// Same as GetWindBarbTexture, but returns the red WindBarbGustNNN
// counterpart instead.
ImageTexture GetWindBarbGustTexture(double speedKnots);

enum class LineTexture
{
   Default1x7,
   TestPattern
};
typedef scwx::util::
   Iterator<LineTexture, LineTexture::Default1x7, LineTexture::TestPattern>
      LineTextureIterator;

const std::string& GetTextureName(ImageTexture imageTexture);
const std::string& GetTextureName(LineTexture lineTexture);
const std::string& GetTexturePath(ImageTexture imageTexture);
const std::string& GetTexturePath(LineTexture lineTexture);

} // namespace types
} // namespace qt
} // namespace scwx
