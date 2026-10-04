#include <scwx/qt/volume/radar_volume.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace scwx::qt::volume
{

// 4/3 earth radius model for standard refraction.
static constexpr double kEarthRadiusKm_     = 6371.0;
static constexpr double kEffectiveRadiusKm_ = kEarthRadiusKm_ * 4.0 / 3.0;
static constexpr double kKmPerDegreeLatitude =
   kEarthRadiusKm_ * std::numbers::pi / 180.0;

// Raw data words below 2 carry no data; 1 is range folded.
static constexpr std::uint16_t kRangeFolded_         = 1u;
static constexpr std::uint16_t kMinSnrThreshold_     = 2u;
static constexpr std::uint8_t  kDataWordSize8_       = 8u;
static constexpr std::size_t   kSuperResRadials_     = 400u;
static constexpr float         kSuperResWidthDeg_    = 0.5f;
static constexpr float         kLegacyWidthDeg_      = 1.0f;
static constexpr double        kSameElevationDeg_    = 0.05;
static constexpr double        kFullDetailHalfKm_    = 50.0;
static constexpr double        kHalfDetailHalfKm_    = 100.0;
static constexpr double        kQuarterDetailHalfKm_ = 200.0;
static constexpr double        kHalfTurnDeg_         = 180.0;
static constexpr double        kHalfGate_            = 0.5;

static constexpr CellSize kFullDetail_ {.gateKm = 0.25, .azimuthDeg = 0.5};
static constexpr CellSize kHalfDetail_ {.gateKm = 0.5, .azimuthDeg = 0.5};
static constexpr CellSize kQuarterDetail_ {.gateKm = 1.0, .azimuthDeg = 1.0};
static constexpr CellSize kCoarseDetail_ {.gateKm = 2.0, .azimuthDeg = 1.0};

static double DegToRad(double deg)
{
   return deg * std::numbers::pi / kHalfTurnDeg_;
}

double BeamHeightKm(double rangeKm, double elevationDeg)
{
   // sqrt(r^2 + ka^2 + 2 r ka sin(el)) - ka, written as the hypotenuse of
   // the beam point's offsets along and across the antenna's vertical.
   const double ka        = kEffectiveRadiusKm_;
   const double elevation = DegToRad(elevationDeg);
   return std::hypot(rangeKm + ka * std::sin(elevation),
                     ka * std::cos(elevation)) -
          ka;
}

double GroundRangeKm(double rangeKm, double elevationDeg)
{
   const double ka = kEffectiveRadiusKm_;
   const double h  = BeamHeightKm(rangeKm, elevationDeg);
   return ka * std::asin(rangeKm * std::cos(DegToRad(elevationDeg)) / (ka + h));
}

std::array<double, 2> LocalOffsetKm(double originLatitude,
                                    double originLongitude,
                                    double latitude,
                                    double longitude)
{
   const double dLon = std::remainder(longitude - originLongitude, 360.0);
   return {dLon * kKmPerDegreeLatitude * std::cos(DegToRad(originLatitude)),
           (latitude - originLatitude) * kKmPerDegreeLatitude};
}

bool VolumeRegion::Contains(double xKm, double yKm) const
{
   return std::abs(xKm - centerXKm) <= halfSizeKm &&
          std::abs(yKm - centerYKm) <= halfSizeKm;
}

CellSize ChooseCellSize(double halfSizeKm)
{
   if (halfSizeKm <= kFullDetailHalfKm_)
   {
      return kFullDetail_;
   }
   if (halfSizeKm <= kHalfDetailHalfKm_)
   {
      return kHalfDetail_;
   }
   if (halfSizeKm <= kQuarterDetailHalfKm_)
   {
      return kQuarterDetail_;
   }
   return kCoarseDetail_;
}

std::size_t RadarVolume::CellCount() const
{
   std::size_t count = 0;
   for (const auto& tilt : tilts)
   {
      count += tilt.cells.size();
   }
   return count;
}

static std::size_t StrideFor(double target, double actual)
{
   if (actual <= 0.0)
   {
      return 1;
   }
   return std::max<std::size_t>(
      1, static_cast<std::size_t>(std::lround(target / actual)));
}

static std::uint16_t
RawAt(const void* moments, std::uint8_t wordSize, std::size_t gate)
{
   // Bounds are checked by the caller against the block's gate count.
   // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
   if (wordSize == kDataWordSize8_)
   {
      return static_cast<const std::uint8_t*>(moments)[gate];
   }
   return static_cast<const std::uint16_t*>(moments)[gate];
   // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

VolumeTilt ExtractTilt(const wsr88d::rda::ElevationScan& scan,
                       wsr88d::rda::DataBlockType        dataBlockType,
                       float                             elevationDeg,
                       const VolumeRegion&               region,
                       CellSize                          cellSize,
                       bool                              signedProduct)
{
   VolumeTilt tilt {.elevationDeg = elevationDeg, .cells = {}};
   if (scan.empty())
   {
      return tilt;
   }

   const std::size_t radialCount = std::size_t {scan.crbegin()->first} + 1u;
   const float       widthDeg =
      radialCount > kSuperResRadials_ ? kSuperResWidthDeg_ : kLegacyWidthDeg_;
   const std::size_t radialStride = StrideFor(cellSize.azimuthDeg, widthDeg);

   struct Best
   {
      bool  valid {false};
      float value {};
   };

   // Radials are grouped by index / radialStride; a group's cells are filled
   // from every radial in it, on the first radial's gate layout (the gate
   // layout is the same along every radial of a scan).
   auto it = scan.cbegin();
   while (it != scan.cend())
   {
      const std::size_t group    = it->first / radialStride;
      auto              groupEnd = it;
      while (groupEnd != scan.cend() && groupEnd->first / radialStride == group)
      {
         ++groupEnd;
      }

      const auto& firstRadial = it->second;
      if (firstRadial == nullptr)
      {
         it = groupEnd;
         continue;
      }
      const auto firstMoment = firstRadial->moment_data_block(dataBlockType);
      if (firstMoment == nullptr)
      {
         it = groupEnd;
         continue;
      }

      const double gateKm =
         firstMoment->data_moment_range_sample_interval().value();
      const double      firstGateKm = firstMoment->data_moment_range().value();
      const std::size_t gateStride  = StrideFor(cellSize.gateKm, gateKm);
      const std::size_t gateCount = firstMoment->number_of_data_moment_gates();
      if (gateKm <= 0.0 || gateCount == 0)
      {
         it = groupEnd;
         continue;
      }

      const float az0 = firstRadial->azimuth_angle().value() - widthDeg / 2.0f;
      const float az1 =
         az0 + widthDeg * static_cast<float>(std::distance(it, groupEnd));
      const double azCenter = DegToRad((az0 + az1) / 2.0);
      const double sinAz    = std::sin(azCenter);
      const double cosAz    = std::cos(azCenter);

      const std::size_t blockCount = (gateCount + gateStride - 1) / gateStride;
      std::vector<Best> best(blockCount);
      std::vector<bool> inside(blockCount);
      bool              anyInside = false;
      for (std::size_t b = 0; b < blockCount; ++b)
      {
         const double centerGate =
            static_cast<double>(b * gateStride) +
            (static_cast<double>(
                std::min(gateStride, gateCount - b * gateStride)) -
             1.0) /
               2.0;
         const double s =
            GroundRangeKm(firstGateKm + centerGate * gateKm, elevationDeg);
         inside[b] = region.Contains(s * sinAz, s * cosAz);
         anyInside = anyInside || inside[b];
      }
      if (!anyInside)
      {
         it = groupEnd;
         continue;
      }

      for (auto radial = it; radial != groupEnd; ++radial)
      {
         if (radial->second == nullptr)
         {
            continue;
         }
         const auto moment = radial->second->moment_data_block(dataBlockType);
         if (moment == nullptr || moment->data_moments() == nullptr ||
             moment->scale() == 0.0f)
         {
            continue;
         }

         const std::uint8_t  wordSize     = moment->data_word_size();
         const std::uint16_t snrThreshold = std::max<std::int16_t>(
            kMinSnrThreshold_, moment->snr_threshold_raw());
         const float       scale  = moment->scale();
         const float       offset = moment->offset();
         const std::size_t gates  = std::min<std::size_t>(
            gateCount, moment->number_of_data_moment_gates());

         for (std::size_t gate = 0; gate < gates; ++gate)
         {
            const std::size_t b = gate / gateStride;
            if (!inside[b])
            {
               continue;
            }
            const std::uint16_t raw =
               RawAt(moment->data_moments(), wordSize, gate);
            if (raw < snrThreshold || raw == kRangeFolded_)
            {
               continue;
            }
            const float value     = (static_cast<float>(raw) - offset) / scale;
            const float magnitude = signedProduct ? std::abs(value) : value;
            Best&       cell      = best[b];
            if (!cell.valid ||
                magnitude > (signedProduct ? std::abs(cell.value) : cell.value))
            {
               cell = {.valid = true, .value = value};
            }
         }
      }

      for (std::size_t b = 0; b < blockCount; ++b)
      {
         if (!best[b].valid)
         {
            continue;
         }
         const std::size_t firstGate = b * gateStride;
         const std::size_t lastGate =
            std::min(firstGate + gateStride, gateCount) - 1;
         tilt.cells.push_back(
            {.az0Deg = az0,
             .az1Deg = az1,
             .r0Km   = static_cast<float>(
                firstGateKm +
                (static_cast<double>(firstGate) - kHalfGate_) * gateKm),
             .r1Km = static_cast<float>(
                firstGateKm +
                (static_cast<double>(lastGate) + kHalfGate_) * gateKm),
             .value = best[b].value});
      }

      it = groupEnd;
   }

   tilt.cells.shrink_to_fit();
   return tilt;
}

std::vector<float> DistinctElevations(const std::vector<float>& elevations)
{
   std::vector<float> result {elevations};
   std::ranges::sort(result);
   const auto [first, last] = std::ranges::unique(
      result,
      [](float a, float b) { return std::abs(a - b) < kSameElevationDeg_; });
   result.erase(first, last);
   return result;
}

VolumeMesh BuildMesh(const RadarVolume&   volume,
                     const ColorFunction& color,
                     const MeshOptions&   options)
{
   VolumeMesh mesh {};

   std::vector<const VolumeTilt*> tilts {};
   tilts.reserve(volume.tilts.size());
   for (const auto& tilt : volume.tilts)
   {
      tilts.push_back(&tilt);
   }
   std::ranges::stable_sort(tilts,
                            [](const VolumeTilt* a, const VolumeTilt* b)
                            { return a->elevationDeg < b->elevationDeg; });

   const double cx = volume.region.centerXKm;
   const double cy = volume.region.centerYKm;
   const double z0 = volume.siteHeightKm;

   for (const VolumeTilt* tilt : tilts)
   {
      VolumeMesh::TiltRange range {
         .elevationDeg = tilt->elevationDeg,
         .firstIndex   = static_cast<std::uint32_t>(mesh.indices.size()),
         .indexCount   = 0};

      for (const VolumeCell& cell : tilt->cells)
      {
         if (options.threshold.has_value())
         {
            const float magnitude =
               options.signedProduct ? std::abs(cell.value) : cell.value;
            if (magnitude < *options.threshold)
            {
               continue;
            }
         }

         const std::array<std::uint8_t, 4> rgba = color(cell.value);
         if (rgba[3] == 0)
         {
            continue;
         }

         const double s0 = GroundRangeKm(cell.r0Km, tilt->elevationDeg);
         const double s1 = GroundRangeKm(cell.r1Km, tilt->elevationDeg);
         const auto   h0 = static_cast<float>(
            z0 + BeamHeightKm(cell.r0Km, tilt->elevationDeg));
         const auto h1 = static_cast<float>(
            z0 + BeamHeightKm(cell.r1Km, tilt->elevationDeg));
         const double sinStart = std::sin(DegToRad(cell.az0Deg));
         const double cosStart = std::cos(DegToRad(cell.az0Deg));
         const double sinEnd   = std::sin(DegToRad(cell.az1Deg));
         const double cosEnd   = std::cos(DegToRad(cell.az1Deg));

         const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
         auto       add  = [&](double s, double sinAz, double cosAz, float h)
         {
            mesh.vertices.push_back({.x    = static_cast<float>(s * sinAz - cx),
                                     .y    = static_cast<float>(s * cosAz - cy),
                                     .z    = h,
                                     .rgba = rgba});
         };
         add(s0, sinStart, cosStart, h0);
         add(s0, sinEnd, cosEnd, h0);
         add(s1, sinEnd, cosEnd, h1);
         add(s1, sinStart, cosStart, h1);

         mesh.indices.insert(
            mesh.indices.end(),
            {base, base + 1, base + 2, base, base + 2, base + 3});
      }

      range.indexCount =
         static_cast<std::uint32_t>(mesh.indices.size()) - range.firstIndex;
      mesh.tilts.push_back(range);
   }

   mesh.vertices.shrink_to_fit();
   mesh.indices.shrink_to_fit();
   return mesh;
}

} // namespace scwx::qt::volume
