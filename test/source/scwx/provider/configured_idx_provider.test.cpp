#include <scwx/provider/configured_idx_provider.hpp>
#include <scwx/provider/nbm_data_provider.hpp>

#include <chrono>

#include <gtest/gtest.h>

namespace scwx::provider
{

using namespace std::chrono_literals;
using util::grib_model_config::SourceSpec;

using sys_days = std::chrono::sys_days;

static SourceSpec HrrrLike()
{
   SourceSpec s;
   s.bucket               = "noaa-hrrr-bdp-pds";
   s.keyPattern           = "hrrr.{yyyymmdd}/conus/hrrr.t{hh}z.wrfsfcf{fh2}.grib2";
   s.cycleHours           = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                             12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
   s.minForecastHour      = 0;
   s.maxForecastHour      = 18;
   s.availabilityLagHours = 1;
   return s;
}

static SourceSpec SixHourly()
{
   SourceSpec s;
   s.bucket               = "example-bucket";
   s.keyPattern           = "m.{yyyymmdd}/{hh}/f{fh3}.grib2";
   s.cycleHours           = {0, 6, 12, 18};
   s.minForecastHour      = 3;
   s.maxForecastHour      = 24;
   s.forecastHourStep     = 3;
   s.availabilityLagHours = 4;
   return s;
}

TEST(ConfiguredIdxProviderTest, BuildsKeysFromThePattern)
{
   const auto cycle = sys_days {std::chrono::year {2026} / 9 / 28} + 6h;

   EXPECT_EQ(ConfiguredIdxProvider::BuildKey(HrrrLike(), cycle, 7),
             "hrrr.20260928/conus/hrrr.t06z.wrfsfcf07.grib2");
   EXPECT_EQ(ConfiguredIdxProvider::BuildKey(SixHourly(), cycle, 12),
             "m.20260928/06/f012.grib2");
}

TEST(ConfiguredIdxProviderTest, SnapsHoursIntoTheRealRange)
{
   const auto s = SixHourly(); // 3..24 every 3

   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, -5), 3);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 0), 3);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 3), 3);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 4), 6);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 6), 6);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 22), 24);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(s, 99), 24);

   // A step that doesn't divide the range still never exceeds the maximum.
   auto odd             = s;
   odd.forecastHourStep = 7; // 3, 10, 17, 24
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(odd, 11), 17);
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(odd, 18), 24);
   odd.maxForecastHour = 20;
   EXPECT_EQ(ConfiguredIdxProvider::SnapHour(odd, 18), 20);

   // Keys are built for the snapped hour, never one that doesn't exist.
   const auto cycle = sys_days {std::chrono::year {2026} / 9 / 28} + 0h;
   EXPECT_EQ(ConfiguredIdxProvider::BuildKey(s, cycle, 4),
             "m.20260928/00/f006.grib2");
}

TEST(ConfiguredIdxProviderTest, LatestCycleIsNewestOneOldEnough)
{
   const auto day = sys_days {std::chrono::year {2026} / 9 / 28};

   // Hourly model, 1h lag: 14:30 -> the 13z cycle.
   EXPECT_EQ(ConfiguredIdxProvider::LatestCycle(HrrrLike(), day + 14h + 30min),
             day + 13h);

   // 6-hourly, 4h lag: at 09:59 the 06z cycle is only ~4h old (ok is >= 4h
   // after 06z = 10:00, so not yet) -> falls back to 00z.
   EXPECT_EQ(ConfiguredIdxProvider::LatestCycle(SixHourly(), day + 9h + 59min),
             day + 0h);
   EXPECT_EQ(ConfiguredIdxProvider::LatestCycle(SixHourly(), day + 10h),
             day + 6h);

   // Crosses midnight: at 02:00 the newest eligible cycle is yesterday's 18z.
   EXPECT_EQ(ConfiguredIdxProvider::LatestCycle(SixHourly(), day + 2h),
             day - 24h + 18h);
}

TEST(ConfiguredIdxProviderTest, ProviderTracksSelectionAndAnswersRules)
{
   ConfiguredIdxProvider provider {SixHourly()};

   EXPECT_TRUE(provider.IsUsingLatestCycle());
   EXPECT_EQ(provider.ForecastHour(), 3); // starts at the first real hour
   EXPECT_EQ(provider.MinForecastHourFor(), 3);
   EXPECT_EQ(provider.MaxForecastHourFor({}), 24);
   EXPECT_EQ(provider.SnapForecastHourFor({}, 5), 6);

   const auto day = sys_days {std::chrono::year {2026} / 9 / 28};
   EXPECT_TRUE(provider.RunsCycleAt(day + 6h));
   EXPECT_FALSE(provider.RunsCycleAt(day + 7h));
   EXPECT_TRUE(NbmDataProvider().RunsCycleAt(day + 7h));

   const auto cycle = sys_days {std::chrono::year {2026} / 9 / 28} + 12h;
   provider.SetCycle(cycle + 25min); // floored to the hour
   EXPECT_FALSE(provider.IsUsingLatestCycle());
   EXPECT_EQ(provider.CurrentCycle(), cycle);

   provider.SetForecastHour(9);
   EXPECT_EQ(provider.ForecastHour(), 9);
   EXPECT_EQ(provider.BuildKeyFor(provider.CurrentCycle(), 9),
             "m.20260928/12/f009.grib2");

   provider.UseLatestCycle();
   EXPECT_TRUE(provider.IsUsingLatestCycle());
}

// The hard-coded NBM provider carries real, live-verified rules. Expressing
// its plain hourly case (a short cycle: F001-F036, one file per hour) as a
// config must give the same keys and the same hour snapping -- the oracle for
// "an idx model is data, not code".
TEST(ConfiguredIdxProviderTest, ReproducesTheBuiltInNbmRulesForAShortCycle)
{
   SourceSpec nbm;
   nbm.bucket          = "noaa-nbm-grib2-pds";
   nbm.keyPattern      =
      "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2";
   nbm.cycleHours      = {1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 19,
                          20, 21, 22, 23};
   nbm.minForecastHour = 1;
   nbm.maxForecastHour = 36;

   const auto day = sys_days {std::chrono::year {2026} / 9 / 28};
   for (int cycleHour : {1, 9, 15, 23})
   {
      const auto cycle = day + std::chrono::hours {cycleHour};
      ASSERT_FALSE(NbmDataProvider::UsesExtendedRange(cycle));

      for (int hour = -2; hour <= 40; ++hour)
      {
         EXPECT_EQ(ConfiguredIdxProvider::SnapHour(nbm, hour),
                   NbmDataProvider::SnapForecastHour(cycle, hour))
            << "cycle " << cycleHour << "z hour " << hour;
         EXPECT_EQ(ConfiguredIdxProvider::BuildKey(nbm, cycle, hour),
                   NbmDataProvider::BuildKey(cycle, hour))
            << "cycle " << cycleHour << "z hour " << hour;
      }
   }
}

} // namespace scwx::provider
