#include <scwx/provider/aws_nexrad_data_provider.hpp>

#include <cstdio>
#include <fstream>

#include <fmt/chrono.h>
#include <gtest/gtest.h>

namespace scwx
{
namespace provider
{

namespace
{

// Minimal concrete subclass -- AwsNexradDataProvider's only pure
// virtual is GetPrefix(), which these tests never exercise (they call
// the protected Download* primitives directly, the same way Rtma/Rrfs
// data providers call DownloadObject()).
class TestDataProvider : public AwsNexradDataProvider
{
public:
   TestDataProvider(const std::string& bucketName, const std::string& region) :
       AwsNexradDataProvider("test", bucketName, region)
   {
   }

   using AwsNexradDataProvider::DownloadGribMessageByIndex;

   // Not exercised by these tests (they call Download* directly) --
   // trivial overrides just to make the class concrete.
   [[nodiscard]] std::chrono::system_clock::time_point
   GetTimePointByKey(const std::string&) const override
   {
      return {};
   }

protected:
   std::string GetPrefix(std::chrono::system_clock::time_point) override
   {
      return "";
   }
};

bool IsWellFormedGribMessage(const std::string& path)
{
   std::ifstream file(path, std::ios::binary);
   if (!file.is_open())
   {
      return false;
   }

   std::string contents((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

   return contents.size() >= 8 && contents.substr(0, 4) == "GRIB" &&
          contents.substr(contents.size() - 4) == "7777";
}

} // namespace

// Live network tests proving the shared byte-range idx primitive
// (AwsNexradDataProvider::DownloadGribMessageByIndex(), backed by
// scwx::util::grib_idx) generalizes across two real, independently
// operated models -- not just RRFS's own small per-cycle files. Both
// GFS and NBM publish every field for a cycle/hour in one object
// hundreds of MB in size; these tests fetch exactly one small field out
// of each via its own real ".idx" sidecar and check only that the
// result is a well-formed, single-message GRIB2 file -- decoding it is
// decode_grib's job at the GribManager layer, a separate concern from
// this primitive.
TEST(AwsNexradDataProvider, DownloadGribMessageByIndexGfs)
{
   using namespace std::chrono;

   TestDataProvider provider("noaa-gfs-bdp-pds", "us-east-1");

   // Yesterday's 00z cycle -- GFS publishes 00/06/12/18z, so this is
   // always complete and available by the time this test runs, and
   // stays valid for GFS's ~2-week open-data retention window.
   auto date = floor<days>(system_clock::now()) - days {1};
   auto key  = fmt::format("gfs.{0:%Y%m%d}/00/atmos/gfs.t00z.pgrb2.0p25.f000",
                          fmt::gmtime(date));

   const std::string outputPath = "gfs_idx_fetch_test.grib2";

   auto result = provider.DownloadGribMessageByIndex(
      "noaa-gfs-bdp-pds", key, "PRMSL", "mean sea level", "", outputPath);

   ASSERT_TRUE(result.has_value());
   EXPECT_TRUE(IsWellFormedGribMessage(outputPath));

   // A single field, byte-range-fetched, should be a tiny fraction of
   // the ~500MB whole file -- proves this isn't silently falling back
   // to a full download.
   std::ifstream file(outputPath, std::ios::binary | std::ios::ate);
   EXPECT_LT(static_cast<std::int64_t>(file.tellg()), 5 * 1024 * 1024);

   std::remove(outputPath.c_str());
}

TEST(AwsNexradDataProvider, DownloadGribMessageByIndexNbmQualifier)
{
   using namespace std::chrono;

   TestDataProvider provider("noaa-nbm-grib2-pds", "us-east-1");

   auto date = floor<days>(system_clock::now()) - days {1};
   auto key  = fmt::format("blend.{0:%Y%m%d}/00/core/blend.t00z.core.f001.co.grib2",
                          fmt::gmtime(date));

   const std::string outputPath = "nbm_idx_fetch_test.grib2";

   // The real ambiguous-shortName case this primitive exists to solve:
   // two CAPE:surface:1 hour fcst records share one file, picked apart
   // by the idx's own trailing qualifier text (eccodes shortName alone
   // can't distinguish them -- both are "cape").
   auto result = provider.DownloadGribMessageByIndex("noaa-nbm-grib2-pds",
                                                      key,
                                                      "CAPE",
                                                      "surface",
                                                      "ens std dev",
                                                      outputPath);

   ASSERT_TRUE(result.has_value());
   EXPECT_TRUE(IsWellFormedGribMessage(outputPath));

   std::remove(outputPath.c_str());
}

} // namespace provider
} // namespace scwx
