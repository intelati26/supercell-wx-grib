#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace scwx
{
namespace qt
{
namespace manager
{

// Real S3 access below (SetRrfsCycle()/SetRrfsForecastHour()/
// UseLatestRrfsCycle() all fetch immediately, see FetchRrfsSelection() in
// grib_manager.cpp) -- mirrors RrfsDataProvider's own FindKeyFixedCycle
// test, which this reuses the same known-real historical cycle from.
// GribManager::Instance() is a process-wide singleton keyed by category
// (see its own comment on why), so this is the one test file allowed to
// touch map::GribCategory::Rrfs through it.
TEST(GribManagerTest, RrfsForecastHourSelection)
{
   using namespace std::chrono;
   using sys_days = time_point<system_clock, days>;

   auto gribManager = GribManager::Instance(map::GribCategory::Rrfs);

   // Rrfs starts with no product active (see GribManager's own
   // per-category default) -- CurrentRrfsCycle()/MaxRrfsForecastHour()
   // below need a real active product to resolve against, so activate
   // one explicitly rather than relying on a default that no longer
   // exists. Which product doesn't matter here (unlike
   // PrslevProductDecodesRealFile/ShipProductDecodesRealFile below, this
   // test never checks a decoded value), so the first one is fine.
   gribManager->SetProductActive(gribManager->ProductNames().front(), true);

   // Defaults, before any selection has been made.
   EXPECT_TRUE(gribManager->IsUsingLatestRrfsCycle());
   EXPECT_EQ(gribManager->RrfsForecastHour(), 0);

   // Forecast-hour selection alone doesn't fix the cycle -- "latest cycle,
   // hour 5" is a valid, still-auto-tracking combination (see
   // SetRrfsForecastHour()'s own doc in grib_manager.hpp).
   gribManager->SetRrfsForecastHour(5);
   EXPECT_EQ(gribManager->RrfsForecastHour(), 5);
   EXPECT_TRUE(gribManager->IsUsingLatestRrfsCycle());

   // A fixed cycle -- same known-real historical cycle
   // RrfsDataProvider.FindKeyFixedCycle uses, confirmed live while this
   // feature was built. 12z is a 6-hourly cycle, so 84h.
   const auto fixedCycle = sys_days {2026y / September / 25d} + 12h;
   gribManager->SetRrfsCycle(fixedCycle);
   EXPECT_FALSE(gribManager->IsUsingLatestRrfsCycle());
   EXPECT_EQ(gribManager->CurrentRrfsCycle(), fixedCycle);
   EXPECT_EQ(gribManager->MaxRrfsForecastHour(), 84);
   // The forecast-hour selection from above persists across a cycle
   // change -- SetRrfsCycle() doesn't reset it.
   EXPECT_EQ(gribManager->RrfsForecastHour(), 5);

   // A 3-hourly (non-6-hourly) cycle -- 18h.
   gribManager->SetRrfsCycle(sys_days {2026y / September / 25d} + 15h);
   EXPECT_EQ(gribManager->MaxRrfsForecastHour(), 18);

   gribManager->UseLatestRrfsCycle();
   EXPECT_TRUE(gribManager->IsUsingLatestRrfsCycle());

   gribManager->SetProductActive(gribManager->ProductNames().front(), false);
}

// Real S3 access + a real ~580MB fetch and decode below -- confirms the
// prslev file-family axis (see provider::RrfsFileFamily) actually
// reaches a correctly-targeted real decode end to end, not just that it
// compiles. Same known-real historical cycle/hour as
// RrfsForecastHourSelection above (and RrfsDataProvider's own
// FindKeyFixedCyclePrslev, confirmed live while building this feature).
TEST(GribManagerTest, PrslevProductDecodesRealFile)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   auto gribManager = GribManager::Instance(map::GribCategory::Rrfs);

   const auto names = gribManager->ProductNames();
   const auto it    = std::find(names.begin(), names.end(), "500mb Height");
   ASSERT_NE(it, names.end());
   const std::size_t productIndex =
      static_cast<std::size_t>(std::distance(names.begin(), it));

   gribManager->SetProductActive("500mb Height", true);
   gribManager->SetRrfsCycle(sys_days {2026y / September / 25d} + 12h);
   gribManager->SetRrfsForecastHour(3);

   // wxtest has its own, separate download cache from the real app (see
   // HodographManagerTest's own comment on this) -- a real cold ~580MB
   // fetch (prslev is larger than 2dfld's own ~320MB) plus decode runs on
   // a background thread; poll for the result rather than assuming
   // timing. ~150s of headroom, scaled up from HodographManagerTest's
   // own ~90s/320MB budget for this file's larger size.
   //
   // Checks the *decoded field's own mean*, not the frame header's
   // "validTime" -- a real trap found while building this test:
   // decode_grib's ProductInfoFromHandle reports a GRIB2 message's own
   // dataDate/dataTime, which for a forecast field is the *cycle's*
   // reference time, not (reference + forecast hour) -- so F000 and F003
   // of the same cycle report the identical "validTime" regardless, and
   // a validTime-based check can never actually distinguish them (it
   // passed once against a genuinely-stale F000 frame purely because
   // both said the same timestamp). 500mb height's own real mean
   // *does* differ meaningfully between these two specific hours
   // (confirmed live: 5834.78 at F000, 5837.13 at F003, on this exact
   // historical cycle) -- tight enough tolerance to tell them apart, but
   // real model data, not a synthetic marker value.
   const std::string framePath =
      map::GetGribFramePath(map::GribCategory::Rrfs, productIndex);
   constexpr double   kExpectedF3Mean = 5837.13;
   constexpr double   kTolerance      = 0.5;
   bool               found           = false;
   double             lastMean        = 0.0;
   std::vector<float> payload;

   for (int i = 0; i < 900 && !found; ++i)
   {
      std::ifstream in(framePath, std::ios::binary);
      if (in.is_open())
      {
         std::string header;
         std::getline(in, header);

         payload.assign(1905141, 0.0f);
         in.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size() * sizeof(float)));

         if (in.good() || in.eof())
         {
            const double sum =
               std::accumulate(payload.begin(), payload.end(), 0.0);
            lastMean = sum / static_cast<double>(payload.size());

            if (std::abs(lastMean - kExpectedF3Mean) < kTolerance)
            {
               found = true;
               break;
            }
         }
      }
      // Windows refuses to rename a new frame over a file still open here.
      in.close();
      std::this_thread::sleep_for(500ms);
   }

   EXPECT_TRUE(found) << "Last decoded mean seen: " << lastMean
                      << " (expected ~" << kExpectedF3Mean << ")";

   // Leave the shared singleton no worse than found, same discipline
   // RrfsForecastHourSelection's own final UseLatestRrfsCycle() call
   // uses -- this is the one test file allowed to touch
   // GribCategory::Rrfs through it (see that test's own comment), so
   // later tests in this binary shouldn't see this product still active.
   gribManager->SetProductActive("500mb Height", false);
   gribManager->UseLatestRrfsCycle();
}

// Confirms SHIP's own two-file dispatch (FetchShipSelection()/
// QueueShipInput()/ApplyShipIfReady()/ApplyShipDownload(), see their own
// docs) end to end against real data, not just that decode_grib's
// `--derived ship` mode compiles. Same known-real historical cycle/hour
// as PrslevProductDecodesRealFile above -- deliberately reuses it so
// both of SHIP's inputs (2dfld and prslev) are typically already cached
// from that test's own run, making this one a fast synchronous decode
// rather than two fresh multi-hundred-MB downloads; either way, the
// poll loop below doesn't assume which.
TEST(GribManagerTest, ShipProductDecodesRealFile)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   auto gribManager = GribManager::Instance(map::GribCategory::Rrfs);

   const auto names = gribManager->ProductNames();
   const auto it    = std::find(names.begin(), names.end(), "SHIP");
   ASSERT_NE(it, names.end());
   const std::size_t productIndex =
      static_cast<std::size_t>(std::distance(names.begin(), it));

   gribManager->SetProductActive("SHIP", true);
   gribManager->SetRrfsCycle(sys_days {2026y / September / 25d} + 12h);
   gribManager->SetRrfsForecastHour(3);

   // Checks the decoded field's own mean, not header metadata -- same
   // lesson as PrslevProductDecodesRealFile's own comment on
   // decode_grib's validTime not being usable to distinguish forecast
   // hours. SHIP's real mean at this exact cycle/hour (verified live via
   // a direct decode_grib --derived ship CLI run while building this
   // feature): 0.019086.
   const std::string framePath =
      map::GetGribFramePath(map::GribCategory::Rrfs, productIndex);
   constexpr double   kExpectedMean = 0.019086;
   constexpr double   kTolerance    = 0.001;
   bool               found         = false;
   double             lastMean      = 0.0;
   std::vector<float> payload;

   for (int i = 0; i < 900 && !found; ++i)
   {
      std::ifstream in(framePath, std::ios::binary);
      if (in.is_open())
      {
         std::string header;
         std::getline(in, header);

         payload.assign(1905141, 0.0f);
         in.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size() * sizeof(float)));

         if (in.good() || in.eof())
         {
            const double sum =
               std::accumulate(payload.begin(), payload.end(), 0.0);
            lastMean = sum / static_cast<double>(payload.size());

            if (std::abs(lastMean - kExpectedMean) < kTolerance)
            {
               found = true;
               break;
            }
         }
      }
      // Windows refuses to rename a new frame over a file still open here.
      in.close();
      std::this_thread::sleep_for(500ms);
   }

   EXPECT_TRUE(found) << "Last decoded mean seen: " << lastMean
                      << " (expected ~" << kExpectedMean << ")";

   gribManager->SetProductActive("SHIP", false);
   gribManager->UseLatestRrfsCycle();
}

// Rrfs and Nbm (and any category added later) start with no product
// active; Mrms and Rtma keep an always-at-least-one-active invariant. No
// network access: nothing is fetched until a product is activated.
TEST(GribManagerTest, ProductsActiveByDefault)
{
   for (auto category : {map::GribCategory::Rrfs, map::GribCategory::Nbm})
   {
      auto gribManager = GribManager::Instance(category);
      EXPECT_TRUE(gribManager->ActiveProductNames().empty());
      EXPECT_FALSE(gribManager->CurrentProductIndex().has_value());
      EXPECT_EQ(gribManager->CurrentProductName(), "");
      EXPECT_EQ(gribManager->MaxRrfsForecastHour(), 0);
      EXPECT_EQ(gribManager->MaxIdxForecastHour(), 0);
   }

   for (auto category : {map::GribCategory::Mrms, map::GribCategory::Rtma})
   {
      auto gribManager = GribManager::Instance(category);
      ASSERT_EQ(gribManager->ActiveProductNames().size(), 1u);
      EXPECT_TRUE(gribManager->CurrentProductIndex().has_value());

      // The last active product can't be deactivated.
      gribManager->SetProductActive(gribManager->ActiveProductNames().front(),
                                    false);
      EXPECT_EQ(gribManager->ActiveProductNames().size(), 1u);
   }
}

// The download cache's size budget: 40GB at most, shrunk so the disk keeps
// max(5GB, 10%) free, never below a 2GB floor. Pure arithmetic, no I/O.
TEST(GribManagerTest, DownloadCacheBudget)
{
   constexpr std::uintmax_t kGB = 1024ULL * 1024 * 1024;

   // Plenty of room: the 40GB ceiling.
   EXPECT_EQ(GribManager::DownloadCacheBudgetBytes(0, 500 * kGB, 1000 * kGB),
             40 * kGB);

   // 1TB disk, 110GB free, 10GB of cache: reserve is 100GB (10%), so the
   // cache may reach 10 + 110 - 100 = 20GB.
   EXPECT_EQ(
      GribManager::DownloadCacheBudgetBytes(10 * kGB, 110 * kGB, 1000 * kGB),
      20 * kGB);

   // Small disk: 5GB minimum reserve beats 10% of 20GB.
   EXPECT_EQ(GribManager::DownloadCacheBudgetBytes(1 * kGB, 12 * kGB, 20 * kGB),
             8 * kGB);

   // Nearly full: the 2GB floor.
   EXPECT_EQ(
      GribManager::DownloadCacheBudgetBytes(1 * kGB, 1 * kGB, 1000 * kGB),
      2 * kGB);
}

// Eviction goes least recently used first, stops once under budget, and
// never touches a file used within minAge -- the case that stranded a SHIP
// decode on a nearly-full CI disk: its already-cached input was the oldest
// file in the cache and was evicted while the other input downloaded.
TEST(GribManagerTest, DownloadCacheEvictions)
{
   using namespace std::chrono_literals;
   constexpr std::uintmax_t kMB = 1024ULL * 1024;

   const std::vector<GribManager::CachedFile> files {
      {100 * kMB, 3h},   // 0: old
      {300 * kMB, 5min}, // 1: in use (e.g. a SHIP input)
      {200 * kMB, 2h},   // 2: old
      {50 * kMB, 1h},    // 3: newer
   };

   // 650MB against a 400MB budget: the two oldest (0, then 2) are enough.
   EXPECT_EQ(
      GribManager::DownloadCacheEvictions(files, 650 * kMB, 400 * kMB, 15min),
      (std::vector<std::size_t> {0, 2}));

   // Against 100MB: every old file goes, but the in-use one stays even
   // though that leaves the cache over budget.
   EXPECT_EQ(
      GribManager::DownloadCacheEvictions(files, 650 * kMB, 100 * kMB, 15min),
      (std::vector<std::size_t> {0, 2, 3}));

   // Already under budget: nothing.
   EXPECT_TRUE(
      GribManager::DownloadCacheEvictions(files, 650 * kMB, 1000 * kMB, 15min)
         .empty());
}

// SetRrfsCycle()/SetRrfsForecastHour()/UseLatestRrfsCycle() are refused
// (logged, not crashing) against a non-Rrfs instance -- Mrms/Rtma have no
// forecast-hour axis at all (see the class comment on these methods).
TEST(GribManagerTest, RrfsSelectionNoOpForOtherCategories)
{
   auto gribManager = GribManager::Instance(map::GribCategory::Mrms);

   EXPECT_TRUE(gribManager->IsUsingLatestRrfsCycle());
   EXPECT_EQ(gribManager->RrfsForecastHour(), 0);
   EXPECT_EQ(gribManager->MaxRrfsForecastHour(), 0);
   EXPECT_EQ(gribManager->CurrentRrfsCycle(),
             std::chrono::system_clock::time_point {});

   gribManager->SetRrfsForecastHour(5);
   EXPECT_EQ(gribManager->RrfsForecastHour(), 0);
}

// Real S3 access below (SetIdxCycle()/SetIdxForecastHour()/
// UseLatestIdxCycle() all fetch immediately, see FetchIdxSelection() in
// grib_manager.cpp) -- same reasoning and category-singleton caveat as
// RrfsForecastHourSelection above, but for map::GribCategory::Nbm. Uses a
// non-6-hourly-multiple forecast hour (70) specifically to exercise the
// snap-to-valid-hour behavior SetIdxForecastHour() has that
// SetRrfsForecastHour() doesn't need (RRFS's own step is uniform; NBM's
// isn't beyond F069 for an extended cycle -- see
// NbmDataProvider::SnapForecastHour()'s own doc).
TEST(GribManagerTest, NbmForecastHourSelection)
{
   using namespace std::chrono;
   using sys_days = time_point<system_clock, days>;

   auto gribManager = GribManager::Instance(map::GribCategory::Nbm);

   // Nbm starts with no product active -- CurrentIdxCycle()/
   // MaxIdxForecastHour() below need one to resolve against. Which one
   // doesn't matter (no decoded value is checked here).
   gribManager->SetProductActive(gribManager->ProductNames().front(), true);

   EXPECT_TRUE(gribManager->IsUsingLatestIdxCycle());
   // 1, not 0 -- NBM has no F000 file at all (see NbmDataProvider's own
   // kMinForecastHour_ comment).
   EXPECT_EQ(gribManager->IdxForecastHour(), 1);

   // A fixed, extended (6-hourly) cycle -- 264h max.
   const auto fixedCycle = sys_days {2026y / September / 25d} + 12h;
   gribManager->SetIdxCycle(fixedCycle);
   EXPECT_FALSE(gribManager->IsUsingLatestIdxCycle());
   EXPECT_EQ(gribManager->CurrentIdxCycle(), fixedCycle);
   EXPECT_EQ(gribManager->MaxIdxForecastHour(), 264);

   // F070 doesn't exist for an extended cycle (confirmed live 2026-09-26
   // -- see NbmDataProvider's own class comment) -- snapped up to the
   // next real hour, F072, rather than stored as asked.
   gribManager->SetIdxForecastHour(70);
   EXPECT_EQ(gribManager->IdxForecastHour(), 72);

   // A short (non-extended) cycle -- 36h max, uniformly hourly throughout,
   // so no snapping needed for an in-range hour.
   gribManager->SetIdxCycle(sys_days {2026y / September / 25d} + 9h);
   EXPECT_EQ(gribManager->MaxIdxForecastHour(), 36);
   gribManager->SetIdxForecastHour(20);
   EXPECT_EQ(gribManager->IdxForecastHour(), 20);

   gribManager->UseLatestIdxCycle();
   EXPECT_TRUE(gribManager->IsUsingLatestIdxCycle());

   gribManager->SetProductActive(gribManager->ProductNames().front(), false);
}

// Confirms the whole Nbm chain -- FetchIdxSelectionForProduct()/
// QueueIdxDownload()/NbmDataProvider::FetchField() (the idx-based range
// fetch) -- reaches a correctly-targeted real decode end to end, not just
// that it compiles. Real S3 access: a small (~1-2MB) range fetch plus
// decode, so this runs far faster than the whole-file RRFS tests above.
TEST(GribManagerTest, NbmProductDecodesRealFile)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   auto gribManager = GribManager::Instance(map::GribCategory::Nbm);

   const auto names = gribManager->ProductNames();
   const auto it = std::find(names.begin(), names.end(), "2m Temperature");
   ASSERT_NE(it, names.end());
   const std::size_t productIndex =
      static_cast<std::size_t>(std::distance(names.begin(), it));

   gribManager->SetProductActive("2m Temperature", true);
   gribManager->SetIdxCycle(sys_days {2026y / September / 25d} + 12h);
   gribManager->SetIdxForecastHour(24);

   // Checks the decoded field's own mean, not header metadata -- same
   // lesson as PrslevProductDecodesRealFile's own comment on
   // decode_grib's validTime not distinguishing forecast hours. Real
   // mean confirmed live via a direct decode_grib CLI run against this
   // exact cycle/hour's real 2t field: 288.995 K, grid 2345x1597
   // (3744965 cells) -- NBM's own CONUS grid, a different size from
   // RRFS's (1905141).
   const std::string framePath =
      map::GetGribFramePath(map::GribCategory::Nbm, productIndex);
   constexpr double   kExpectedMean = 288.995;
   constexpr double   kTolerance    = 0.5;
   bool                found        = false;
   double              lastMean     = 0.0;
   std::vector<float>  payload;

   for (int i = 0; i < 60 && !found; ++i)
   {
      std::ifstream in(framePath, std::ios::binary);
      if (in.is_open())
      {
         std::string header;
         std::getline(in, header);

         payload.assign(3744965, 0.0f);
         in.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size() * sizeof(float)));

         if (in.good() || in.eof())
         {
            const double sum =
               std::accumulate(payload.begin(), payload.end(), 0.0);
            lastMean = sum / static_cast<double>(payload.size());

            if (std::abs(lastMean - kExpectedMean) < kTolerance)
            {
               found = true;
               break;
            }
         }
      }
      // Windows refuses to rename a new frame over a file still open here.
      in.close();
      std::this_thread::sleep_for(500ms);
   }

   EXPECT_TRUE(found) << "Last decoded mean seen: " << lastMean
                      << " (expected ~" << kExpectedMean << ")";

   gribManager->SetProductActive("2m Temperature", false);
   gribManager->UseLatestIdxCycle();
}

} // namespace manager
} // namespace qt
} // namespace scwx
