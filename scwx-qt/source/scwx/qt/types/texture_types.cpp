#include <scwx/qt/types/texture_types.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace scwx
{
namespace qt
{
namespace types
{

struct TextureInfo
{
   std::string name_ {};
   std::string path_ {};
};

static const std::unordered_map<ImageTexture, TextureInfo> imageTextureInfo_ {
   {ImageTexture::CardinalPoint24,
    {"images/cardinal-point-24", ":/res/icons/flaticon/cardinal-point-24.png"}},
   {ImageTexture::Compass24,
    {"images/compass-24", ":/res/icons/flaticon/compass-24.png"}},
   {ImageTexture::Crosshairs24,
    {"images/crosshairs-24", ":/res/textures/images/crosshairs-24.png"}},
   {ImageTexture::Cursor17,
    {"images/cursor-17", ":/res/textures/images/cursor-17.png"}},
   {ImageTexture::Dot3, {"images/dot-3", ":/res/textures/images/dot.svg"}},
   {ImageTexture::LocationBriefcase,
    {"images/location-briefcase",
     ":/res/icons/font-awesome-6/briefcase-solid.svg"}},
   {ImageTexture::LocationBuildingColumns,
    {"images/location-building-columns",
     ":/res/icons/font-awesome-6/building-columns-solid.svg"}},
   {ImageTexture::LocationBuilding,
    {"images/location-building",
     ":/res/icons/font-awesome-6/building-solid.svg"}},
   {ImageTexture::LocationCaravan,
    {"images/location-caravan",
     ":/res/icons/font-awesome-6/caravan-solid.svg"}},
   {ImageTexture::LocationCrosshair,
    {"images/location-crosshair",
     ":/res/icons/font-awesome-6/location-crosshairs-solid.svg"}},
   {ImageTexture::LocationHouse,
    {"images/location-house",
     ":/res/icons/font-awesome-6/house-solid-white.svg"}},
   {ImageTexture::LocationMarker,
    {"images/location-marker", ":/res/textures/images/location-marker.svg"}},
   {ImageTexture::LocationPin,
    {"images/location-pin", ":/res/icons/font-awesome-6/location-pin.svg"}},
   {ImageTexture::LocationStar,
    {"images/location-star",
     ":/res/icons/font-awesome-6/star-solid-white.svg"}},
   {ImageTexture::LocationTent,
    {"images/location-tent", ":/res/icons/font-awesome-6/tent-solid.svg"}},
   {ImageTexture::MapboxLogo,
    {"images/mapbox-logo", ":/res/textures/images/mapbox-logo.svg"}},
   {ImageTexture::MapTilerLogo,
    {"images/maptiler-logo", ":/res/textures/images/maptiler-logo.svg"}},
   {ImageTexture::OpenFreeMapLogo,
    {"images/openfreemap-logo", ":res/textures/images/openfreemap-logo.jpg"}},
   {ImageTexture::WindBarb000,
    {"images/wind-barb-000", ":/res/icons/wind-barbs/barb_000.svg"}},
   {ImageTexture::WindBarb005,
    {"images/wind-barb-005", ":/res/icons/wind-barbs/barb_005.svg"}},
   {ImageTexture::WindBarb010,
    {"images/wind-barb-010", ":/res/icons/wind-barbs/barb_010.svg"}},
   {ImageTexture::WindBarb015,
    {"images/wind-barb-015", ":/res/icons/wind-barbs/barb_015.svg"}},
   {ImageTexture::WindBarb020,
    {"images/wind-barb-020", ":/res/icons/wind-barbs/barb_020.svg"}},
   {ImageTexture::WindBarb025,
    {"images/wind-barb-025", ":/res/icons/wind-barbs/barb_025.svg"}},
   {ImageTexture::WindBarb030,
    {"images/wind-barb-030", ":/res/icons/wind-barbs/barb_030.svg"}},
   {ImageTexture::WindBarb035,
    {"images/wind-barb-035", ":/res/icons/wind-barbs/barb_035.svg"}},
   {ImageTexture::WindBarb040,
    {"images/wind-barb-040", ":/res/icons/wind-barbs/barb_040.svg"}},
   {ImageTexture::WindBarb045,
    {"images/wind-barb-045", ":/res/icons/wind-barbs/barb_045.svg"}},
   {ImageTexture::WindBarb050,
    {"images/wind-barb-050", ":/res/icons/wind-barbs/barb_050.svg"}},
   {ImageTexture::WindBarb055,
    {"images/wind-barb-055", ":/res/icons/wind-barbs/barb_055.svg"}},
   {ImageTexture::WindBarb060,
    {"images/wind-barb-060", ":/res/icons/wind-barbs/barb_060.svg"}},
   {ImageTexture::WindBarb065,
    {"images/wind-barb-065", ":/res/icons/wind-barbs/barb_065.svg"}},
   {ImageTexture::WindBarb070,
    {"images/wind-barb-070", ":/res/icons/wind-barbs/barb_070.svg"}},
   {ImageTexture::WindBarb075,
    {"images/wind-barb-075", ":/res/icons/wind-barbs/barb_075.svg"}},
   {ImageTexture::WindBarb080,
    {"images/wind-barb-080", ":/res/icons/wind-barbs/barb_080.svg"}},
   {ImageTexture::WindBarb085,
    {"images/wind-barb-085", ":/res/icons/wind-barbs/barb_085.svg"}},
   {ImageTexture::WindBarb090,
    {"images/wind-barb-090", ":/res/icons/wind-barbs/barb_090.svg"}},
   {ImageTexture::WindBarb095,
    {"images/wind-barb-095", ":/res/icons/wind-barbs/barb_095.svg"}},
   {ImageTexture::WindBarb100,
    {"images/wind-barb-100", ":/res/icons/wind-barbs/barb_100.svg"}},
   {ImageTexture::WindBarbGust000,
    {"images/wind-barb-gust-000", ":/res/icons/wind-barbs/barb_gust_000.svg"}},
   {ImageTexture::WindBarbGust005,
    {"images/wind-barb-gust-005", ":/res/icons/wind-barbs/barb_gust_005.svg"}},
   {ImageTexture::WindBarbGust010,
    {"images/wind-barb-gust-010", ":/res/icons/wind-barbs/barb_gust_010.svg"}},
   {ImageTexture::WindBarbGust015,
    {"images/wind-barb-gust-015", ":/res/icons/wind-barbs/barb_gust_015.svg"}},
   {ImageTexture::WindBarbGust020,
    {"images/wind-barb-gust-020", ":/res/icons/wind-barbs/barb_gust_020.svg"}},
   {ImageTexture::WindBarbGust025,
    {"images/wind-barb-gust-025", ":/res/icons/wind-barbs/barb_gust_025.svg"}},
   {ImageTexture::WindBarbGust030,
    {"images/wind-barb-gust-030", ":/res/icons/wind-barbs/barb_gust_030.svg"}},
   {ImageTexture::WindBarbGust035,
    {"images/wind-barb-gust-035", ":/res/icons/wind-barbs/barb_gust_035.svg"}},
   {ImageTexture::WindBarbGust040,
    {"images/wind-barb-gust-040", ":/res/icons/wind-barbs/barb_gust_040.svg"}},
   {ImageTexture::WindBarbGust045,
    {"images/wind-barb-gust-045", ":/res/icons/wind-barbs/barb_gust_045.svg"}},
   {ImageTexture::WindBarbGust050,
    {"images/wind-barb-gust-050", ":/res/icons/wind-barbs/barb_gust_050.svg"}},
   {ImageTexture::WindBarbGust055,
    {"images/wind-barb-gust-055", ":/res/icons/wind-barbs/barb_gust_055.svg"}},
   {ImageTexture::WindBarbGust060,
    {"images/wind-barb-gust-060", ":/res/icons/wind-barbs/barb_gust_060.svg"}},
   {ImageTexture::WindBarbGust065,
    {"images/wind-barb-gust-065", ":/res/icons/wind-barbs/barb_gust_065.svg"}},
   {ImageTexture::WindBarbGust070,
    {"images/wind-barb-gust-070", ":/res/icons/wind-barbs/barb_gust_070.svg"}},
   {ImageTexture::WindBarbGust075,
    {"images/wind-barb-gust-075", ":/res/icons/wind-barbs/barb_gust_075.svg"}},
   {ImageTexture::WindBarbGust080,
    {"images/wind-barb-gust-080", ":/res/icons/wind-barbs/barb_gust_080.svg"}},
   {ImageTexture::WindBarbGust085,
    {"images/wind-barb-gust-085", ":/res/icons/wind-barbs/barb_gust_085.svg"}},
   {ImageTexture::WindBarbGust090,
    {"images/wind-barb-gust-090", ":/res/icons/wind-barbs/barb_gust_090.svg"}},
   {ImageTexture::WindBarbGust095,
    {"images/wind-barb-gust-095", ":/res/icons/wind-barbs/barb_gust_095.svg"}},
   {ImageTexture::WindBarbGust100,
    {"images/wind-barb-gust-100", ":/res/icons/wind-barbs/barb_gust_100.svg"}}};

static const std::unordered_map<LineTexture, TextureInfo> lineTextureInfo_ {
   {LineTexture::Default1x7,
    {"lines/default-1x7", ":/res/textures/lines/default-1x7.png"}},
   {LineTexture::TestPattern,
    {"lines/test-pattern", ":/res/textures/lines/test-pattern.png"}}};

const std::string& GetTextureName(ImageTexture imageTexture)
{
   return imageTextureInfo_.at(imageTexture).name_;
}

const std::string& GetTextureName(LineTexture lineTexture)
{
   return lineTextureInfo_.at(lineTexture).name_;
}

const std::string& GetTexturePath(ImageTexture imageTexture)
{
   return imageTextureInfo_.at(imageTexture).path_;
}

const std::string& GetTexturePath(LineTexture lineTexture)
{
   return lineTextureInfo_.at(lineTexture).path_;
}

ImageTexture GetWindBarbTexture(double speedKnots)
{
   // WindBarb000..WindBarb100 are declared consecutively (5kt apart), so
   // the nearest bucket is just an offset from WindBarb000 -- no lookup
   // table needed.
   constexpr int kMaxBucket = 20; // (100 - 0) / 5

   int bucket = static_cast<int>(std::lround(speedKnots / 5.0));
   bucket     = std::clamp(bucket, 0, kMaxBucket);

   return static_cast<ImageTexture>(
      static_cast<int>(ImageTexture::WindBarb000) + bucket);
}

ImageTexture GetWindBarbGustTexture(double speedKnots)
{
   constexpr int kMaxBucket = 20; // (100 - 0) / 5

   int bucket = static_cast<int>(std::lround(speedKnots / 5.0));
   bucket     = std::clamp(bucket, 0, kMaxBucket);

   return static_cast<ImageTexture>(
      static_cast<int>(ImageTexture::WindBarbGust000) + bucket);
}

} // namespace types
} // namespace qt
} // namespace scwx
