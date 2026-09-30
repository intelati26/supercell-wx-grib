#include <scwx/qt/util/line_simplification.hpp>

#include <cmath>

#include <gtest/gtest.h>

namespace scwx::qt::util
{

struct Point
{
   double latitude_;
   double longitude_;
};

TEST(LineSimplificationTest, KeepsEverythingWithoutATolerance)
{
   const std::vector<Point> line {{35, -100}, {35.1, -99.9}, {35, -99.8}};
   EXPECT_EQ(SimplifyLine(line, 0.0).size(), 3u);
   EXPECT_EQ(SimplifyLine(std::vector<Point> {{35, -100}, {36, -99}}, 1.0).size(),
             2u);
   EXPECT_TRUE(SimplifyLine(std::vector<Point> {}, 1.0).empty());
}

TEST(LineSimplificationTest, DropsWigglesSmallerThanTheTolerance)
{
   // A straight west-east line with tiny north-south noise on every vertex.
   std::vector<Point> line;
   for (int i = 0; i <= 100; ++i)
   {
      line.push_back({35.0 + ((i % 2) ? 1e-5 : -1e-5), -100.0 + i * 0.01});
   }

   // 2px at zoom 4 is ~0.00024 world units (~0.09 degrees): far larger than
   // the noise, so only the endpoints should survive.
   const auto kept = SimplifyLine(line, SimplificationTolerance(4));
   EXPECT_EQ(kept.size(), 2u);
   EXPECT_EQ(kept.front(), 0u);
   EXPECT_EQ(kept.back(), 100u);
}

TEST(LineSimplificationTest, KeepsRealCorners)
{
   // An L-shape: the corner is a real feature and must survive.
   std::vector<Point> line;
   for (int i = 0; i <= 10; ++i)
   {
      line.push_back({35.0, -100.0 + i * 0.5});
   }
   for (int i = 1; i <= 10; ++i)
   {
      line.push_back({35.0 + i * 0.5, -95.0});
   }

   const auto kept = SimplifyLine(line, SimplificationTolerance(4));
   ASSERT_EQ(kept.size(), 3u);
   EXPECT_EQ(kept[1], 10u); // the corner
}

TEST(LineSimplificationTest, ClosedRingKeepsItsShape)
{
   // A square ring sampled densely; the four corners (plus the repeated
   // start/end) must remain so it still reads as a square.
   std::vector<Point> ring;
   for (int i = 0; i < 10; ++i) ring.push_back({35.0, -100.0 + i * 0.5});
   for (int i = 0; i < 10; ++i) ring.push_back({35.0 + i * 0.5, -95.0});
   for (int i = 0; i < 10; ++i) ring.push_back({40.0, -95.0 - i * 0.5});
   for (int i = 0; i < 10; ++i) ring.push_back({40.0 - i * 0.5, -100.0});
   ring.push_back(ring.front());

   const auto kept = SimplifyLine(ring, SimplificationTolerance(4));
   EXPECT_GE(kept.size(), 5u);
   EXPECT_LE(kept.size(), 6u);
   EXPECT_EQ(kept.front(), 0u);
   EXPECT_EQ(kept.back(), ring.size() - 1);
}

TEST(LineSimplificationTest, SpecksAreDroppedUntilYouZoomIn)
{
   // A ~0.05 degree loop (~5km): a few pixels at zoom 5, easily visible at 9.
   const std::vector<Point> loop {
      {35.00, -100.00}, {35.05, -100.00}, {35.05, -99.95}, {35.00, -99.95},
      {35.00, -100.00}};

   EXPECT_TRUE(IsNegligibleAtTier(loop, 5));
   EXPECT_FALSE(IsNegligibleAtTier(loop, 9));
   EXPECT_FALSE(IsNegligibleAtTier(loop, kFullDetailZoomTier));

   // A continent-sized line is never negligible at any tier.
   const std::vector<Point> big {{25, -125}, {49, -67}};
   EXPECT_FALSE(IsNegligibleAtTier(big, 0));
   EXPECT_FALSE(IsNegligibleAtTier(big, 5));
}

TEST(LineSimplificationTest, ToleranceShrinksWithZoomAndEndsAtFullDetail)
{
   EXPECT_GT(SimplificationTolerance(3), SimplificationTolerance(6));
   EXPECT_NEAR(SimplificationTolerance(5) / SimplificationTolerance(6), 2.0, 1e-9);
   EXPECT_EQ(SimplificationTolerance(kFullDetailZoomTier), 0.0);
   EXPECT_EQ(SimplificationTier(-3.0), 0);
   EXPECT_EQ(SimplificationTier(7.9), 7);
   EXPECT_EQ(SimplificationTier(20.0), kFullDetailZoomTier);
}

} // namespace scwx::qt::util
