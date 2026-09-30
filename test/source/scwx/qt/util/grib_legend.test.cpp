#include <scwx/qt/util/grib_legend.hpp>

#include <gtest/gtest.h>

namespace scwx::qt::util::grib_legend
{

TEST(GribLegendLayoutTest, PanelsStackUpwardFromTheBottomLeft)
{
   const auto placements = LayoutPanels({1600, 1000}, 3);
   ASSERT_EQ(placements.size(), 3u);
   ASSERT_TRUE(placements[0] && placements[1] && placements[2]);

   // Same column, the list's last panel lowest, first panel highest
   EXPECT_EQ(placements[0]->barX, placements[2]->barX);
   EXPECT_LT(placements[0]->barY, placements[1]->barY);
   EXPECT_LT(placements[1]->barY, placements[2]->barY);
   // ...and the lowest sits just above the bottom margin, its value labels
   // (below the bar) still inside the image
   EXPECT_LT(placements[2]->barY, 1000 - 16 - 18 - 14);
   EXPECT_GT(placements[2]->barY, 900);
}

TEST(GribLegendLayoutTest, ContinuesInAnotherColumnWhenTheImageIsShort)
{
   // Room for about two panels per column
   const auto placements = LayoutPanels({1600, 220}, 3);
   ASSERT_EQ(placements.size(), 3u);
   ASSERT_TRUE(placements[0] && placements[1] && placements[2]);

   EXPECT_EQ(placements[1]->barX, placements[2]->barX);
   EXPECT_GT(placements[0]->barX, placements[1]->barX); // top-most wraps right
   EXPECT_EQ(placements[0]->barY, placements[2]->barY); // to the bottom row
}

TEST(GribLegendLayoutTest, PanelsThatFitNowhereGetNoPlacement)
{
   // One column wide, one panel tall
   const auto placements = LayoutPanels({400, 140}, 2);
   ASSERT_EQ(placements.size(), 2u);
   EXPECT_FALSE(placements[0].has_value()); // top-most is the one dropped
   EXPECT_TRUE(placements[1].has_value());

   // Too short for even one
   for (const auto& placement : LayoutPanels({1600, 40}, 2))
   {
      EXPECT_FALSE(placement.has_value());
   }
}

TEST(GribLegendLayoutTest, NothingToPlace)
{ EXPECT_TRUE(LayoutPanels({1600, 1000}, 0).empty()); }

TEST(GribLegendTest, NoSourcesLeavesTheImageAlone)
{
   QImage image(400, 300, QImage::Format_ARGB32);
   image.fill(Qt::blue);
   const QImage before = image.copy();

   DrawLegends(image, {});

   EXPECT_EQ(image, before);
}

} // namespace scwx::qt::util::grib_legend
