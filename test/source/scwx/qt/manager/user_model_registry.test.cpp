#include <scwx/qt/manager/user_model_registry.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace scwx::qt::manager
{

namespace fs = std::filesystem;

namespace
{

constexpr const char* kProducts =
   "name,parameter,level,short_name\n"
   "2m Temperature,TMP,2 m above ground,2t\n";

std::string ModelJson(const std::string& name)
{
   return R"({"model": {"name": ")" + name +
          R"("}, "source": {"bucket": "some-bucket",
             "key_pattern": "m.{yyyymmdd}/{hh}/f{fh3}.grib2",
             "cycle_hours": [0, 12], "max_forecast_hour": 24}})";
}

void Write(const fs::path& path, const std::string& text)
{
   fs::create_directories(path.parent_path());
   std::ofstream(path, std::ios::binary) << text;
}

fs::path MakeModel(const fs::path&    parent,
                   const std::string& folder,
                   const std::string& name)
{
   Write(parent / folder / "model.json", ModelJson(name));
   Write(parent / folder / "products.csv", kProducts);
   return parent / folder;
}

class UserModelRegistryTest : public testing::Test
{
protected:
   void SetUp() override
   {
      root_ = fs::temp_directory_path() /
              ("scwx-user-models-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::remove_all(root_);
      fs::create_directories(root_);
      models_ = root_ / "grib-models";
      source_ = root_ / "source";
   }

   void TearDown() override { fs::remove_all(root_); }

   fs::path root_;
   fs::path models_;
   fs::path source_;
};

} // namespace

TEST_F(UserModelRegistryTest, ScanLoadsModelsSortedAndReportsBadOnes)
{
   MakeModel(models_, "z-folder", "Zulu Model");
   MakeModel(models_, "a-folder", "Alpha Model");
   Write(models_ / "broken" / "model.json", "{ not json");
   Write(models_ / "broken" / "products.csv", kProducts);
   Write(models_ / "stray-file.txt", "not a model");

   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   UserModelRegistry::Scan(models_, models, issues);

   ASSERT_EQ(models.size(), 2u);
   EXPECT_EQ(models[0].config.name, "Alpha Model");
   EXPECT_EQ(models[0].folderName, "a-folder");
   EXPECT_EQ(models[1].config.name, "Zulu Model");

   ASSERT_EQ(issues.size(), 1u);
   EXPECT_EQ(issues[0].folderName, "broken");
   EXPECT_FALSE(issues[0].errors.empty());
}

TEST_F(UserModelRegistryTest, ScanSkipsASecondModelWithTheSameName)
{
   MakeModel(models_, "one", "Same Name");
   MakeModel(models_, "two", "Same Name");

   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   UserModelRegistry::Scan(models_, models, issues);

   ASSERT_EQ(models.size(), 1u);
   EXPECT_EQ(models[0].folderName, "one");
   ASSERT_EQ(issues.size(), 1u);
   EXPECT_EQ(issues[0].folderName, "two");
   EXPECT_NE(issues[0].errors[0].find("already loaded"), std::string::npos);
}

TEST_F(UserModelRegistryTest, ScanOfAMissingDirectoryIsEmptyNotAnError)
{
   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   UserModelRegistry::Scan(root_ / "does-not-exist", models, issues);
   EXPECT_TRUE(models.empty());
   EXPECT_TRUE(issues.empty());
}

TEST_F(UserModelRegistryTest, ImportCopiesBothFilesIntoASlugFolder)
{
   MakeModel(root_, "source", "My HRRR (CONUS)!");

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
   EXPECT_EQ(result.modelName, "My HRRR (CONUS)!");
   EXPECT_TRUE(fs::is_regular_file(models_ / "my-hrrr-conus" / "model.json"));
   EXPECT_TRUE(fs::is_regular_file(models_ / "my-hrrr-conus" / "products.csv"));

   // And what was imported is what a scan then finds.
   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   UserModelRegistry::Scan(models_, models, issues);
   ASSERT_EQ(models.size(), 1u);
   EXPECT_EQ(models[0].config.name, "My HRRR (CONUS)!");
   EXPECT_EQ(models[0].config.products.size(), 1u);
}

TEST_F(UserModelRegistryTest, ReimportReplacesTheEarlierCopy)
{
   MakeModel(root_, "source", "Model");
   ASSERT_TRUE(UserModelRegistry::ImportInto(source_, models_).ok);

   Write(source_ / "products.csv",
         std::string(kProducts) + "Dew Point,DPT,2 m above ground,2d\n");
   ASSERT_TRUE(UserModelRegistry::ImportInto(source_, models_).ok);

   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   UserModelRegistry::Scan(models_, models, issues);
   ASSERT_EQ(models.size(), 1u);
   EXPECT_EQ(models[0].config.products.size(), 2u);
}

TEST_F(UserModelRegistryTest, AHostileModelNameCannotEscapeTheModelsFolder)
{
   MakeModel(root_, "source", "../../evil");

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   ASSERT_TRUE(result.ok);
   EXPECT_TRUE(fs::is_regular_file(models_ / "evil" / "model.json"));
   EXPECT_FALSE(fs::exists(root_.parent_path() / "evil"));
   for (const auto& entry : fs::recursive_directory_iterator(root_))
   {
      // Nothing was written outside models_ or the source we made.
      const auto rel = fs::relative(entry.path(), root_).string();
      EXPECT_TRUE(rel.starts_with("grib-models") || rel.starts_with("source"))
         << rel;
   }
}

TEST_F(UserModelRegistryTest, AnInvalidModelCopiesNothing)
{
   Write(source_ / "model.json", "{ nope");
   Write(source_ / "products.csv", kProducts);

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   EXPECT_FALSE(result.ok);
   EXPECT_FALSE(result.errors.empty());
   EXPECT_FALSE(fs::exists(models_));
}

TEST_F(UserModelRegistryTest, MissingFilesAreReportedByName)
{
   Write(source_ / "model.json", ModelJson("M"));

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   EXPECT_FALSE(result.ok);
   ASSERT_FALSE(result.errors.empty());
   EXPECT_NE(result.errors[0].find("products.csv"), std::string::npos);
   EXPECT_FALSE(fs::exists(models_));
}

TEST_F(UserModelRegistryTest, OversizedFilesAreRefused)
{
   MakeModel(root_, "source", "Model");
   Write(source_ / "products.csv",
         std::string(kProducts) +
            std::string(UserModelRegistry::kMaxImportFileBytes, '#'));

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   EXPECT_FALSE(result.ok);
   EXPECT_NE(result.errors[0].find("larger than"), std::string::npos);
   EXPECT_FALSE(fs::exists(models_));
}

TEST_F(UserModelRegistryTest, SymlinkedFilesAreRefused)
{
   MakeModel(root_, "source", "Model");
   const auto real = root_ / "real-products.csv";
   Write(real, kProducts);
   fs::remove(source_ / "products.csv");

   std::error_code ec;
   fs::create_symlink(real, source_ / "products.csv", ec);
   if (ec)
   {
      GTEST_SKIP() << "symlinks unavailable here: " << ec.message();
   }

   const auto result = UserModelRegistry::ImportInto(source_, models_);

   EXPECT_FALSE(result.ok);
   EXPECT_NE(result.errors[0].find("products.csv"), std::string::npos);
   EXPECT_FALSE(fs::exists(models_));
}

} // namespace scwx::qt::manager
