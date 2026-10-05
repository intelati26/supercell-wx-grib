#include <scwx/provider/rrfs_data_provider.hpp>
#include <scwx/qt/manager/hodograph_manager.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

namespace scwx
{
namespace qt
{
namespace manager
{

// Reads a frame file's JSON header line, closing the file before returning:
// Windows cannot replace a file that is open, so a poll that kept it open
// across its sleep made the manager's every replace attempt fail ("Access is
// denied") and the frame never updated.
static bool ReadFrameHeader(const std::string& path, std::string& header)
{
   std::ifstream in(path, std::ios::binary);
   return in.is_open() && std::getline(in, header).good();
}

// Real S3/decode access below -- mirrors GribManagerTest's own
// RrfsForecastHourSelection test, and deliberately reuses the exact same
// known-real historical cycle (2026-09-25 12z, F000). Unlike that test's
// own assertions (which only read the *provider's* resolved state --
// never needing a download to finish), this one has to actually confirm
// a decoded frame's content, so it can't sidestep the async path the
// same way. HodographManager::SetCycle()/SetForecastHour() are thin
// forwards to the owned RrfsDataProvider (already proven correct by
// RrfsDataProvider's own tests) followed by a re-poll -- this test's own
// job is confirming that composition actually reaches a real,
// correctly-targeted decode, not re-proving the provider or decode_grib
// themselves.
TEST(HodographManagerTest, CycleForecastHourSelection)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   auto hodographManager = HodographManager::Instance();

   // The manager only polls while a layer reports it is drawing; stand in
   // for one, or SetCycle()/SetForecastHour() below would (correctly) do
   // nothing.
   hodographManager->SetDrawing(&hodographManager, true);

   // NOAA only keeps roughly a day of cycles: newest 6-hourly cycle at
   // least 8 hours old (fully published, still retained).
   const auto t = floor<hours>(system_clock::now() - hours {8});
   const auto fixedCycle =
      t - hours {duration_cast<hours>(t - floor<days>(t)).count() % 6};
   hodographManager->SetCycle(fixedCycle);
   hodographManager->SetForecastHour(0);

   // wxtest runs under its own application name, so it has its own,
   // separate download cache from the real app (confirmed live: neither
   // ~/.cache/wxtest nor ~/.local/share/wxtest had this file cached even
   // after this session's own extensive manual use of the real app) --
   // Poll() (invoked internally by both setters above) can't apply an
   // already-cached download synchronously here the way it might in a
   // warmer cache, so it posts the real ~320MB fetch + 35-field decode to
   // a background thread pool and returns immediately. Poll for the
   // result rather than assuming either timing; ~90s of headroom, well
   // above the ~52s a real cold run took while building this test.
   const std::string uFramePath = HodographManager::GetUFramePath(0);
   bool              found      = false;
   std::string       header;

   for (int i = 0; i < 180 && !found; ++i)
   {
      if (ReadFrameHeader(uFramePath, header))
      {
         if (header.find(std::format(R"("validTime":"{:%Y-%m-%dT%H:%M:%SZ}")",
                                     floor<seconds>(fixedCycle))) !=
             std::string::npos)
         {
            found = true;
            break;
         }
      }
      std::this_thread::sleep_for(500ms);
   }

   EXPECT_TRUE(found) << "Last header seen: " << header;
}

// Real S3 access. "Latest" is the newest cycle S3 really has, which is usually
// an hourly one -- and an hourly cycle has no F000, the hour a hodograph asks
// for by default. It must move to a published hour and load, not sit empty.
TEST(HodographManagerTest, LatestCycleHasEveryLevel)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;

   provider::RrfsDataProvider::ResetAvailabilityForTesting();

   auto      hodographManager = HodographManager::Instance();
   const int marker           = 0;

   // Drawing first: the manager only polls while a layer reports it is drawing
   // (and SetDrawing()'s own poll waits for an event loop this test does not
   // run), so the setters below -- which poll directly -- must come after.
   hodographManager->SetDrawing(&marker, true);
   hodographManager->SetForecastHour(0);
   hodographManager->UseLatestCycle();

   // A frame from a recent cycle (the pinned-cycle test above leaves one from
   // 8-14 hours ago in the same cache): the newest cycle is at most a few hours
   // old.
   using sys_days = time_point<system_clock, days>;

   const auto cutoff = system_clock::now() - hours {7};
   // The highest level: absent from an hourly cycle's file
   const std::string uFramePath =
      HodographManager::GetUFramePath(HodographManager::Levels().size() - 1);

   bool        found = false;
   std::string header;
   for (int i = 0; i < 240 && !found; ++i) // up to two minutes
   {
      if (ReadFrameHeader(uFramePath, header))
      {
         const auto at = header.find("\"validTime\":\"");
         int        y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
         if (at != std::string::npos && std::sscanf(header.c_str() + at + 13,
                                                    "%4d-%2d-%2dT%2d:%2d:%2d",
                                                    &y,
                                                    &mo,
                                                    &d,
                                                    &h,
                                                    &mi,
                                                    &sec) == 6)
         {
            const auto valid =
               sys_days {year {y} / month {static_cast<unsigned>(mo)} /
                         day {static_cast<unsigned>(d)}} +
               hours {h} + minutes {mi} + seconds {sec};
            found = valid > cutoff;
         }
      }
      if (!found)
      {
         std::this_thread::sleep_for(500ms);
      }
   }

   hodographManager->SetDrawing(&marker, false);
   provider::RrfsDataProvider::ResetAvailabilityForTesting();

   EXPECT_TRUE(found) << "No hodograph frame from a recent cycle; last header: "
                      << header;
}

} // namespace manager
} // namespace qt
} // namespace scwx
