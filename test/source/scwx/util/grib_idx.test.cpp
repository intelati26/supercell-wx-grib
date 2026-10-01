#include <scwx/util/grib_idx.hpp>

#include <gtest/gtest.h>

namespace scwx
{
namespace util
{
namespace grib_idx
{

// Real text captured live from
// noaa-gfs-bdp-pds/gfs.20260925/00/atmos/gfs.t00z.pgrb2.0p25.f000.idx
// (2026-09-26) -- not synthesized, matching this project's own
// discipline of verifying against real downloaded data.
static const std::string kGfsIdxSample =
   "1:0:d=2026092500:PRMSL:mean sea level:anl:\n"
   "2:991171:d=2026092500:CLMR:1 hybrid level:anl:\n"
   "3:1076176:d=2026092500:ICMR:1 hybrid level:anl:\n"
   "4:1312828:d=2026092500:RWMR:1 hybrid level:anl:\n";

// Real text captured live from
// noaa-nbm-grib2-pds/blend.20260925/00/core/blend.t00z.core.f001.co.grib2.idx
// (2026-09-26) -- includes the real ambiguous-shortName case flagged for
// NBM: two CAPE:surface:1 hour fcst records, one with an "ens std dev"
// qualifier and one without.
static const std::string kNbmIdxSample =
   "12:11449623:d=2026092500:TCDC:high cloud layer:1 hour fcst:\n"
   "13:12084465:d=2026092500:CAPE:surface:1 hour fcst:\n"
   "14:14154571:d=2026092500:CAPE:surface:1 hour fcst:ens std dev\n"
   "15:18527237:d=2026092500:CEIL:cloud ceiling:1 hour fcst:\n";

TEST(GribIdx, ParseIdxGfsSample)
{
   auto records = ParseIdx(kGfsIdxSample);

   ASSERT_EQ(records.size(), 4u);

   EXPECT_EQ(records[0].messageNumber, 1);
   EXPECT_EQ(records[0].byteOffset, 0);
   EXPECT_EQ(records[0].referenceTime, "d=2026092500");
   EXPECT_EQ(records[0].parameter, "PRMSL");
   EXPECT_EQ(records[0].level, "mean sea level");
   EXPECT_EQ(records[0].step, "anl");
   EXPECT_EQ(records[0].qualifier, "");

   EXPECT_EQ(records[3].messageNumber, 4);
   EXPECT_EQ(records[3].byteOffset, 1312828);
   EXPECT_EQ(records[3].parameter, "RWMR");
}

TEST(GribIdx, ParseIdxNbmQualifier)
{
   auto records = ParseIdx(kNbmIdxSample);

   ASSERT_EQ(records.size(), 4u);

   // The plain CAPE record has no qualifier.
   EXPECT_EQ(records[1].parameter, "CAPE");
   EXPECT_EQ(records[1].qualifier, "");

   // The ensemble-std-dev CAPE record shares parameter+level with the
   // plain one, distinguished only by its trailing qualifier text.
   EXPECT_EQ(records[2].parameter, "CAPE");
   EXPECT_EQ(records[2].level, "surface");
   EXPECT_EQ(records[2].qualifier, "ens std dev");
}

TEST(GribIdx, ParseIdxSkipsMalformedLines)
{
   // A blank line and a truncated line (too few fields) should be
   // skipped, not throw or abort the rest of the parse.
   const std::string idxText =
      "1:0:d=2026092500:PRMSL:mean sea level:anl:\n"
      "\n"
      "not:enough:fields\n"
      "2:991171:d=2026092500:CLMR:1 hybrid level:anl:\n";

   auto records = ParseIdx(idxText);

   ASSERT_EQ(records.size(), 2u);
   EXPECT_EQ(records[0].parameter, "PRMSL");
   EXPECT_EQ(records[1].parameter, "CLMR");
}

TEST(GribIdx, RangeForRecordMiddleAndLast)
{
   auto records = ParseIdx(kGfsIdxSample);

   auto middle = RangeForRecord(records, 1);
   EXPECT_EQ(middle.start, 991171);
   ASSERT_TRUE(middle.end.has_value());
   EXPECT_EQ(*middle.end, 1076175);

   // The last record's range is unbounded (extends to EOF).
   auto last = RangeForRecord(records, 3);
   EXPECT_EQ(last.start, 1312828);
   EXPECT_FALSE(last.end.has_value());
}

TEST(GribIdx, FindRecordDistinguishesQualifier)
{
   auto records = ParseIdx(kNbmIdxSample);

   auto plain = FindRecord(records, "CAPE", "surface");
   ASSERT_TRUE(plain.has_value());
   EXPECT_EQ(*plain, 1u);

   auto stdDev = FindRecord(records, "CAPE", "surface", "ens std dev");
   ASSERT_TRUE(stdDev.has_value());
   EXPECT_EQ(*stdDev, 2u);

   auto missing = FindRecord(records, "CAPE", "no such level");
   EXPECT_FALSE(missing.has_value());
}

TEST(GribIdx, ToRangeHeaderBoundedAndUnbounded)
{
   ByteRange bounded {991171, 1076175};
   EXPECT_EQ(ToRangeHeader(bounded), "bytes=991171-1076175");

   ByteRange unbounded {1312828, std::nullopt};
   EXPECT_EQ(ToRangeHeader(unbounded), "bytes=1312828-");
}

// Real text captured live from
// noaa-rrfs-ops-pds/rrfs.20260930/18/rrfs.t18z.2dfld.3km.f006.conus.grib2.idx
// (2026-09-30): REFD at three levels, then (further down) the two APCP records
// that share a parameter and level and differ only in step.
static const std::string kRrfsIdxSample =
   "1:0:d=2026093018:REFC:entire atmosphere (considered as a single "
   "layer):6 hour fcst:\n"
   "2:1200000:d=2026093018:REFD:1000 m above ground:6 hour fcst:\n"
   "3:1800000:d=2026093018:REFD:4000 m above ground:6 hour fcst:\n"
   "4:2500000:d=2026093018:TMP:2 m above ground:6 hour fcst:\n"
   "5:4300000:d=2026093018:APCP:surface:5-6 hour acc fcst:\n"
   "6:4527108:d=2026093018:APCP:surface:0-6 hour acc fcst:\n"
   "7:4900000:d=2026093018:PRES:surface:6 hour fcst:\n";

TEST(GribIdx, SelectRecordsMatchesParameterAndLevelAtAnyStep)
{
   const auto records = ParseIdx(kRrfsIdxSample);
   ASSERT_EQ(records.size(), 7u);

   // Both APCP records share parameter and level, so both are wanted -- the
   // decoder separates them by step, as it does from the whole file.
   const auto apcp = SelectRecords(records, {{"APCP", "surface"}});
   ASSERT_EQ(apcp.size(), 2u);
   EXPECT_EQ(apcp[0], 4u);
   EXPECT_EQ(apcp[1], 5u);

   // A level that is not the one named is not a match
   const auto refd = SelectRecords(records, {{"REFD", "1000 m above ground"}});
   ASSERT_EQ(refd.size(), 1u);
   EXPECT_EQ(refd[0], 1u);

   EXPECT_TRUE(
      SelectRecords(records, {{"REFD", "500 m above ground"}}).empty());
   EXPECT_TRUE(SelectRecords(records, {}).empty());
}

TEST(GribIdx, SelectRecordsKeepsFileOrderAcrossSelectors)
{
   const auto records = ParseIdx(kRrfsIdxSample);

   // Selectors listed out of file order still come back in file order, once
   // each
   const auto selected = SelectRecords(
      records,
      {{"PRES", "surface"},
       {"REFC", "entire atmosphere (considered as a single layer)"},
       {"PRES", "surface"}});

   ASSERT_EQ(selected.size(), 2u);
   EXPECT_EQ(selected[0], 0u);
   EXPECT_EQ(selected[1], 6u);
}

TEST(GribIdx, MergedRangesJoinsRecordsThatSitNextToEachOther)
{
   const auto records = ParseIdx(kRrfsIdxSample);

   // Records 1,2 (REFD 1000 and 4000) are neighbours: one request. TMP (3) is
   // adjacent to 2 as well, so 1-3 is one range; APCP (4,5) follows directly
   // and joins it too. PRES (6) is the last record: open-ended.
   const auto all = MergedRanges(records, {1, 2, 3, 4, 5, 6});
   ASSERT_EQ(all.size(), 1u);
   EXPECT_EQ(all[0].start, 1200000);
   EXPECT_FALSE(all[0].end.has_value());

   // A gap keeps them apart, each range ending one byte before the next record
   const auto gap = MergedRanges(records, {1, 4});
   ASSERT_EQ(gap.size(), 2u);
   EXPECT_EQ(gap[0].start, 1200000);
   EXPECT_EQ(*gap[0].end, 1800000 - 1);
   EXPECT_EQ(gap[1].start, 4300000);
   EXPECT_EQ(*gap[1].end, 4527108 - 1);
}

TEST(GribIdx, MergedRangesIgnoresOrderAndDuplicates)
{
   const auto records = ParseIdx(kRrfsIdxSample);

   const auto ranges = MergedRanges(records, {5, 4, 4, 1, 1});
   ASSERT_EQ(ranges.size(), 2u);
   EXPECT_EQ(ranges[0].start, 1200000);
   EXPECT_EQ(ranges[1].start, 4300000);
   EXPECT_EQ(*ranges[1].end, 4900000 - 1); // records 4 and 5, merged

   EXPECT_TRUE(MergedRanges(records, {}).empty());
}

} // namespace grib_idx
} // namespace util
} // namespace scwx
