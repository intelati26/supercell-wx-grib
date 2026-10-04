#pragma once

#include <scwx/wsr88d/rda/generic_radar_data.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace scwx::qt::volume
{

// Data model and geometry for the 3D volume pane: every tilt of a radar volume
// as cells on its own beam cone, cropped to a square region, then turned into a
// triangle mesh. Nothing here touches OpenGL or Qt widgets, so it is unit
// tested on its own (radar_volume.test.cpp).
//
// Coordinates are kilometres. x is east and y is north of the region centre;
// z is height above mean sea level. Positions come from the standard 4/3 earth
// radius beam model, so a tilt is a cone that bends upward with range.

// Height of the beam centre above the radar antenna at slant range rangeKm.
double BeamHeightKm(double rangeKm, double elevationDeg);

// Distance along the ground from the radar to below the beam centre.
double GroundRangeKm(double rangeKm, double elevationDeg);

// East/north offset (km) of (latitude, longitude) from (originLatitude,
// originLongitude): a local equirectangular projection, accurate to well under
// a gate within a radar's 460 km range.
std::array<double, 2> LocalOffsetKm(double originLatitude,
                                    double originLongitude,
                                    double latitude,
                                    double longitude);

// The square the pane shows: centre as an offset from the radar (km) and half
// the side length.
struct VolumeRegion
{
   double centerXKm {};
   double centerYKm {};
   double halfSizeKm {};

   [[nodiscard]] bool Contains(double xKm, double yKm) const;
};

// The cell size the pane aims for. Gates and radials are combined per cell so
// a large region stays a manageable size: full resolution up to a 50 km
// half-size, coarser in steps beyond that. Each tilt's own gate spacing and
// radial width decide how many of its bins make up one cell.
struct CellSize
{
   double gateKm {};
   double azimuthDeg {};
};
CellSize ChooseCellSize(double halfSizeKm);

// One drawn bin: azimuth span (degrees clockwise from north, az1 > az0, may
// pass 360), slant range span and the value in the product's native units.
struct VolumeCell
{
   float az0Deg {};
   float az1Deg {};
   float r0Km {};
   float r1Km {};
   float value {};
};

struct VolumeTilt
{
   float                   elevationDeg {};
   std::vector<VolumeCell> cells {};
};

struct RadarVolume
{
   std::string                           siteId {};
   std::string                           product {};
   double                                siteLatitude {};
   double                                siteLongitude {};
   double                                siteHeightKm {};
   VolumeRegion                          region {};
   std::chrono::system_clock::time_point time {};
   std::uint16_t                         vcp {};
   std::vector<VolumeTilt>               tilts {};

   [[nodiscard]] std::size_t CellCount() const;
};

// Cells of one elevation scan whose centres fall inside region.
// Gates below the signal threshold and range-folded gates are dropped. When
// cells are combined, the one with the largest magnitude wins (|v| for signed
// products such as velocity), which keeps cores and couplets visible when
// zoomed out.
VolumeTilt ExtractTilt(const wsr88d::rda::ElevationScan& scan,
                       wsr88d::rda::DataBlockType        dataBlockType,
                       float                             elevationDeg,
                       const VolumeRegion&               region,
                       CellSize                          cellSize,
                       bool                              signedProduct);

// Distinct elevation angles in ascending order: a VCP repeats low tilts
// (split cuts, SAILS/MESO-SAILS), but each angle is drawn once.
std::vector<float> DistinctElevations(const std::vector<float>& elevations);

struct MeshVertex
{
   float                       x {};
   float                       y {};
   float                       z {};
   std::array<std::uint8_t, 4> rgba {};
};
// Tightly packed: the GL vertex layout relies on it.
static_assert(sizeof(MeshVertex) ==
              3 * sizeof(float) + sizeof(std::array<std::uint8_t, 4>));

struct VolumeMesh
{
   struct TiltRange
   {
      float         elevationDeg {};
      std::uint32_t firstIndex {};
      std::uint32_t indexCount {};
   };

   std::vector<MeshVertex>    vertices {};
   std::vector<std::uint32_t> indices {};
   // Per tilt, ascending elevation, so drawing tilts [0, n) is one call.
   std::vector<TiltRange> tilts {};
};

using ColorFunction = std::function<std::array<std::uint8_t, 4>(float value)>;

struct MeshOptions
{
   // Cells below this are left out (|value| for signed products).
   std::optional<float> threshold {};
   bool                 signedProduct {false};
};

// Two triangles per cell on the tilt's cone, coloured by color (alpha 0 =
// left out). Vertices are not shared between cells, so each keeps its own
// flat colour.
VolumeMesh BuildMesh(const RadarVolume&   volume,
                     const ColorFunction& color,
                     const MeshOptions&   options);

} // namespace scwx::qt::volume
