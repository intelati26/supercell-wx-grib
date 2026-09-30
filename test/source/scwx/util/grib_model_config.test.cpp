#include <scwx/util/grib_model_config.hpp>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace scwx::util::grib_model_config
{

// Mirrors the built-in NBM rows (kNbmProducts_ in grib_manager.cpp) as a
// user-authored folder: settings in JSON, fields in a spreadsheet.
static const std::string kNbmJson = R"({
  "model": { "name": "NBM CONUS", "kind": "idx" },
  "source": {
    "bucket": "noaa-nbm-grib2-pds",
    "key_pattern": "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2",
    "cycle_hours": [0, 6, 12, 18],
    "max_forecast_hour": 36
  },
  "defaults": { "type": "fill", "units": "K", "quantity": "temperature_kelvin",
                "color_offset": 250, "color_scale": 70 }
})";

static const std::string kNbmCsv =
   "name,parameter,level,qualifier,short_name,units,quantity,color_offset,"
   "color_scale\n"
   "2m Temperature,TMP,2 m above ground,,2t,,,,\n"
   "CAPE Ens Std Dev,CAPE,surface,ens std dev,cape,J/kg,none,0,4000\n";

static bool HasFragment(const std::vector<std::string>& lines,
                        const std::string&              fragment)
{
   for (const auto& line : lines)
   {
      if (line.find(fragment) != std::string::npos)
      {
         return true;
      }
   }
   return false;
}

// ------------------------------------------------------------ settings

TEST(GribModelConfig, ParsesRealShapedSettings)
{
   auto result = ParseModelSettings(kNbmJson);

   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_TRUE(result.config.has_value());

   const auto& config = *result.config;
   EXPECT_EQ(config.name, "NBM CONUS");
   EXPECT_FALSE(config.alwaysOneActive);
   EXPECT_EQ(config.source.idxSuffix, ".idx");
   EXPECT_EQ(config.source.maxForecastHour, 36);
   EXPECT_EQ(config.source.cycleHours, (std::vector<int> {0, 6, 12, 18}));
   EXPECT_EQ(config.defaults.colorOffset, 250.0f);
   EXPECT_EQ(config.defaults.quantity, "temperature_kelvin");
   EXPECT_TRUE(config.products.empty());
}

TEST(GribModelConfig, MalformedJsonReportsErrorNotException)
{
   auto result = ParseModelSettings("{ not json");
   EXPECT_FALSE(result.config.has_value());
   ASSERT_EQ(result.errors.size(), 1u);
}

TEST(GribModelConfig, SettingsReportEveryProblemWithItsPath)
{
   auto result = ParseModelSettings(R"({
     "model": { "name": "X", "kind": "s3-whole-file" },
     "source": { "bucket": "b", "key_pattern": "k/{hh}", "cycle_hours": [24] },
     "defaults": { "color_scale": 0, "type": "contour" },
     "products": []
   })");

   EXPECT_FALSE(result.config.has_value());
   EXPECT_TRUE(HasFragment(result.errors, "model.kind"));
   EXPECT_TRUE(HasFragment(result.errors, "source.cycle_hours"));
   EXPECT_TRUE(HasFragment(result.errors, "defaults.color_scale"));
   EXPECT_TRUE(HasFragment(result.errors, "products: not allowed"));
}

// ----------------------------------------------------------------- CSV

TEST(GribModelConfig, CsvBlankCellsInheritDefaults)
{
   auto settings = ParseModelSettings(kNbmJson);
   ASSERT_TRUE(settings.config.has_value());

   auto result = ParseProductsCsv(kNbmCsv, settings.config->defaults);
   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   EXPECT_TRUE(result.warnings.empty());
   ASSERT_EQ(result.products.size(), 2u);

   EXPECT_EQ(result.products[0].shortName, "2t");
   EXPECT_EQ(result.products[0].display.units, "K");
   EXPECT_EQ(result.products[0].display.colorOffset, 250.0f);
   EXPECT_EQ(result.products[0].display.colorScale, 70.0f);
   EXPECT_EQ(result.products[1].index.qualifier, "ens std dev");
   EXPECT_EQ(result.products[1].display.units, "J/kg");
   EXPECT_EQ(result.products[1].display.colorScale, 4000.0f);
}

TEST(GribModelConfig, CsvColumnsMatchByNameInAnyOrderAndCase)
{
   auto result = ParseProductsCsv(
      "SHORT_NAME,Level,PARAMETER,Name\n2t,2 m above ground,TMP,Temp\n", {});
   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_EQ(result.products.size(), 1u);
   EXPECT_EQ(result.products[0].name, "Temp");
   EXPECT_EQ(result.products[0].index.parameter, "TMP");
}

TEST(GribModelConfig, CsvSurvivesExcelExportQuirks)
{
   // BOM + CRLF + ';' delimiter + decimal comma + quoted field with a ';'.
   const std::string csv =
      "\xEF\xBB\xBF"
      "name;parameter;level;short_name;color_offset\r\n"
      "\"Temp; 2 m\";TMP;2 m above ground;2t;233,5\r\n";

   auto result = ParseProductsCsv(csv, {});
   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_EQ(result.products.size(), 1u);
   EXPECT_EQ(result.products[0].name, "Temp; 2 m");
   EXPECT_EQ(result.products[0].display.colorOffset, 233.5f);
}

TEST(GribModelConfig, CsvDecimalCommaNotAcceptedWithCommaDelimiter)
{
   auto result = ParseProductsCsv(
      "name,parameter,level,short_name,color_offset\n"
      "A,TMP,surface,t,\"233,5\"\n"
      "B,TMP,surface,t,240\n",
      {});
   ASSERT_EQ(result.products.size(), 1u);
   EXPECT_EQ(result.products[0].name, "B");
   EXPECT_TRUE(HasFragment(result.warnings, "row 2 (color_offset)"));
}

TEST(GribModelConfig, CsvQuotedFieldsHoldQuotesAndCommas)
{
   auto result = ParseProductsCsv(
      "name,parameter,level,short_name\n"
      "\"Say \"\"hi\"\", ok\",TMP,surface,t\n",
      {});
   ASSERT_EQ(result.products.size(), 1u);
   EXPECT_EQ(result.products[0].name, "Say \"hi\", ok");
}

TEST(GribModelConfig, CsvSkipsBadRowsWithWarningsAndKeepsTheRest)
{
   auto result = ParseProductsCsv(
      "name,parameter,level,short_name,type,contour_interval\n"
      "# a comment row\n"
      "\n"
      "Good,TMP,surface,t,,\n"
      ",TMP,surface,t,,\n"                // row 5: no name
      "NoInterval,TMP,surface,t,contour,\n" // row 6
      "Good,TMP,surface,t,,\n"            // row 7: duplicate
      "Bad Type,TMP,surface,t,heatmap,\n" // row 8
      "Ok Contour,TMP,surface,t,contour,4\n",
      {});

   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_EQ(result.products.size(), 2u);
   EXPECT_EQ(result.products[1].name, "Ok Contour");
   EXPECT_EQ(result.products[1].display.contourInterval, 4.0f);

   EXPECT_EQ(result.warnings.size(), 4u);
   EXPECT_TRUE(HasFragment(result.warnings, "row 5 (name)"));
   EXPECT_TRUE(HasFragment(result.warnings, "row 6 (contour_interval)"));
   EXPECT_TRUE(HasFragment(result.warnings, "row 7 (name): duplicate"));
   EXPECT_TRUE(HasFragment(result.warnings, "row 8 (type)"));
}

TEST(GribModelConfig, CsvFatalWhenHeaderOrAllRowsUnusable)
{
   auto noColumn = ParseProductsCsv("name,parameter,level\nA,TMP,surface\n", {});
   EXPECT_TRUE(HasFragment(noColumn.errors, "short_name"));

   auto noRows =
      ParseProductsCsv("name,parameter,level,short_name\n,,,\n# only\n", {});
   EXPECT_TRUE(HasFragment(noRows.errors, "no valid product rows"));

   EXPECT_TRUE(HasFragment(ParseProductsCsv("", {}).errors, "no header row"));
   EXPECT_TRUE(HasFragment(
      ParseProductsCsv("name,parameter\n\"open", {}).errors, "unterminated"));
}

// -------------------------------------------------------------- folder

TEST(GribModelConfig, LoadsModelFolder)
{
   namespace fs    = std::filesystem;
   const auto dir  = fs::temp_directory_path() / "scwx_grib_model_config_test";
   fs::remove_all(dir);
   fs::create_directories(dir);
   std::ofstream(dir / "model.json") << kNbmJson;
   std::ofstream(dir / "products.csv") << kNbmCsv;

   auto result = LoadModelFolder(dir.string());
   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_TRUE(result.config.has_value());
   EXPECT_EQ(result.config->products.size(), 2u);
   EXPECT_EQ(result.config->products[0].display.colorOffset, 250.0f);

   fs::remove(dir / "products.csv");
   auto missing = LoadModelFolder(dir.string());
   EXPECT_FALSE(missing.config.has_value());
   EXPECT_TRUE(HasFragment(missing.errors, "products.csv: cannot read"));

   fs::remove_all(dir);
}

// ---------------------------------------------------------- key pattern

TEST(GribModelConfig, ExpandsKnownPlaceholders)
{
   EXPECT_EQ(ExpandKeyPattern("blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core."
                              "f{fh3}.co.grib2",
                              "20260928",
                              6,
                              7),
             "blend.20260928/06/core/blend.t06z.core.f007.co.grib2");
   EXPECT_EQ(ExpandKeyPattern("x.f{fh2}", "20260928", 0, 12), "x.f12");
}

TEST(GribModelConfig, KeyPatternAcceptsRealBucketLayouts)
{
   EXPECT_EQ(ValidateKeyPattern("blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core."
                                "f{fh3}.co.grib2"),
             "");
   EXPECT_EQ(ValidateKeyPattern(
                "gfs.{yyyymmdd}/{hh}/atmos/gfs.t{hh}z.pgrb2.0p25.f{fh3}"),
             "");
   EXPECT_EQ(ValidateKeyPattern("hrrr.{yyyymmdd}/conus/hrrr.t{hh}z.wrfsfcf{fh2}"
                                ".grib2"),
             "");
}

TEST(GribModelConfig, KeyPatternRejectsUnsafeOrUnknown)
{
   for (const char* bad : {"../secrets/{hh}",
                           "/absolute/{hh}",
                           "https://evil.example/{hh}",
                           "blend/{unknown}/x",
                           "blend/{hh/x",
                           "blend/x y?z",
                           ""})
   {
      EXPECT_NE(ValidateKeyPattern(bad), "") << "should reject: " << bad;
   }
}

TEST(GribModelConfig, KeyPatternRejectsTraversalAndEncodingTricks)
{
   for (const char* bad : {"a/./b",
                           "./a",
                           "a/.",
                           "a/../b",
                           "a..b",
                           "a//b",
                           "a/",
                           "blend\\{hh}\\x",
                           "a/%2e%2e/b",
                           "a/%2F/b",
                           "s3://bucket/{hh}",
                           "a?x=1",
                           "a#frag",
                           "a\tb",
                           "caf\xc3\xa9/{hh}",
                           "{HH}/x",
                           "{yyyymmdd",
                           "yyyymmdd}/x",
                           "{{hh}}/x",
                           "{ hh }/x"})
   {
      EXPECT_NE(ValidateKeyPattern(bad), "") << "should reject: " << bad;
   }

   EXPECT_NE(ValidateKeyPattern(std::string(513, 'a')), "");
   EXPECT_EQ(ValidateKeyPattern(std::string(512, 'a')), "");
}

TEST(GribModelConfig, KeyPatternReasonsNameTheProblem)
{
   EXPECT_NE(ValidateKeyPattern("a/{bogus}/b").find("{bogus}"),
             std::string::npos);
   EXPECT_NE(ValidateKeyPattern("a/..").find(".."), std::string::npos);
   EXPECT_NE(ValidateKeyPattern("a b").find("' '"), std::string::npos);
   EXPECT_NE(ValidateKeyPattern("").find("empty"), std::string::npos);
}

TEST(GribModelConfig, KeyPatternAcceptsAllPlaceholdersAndCommonPunctuation)
{
   EXPECT_EQ(ValidateKeyPattern("a_b-c/{yyyymmdd}/{hh}/x.{fh2}.{fh3}.grib2"),
             "");
   EXPECT_EQ(ValidateKeyPattern("noplaceholders.grib2"), "");
}

TEST(GribModelConfig, SourceSettingsHaveDefaultsAndCanBeOverridden)
{
   const auto plain = ParseModelSettings(R"({
      "model":  {"name": "M"},
      "source": {"bucket": "some-bucket", "key_pattern": "a/{hh}/f{fh3}",
                 "cycle_hours": [0, 12], "max_forecast_hour": 24}})");
   ASSERT_TRUE(plain.config) << (plain.errors.empty() ? "" : plain.errors[0]);
   EXPECT_EQ(plain.config->source.region, "us-east-1");
   EXPECT_EQ(plain.config->source.minForecastHour, 0);
   EXPECT_EQ(plain.config->source.forecastHourStep, 1);
   EXPECT_EQ(plain.config->source.availabilityLagHours, 3);

   const auto custom = ParseModelSettings(R"({
      "model":  {"name": "M"},
      "source": {"bucket": "eu-bucket", "region": "eu-central-1",
                 "key_pattern": "a/{hh}/f{fh3}", "cycle_hours": [0],
                 "min_forecast_hour": 3, "max_forecast_hour": 90,
                 "forecast_hour_step": 3, "availability_lag_hours": 8}})");
   ASSERT_TRUE(custom.config) << (custom.errors.empty() ? "" : custom.errors[0]);
   EXPECT_EQ(custom.config->source.region, "eu-central-1");
   EXPECT_EQ(custom.config->source.minForecastHour, 3);
   EXPECT_EQ(custom.config->source.forecastHourStep, 3);
   EXPECT_EQ(custom.config->source.availabilityLagHours, 8);
}

TEST(GribModelConfig, SourceSettingsAreValidatedBeforeTheyReachTheNetwork)
{
   const auto with = [](const std::string& extra)
   {
      return ParseModelSettings(
         R"({"model": {"name": "M"}, "source": {"bucket": "some-bucket",
             "key_pattern": "a/{hh}/f{fh3}", "cycle_hours": [0],
             "max_forecast_hour": 24)" +
         extra + "}}");
   };

   EXPECT_TRUE(with("").config);
   EXPECT_FALSE(with(R"(, "region": "US EAST 1")").config);
   EXPECT_FALSE(with(R"(, "region": "https://x")").config);
   EXPECT_FALSE(with(R"(, "idx_suffix": ".index")").config);
   EXPECT_FALSE(with(R"(, "forecast_hour_step": 0)").config);
   EXPECT_FALSE(with(R"(, "availability_lag_hours": -1)").config);
   EXPECT_FALSE(with(R"(, "min_forecast_hour": 30)").config); // > max (24)

   for (const char* bucket : {"https://bucket.s3.amazonaws.com/x",
                              "s3://bucket",
                              "Has_Caps",
                              "ab",
                              "bucket/with/path",
                              "-leading",
                              "double..dot"})
   {
      const auto r = ParseModelSettings(
         std::string(R"({"model": {"name": "M"}, "source": {"bucket": ")") +
         bucket +
         R"(", "key_pattern": "a/{hh}", "cycle_hours": [0],
             "max_forecast_hour": 24}})");
      EXPECT_FALSE(r.config) << "should reject bucket: " << bucket;
   }
}

} // namespace scwx::util::grib_model_config
