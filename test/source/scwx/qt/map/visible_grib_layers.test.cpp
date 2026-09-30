#include <scwx/qt/map/visible_grib_layers.hpp>

#include <gtest/gtest.h>

namespace scwx::qt::map
{

namespace
{

types::LayerInfo
Data(types::DataLayer layer, std::size_t shownOn, float opacity = 1.0f)
{
   types::LayerInfo info;
   info.type_        = types::LayerType::Data;
   info.description_ = layer;
   info.displayed_.fill(false);
   info.displayed_[shownOn] = true;
   info.opacity_            = opacity;
   return info;
}

} // namespace

TEST(VisibleGribLayersTest, ListsDisplayedGribLayersTopFirst)
{
   types::LayerVector layers;
   layers.push_back(Data(types::DataLayer::WindBarbs, 0));
   layers.push_back(Data(types::DataLayer::GribMrms, 0));
   layers.push_back(Data(types::DataLayer::GribRtma, 0, 0.65f));
   layers.push_back(Data(types::DataLayer::GribRrfs, 0));

   const auto visible = VisibleGribLayers(layers, 0);

   ASSERT_EQ(visible.size(), 3u);
   EXPECT_EQ(visible[0].category, GribCategory::Mrms);
   EXPECT_EQ(visible[1].category, GribCategory::Rtma);
   EXPECT_FLOAT_EQ(visible[1].opacity, 0.65f);
   EXPECT_EQ(visible[2].category, GribCategory::Rrfs);
}

TEST(VisibleGribLayersTest, OnlyThePaneAskedAbout)
{
   types::LayerVector layers;
   layers.push_back(Data(types::DataLayer::GribMrms, 0));
   layers.push_back(Data(types::DataLayer::GribRtma, 1));

   ASSERT_EQ(VisibleGribLayers(layers, 0).size(), 1u);
   EXPECT_EQ(VisibleGribLayers(layers, 0)[0].category, GribCategory::Mrms);
   ASSERT_EQ(VisibleGribLayers(layers, 1).size(), 1u);
   EXPECT_EQ(VisibleGribLayers(layers, 1)[0].category, GribCategory::Rtma);
   EXPECT_TRUE(VisibleGribLayers(layers, 2).empty());
   EXPECT_TRUE(VisibleGribLayers(layers, 99).empty()); // no such pane
}

TEST(VisibleGribLayersTest, InvisibleAndNonGribLayersAreLeftOut)
{
   types::LayerVector layers;
   layers.push_back(Data(types::DataLayer::GribMrms, 0, 0.0f)); // transparent
   layers.push_back(Data(types::DataLayer::Hodograph, 0));
   layers.push_back(Data(types::DataLayer::RadarRange, 0));
   types::LayerInfo map;
   map.type_        = types::LayerType::Map;
   map.description_ = types::MapLayer::MapUnderlay;
   layers.push_back(map);
   layers.push_back(Data(types::DataLayer::GribNbm, 1)); // other pane

   EXPECT_TRUE(VisibleGribLayers(layers, 0).empty());
}

TEST(VisibleGribLayersTest, CategoriesAreNamedAndMapped)
{
   EXPECT_EQ(GribCategoryOf(types::DataLayer::GribUser), GribCategory::User);
   EXPECT_FALSE(GribCategoryOf(types::DataLayer::WindBarbs).has_value());
   EXPECT_FALSE(GribCategoryOf(types::MapLayer::MapUnderlay).has_value());
   EXPECT_FALSE(GribCategoryOf(std::monostate {}).has_value());

   EXPECT_EQ(GribCategoryDisplayName(GribCategory::Mrms), "MRMS");
   EXPECT_EQ(GribCategoryDisplayName(GribCategory::User), "Custom Models");
}

} // namespace scwx::qt::map
