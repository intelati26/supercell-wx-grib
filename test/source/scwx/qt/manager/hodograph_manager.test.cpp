#include <scwx/qt/manager/hodograph_manager.hpp>

#include <chrono>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

namespace scwx
{
namespace qt
{
namespace manager
{

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

   const auto fixedCycle = sys_days {2026y / September / 25d} + 12h;
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
      std::ifstream in(uFramePath, std::ios::binary);
      if (in.is_open())
      {
         std::getline(in, header);
         if (header.find(R"("validTime":"2026-09-25T12:00:00Z")") !=
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

} // namespace manager
} // namespace qt
} // namespace scwx
