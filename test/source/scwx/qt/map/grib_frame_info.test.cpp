#include <scwx/qt/map/grib_frame_info.hpp>

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

} // namespace

TEST(GribFrameInfo, ViewportLatLonBoxCentersOnView)
{
   const LatLonBox box = ViewportLatLonBox(35.47, -97.52, 9.0, 1920, 1080, 1.0);

   EXPECT_LT(box.south, 35.47);
   EXPECT_GT(box.north, 35.47);
   EXPECT_LT(box.west, -97.52);
   EXPECT_GT(box.east, -97.52);

   // Longitude is linear in Web Mercator, so the box is symmetric in it.
   EXPECT_NEAR(-97.52 - box.west, box.east - -97.52, 1e-9);

   // A padded box contains the unpadded one.
   EXPECT_TRUE(
      ViewportLatLonBox(35.47, -97.52, 9.0, 1920, 1080, 2.0).Contains(box));
   EXPECT_FALSE(
      box.Contains(ViewportLatLonBox(35.47, -97.52, 9.0, 1920, 1080, 2.0)));
}

TEST(GribFrameInfo, ViewportLatLonBoxClampsToWorld)
{
   const LatLonBox box = ViewportLatLonBox(0.0, 0.0, 0.0, 4096, 4096, 1.0);

   EXPECT_GE(box.west, -180.0);
   EXPECT_LE(box.east, 180.0);
   EXPECT_GE(box.south, -85.06);
   EXPECT_LE(box.north, 85.06);
}

TEST(GribFrameInfo, LambertGridIndexBoxCoversViewport)
{
   const LatLonBox box = ViewportLatLonBox(35.47, -97.52, 9.0, 1920, 1080, 1.0);
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

   const LatLonBox box = ViewportLatLonBox(35.47, -97.52, 9.0, 1920, 1080, 1.0);
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
   // A continental view spans the whole grid.
   const GridIndexBox whole =
      LambertGridIndexBox(kRtmaGrid,
                          kRtmaNx,
                          kRtmaNy,
                          ViewportLatLonBox(39.0, -97.0, 3.0, 1920, 1080, 1.0));
   EXPECT_EQ(whole.iMin, 0);
   EXPECT_EQ(whole.iMax, kRtmaNx - 1);
   EXPECT_EQ(whole.jMin, 0);
   EXPECT_EQ(whole.jMax, kRtmaNy - 1);

   // A view nowhere near CONUS misses it.
   EXPECT_TRUE(
      LambertGridIndexBox(kRtmaGrid,
                          kRtmaNx,
                          kRtmaNy,
                          ViewportLatLonBox(51.5, 0.0, 9.0, 1920, 1080, 1.0))
         .IsEmpty());
}

} // namespace scwx::qt::map
