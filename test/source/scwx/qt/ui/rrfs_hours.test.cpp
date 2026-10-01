#include <scwx/qt/ui/rrfs_hours.hpp>

#include <gtest/gtest.h>

namespace scwx::qt::ui::rrfs_hours
{

namespace
{

std::set<int> Hours(int first, int last)
{
   std::set<int> hours;
   for (int hour = first; hour <= last; ++hour)
   {
      hours.insert(hour);
   }
   return hours;
}

} // namespace

// Before S3 has been listed the picker covers the cycle's nominal horizon
TEST(RrfsHoursTest, UnknownFallsBackToTheNominalHorizon)
{
   const auto range = Covered(std::nullopt, 84);
   EXPECT_EQ(range.first, 0);
   EXPECT_EQ(range.last, 84);

   EXPECT_TRUE(Available(std::nullopt, 0));
   EXPECT_TRUE(Available(std::nullopt, 84));
}

// The reported case: an 84-hour cycle still being written (the 00z run at F035)
// offered 85 buttons, about 50 of them unusable
TEST(RrfsHoursTest, ARunningCycleCoversOnlyWhatIsPublished)
{
   const std::optional<std::set<int>> published = Hours(0, 35);

   const auto range = Covered(published, 84);
   EXPECT_EQ(range.first, 0);
   EXPECT_EQ(range.last, 35);

   EXPECT_TRUE(Available(published, 35));
   EXPECT_FALSE(Available(published, 36));
   EXPECT_FALSE(Available(published, 84));
}

// An hourly cycle has no F000: the picker starts at F001 and F000 is not
// offered
TEST(RrfsHoursTest, AnHourlyCycleStartsAtF001)
{
   const std::optional<std::set<int>> published = Hours(1, 18);

   const auto range = Covered(published, 18);
   EXPECT_EQ(range.first, 1);
   EXPECT_EQ(range.last, 18);

   EXPECT_FALSE(Available(published, 0));
   EXPECT_TRUE(Available(published, 1));
}

// A gap inside the range (an hour not there while later ones are) is covered by
// the range but not available
TEST(RrfsHoursTest, AGapIsInsideTheRangeButNotAvailable)
{
   std::set<int> hours = Hours(0, 10);
   hours.erase(4);
   const std::optional<std::set<int>> published = hours;

   const auto range = Covered(published, 18);
   EXPECT_EQ(range.first, 0);
   EXPECT_EQ(range.last, 10);
   EXPECT_FALSE(Available(published, 4));
   EXPECT_TRUE(Available(published, 5));
}

// Listed, but nothing in it yet: keep the nominal horizon (every button shows
// disabled, none is dropped)
TEST(RrfsHoursTest, ListedButEmptyKeepsTheNominalHorizon)
{
   const std::optional<std::set<int>> published = std::set<int> {};

   const auto range = Covered(published, 18);
   EXPECT_EQ(range.first, 0);
   EXPECT_EQ(range.last, 18);
   EXPECT_FALSE(Available(published, 3));
}

} // namespace scwx::qt::ui::rrfs_hours
