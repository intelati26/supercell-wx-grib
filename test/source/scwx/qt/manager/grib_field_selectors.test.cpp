#include <scwx/qt/manager/grib_field_selectors.hpp>
#include <scwx/qt/manager/grib_manager.hpp>

#include <set>

#include <gtest/gtest.h>

namespace scwx::qt::manager
{

// A product with no entry would silently fall back to downloading the whole
// ~350MB object, so a newly added RRFS or RTMA product must come with its
// selectors (tools/derive_idx_selectors.py writes them).
TEST(GribFieldSelectorsTest, EveryRrfsAndRtmaProductHasFields)
{
   for (const auto category :
        {map::GribCategory::Rrfs, map::GribCategory::Rtma})
   {
      const auto names = GribManager::Instance(category)->ProductNames();
      ASSERT_FALSE(names.empty());

      for (const auto& name : names)
      {
         const auto* fields = grib_fields::FieldsFor(category, name);
         ASSERT_NE(fields, nullptr)
            << name << " has no entry in grib_field_selectors.cpp -- run "
            << "tools/derive_idx_selectors.py";
         EXPECT_FALSE(fields->primary.empty()) << name;

         for (const auto& selector : fields->primary)
         {
            EXPECT_FALSE(selector.parameter.empty()) << name;
            EXPECT_FALSE(selector.level.empty()) << name;
         }
      }
   }
}

// Only SHIP reads a second file
TEST(GribFieldSelectorsTest, OnlyShipNeedsASecondFile)
{
   const auto names =
      GribManager::Instance(map::GribCategory::Rrfs)->ProductNames();

   std::set<std::string> withSecondary;
   for (const auto& name : names)
   {
      const auto* fields =
         grib_fields::FieldsFor(map::GribCategory::Rrfs, name);
      ASSERT_NE(fields, nullptr) << name;
      if (!fields->secondary.empty())
      {
         withSecondary.insert(name);
      }
   }

   EXPECT_EQ(withSecondary, std::set<std::string> {"SHIP"});
}

// Categories that download whole objects, or select a record by their own
// rules, have no entries
TEST(GribFieldSelectorsTest, OtherCategoriesAreNotInTheTable)
{
   for (const auto category : {map::GribCategory::Mrms,
                               map::GribCategory::Nbm,
                               map::GribCategory::User})
   {
      EXPECT_EQ(grib_fields::FieldsFor(category, "2m Temperature"), nullptr);
   }

   EXPECT_EQ(grib_fields::FieldsFor(map::GribCategory::Rrfs, "No Such Product"),
             nullptr);
}

// Same display name in two categories is two different selections: RTMA's 2 m
// temperature is in a different file from anything RRFS calls the same.
TEST(GribFieldSelectorsTest, SelectionsAreKeyedByCategory)
{
   const auto* rtma =
      grib_fields::FieldsFor(map::GribCategory::Rtma, "10m Wind Speed");
   const auto* rrfs =
      grib_fields::FieldsFor(map::GribCategory::Rrfs, "10m Wind Speed");
   ASSERT_NE(rtma, nullptr);
   ASSERT_NE(rrfs, nullptr);
   EXPECT_NE(rtma, rrfs);
   // RTMA stores wind speed directly; RRFS computes it from u and v
   EXPECT_EQ(rtma->primary.size(), 1u);
   EXPECT_EQ(rrfs->primary.size(), 2u);
}

} // namespace scwx::qt::manager
