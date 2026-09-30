#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/user_model_registry.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>

#include <gtest/gtest.h>

namespace scwx::qt::manager
{

namespace fs = std::filesystem;

namespace
{

constexpr const char* kNbmCloneJson = R"({
  "model":  {"name": "Test NBM Clone"},
  "source": {
    "bucket": "noaa-nbm-grib2-pds",
    "key_pattern": "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2",
    "cycle_hours": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23],
    "min_forecast_hour": 1,
    "max_forecast_hour": 36
  },
  "defaults": {"units": "K", "quantity": "temperature_kelvin",
               "color_offset": 250, "color_scale": 70}
})";

constexpr const char* kNbmCloneCsv =
   "name,parameter,level,short_name\n"
   "2m Temperature,TMP,2 m above ground,2t\n"
   "2m Dewpoint,DPT,2 m above ground,2d\n";

constexpr const char* kSixHourlyJson = R"({
  "model":  {"name": "Aardvark Six-Hourly"},
  "source": {"bucket": "example-bucket",
             "key_pattern": "m.{yyyymmdd}/{hh}/f{fh3}.grib2",
             "cycle_hours": [0, 6, 12, 18], "max_forecast_hour": 48}
})";

constexpr const char* kSixHourlyCsv =
   "name,parameter,level,short_name,type,contour_interval\n"
   "Pressure,PRMSL,mean sea level,prmsl,contour,400\n";

void Write(const fs::path& path, const std::string& text)
{
   fs::create_directories(path.parent_path());
   std::ofstream(path, std::ios::binary) << text;
}

class UserModelGribManagerTest : public testing::Test
{
protected:
   void SetUp() override
   {
      root_ = fs::temp_directory_path() /
              ("scwx-user-gm-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::remove_all(root_);
      fs::create_directories(root_);
      UserModelRegistry::SetModelsDirectoryForTesting(root_ / "grib-models");
   }

   void TearDown() override
   {
      UserModelRegistry::SetModelsDirectoryForTesting({});
      UserModelRegistry::Instance()->Reload();
      GribManager::Instance(map::GribCategory::User)->ReloadUserModel();
      fs::remove_all(root_);
   }

   void InstallModels(bool nbmClone, bool sixHourly)
   {
      if (nbmClone)
      {
         Write(root_ / "grib-models" / "nbm" / "model.json", kNbmCloneJson);
         Write(root_ / "grib-models" / "nbm" / "products.csv", kNbmCloneCsv);
      }
      if (sixHourly)
      {
         Write(root_ / "grib-models" / "six" / "model.json", kSixHourlyJson);
         Write(root_ / "grib-models" / "six" / "products.csv", kSixHourlyCsv);
      }
      UserModelRegistry::Instance()->Reload();
   }

   fs::path root_;
};

} // namespace

TEST_F(UserModelGribManagerTest, NoModelsMeansNoProducts)
{
   InstallModels(false, false);
   auto gribManager = GribManager::Instance(map::GribCategory::User);
   gribManager->ReloadUserModel();

   EXPECT_TRUE(gribManager->ProductNames().empty());
   EXPECT_TRUE(gribManager->UserModelName().empty());
   EXPECT_TRUE(gribManager->ActiveProductNames().empty());
}

TEST_F(UserModelGribManagerTest, ProductsComeFromTheSelectedModelAndSwitch)
{
   using namespace std::chrono_literals;

   InstallModels(true, true);
   auto registry    = UserModelRegistry::Instance();
   auto gribManager = GribManager::Instance(map::GribCategory::User);

   int productsChanged = 0;
   QObject::connect(gribManager.get(),
                    &GribManager::ProductsChanged,
                    [&productsChanged]() { ++productsChanged; });

   // Sorted by name, so "Aardvark Six-Hourly" is selected first.
   registry->SetSelectedModel("Aardvark Six-Hourly");
   gribManager->ReloadUserModel();
   EXPECT_EQ(gribManager->UserModelName(), "Aardvark Six-Hourly");
   EXPECT_EQ(gribManager->ProductNames(), std::vector<std::string> {"Pressure"});
   EXPECT_TRUE(gribManager->ActiveProductNames().empty()); // opt-in, always
   EXPECT_EQ(gribManager->MinIdxForecastHour(), 0);
   EXPECT_EQ(gribManager->MaxIdxForecastHourFor({}), 48);
   EXPECT_EQ(productsChanged, 1);

   registry->SetSelectedModel("Test NBM Clone");
   gribManager->ReloadUserModel();
   EXPECT_EQ(gribManager->UserModelName(), "Test NBM Clone");
   EXPECT_EQ(gribManager->ProductNames(),
             (std::vector<std::string> {"2m Temperature", "2m Dewpoint"}));
   EXPECT_EQ(gribManager->MinIdxForecastHour(), 1); // no F000, like NBM
   EXPECT_EQ(gribManager->MaxIdxForecastHourFor({}), 36);
   EXPECT_EQ(productsChanged, 2);
}

TEST_F(UserModelGribManagerTest, CycleChoicesOnlyIncludeCyclesTheModelRuns)
{
   InstallModels(false, true);
   auto gribManager = GribManager::Instance(map::GribCategory::User);
   gribManager->ReloadUserModel();

   const auto cycles = gribManager->IdxCycleChoices(72);
   ASSERT_FALSE(cycles.empty());
   EXPECT_LE(cycles.size(), 13u); // 72h / 6h, allowing for the boundary
   for (const auto& cycle : cycles)
   {
      const auto hour = std::chrono::duration_cast<std::chrono::hours>(
                           cycle - std::chrono::floor<std::chrono::days>(cycle))
                           .count();
      EXPECT_EQ(hour % 6, 0) << "cycle hour " << hour;
   }
   EXPECT_GT(cycles.front(), cycles.back()); // newest first
}

TEST_F(UserModelGribManagerTest, HourSelectionUsesTheModelsOwnRange)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   InstallModels(true, false);
   auto gribManager = GribManager::Instance(map::GribCategory::User);
   gribManager->ReloadUserModel();

   gribManager->SetIdxForecastHour(0); // below this model's F001
   EXPECT_EQ(gribManager->IdxForecastHour(), 1);
   gribManager->SetIdxForecastHour(999);
   EXPECT_EQ(gribManager->IdxForecastHour(), 36);
   gribManager->SetIdxForecastHour(20);
   EXPECT_EQ(gribManager->IdxForecastHour(), 20);

   gribManager->SetIdxCycle(sys_days {2026y / September / 25d} + 9h);
   EXPECT_FALSE(gribManager->IsUsingLatestIdxCycle());
   gribManager->UseLatestIdxCycle();
   EXPECT_TRUE(gribManager->IsUsingLatestIdxCycle());
}

// The whole point: a user's config, describing NBM's real bucket, must fetch
// and decode the same real field the built-in NBM does. Same cycle, hour and
// expected mean as GribManagerTest.NbmProductDecodesRealFile (live S3, a
// ~1-2MB range fetch plus a decode).
TEST_F(UserModelGribManagerTest, UserConfigReproducesTheBuiltInNbmFetch)
{
   using namespace std::chrono;
   using namespace std::chrono_literals;
   using sys_days = time_point<system_clock, days>;

   InstallModels(true, false);
   auto gribManager = GribManager::Instance(map::GribCategory::User);
   gribManager->ReloadUserModel();

   const auto names = gribManager->ProductNames();
   const auto it = std::find(names.begin(), names.end(), "2m Temperature");
   ASSERT_NE(it, names.end());
   const auto productIndex =
      static_cast<std::size_t>(std::distance(names.begin(), it));

   gribManager->SetProductActive("2m Temperature", true);
   gribManager->SetIdxCycle(sys_days {2026y / September / 25d} + 12h);
   gribManager->SetIdxForecastHour(24);

   const std::string framePath =
      map::GetGribFramePath(map::GribCategory::User, productIndex);
   constexpr double kExpectedMean = 288.995; // NBM's own 2t, that cycle/hour
   constexpr double kTolerance    = 0.5;

   bool               found    = false;
   double             lastMean = 0.0;
   std::vector<float> payload;

   for (int i = 0; i < 60 && !found; ++i)
   {
      std::ifstream in(framePath, std::ios::binary);
      if (in.is_open())
      {
         std::string header;
         std::getline(in, header);

         payload.assign(3744965, 0.0f); // NBM CONUS 2345x1597
         in.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size() * sizeof(float)));

         if (in.good() || in.eof())
         {
            lastMean = std::accumulate(payload.begin(), payload.end(), 0.0) /
                       static_cast<double>(payload.size());
            found = std::abs(lastMean - kExpectedMean) < kTolerance;
         }
      }
      // Windows refuses to rename a new frame over a file still open here.
      in.close();
      if (!found)
      {
         std::this_thread::sleep_for(500ms);
      }
   }

   EXPECT_TRUE(found) << "Last decoded mean seen: " << lastMean
                      << " (expected ~" << kExpectedMean << ")";

   gribManager->SetProductActive("2m Temperature", false);
}

} // namespace scwx::qt::manager
