#include <scwx/qt/map/hodograph_layer.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/map/hodograph_zoom.hpp>
#include <scwx/qt/map/viewport_bounds.hpp>
#include <scwx/qt/gl/draw/geo_lines.hpp>
#include <scwx/qt/manager/hodograph_manager.hpp>
#include <scwx/qt/manager/hodograph_selection.hpp>
#include <scwx/qt/settings/hodograph_settings.hpp>
#include <scwx/qt/util/geographic_lib.hpp>
#include <scwx/util/logger.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include <QGuiApplication>

namespace scwx::qt::map
{

using hodograph_zoom::TierForZoom;
using hodograph_zoom::ZoomTier;


static const std::string logPrefix_ = "scwx::qt::map::hodograph_layer";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// 1 m/s = 1.9438445 kt (exact: 1 kt = 1852 m / 3600 s) -- same constant
// WindBarbLayer's own file defines, duplicated rather than shared since
// it's a plain, well-known unit conversion, not shared logic.
static constexpr double kMetersPerSecondToKnots_ = 1.9438445;

namespace
{

// Maps a full-grid row/column index to its slot in a ParsedFrame's
// compacted values, or -1 for an index no tier ever samples.
std::vector<long> SampledAxisSlots(long size)
{
   std::vector<long> slotOf(static_cast<std::size_t>(std::max(size, 0L)), -1);
   long              next = 0;
   for (long index = 0; index < size; ++index)
   {
      for (const long stride : hodograph_zoom::kTierStrides)
      {
         if (index % stride == 0)
         {
            slotOf[static_cast<std::size_t>(index)] = next++;
            break;
         }
      }
   }
   return slotOf;
}

// One decoded field's Lambert grid geometry plus its values -- but only at
// the rows/columns some zoom tier actually samples (see hodograph_zoom::kTierStrides),
// not the whole grid. The layer holds 34 levels x (u, v) plus terrain at
// once; at RRFS's full ~1800 x 1060 grid that was ~0.5GB of floats, nearly
// all of it never read. Keeping just the sampled rows/columns is ~5% of
// that. Otherwise the same shape as WindBarbLayer's own ParsedFrame.
struct ParsedFrame
{
   long               grid_nx {};
   long               grid_ny {};
   LambertGrid        grid {};
   std::string        validTime;
   double             missingValue {};
   std::vector<long>  colSlots; // grid_nx entries, see SampledAxisSlots()
   std::vector<long>  rowSlots; // grid_ny entries
   long               sampledCols {};
   std::vector<float> values; // sampled rows x sampledCols, row-major

   // Value at full-grid index (i, j), or std::nullopt if (i, j) is outside
   // the grid or isn't a sampled point.
   [[nodiscard]] std::optional<float> At(long i, long j) const
   {
      if (i < 0 || i >= grid_nx || j < 0 || j >= grid_ny)
      {
         return std::nullopt;
      }
      const long col = colSlots[static_cast<std::size_t>(i)];
      const long row = rowSlots[static_cast<std::size_t>(j)];
      if (col < 0 || row < 0)
      {
         return std::nullopt;
      }
      return values[static_cast<std::size_t>(row) *
                       static_cast<std::size_t>(sampledCols) +
                    static_cast<std::size_t>(col)];
   }
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
      frame.grid_nx      = static_cast<long>(ExtractNumber(header, "nx"));
      frame.grid_ny      = static_cast<long>(ExtractNumber(header, "ny"));
      frame.grid.lat1    = ExtractNumber(header, "lat1");
      frame.grid.lon1    = ExtractNumber(header, "lon1");
      frame.grid.lov     = ExtractNumberOr(header, "lov", 0.0);
      frame.grid.lad     = ExtractNumberOr(header, "lad", 0.0);
      frame.grid.latin1  = ExtractNumberOr(header, "latin1", 0.0);
      frame.grid.latin2  = ExtractNumberOr(header, "latin2", 0.0);
      frame.grid.dx      = ExtractNumberOr(header, "dx", 0.0);
      frame.grid.dy      = ExtractNumberOr(header, "dy", 0.0);
      frame.grid.radius  = ExtractNumberOr(header, "radius", 0.0);
      frame.validTime    = ExtractStringOr(header, "validTime", {});
      frame.missingValue = ExtractNumber(header, "missingValue");

      const auto byteLength =
         static_cast<std::size_t>(ExtractNumber(header, "byteLength"));

      if (frame.grid_nx <= 0 || frame.grid_ny <= 0 ||
          byteLength != static_cast<std::size_t>(frame.grid_nx) *
                           static_cast<std::size_t>(frame.grid_ny) *
                           sizeof(float))
      {
         return std::nullopt;
      }

      frame.colSlots = SampledAxisSlots(frame.grid_nx);
      frame.rowSlots = SampledAxisSlots(frame.grid_ny);
      frame.sampledCols =
         static_cast<long>(std::count_if(frame.colSlots.cbegin(),
                                         frame.colSlots.cend(),
                                         [](long slot) { return slot >= 0; }));
      const auto sampledRows = static_cast<std::size_t>(
         std::count_if(frame.rowSlots.cbegin(),
                       frame.rowSlots.cend(),
                       [](long slot) { return slot >= 0; }));

      frame.values.reserve(sampledRows *
                           static_cast<std::size_t>(frame.sampledCols));

      // Row by row, reading (and seeking past) whole rows so only one
      // row's worth of the full grid is ever in memory at once.
      const auto rowBytes = static_cast<std::streamsize>(
         static_cast<std::size_t>(frame.grid_nx) * sizeof(float));
      std::vector<float> row(static_cast<std::size_t>(frame.grid_nx));

      for (long j = 0; j < frame.grid_ny; ++j)
      {
         if (frame.rowSlots[static_cast<std::size_t>(j)] < 0)
         {
            in.seekg(rowBytes, std::ios::cur);
            continue;
         }

         in.read(reinterpret_cast<char*>(row.data()), rowBytes);
         if (!in)
         {
            return std::nullopt;
         }

         for (long i = 0; i < frame.grid_nx; ++i)
         {
            if (frame.colSlots[static_cast<std::size_t>(i)] >= 0)
            {
               frame.values.push_back(row[static_cast<std::size_t>(i)]);
            }
         }
      }

      // seekg() past the end doesn't fail by itself -- make sure the file
      // really held the whole grid, trailing skipped rows included.
      in.seekg(0, std::ios::end);
      const auto fileEnd = in.tellg();
      if (!in ||
          fileEnd < static_cast<std::streamoff>(header.size() + 1 + byteLength))
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

// Every level's u/v plus terrain, parsed once and shared by every map pane's
// HodographLayer. As 35 full RRFS grids that was ~525MB, held per pane --
// and kept even while the layer was hidden below zoom 6 -- the app's single
// largest allocation (a 6-pane profile put ~3GB here). Shared, and with
// ParseFrame() keeping only the sampled rows/columns, it's now ~24MB total.
struct SharedFrames
{
   std::uint64_t                           generation {};
   std::vector<std::optional<ParsedFrame>> u;
   std::vector<std::optional<ParsedFrame>> v;
   std::optional<ParsedFrame>              terrain;
};

// weak_ptr, so the parsed grids are freed as soon as the last pane that
// needs them lets go (e.g. every pane zoomed out below the visible tier).
std::mutex                        sharedFramesMutex_;
std::weak_ptr<const SharedFrames> sharedFrames_;

std::shared_ptr<const SharedFrames> ParseAllFrames(std::uint64_t generation)
{
   const auto& levels = manager::HodographManager::Levels();

   auto frames        = std::make_shared<SharedFrames>();
   frames->generation = generation;
   frames->u.resize(levels.size());
   frames->v.resize(levels.size());

   for (std::size_t i = 0; i < levels.size(); ++i)
   {
      frames->u[i] = ParseFrame(manager::HodographManager::GetUFramePath(i));
      frames->v[i] = ParseFrame(manager::HodographManager::GetVFramePath(i));

      if (!frames->u[i] || !frames->v[i])
      {
         logger_->warn("Could not load hodograph frame for level {}", i);
         return nullptr;
      }
   }

   frames->terrain =
      ParseFrame(manager::HodographManager::GetTerrainFramePath());
   if (!frames->terrain)
   {
      logger_->warn("Could not load hodograph terrain frame");
      return nullptr;
   }

   // Every level plus terrain comes from the same decoded file, so their
   // grids should always match -- this guards a partial/corrupt write,
   // not a real mismatch.
   for (std::size_t i = 0; i < levels.size(); ++i)
   {
      if (frames->u[i]->grid_nx != frames->terrain->grid_nx ||
          frames->u[i]->grid_ny != frames->terrain->grid_ny ||
          frames->v[i]->grid_nx != frames->terrain->grid_nx ||
          frames->v[i]->grid_ny != frames->terrain->grid_ny)
      {
         logger_->warn("Hodograph frame grid mismatch at level {}", i);
         return nullptr;
      }
   }

   return frames;
}

// Same decimation idiom as WindBarbLayer's own SelectBarbPoints: every
// `stride`-th row and column within `window`, kept on whole-grid multiples
// of `stride` (which is also what keeps every point on a row/column
// ParseFrame() sampled).
std::vector<std::pair<long, long>>
SelectHodographPoints(const GridIndexBox& window, long stride)
{
   std::vector<std::pair<long, long>> points;

   if (stride < 1 || window.IsEmpty())
   {
      return points;
   }

   const long iStart = (window.iMin + stride - 1) / stride * stride;
   const long jStart = (window.jMin + stride - 1) / stride * stride;

   points.reserve(
      static_cast<std::size_t>(((window.iMax - iStart) / stride + 1) *
                               ((window.jMax - jStart) / stride + 1)));

   for (long j = jStart; j <= window.jMax; j += stride)
   {
      for (long i = iStart; i <= window.iMax; i += stride)
      {
         points.emplace_back(i, j);
      }
   }

   return points;
}

// How much larger than the visible view the built area is. 2.0 lets the view
// pan by roughly half its own size before geometry is rebuilt.
constexpr double kBuildMargin_ = 2.0;

// Reads manager::HodographManager::BandForHeight()'s own shared table
// (not a locally-hardcoded one) specifically so this and
// GribDockWidget's legend can never drift apart -- see that table's own
// doc for the color/band convention itself.
boost::gil::rgba8_pixel_t HeightBandColor(float heightMeters)
{
   const auto& rgb = manager::HodographManager::BandForHeight(heightMeters).rgb;
   return {rgb[0], rgb[1], rgb[2], 255};
}

// Concentric speed-reference rings drawn behind each hodograph, the same
// idea as the reference screenshot that originally motivated this
// feature (see docs/gridded-hodograph-plan.md) -- one of that doc's own
// open questions, resolved here rather than left out entirely. Every
// 20kt out to 60kt is a first-guess interval, not derived from anything
// (most real hodographs of interest fall well inside a 60kt outer ring);
// segment count is a circle-smoothness/cost tradeoff, cheap enough at
// this decimation density to not matter.
constexpr double kRangeRingIntervalKnots_ = 20.0;
constexpr int    kRangeRingCount_         = 3;
constexpr int    kRangeRingSegments_      = 24;

// Muted gray, fully opaque -- deliberately not relying on alpha
// blending (unconfirmed whether GeoLines' draw state enables it) to read
// as "background reference," just a plain, visually quieter color than
// any real height-band line color.
const boost::gil::rgba8_pixel_t kRangeRingColor_ {170, 170, 170, 255};

// Draws one hodograph's full set of range rings (see kRangeRingCount_)
// around `center`, scaled by the same metersPerMs the wind vectors
// themselves use so a ring genuinely means "this many knots" at the
// current zoom tier. Approximates each ring as a kRangeRingSegments_-
// sided polygon via GeographicLib::GetCoordinate's angle+distance
// overload (the same helper LinkedVectors' own tick marks already use) --
// GeoLines has no native circle/arc primitive, so this is the
// straightforward way to get one out of straight line segments.
void DrawRangeRings(gl::draw::GeoLines&       geoLines,
                    const common::Coordinate& center,
                    double                    metersPerMs)
{
   for (int ring = 1; ring <= kRangeRingCount_; ++ring)
   {
      const double speedMs =
         (kRangeRingIntervalKnots_ * ring) / kMetersPerSecondToKnots_;
      const double radiusM = speedMs * metersPerMs;

      common::Coordinate prev = util::GeographicLib::GetCoordinate(
         center,
         units::angle::degrees<double> {0.0},
         units::length::meters<double> {radiusM});

      for (int seg = 1; seg <= kRangeRingSegments_; ++seg)
      {
         const double             angleDeg = 360.0 * seg / kRangeRingSegments_;
         const common::Coordinate next     = util::GeographicLib::GetCoordinate(
            center,
            units::angle::degrees<double> {angleDeg},
            units::length::meters<double> {radiusM});

         auto line = geoLines.AddLine();
         geoLines.SetLineLocation(line,
                                  static_cast<float>(prev.latitude_),
                                  static_cast<float>(prev.longitude_),
                                  static_cast<float>(next.latitude_),
                                  static_cast<float>(next.longitude_));
         geoLines.SetLineModulate(line, kRangeRingColor_);
         geoLines.SetLineWidth(line, 1.0f);
         geoLines.SetLineVisible(line, true);

         prev = next;
      }
   }
}

} // namespace

class HodographLayer::Impl
{
public:
   explicit Impl(HodographLayer*                       self,
                 const std::shared_ptr<gl::GlContext>& glContext) :
       self_ {self}, geoLines_ {std::make_shared<gl::draw::GeoLines>(glContext)}
   {
      QObject::connect(hodographManager_.get(),
                       &manager::HodographManager::HodographDataReady,
                       self_,
                       [this]() { ReloadHodographs(); });

      // Picking or dropping the gridded hodograph product in the RRFS list
      // starts or stops everything here: re-evaluate what is being drawn.
      QObject::connect(&manager::HodographSelection::Instance(),
                       &manager::HodographSelection::EnabledChanged,
                       self_,
                       [this]()
                       {
                          if (lastTier_.visible)
                          {
                             RebuildLines(lastTier_, builtBounds_);
                          }
                       });
   }
   ~Impl() { hodographManager_->SetDrawing(this, false); }

   Impl(const Impl&)            = delete;
   Impl& operator=(const Impl&) = delete;

   void ReloadHodographs();
   void RebuildLines(ZoomTier tier, const GeoBounds& bounds);
   bool AcquireFrames();

   HodographLayer* self_;

   std::shared_ptr<gl::draw::GeoLines>        geoLines_;
   std::shared_ptr<manager::HodographManager> hodographManager_ {
      manager::HodographManager::Instance()};

   // Cached by ReloadHodographs() (fires on HodographManager::
   // HodographDataReady) and consumed by RebuildLines() (also called
   // from Render() on a zoom-tier change) -- same reasoning as
   // WindBarbLayer's own dirFrame_/speedFrame_/gustFrame_ split from
   // RebuildIcons.
   // Shared with every other pane (see SharedFrames); only held while this
   // pane's tier is actually drawing hodographs.
   std::shared_ptr<const SharedFrames> frames_;

   // New data has arrived (or nothing has been parsed yet) since frames_
   // was last acquired -- checked lazily by RebuildLines() so nothing is
   // parsed for a pane that isn't drawing.
   bool framesStale_ {true};

   // {-1, 0.0, false} never matches a real computed tier, guaranteeing
   // the first Render() call rebuilds.
   ZoomTier lastTier_ {-1, 0.0, false};

   // The area geoLines_ was last built for (see VisibleBounds()); Render()
   // rebuilds when the view pans/zooms out of it. Unlimited until the first
   // real build.
   GeoBounds builtBounds_ {};
};

bool HodographLayer::Impl::AcquireFrames()
{
   // Cleared whether or not this succeeds, so a failed load isn't retried on
   // every Render() -- the next HodographDataReady() sets it again.
   framesStale_ = false;

   const auto generation = hodographManager_->DataGeneration();
   if (frames_ && frames_->generation == generation)
   {
      return true;
   }

   // Drop our own reference first: if this pane held the last one, the old
   // grids are freed before the new ones are parsed, not after.
   frames_.reset();

   // Held across the parse on purpose: a second pane arriving for the same
   // generation waits here and then reuses the result instead of parsing its
   // own copy.
   std::lock_guard lock(sharedFramesMutex_);

   if (auto shared = sharedFrames_.lock();
       shared && shared->generation == generation)
   {
      frames_ = std::move(shared);
      return true;
   }

   auto fresh = ParseAllFrames(generation);
   if (!fresh)
   {
      return false;
   }

   sharedFrames_ = fresh;
   frames_       = std::move(fresh);
   return true;
}

void HodographLayer::Impl::ReloadHodographs()
{
   logger_->debug("ReloadHodographs()");

   framesStale_ = true;

   // Nothing to parse for a pane that isn't currently drawing hodographs;
   // RebuildLines() picks the new data up when it next becomes visible.
   if (lastTier_.visible)
   {
      RebuildLines(lastTier_, builtBounds_);
   }
}

void HodographLayer::Impl::RebuildLines(ZoomTier tier, const GeoBounds& bounds)
{
   // Hodographs exist only while the user has picked them as a product (see
   // HodographSelection) and the view is zoomed in far enough to read them.
   const bool drawing = tier.visible && tier.stride >= 1 &&
                        manager::HodographSelection::Instance().IsEnabled();

   // Tells the manager whether anything needs its data: it only downloads
   // and decodes while some layer is actually drawing.
   hodographManager_->SetDrawing(this, drawing);

   if (drawing)
   {
      if (framesStale_ && !AcquireFrames())
      {
         return;
      }
      if (!frames_)
      {
         return;
      }
   }
   else
   {
      frames_.reset();
   }

   logger_->debug("RebuildLines(stride={}, metersPerMs={}, visible={}, "
                  "culled={})",
                  tier.stride,
                  tier.metersPerMs,
                  tier.visible,
                  bounds.limited);

   lastTier_    = tier;
   builtBounds_ = bounds;

   geoLines_->StartLines();

   if (drawing)
   {
      // Read once per rebuild, not per point -- same reasoning as
      // WindBarbLayer's own showGustBarbs read.
      const bool showRangeRings =
         settings::HodographSettings::Instance().show_range_rings().GetValue();

      const auto& levels  = manager::HodographManager::Levels();
      const auto& terrain = *frames_->terrain;

      // Only build what's on screen (plus margin): this grid covers all of
      // CONUS at every zoom, and each hodograph is a whole polyline (one
      // GeoLines entry per level, each with its own hover text). The
      // grid-index window bounding `bounds` limits which points are visited
      // at all; the per-point Contains() then trims its corners, since a
      // lat/lon rectangle isn't a rectangle in grid indices.
      const GridIndexBox window =
         bounds.limited ?
            LambertGridIndexBox(
               terrain.grid,
               terrain.grid_nx,
               terrain.grid_ny,
               LatLonBox {
                  bounds.south, bounds.west, bounds.north, bounds.east}) :
            GridIndexBox {0, terrain.grid_nx - 1, 0, terrain.grid_ny - 1};

      for (const auto& [i, j] : SelectHodographPoints(window, tier.stride))
      {
         const glm::dvec2 latLon = LambertGridToLatLon(
            terrain.grid, static_cast<double>(i), static_cast<double>(j));
         if (!bounds.Contains(latLon.x, latLon.y))
         {
            continue;
         }

         const std::optional<float> terrainValue = terrain.At(i, j);
         if (!terrainValue.has_value())
         {
            continue;
         }

         const float terrainM = *terrainValue;
         if (!std::isfinite(terrainM) ||
             static_cast<double>(terrainM) == terrain.missingValue)
         {
            continue;
         }

         // (east, north) offsets in metres for every valid (non-terrain-
         // masked) level at this point, in height order -- see
         // HodographManager::Level's own doc on why a heightAboveSea
         // level below terrain is real, not missing, and must be
         // skipped rather than decoded wrong.
         std::vector<std::pair<double, double>> offsets;
         std::vector<float>                     offsetHeights;
         offsets.reserve(levels.size());
         offsetHeights.reserve(levels.size());

         for (std::size_t lvl = 0; lvl < levels.size(); ++lvl)
         {
            const std::optional<float> uValue = frames_->u[lvl]->At(i, j);
            const std::optional<float> vValue = frames_->v[lvl]->At(i, j);
            if (!uValue.has_value() || !vValue.has_value())
            {
               continue;
            }

            const float u = *uValue;
            const float v = *vValue;

            // eccodes' own missingValue sentinel (a real, finite number,
            // e.g. RRFS's own convention -- *not* NaN, confirmed by
            // decoding a real heightAboveSea level directly: ~20% of a
            // real CONUS grid came back exactly equal to it, matching
            // decode_grib's own ComputeStp/ComputeVectorMagnitude
            // handling of the same sentinel) is the authoritative "this
            // cell has no data here" signal -- RRFS appears to already
            // mask a heightAboveSea level below its own model terrain
            // this way. The isfinite() checks are a defensive second
            // layer, and the explicit terrainM comparison below is a
            // third, independent physical check against the *public*
            // orog field (not necessarily identical to whatever internal
            // terrain RRFS masked against) -- keeping all three is cheap
            // and each catches a slightly different failure mode.
            if (!std::isfinite(u) || !std::isfinite(v) ||
                static_cast<double>(u) == frames_->u[lvl]->missingValue ||
                static_cast<double>(v) == frames_->v[lvl]->missingValue)
            {
               continue;
            }

            if (levels[lvl].aboveSea && levels[lvl].heightMeters < terrainM)
            {
               continue;
            }

            offsets.emplace_back(static_cast<double>(u) * tier.metersPerMs,
                                 static_cast<double>(v) * tier.metersPerMs);
            offsetHeights.push_back(levels[lvl].heightMeters);
         }

         if (offsets.size() < 2)
         {
            // Nothing meaningful to connect (e.g. every heightAboveSea
            // level masked below terrain, leaving only 0-1 AGL points).
            continue;
         }

         const common::Coordinate center {latLon.x, latLon.y};

         // Drawn first so the actual wind polyline renders on top of it,
         // not the other way around.
         if (showRangeRings)
         {
            DrawRangeRings(*geoLines_, center, tier.metersPerMs);
         }

         // Surface (first/lowest) level's own wind, for the hover text --
         // standard meteorological "direction FROM" convention: the
         // vector (u, v) points in the direction the wind blows *toward*
         // (bearing = atan2(east, north)); "from" is that plus 180
         // degrees.
         const double surfaceU = offsets[0].first / tier.metersPerMs;
         const double surfaceV = offsets[0].second / tier.metersPerMs;
         const double speedKnots =
            std::hypot(surfaceU, surfaceV) * kMetersPerSecondToKnots_;
         double towardDeg = std::atan2(surfaceU, surfaceV) * 180.0 / M_PI;
         double fromDeg   = std::fmod(towardDeg + 180.0 + 360.0, 360.0);

         const std::string hoverText = fmt::format(
            "Hodograph ({} levels)\n{:.0f} kt @ {:03.0f}° (10m)\n"
            "Valid: {}",
            offsets.size(),
            speedKnots,
            fromDeg,
            terrain.validTime);

         for (std::size_t seg = 0; seg + 1 < offsets.size(); ++seg)
         {
            const auto c1 = util::GeographicLib::GetCoordinate(
               center,
               units::meters<double> {offsets[seg].first},
               units::meters<double> {offsets[seg].second});
            const auto c2 = util::GeographicLib::GetCoordinate(
               center,
               units::meters<double> {offsets[seg + 1].first},
               units::meters<double> {offsets[seg + 1].second});

            auto line = geoLines_->AddLine();
            geoLines_->SetLineLocation(line,
                                       static_cast<float>(c1.latitude_),
                                       static_cast<float>(c1.longitude_),
                                       static_cast<float>(c2.latitude_),
                                       static_cast<float>(c2.longitude_));
            geoLines_->SetLineModulate(line,
                                       HeightBandColor(offsetHeights[seg + 1]));
            geoLines_->SetLineWidth(line, 2.0f);
            geoLines_->SetLineVisible(line, true);
            geoLines_->SetLineHoverText(line, hoverText);
         }
      }
   }

   geoLines_->FinishLines();
}

HodographLayer::HodographLayer(
   const std::shared_ptr<gl::GlContext>& glContext) :
    DrawLayer(glContext, "HodographLayer"),
    p(std::make_unique<Impl>(this, glContext))
{ AddDrawItem(p->geoLines_); }

HodographLayer::~HodographLayer() = default;

void HodographLayer::Initialize(const std::shared_ptr<MapContext>& mapContext)
{
   logger_->debug("Initialize()");
   DrawLayer::Initialize(mapContext);

   p->ReloadHodographs();
}

void HodographLayer::Render(
   const std::shared_ptr<MapContext>&            mapContext,
   const QMapLibre::CustomLayerRenderParameters& params)
{
   const ZoomTier baseTier = TierForZoom(params.zoom);

   // User-adjustable multiplier on top of the fixed per-tier base scale
   // (see HodographSettings::size_scale()) -- read every Render() call,
   // not cached, same reasoning as WindBarbLayer's own settings read.
   const double sizeScale =
      settings::HodographSettings::Instance().size_scale().GetValue();

   const ZoomTier tier {
      baseTier.stride, baseTier.metersPerMs * sizeScale, baseTier.visible};

   // Rebuilt for a new tier, or -- while actually drawing -- once the view
   // has panned/zoomed out of the area last built (larger than the view, see
   // kBuildMargin_, so ordinary panning doesn't rebuild every frame).
   if (!(tier == p->lastTier_) ||
       (tier.visible &&
        !p->builtBounds_.ContainsBounds(VisibleBounds(params, 1.0))))
   {
      p->RebuildLines(tier, VisibleBounds(params, kBuildMargin_));
   }

   DrawLayer::Render(mapContext, params);
}

bool HodographLayer::RunMousePicking(
   const std::shared_ptr<MapContext>&            mapContext,
   const QMapLibre::CustomLayerRenderParameters& params,
   const QPointF&                                mouseLocalPos,
   const QPointF&                                mouseGlobalPos,
   const glm::vec2&                              mouseCoords,
   const common::Coordinate&                     mouseGeoCoords,
   std::shared_ptr<types::EventHandler>&         eventHandler)
{
   // Shift-gated, matching the app-wide "Shift = show me the data"
   // convention (RadarProductLayer/GribProductLayer/WindBarbLayer) --
   // hodograph hover text is speed/direction/valid-time data, not a
   // label.
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

void HodographLayer::Deinitialize()
{
   logger_->debug("Deinitialize()");

   DrawLayer::Deinitialize();
}

} // namespace scwx::qt::map
