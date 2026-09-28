#include <scwx/util/grib_model_config.hpp>

#include <gtest/gtest.h>

namespace scwx::util::grib_model_config
{

// Mirrors the built-in NBM "2m Temperature" and CAPE rows (see
// kNbmProducts_ in grib_manager.cpp) as a user-authored file.
static const std::string kNbmConfig = R"({
  "model": { "name": "NBM CONUS", "kind": "idx" },
  "source": {
    "bucket": "noaa-nbm-grib2-pds",
    "key_pattern": "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2",
    "cycle_hours": [0, 6, 12, 18],
    "max_forecast_hour": 36
  },
  "products": [
    {
      "name": "2m Temperature",
      "index": { "parameter": "TMP", "level": "2 m above ground" },
      "decode": { "short_name": "2t" },
      "display": { "units": "K", "quantity": "temperature_kelvin",
                   "color_offset": 250, "color_scale": 70 }
    },
    {
      "name": "CAPE Ens Std Dev",
      "index": { "parameter": "CAPE", "level": "surface",
                 "qualifier": "ens std dev" },
      "decode": { "short_name": "cape" }
    }
  ]
})";

TEST(GribModelConfig, ParsesRealShapedNbmConfig)
{
   auto result = ParseModelConfig(kNbmConfig);

   ASSERT_TRUE(result.errors.empty()) << result.errors.front();
   ASSERT_TRUE(result.config.has_value());

   const auto& config = *result.config;
   EXPECT_EQ(config.name, "NBM CONUS");
   EXPECT_FALSE(config.alwaysOneActive);
   EXPECT_EQ(config.source.idxSuffix, ".idx");
   EXPECT_EQ(config.source.cycleHours, (std::vector<int> {0, 6, 12, 18}));
   ASSERT_EQ(config.products.size(), 2u);

   EXPECT_EQ(config.products[0].index.parameter, "TMP");
   EXPECT_EQ(config.products[0].shortName, "2t");
   EXPECT_EQ(config.products[0].display.colorOffset, 250.0f);
   EXPECT_EQ(config.products[0].display.type, "fill");
   EXPECT_EQ(config.products[1].index.qualifier, "ens std dev");
}

TEST(GribModelConfig, MalformedJsonReportsErrorNotException)
{
   auto result = ParseModelConfig("{ not json");
   EXPECT_FALSE(result.config.has_value());
   ASSERT_EQ(result.errors.size(), 1u);
}

TEST(GribModelConfig, ReportsEveryProblemWithItsPath)
{
   auto result = ParseModelConfig(R"({
     "model": { "name": "X", "kind": "s3-whole-file" },
     "source": { "bucket": "b", "key_pattern": "k/{hh}", "cycle_hours": [24] },
     "products": [
       { "name": "A", "index": { "parameter": "TMP" },
         "decode": { "short_name": "2t" },
         "display": { "type": "contour" } },
       { "name": "A", "index": { "parameter": "TMP", "level": "surface" },
         "decode": { "short_name": "2t" } }
     ]
   })");

   EXPECT_FALSE(result.config.has_value());

   auto has = [&](const std::string& fragment)
   {
      for (const auto& e : result.errors)
      {
         if (e.find(fragment) != std::string::npos)
         {
            return true;
         }
      }
      return false;
   };
   EXPECT_TRUE(has("model.kind"));
   EXPECT_TRUE(has("source.cycle_hours"));
   EXPECT_TRUE(has("products[0].index.level"));
   EXPECT_TRUE(has("products[0].display.contour_interval"));
   EXPECT_TRUE(has("products[1].name: duplicate"));
}

TEST(GribModelConfig, ExpandsKnownPlaceholders)
{
   EXPECT_EQ(ExpandKeyPattern("blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core."
                              "f{fh3}.co.grib2",
                              "20260925",
                              6,
                              7),
             "blend.20260925/06/core/blend.t06z.core.f007.co.grib2");
   EXPECT_EQ(ExpandKeyPattern("f{fh2}", "20260925", 0, 5), "f05");
}

TEST(GribModelConfig, KeyPatternAcceptsRealBucketLayouts)
{
   EXPECT_EQ(ValidateKeyPattern("blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core."
                                "f{fh3}.co.grib2"),
             "");
   EXPECT_EQ(ValidateKeyPattern(
                "gfs.{yyyymmdd}/{hh}/atmos/gfs.t{hh}z.pgrb2.0p25.f{fh3}"),
             "");
}

TEST(GribModelConfig, KeyPatternRejectsUnsafeOrUnknown)
{
   EXPECT_NE(ValidateKeyPattern("../secrets/{hh}"), "");
   EXPECT_NE(ValidateKeyPattern("/absolute/{hh}"), "");
   EXPECT_NE(ValidateKeyPattern("https://evil.example/{hh}"), "");
   EXPECT_NE(ValidateKeyPattern("blend/{unknown}/x"), "");
   EXPECT_NE(ValidateKeyPattern("blend/{hh/x"), "");
   EXPECT_NE(ValidateKeyPattern("blend/x y?z"), "");
   EXPECT_NE(ValidateKeyPattern(""), "");
}

} // namespace scwx::util::grib_model_config
