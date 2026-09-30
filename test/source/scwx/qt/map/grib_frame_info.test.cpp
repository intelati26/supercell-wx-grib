#include <scwx/qt/map/grib_frame_info.hpp>

#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace scwx::qt::map
{

namespace
{

// RTMA's 2.5km CONUS Lambert grid, as decode_grib writes it to a frame
// header (longitudes normalized to [-180, 180]).
const LambertGrid kRtmaGrid {-95.0,
                             25.0,
                             25.0,
                             25.0,
                             19.228976,
                             -126.276552,
                             2539.703,
                             2539.703,
                             6371200.0};
constexpr long    kRtmaNx = 2345;
constexpr long    kRtmaNy = 1597;

// What a 1920x1080 view centered on Oklahoma City at zoom 9 shows.
const LatLonBox kOklahomaCityView {34.229, -99.033, 36.692, -96.007};

} // namespace

TEST(GribFrameInfo, LambertGridIndexBoxCoversViewport)
{
   const LatLonBox    box = kOklahomaCityView;
   const GridIndexBox window =
      LambertGridIndexBox(kRtmaGrid, kRtmaNx, kRtmaNy, box);

   ASSERT_FALSE(window.IsEmpty());

   // A small window, not the whole grid.
   EXPECT_LT(window.iMax - window.iMin, kRtmaNx / 10);
   EXPECT_LT(window.jMax - window.jMin, kRtmaNy / 10);

   // Every point inside the box falls inside the window.
   const LambertConstants c = ComputeLambertConstants(kRtmaGrid);
   const glm::dvec2       origin =
      LambertForward(kRtmaGrid, c, kRtmaGrid.lat1, kRtmaGrid.lon1);
   constexpr int kSamples = 40;
   for (int s = 0; s <= kSamples; ++s)
   {
      for (int t = 0; t <= kSamples; ++t)
      {
         const double lat = box.south + (box.north - box.south) * s / kSamples;
         const double lon = box.west + (box.east - box.west) * t / kSamples;
         const glm::dvec2 xy = LambertForward(kRtmaGrid, c, lat, lon);
         const double     i  = (xy.x - origin.x) / kRtmaGrid.dx;
         const double     j  = (xy.y - origin.y) / kRtmaGrid.dy;

         EXPECT_GE(i, window.iMin);
         EXPECT_LE(i, window.iMax);
         EXPECT_GE(j, window.jMin);
         EXPECT_LE(j, window.jMax);
      }
   }
}

TEST(GribFrameInfo, LambertGridIndexBoxAcceptsZeroTo360Longitudes)
{
   LambertGrid grid = kRtmaGrid;
   grid.lov += 360.0;
   grid.lon1 += 360.0;

   const LatLonBox    box = kOklahomaCityView;
   const GridIndexBox normalized =
      LambertGridIndexBox(kRtmaGrid, kRtmaNx, kRtmaNy, box);
   const GridIndexBox shifted =
      LambertGridIndexBox(grid, kRtmaNx, kRtmaNy, box);

   EXPECT_EQ(shifted.iMin, normalized.iMin);
   EXPECT_EQ(shifted.iMax, normalized.iMax);
   EXPECT_EQ(shifted.jMin, normalized.jMin);
   EXPECT_EQ(shifted.jMax, normalized.jMax);
}

TEST(GribFrameInfo, LambertGridIndexBoxWholeAndEmpty)
{
   // A continental (wider than 90 degrees) view spans the whole grid.
   const GridIndexBox whole = LambertGridIndexBox(
      kRtmaGrid, kRtmaNx, kRtmaNy, LatLonBox {10.0, -150.0, 65.0, -45.0});
   EXPECT_EQ(whole.iMin, 0);
   EXPECT_EQ(whole.iMax, kRtmaNx - 1);
   EXPECT_EQ(whole.jMin, 0);
   EXPECT_EQ(whole.jMax, kRtmaNy - 1);

   // A view nowhere near CONUS misses it.
   EXPECT_TRUE(
      LambertGridIndexBox(
         kRtmaGrid, kRtmaNx, kRtmaNy, LatLonBox {50.5, -1.5, 52.5, 1.5})
         .IsEmpty());
}

TEST(GribFrameInfo, HalfFloatForReflectivity)
{
   // dBZ with MRMS's -999 "no coverage" sentinel below the cutoff.
   const std::vector<float> values {-999.0f, -20.0f, 12.5f, 47.3f, 75.0f};
   EXPECT_TRUE(FitsHalfFloatTexture(
      values.data(), values.size(), -30.0f, 95.0f, 0.0f, 9999.0));
}

TEST(GribFrameInfo, HalfFloatForTemperature)
{
   // Kelvin: ~300K rounds by at most 0.125K, under half a color step of
   // RTMA's 65K range (65 / 512 = ~0.127K). 9999 is eccodes' missing
   // marker -- drawn (it's above the cutoff), but its precision doesn't
   // matter.
   const std::vector<float> values {9999.0f, 255.3f, 288.9f, 318.2f};
   EXPECT_TRUE(FitsHalfFloatTexture(
      values.data(), values.size(), -999.0f, 65.0f, 0.0f, 9999.0));

   // The same 9999, if it weren't the frame's missing marker, is a real
   // drawn value that rounds too coarsely for this color range.
   EXPECT_FALSE(FitsHalfFloatTexture(
      values.data(), values.size(), -999.0f, 65.0f, 0.0f, -9999.0));
}

TEST(GribFrameInfo, NoHalfFloatBeyondRange)
{
   // Mean sea level pressure in Pa exceeds half precision's 65504 max.
   const std::vector<float> values {-999.0f, 98500.0f, 101325.0f};
   EXPECT_FALSE(FitsHalfFloatTexture(
      values.data(), values.size(), -999.0f, 10000.0f, 0.0f, 9999.0));

   // A drawn missing marker must still fit the range.
   const std::vector<float> marker {10.0f, 1.0e20f};
   EXPECT_FALSE(FitsHalfFloatTexture(
      marker.data(), marker.size(), 0.0f, 50.0f, 0.0f, 1.0e20));
}

TEST(GribFrameInfo, NoHalfFloatWhenRoundingIsVisible)
{
   // Values near 3000 round by up to 1, more than half a step of a
   // 100-wide color range (100 / 512 = ~0.2).
   const std::vector<float> values {2950.0f, 3010.0f};
   EXPECT_FALSE(FitsHalfFloatTexture(
      values.data(), values.size(), -999.0f, 100.0f, 0.0f, 9999.0));
}

TEST(GribFrameInfo, NoHalfFloatForContours)
{
   const std::vector<float> values {5400.0f, 5700.0f};
   EXPECT_FALSE(FitsHalfFloatTexture(
      values.data(), values.size(), -999.0f, 1200.0f, 60.0f, 9999.0));
}

TEST(GribFrameInfo, HalfFloatIgnoresUndrawnValues)
{
   // Huge or NaN values below the cutoff are never drawn.
   const std::vector<float> values {
      -1.0e9f, std::numeric_limits<float>::quiet_NaN(), 10.0f};
   EXPECT_TRUE(FitsHalfFloatTexture(
      values.data(), values.size(), 0.0f, 50.0f, 0.0f, 9999.0));

   // ...but an infinite value that would be drawn rules it out.
   const std::vector<float> infinite {10.0f,
                                      std::numeric_limits<float>::infinity()};
   EXPECT_FALSE(FitsHalfFloatTexture(
      infinite.data(), infinite.size(), 0.0f, 50.0f, 0.0f, 9999.0));
}

} // namespace scwx::qt::map
