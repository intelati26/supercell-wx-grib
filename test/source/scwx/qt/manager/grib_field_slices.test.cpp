#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace scwx::qt::manager
{

namespace fs = std::filesystem;

namespace
{

struct FrameStats
{
   bool   ok {false};
   double mean {0.0};
   double max {0.0};
};

// Reads a decoded frame (header line, then float32 values) -- opened and closed
// per call so Windows can replace the file between polls.
FrameStats ReadFrame(const std::string& path)
{
   FrameStats stats;

   std::ifstream in(path, std::ios::binary);
   if (!in)
   {
      return stats;
   }

   std::string header;
   std::getline(in, header);

   try
   {
      const auto nx = static_cast<long>(map::ExtractNumber(header, "nx"));
      const auto ny = static_cast<long>(map::ExtractNumber(header, "ny"));
      std::vector<float> values(static_cast<std::size_t>(nx * ny));
      in.read(reinterpret_cast<char*>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(float)));
      if (!in)
      {
         return stats;
      }

      double sum = 0.0;
      double max = -1.0e30;
      for (const float value : values)
      {
         sum += value;
         max = std::max(max, static_cast<double>(value));
      }
      stats.ok   = true;
      stats.mean = sum / static_cast<double>(values.size());
      stats.max  = max;
   }
   catch (const std::exception&)
   {
      stats.ok = false; // a frame still being written
   }

   return stats;
}

// A file under the download cache whose name holds every one of `parts`
fs::path FindCached(const std::vector<std::string>& parts)
{
   std::error_code ec;
   const fs::path  root = map::GetGribDataDirectory() / "cache";
   if (!fs::exists(root, ec))
   {
      return {};
   }

   for (const auto& entry : fs::recursive_directory_iterator(root, ec))
   {
      if (!entry.is_regular_file())
      {
         continue;
      }

      const std::string name = entry.path().generic_string();
      bool              all  = true;
      for (const auto& part : parts)
      {
         all = all && name.find(part) != std::string::npos;
      }
      if (all)
      {
         return entry.path();
      }
   }

   return {};
}

} // namespace

// Real S3 access. RRFS publishes one ~350MB object per forecast hour holding
// every field; a reflectivity layer reads a ~0.5MB message of it. Stepping the
// forecast hour must download only that message (found through the object's
// .idx), keep it as its own small cache entry, and still decode to a frame that
// changes from one hour to the next.
TEST(GribFieldSlicesTest, ReflectivityHoursDownloadOnlyTheirField)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;

   // A recent 3-hourly cycle (the plain, not ".subh.", file layout), old
   // enough that its hours 1 and 2 are published.
   auto cycle = floor<hours>(system_clock::now()) - 8h;
   cycle -=
      hours {(duration_cast<hours>(cycle.time_since_epoch()).count()) % 3};
   const auto cycleHour = duration_cast<hours>(cycle - floor<days>(cycle));

   const std::string productName = "Simulated Reflectivity (1km AGL)";

   auto gribManager = GribManager::Instance(map::GribCategory::Rrfs);

   const auto names = gribManager->ProductNames();
   const auto it    = std::ranges::find(names, productName);
   ASSERT_NE(it, names.end());
   const auto productIndex =
      static_cast<std::size_t>(std::distance(names.begin(), it));

   const std::string framePath =
      map::GetGribFramePath(map::GribCategory::Rrfs, productIndex);

   gribManager->SetProductActive(productName, true);
   gribManager->SetRrfsCycle(cycle);

   FrameStats previous;

   const auto stepTo = [&](int hour) -> FrameStats
   {
      gribManager->SetRrfsForecastHour(hour);

      for (int i = 0; i < 360; ++i) // up to 3 minutes
      {
         const FrameStats now = ReadFrame(framePath);
         if (now.ok && (!previous.ok || now.mean != previous.mean ||
                        now.max != previous.max))
         {
            return now;
         }
         std::this_thread::sleep_for(500ms);
      }
      return {};
   };

   const FrameStats first = stepTo(1);
   ASSERT_TRUE(first.ok) << "no frame for F001";
   previous = first;

   const FrameStats second = stepTo(2);
   EXPECT_TRUE(second.ok) << "the frame did not advance to F002";
   EXPECT_TRUE(second.ok &&
               (second.mean != first.mean || second.max != first.max));

   // What was downloaded for each hour: the one field, as its own cache entry,
   // not the whole object
   for (const int hour : {1, 2})
   {
      const fs::path slice = FindCached(
         {fmt::format("t{:02d}z.2dfld.3km.f{:03d}", cycleHour.count(), hour),
          ".fields-simulated-reflectivity-1km-agl"});
      ASSERT_FALSE(slice.empty()) << "no field slice cached for F00" << hour;

      std::error_code ec;
      const auto      size = fs::file_size(slice, ec);
      EXPECT_GT(size, 100'000u) << slice;
      EXPECT_LT(size, 5'000'000u)
         << slice << " is not a single field (the whole object is ~350MB)";
   }

   gribManager->SetProductActive(productName, false);
   gribManager->UseLatestRrfsCycle();
}

} // namespace scwx::qt::manager
