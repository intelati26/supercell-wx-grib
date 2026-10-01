#include <scwx/qt/map/wind_barb_layer.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/gl/draw/geo_icons.hpp>
#include <scwx/qt/manager/wind_barb_manager.hpp>
#include <scwx/qt/settings/wind_barb_settings.hpp>
#include <scwx/qt/types/texture_types.hpp>
#include <scwx/util/logger.hpp>

#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <units/angle.h>

#include <QGuiApplication>

namespace scwx::qt::map
{

static const std::string logPrefix_ = "scwx::qt::map::wind_barb_layer";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// 1 m/s = 1.9438445 kt (exact: 1 kt = 1852 m / 3600 s).
static constexpr double kMetersPerSecondToKnots_ = 1.9438445;

// Number of WindBarb000..WindBarb100 icon sheets (5kt buckets, 0-100kt).
static constexpr int kWindBarbSheetCount_ = 21;

// Anchor point within each 64x64 barb SVG -- the shaft's base, i.e. the
// actual station location (see grib-helper/src/gen_wind_barbs.cpp), *not*
// the image's visual center (which AddIconSheet's -1/-1 default would
// use). Same non-centered-hotspot idiom MarkerLayer already uses for pin
// icons.
static constexpr std::int32_t kBarbHotX_ = 32;
static constexpr std::int32_t kBarbHotY_ = 56;

namespace
{

// One parsed frame file: RTMA's Lambert grid geometry plus this field's
// raw decoded values (row-major, nx * ny floats). Deliberately not reusing
// GribProductLayer's LoadFrame wholesale -- that method is entangled with
// its own per-product palette/color state this layer has no use for -- but
// it does go through the same shared ExtractNumber/LambertGrid grib_frame_
// info.hpp helpers that extraction promoted, rather than re-deriving them.
struct ParsedFrame
{
   long               grid_nx {};
   long               grid_ny {};
   LambertGrid        grid {};
   std::string        validTime;
   std::vector<float> values;
};

std::optional<ParsedFrame> ParseFrame(const std::string& path)
{
   std::ifstream in(path, std::ios::binary);
   if (!in)
   {
      return std::nullopt;
   }

   std::string header;
   std::getline(in, header);

   try
   {
      ParsedFrame frame;
      frame.grid_nx     = static_cast<long>(ExtractNumber(header, "nx"));
      frame.grid_ny     = static_cast<long>(ExtractNumber(header, "ny"));
      frame.grid.lat1   = ExtractNumber(header, "lat1");
      frame.grid.lon1   = ExtractNumber(header, "lon1");
      frame.grid.lov    = ExtractNumberOr(header, "lov", 0.0);
      frame.grid.lad    = ExtractNumberOr(header, "lad", 0.0);
      frame.grid.latin1 = ExtractNumberOr(header, "latin1", 0.0);
      frame.grid.latin2 = ExtractNumberOr(header, "latin2", 0.0);
      frame.grid.dx     = ExtractNumberOr(header, "dx", 0.0);
      frame.grid.dy     = ExtractNumberOr(header, "dy", 0.0);
      frame.grid.radius = ExtractNumberOr(header, "radius", 0.0);
      frame.validTime   = ExtractStringOr(header, "validTime", {});

      const auto byteLength =
         static_cast<std::size_t>(ExtractNumber(header, "byteLength"));
      frame.values.resize(byteLength / sizeof(float));
      in.read(reinterpret_cast<char*>(frame.values.data()),
              static_cast<std::streamsize>(byteLength));

      if (!in || frame.values.size() !=
                    static_cast<std::size_t>(frame.grid_nx * frame.grid_ny))
      {
         return std::nullopt;
      }

      return frame;
   }
   catch (const std::runtime_error& ex)
   {
      logger_->warn("Could not parse frame {}: {}", path, ex.what());
      return std::nullopt;
   }
}

// Picks which grid points get a barb: every `stride`-th row and column.
// RTMA's full 2.5km CONUS grid (~2345 x 1597) is far too dense to plot one
// icon per point, so the caller picks `stride` based on how zoomed in the
// view currently is -- see StrideForZoom below.
std::vector<std::pair<long, long>>
SelectBarbPoints(long nx, long ny, long stride)
{
   std::vector<std::pair<long, long>> points;

   if (stride < 1)
   {
      return points;
   }

   points.reserve(
      static_cast<std::size_t>((nx / stride + 1) * (ny / stride + 1)));

   for (long j = 0; j < ny; j += stride)
   {
      for (long i = 0; i < nx; i += stride)
      {
         points.emplace_back(i, j);
      }
   }

   return points;
}

// Decimation stride and icon scale for one zoom band -- paired together
// since denser points and larger icons both need more screen room, and
// sparser/smaller need less (see TierForZoom).
struct ZoomTier
{
   long  stride;
   float iconScale;

   bool operator==(const ZoomTier&) const = default;
};

// Maps the map's current zoom level to a ZoomTier -- smaller stride
// (denser barbs) and a larger iconScale as the user zooms in.
ZoomTier TierForZoom(double zoom)
{
   // Tiered rather than continuous -- easier to tune by eye than derive
   // analytically. RTMA is ~2.5km/point, so stride lands roughly:
   // zoom <5 (CONUS/regional): stride 40 -> ~100km spacing
   // zoom <7 (state-level):    stride 16 -> ~40km spacing
   // zoom <9 (multi-county):   stride 7  -> ~18km spacing
   // zoom >=9 (metro area):    stride 3  -> ~8km spacing
   //
   // iconScale is relative to the barb SVGs' native 64x64 px -- a
   // regional/CONUS-zoomed view showing barbs at their full native size
   // was reported as comically oversized (each icon dwarfing the state
   // it sat over), so every tier renders well under native size, growing
   // toward (but still under) native only at the closest tier. Bumped up
   // (and stride tightened above) from an even smaller/sparser first
   // guess, per user feedback that it read as too small/sparse -- these
   // are still just a starting point, not a final answer: see
   // WindBarbSettings::icon_scale()/density_scale() for the real,
   // user-adjustable multiplier on top of these base values.
   if (zoom < 5.0)
   {
      return {40, 0.26f}; // ~17px
   }
   else if (zoom < 7.0)
   {
      return {16, 0.38f}; // ~24px
   }
   else if (zoom < 9.0)
   {
      return {7, 0.52f}; // ~33px
   }
   else
   {
      return {3, 0.7f}; // ~45px
   }
}

} // namespace

class WindBarbLayer::Impl
{
public:
   explicit Impl(WindBarbLayer*                        self,
                 const std::shared_ptr<gl::GlContext>& glContext) :
       self_ {self}, geoIcons_ {std::make_shared<gl::draw::GeoIcons>(glContext)}
   {
      QObject::connect(windBarbManager_.get(),
                       &manager::WindBarbManager::WindDataReady,
                       self_,
                       [this]() { ReloadBarbs(); });
   }
   ~Impl() = default;

   Impl(const Impl&)            = delete;
   Impl& operator=(const Impl&) = delete;

   void SetIconSheets();
   void ReloadBarbs();
   void RebuildIcons(ZoomTier tier);

   WindBarbLayer* self_;

   std::shared_ptr<gl::draw::GeoIcons>       geoIcons_;
   std::shared_ptr<manager::WindBarbManager> windBarbManager_ {
      manager::WindBarbManager::Instance()};

   // Cached by ReloadBarbs() (fires on WindBarbManager::WindDataReady, so
   // only on an actual hourly RTMA update) and consumed by RebuildIcons()
   // (also called from Render() on a zoom-tier change) -- keeps a zoom
   // change from re-reading/re-parsing the frame files off disk, only the
   // already-decoded values need to change on that path.
   std::optional<ParsedFrame> dirFrame_;
   std::optional<ParsedFrame> speedFrame_;
   std::optional<ParsedFrame> gustFrame_;

   // Tier actually built into geoIcons_ right now (post density/icon-scale
   // multiplier -- see Render()); Render() only calls RebuildIcons() when
   // the freshly-computed tier differs from this, so an unchanging zoom
   // and unchanged settings cost one comparison per frame, not a GeoIcons
   // rebuild + GPU buffer upload. {0, 0.0f} never matches a real computed
   // tier, guaranteeing the first Render() call rebuilds.
   ZoomTier lastTier_ {0, 0.0f};
};

void WindBarbLayer::Impl::SetIconSheets()
{
   geoIcons_->StartIconSheets();
   for (int bucket = 0; bucket < kWindBarbSheetCount_; ++bucket)
   {
      const auto texture = static_cast<types::ImageTexture>(
         static_cast<int>(types::ImageTexture::WindBarb000) + bucket);
      geoIcons_->AddIconSheet(
         types::GetTextureName(texture), 0, 0, kBarbHotX_, kBarbHotY_);
   }
   for (int bucket = 0; bucket < kWindBarbSheetCount_; ++bucket)
   {
      const auto texture = static_cast<types::ImageTexture>(
         static_cast<int>(types::ImageTexture::WindBarbGust000) + bucket);
      geoIcons_->AddIconSheet(
         types::GetTextureName(texture), 0, 0, kBarbHotX_, kBarbHotY_);
   }
   geoIcons_->FinishIconSheets();
}

void WindBarbLayer::Impl::ReloadBarbs()
{
   auto dirFrame =
      ParseFrame(manager::WindBarbManager::GetWindDirectionFramePath());
   auto speedFrame =
      ParseFrame(manager::WindBarbManager::GetWindSpeedFramePath());
   auto gustFrame =
      ParseFrame(manager::WindBarbManager::GetWindGustFramePath());

   if (!dirFrame || !speedFrame || !gustFrame ||
       dirFrame->grid_nx != speedFrame->grid_nx ||
       dirFrame->grid_ny != speedFrame->grid_ny ||
       dirFrame->grid_nx != gustFrame->grid_nx ||
       dirFrame->grid_ny != gustFrame->grid_ny)
   {
      logger_->warn("Could not load wind barb frames");
      return;
   }

   logger_->debug("ReloadBarbs()");

   dirFrame_   = std::move(dirFrame);
   speedFrame_ = std::move(speedFrame);
   gustFrame_  = std::move(gustFrame);

   RebuildIcons(lastTier_);
}

void WindBarbLayer::Impl::RebuildIcons(ZoomTier tier)
{
   if (!dirFrame_ || !speedFrame_ || !gustFrame_)
   {
      return;
   }

   logger_->debug(
      "RebuildIcons(stride={}, iconScale={})", tier.stride, tier.iconScale);

   lastTier_ = tier;

   // Read once per rebuild, not per point -- SettingsVariable access
   // isn't free, and this doesn't change mid-loop.
   const bool showGustBarbs =
      settings::WindBarbSettings::Instance().show_gust_barbs().GetValue();

   geoIcons_->StartIcons();

   for (const auto& [i, j] :
        SelectBarbPoints(dirFrame_->grid_nx, dirFrame_->grid_ny, tier.stride))
   {
      const auto index = static_cast<std::size_t>(j) *
                            static_cast<std::size_t>(dirFrame_->grid_nx) +
                         static_cast<std::size_t>(i);
      if (index >= dirFrame_->values.size() ||
          index >= speedFrame_->values.size() ||
          index >= gustFrame_->values.size())
      {
         continue;
      }

      const float directionDeg = dirFrame_->values[index];
      const float speedMs      = speedFrame_->values[index];
      const float gustMs       = gustFrame_->values[index];
      if (!std::isfinite(directionDeg) || !std::isfinite(speedMs) ||
          speedMs < 0.0f)
      {
         continue;
      }

      const double speedKnots = speedMs * kMetersPerSecondToKnots_;
      const double gustKnots =
         std::isfinite(gustMs) ? gustMs * kMetersPerSecondToKnots_ : 0.0;

      const glm::dvec2 latLon = LambertGridToLatLon(
         dirFrame_->grid, static_cast<double>(i), static_cast<double>(j));

      // Negated: GeoIcons' shared shader rotates opposite to standard
      // compass/meteorological convention (0=N, clockwise) -- confirmed
      // via OverlayLayer's own compass icon, which negates params.bearing
      // the same way (`-45 - params.bearing`) to land correctly on
      // screen. Passing directionDeg unnegated produced a mirrored
      // rotation -- reported by the user as "barbs are reversed" against
      // a real screenshot, fixed here. Same angle for both the gust and
      // sustained icon below -- only speed differs between them.
      const auto angle = units::angle::degrees<double> {-directionDeg};

      // Gust barb drawn first (underneath), sustained barb drawn second
      // (on top) -- the actual technique wX uses (see
      // NexradLevel3WindBarbs.cpp/NexradWidget.cpp in the wxqt repo: gust
      // rendered in red first, sustained rendered in the normal color on
      // top), not a bespoke two-tone icon. Both icons place their
      // decorations at identical positions counting outward from the
      // same shaft-top reference point (see gen_wind_barbs.cpp), so the
      // opaque sustained decorations exactly cover the overlapping
      // portion of the gust decorations beneath -- only gust's *excess*
      // length (gust minus sustained, in whole barb/pennant units) ends
      // up visible in red, with no excess math needed here. Skipped
      // entirely when gust doesn't exceed sustained by a full 5kt bucket
      // (GetWindBarbGustTexture/GetWindBarbTexture would pick the same
      // bucket, so the red icon would be fully hidden anyway) or when
      // the user has turned it off in Settings.
      if (showGustBarbs && gustKnots >= speedKnots + 5.0)
      {
         const auto gustIcon = geoIcons_->AddIcon();
         geoIcons_->SetIconTexture(
            gustIcon,
            types::GetTextureName(types::GetWindBarbGustTexture(gustKnots)),
            0);
         geoIcons_->SetIconLocation(gustIcon, latLon.x, latLon.y);
         geoIcons_->SetIconAngle(gustIcon, angle);
         geoIcons_->SetIconScale(gustIcon, tier.iconScale);
      }

      const auto icon = geoIcons_->AddIcon();
      geoIcons_->SetIconTexture(
         icon, types::GetTextureName(types::GetWindBarbTexture(speedKnots)), 0);
      geoIcons_->SetIconLocation(icon, latLon.x, latLon.y);
      geoIcons_->SetIconAngle(icon, angle);
      geoIcons_->SetIconScale(icon, tier.iconScale);
      geoIcons_->SetIconHoverText(
         icon,
         (gustKnots >= speedKnots + 5.0) ?
            fmt::format("{:.0f} kt @ {:03.0f}° (gust {:.0f} kt)\nValid: {}",
                        speedKnots,
                        directionDeg,
                        gustKnots,
                        dirFrame_->validTime) :
            fmt::format("{:.0f} kt @ {:03.0f}°\nValid: {}",
                        speedKnots,
                        directionDeg,
                        dirFrame_->validTime));
   }

   geoIcons_->FinishIcons();
}

WindBarbLayer::WindBarbLayer(const std::shared_ptr<gl::GlContext>& glContext) :
    DrawLayer(glContext, "WindBarbLayer"),
    p(std::make_unique<Impl>(this, glContext))
{ AddDrawItem(p->geoIcons_); }

WindBarbLayer::~WindBarbLayer() = default;

void WindBarbLayer::Initialize(const std::shared_ptr<MapContext>& mapContext)
{
   logger_->debug("Initialize()");
   DrawLayer::Initialize(mapContext);

   p->SetIconSheets();
   p->ReloadBarbs();
}

void WindBarbLayer::Render(const std::shared_ptr<MapContext>& mapContext,
                           const QMapLibre::CustomLayerRenderParameters& params)
{
   const ZoomTier baseTier = TierForZoom(params.zoom);

   // User-adjustable multipliers on top of the fixed per-tier base values
   // (see WindBarbSettings::icon_scale()/density_scale() -- added after
   // feedback that the fixed tiers read as too small/sparse). Read every
   // Render() call, not cached, so a Settings change takes effect on the
   // very next frame without needing its own change signal -- Render()
   // already runs continuously while this layer is visible.
   auto&        windBarbSettings = settings::WindBarbSettings::Instance();
   const double densityScale     = windBarbSettings.density_scale().GetValue();
   const double iconScaleFactor  = windBarbSettings.icon_scale().GetValue();

   const ZoomTier tier {
      // Larger densityScale -> smaller stride -> denser. Floored, not
      // rounded, so density_scale > 1 never fails to tighten the spacing
      // at all for a base stride already close to 1 (e.g. base stride 4 at
      // densityScale 1.5 -> 2, not rounded back down to itself).
      std::max<long>(
         1, static_cast<long>(std::floor(baseTier.stride / densityScale))),
      baseTier.iconScale * static_cast<float>(iconScaleFactor)};

   if (tier != p->lastTier_)
   {
      p->RebuildIcons(tier);
   }

   DrawLayer::Render(mapContext, params);
}

bool WindBarbLayer::RunMousePicking(
   const std::shared_ptr<MapContext>&            mapContext,
   const QMapLibre::CustomLayerRenderParameters& params,
   const QPointF&                                mouseLocalPos,
   const QPointF&                                mouseGlobalPos,
   const glm::vec2&                              mouseCoords,
   const common::Coordinate&                     mouseGeoCoords,
   std::shared_ptr<types::EventHandler>&         eventHandler)
{
   // Shift-gated, matching the app-wide "Shift = show me the data"
   // convention (RadarProductLayer/GribProductLayer) -- barb hover text
   // is speed/direction/valid-time data, not a label like MarkerLayer's
   // own always-on hover (which also goes through GeoIcons, but
   // deliberately isn't gated -- a marker's name is closer to a caption
   // than to revealed data). GeoIcons itself has no Shift concept, so
   // this has to happen here, before DrawLayer::RunMousePicking ever
   // reaches it.
   if (!(QGuiApplication::keyboardModifiers() &
         Qt::KeyboardModifier::ShiftModifier))
   {
      return false;
   }

   return DrawLayer::RunMousePicking(mapContext,
                                     params,
                                     mouseLocalPos,
                                     mouseGlobalPos,
                                     mouseCoords,
                                     mouseGeoCoords,
                                     eventHandler);
}

void WindBarbLayer::Deinitialize()
{
   logger_->debug("Deinitialize()");

   DrawLayer::Deinitialize();
}

} // namespace scwx::qt::map
