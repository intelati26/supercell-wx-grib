#include <scwx/qt/map/viewport_bounds.hpp>

#include <gtest/gtest.h>

namespace scwx::qt::map
{

static QMapLibre::CustomLayerRenderParameters
Params(double latitude, double longitude, double zoom)
{
   QMapLibre::CustomLayerRenderParameters params {};
   params.width     = 1000.0;
   params.height    = 800.0;
   params.latitude  = latitude;
   params.longitude = longitude;
   params.zoom      = zoom;
   return params;
}

TEST(ViewportBoundsTest, MatchesHandComputedMercatorExtent)
{
   // Half-diagonal is 640px; at zoom 7 the world is 512 * 128 px wide, so
   // the half extent is 640 * 360 / (512 * 128) = 3.516 degrees of longitude
   // and, because Mercator stretches latitude, about 2.75 degrees north and
   // 2.85 south of 37.2N (not symmetric). Expected values computed
   // independently of VisibleBounds().
   const auto bounds = VisibleBounds(Params(37.2, -93.4, 7.0), 1.0);

   EXPECT_TRUE(bounds.limited);
   EXPECT_NEAR(bounds.west, -96.916, 0.01);
   EXPECT_NEAR(bounds.east, -89.884, 0.01);
   EXPECT_NEAR(bounds.north, 39.949, 0.01);
   EXPECT_NEAR(bounds.south, 34.347, 0.01);
   EXPECT_TRUE(bounds.Contains(37.2, -93.4));
   EXPECT_FALSE(bounds.Contains(37.2, -80.0));
}

TEST(ViewportBoundsTest, ScaleGrowsAboutTheCenter)
{
   const auto view  = VisibleBounds(Params(37.2, -93.4, 7.0), 1.0);
   const auto built = VisibleBounds(Params(37.2, -93.4, 7.0), 2.0);

   EXPECT_TRUE(built.ContainsBounds(view));
   EXPECT_FALSE(view.ContainsBounds(built));
   EXPECT_NEAR(built.east - built.west, 2.0 * (view.east - view.west), 0.01);
}

TEST(ViewportBoundsTest, PanningPastTheMarginNeedsARebuild)
{
   const auto built = VisibleBounds(Params(37.2, -93.4, 7.0), 2.0);

   // A small pan stays inside the built area; a pan of more than half the
   // view's width does not.
   EXPECT_TRUE(
      built.ContainsBounds(VisibleBounds(Params(37.2, -92.0, 7.0), 1.0)));
   EXPECT_FALSE(
      built.ContainsBounds(VisibleBounds(Params(37.2, -88.0, 7.0), 1.0)));
}

TEST(ViewportBoundsTest, UnlimitedWhenCullingWouldBeWrong)
{
   auto tilted  = Params(37.2, -93.4, 7.0);
   tilted.pitch = 45.0;
   EXPECT_FALSE(VisibleBounds(tilted, 1.0).limited);

   auto empty  = Params(37.2, -93.4, 7.0);
   empty.width = 0.0;
   EXPECT_FALSE(VisibleBounds(empty, 1.0).limited);

   const GeoBounds everything {};
   EXPECT_TRUE(everything.Contains(-80.0, 170.0));
   EXPECT_TRUE(everything.ContainsBounds(VisibleBounds(Params(0, 0, 3), 1.0)));
   EXPECT_FALSE(VisibleBounds(Params(0, 0, 3), 1.0).ContainsBounds(everything));
}

} // namespace scwx::qt::map
