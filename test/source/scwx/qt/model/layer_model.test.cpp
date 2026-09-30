#include <scwx/qt/main/application.hpp>
#include <scwx/qt/manager/placefile_manager.hpp>
#include <scwx/qt/model/layer_model.hpp>
#include <scwx/qt/types/layer_types.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>

#include <boost/json.hpp>
#include <QObject>
#include <gtest/gtest.h>

namespace scwx::qt::model
{

class LayerModelOpacityTest : public ::testing::Test
{
protected:
   static void SetUpTestSuite()
   {
      std::mutex              mutex;
      std::condition_variable cv;
      bool                    placefilesInitialized = false;

      // PlacefileManager posts init work that waits on application
      // initialization, then emits PlacefilesInitialized. Unblock that wait
      // and join the work before creating LayerModel. Otherwise teardown
      // destroys LayerModel while the worker still emits into it (CI:
      // intermittent SIGSEGV after the test body).
      placefileManager_ = manager::PlacefileManager::Instance();
      const QMetaObject::Connection connection =
         QObject::connect(placefileManager_.get(),
                          &manager::PlacefileManager::PlacefilesInitialized,
                          [&]()
                          {
                             const std::unique_lock lock(mutex);
                             placefilesInitialized = true;
                             cv.notify_all();
                          });

      main::Application::FinishInitialization();

      std::unique_lock lock(mutex);
      const bool       initialized =
         cv.wait_for(lock,
                     std::chrono::seconds(10),
                     [&]() { return placefilesInitialized; });
      QObject::disconnect(connection);

      ASSERT_TRUE(initialized)
         << "PlacefilesInitialized was not received before timeout";

      model_ = LayerModel::Instance();
   }

   static void TearDownTestSuite()
   {
      model_.reset();
      placefileManager_.reset();
   }

   void SetUp() override { model_->ResetLayers(); }

   static std::shared_ptr<manager::PlacefileManager> placefileManager_;
   static std::shared_ptr<LayerModel>                model_;
};

std::shared_ptr<manager::PlacefileManager>
                            LayerModelOpacityTest::placefileManager_ {};
std::shared_ptr<LayerModel> LayerModelOpacityTest::model_ {};

int FindRow(const std::shared_ptr<LayerModel>& model, types::LayerType type)
{
   for (int row = 0; row < model->rowCount(); ++row)
   {
      const QModelIndex typeIndex =
         model->index(row, static_cast<int>(LayerModel::Column::Type));
      if (types::GetLayerType(
             typeIndex.data(Qt::DisplayRole).toString().toStdString()) == type)
      {
         return row;
      }
   }
   return -1;
}

TEST_F(LayerModelOpacityTest, RadarOpacityIsEditable)
{
   const int radarRow = FindRow(model_, types::LayerType::Radar);
   ASSERT_GE(radarRow, 0);

   const QModelIndex opacityIndex =
      model_->index(radarRow, static_cast<int>(LayerModel::Column::Opacity));

   EXPECT_TRUE(model_->flags(opacityIndex) & Qt::ItemIsEditable);
   EXPECT_EQ(opacityIndex.data(Qt::DisplayRole).toString(), "100%");
   EXPECT_EQ(opacityIndex.data(Qt::EditRole).toInt(), 100);

   EXPECT_TRUE(model_->setData(opacityIndex, 40, Qt::ItemDataRole::EditRole));
   EXPECT_EQ(opacityIndex.data(Qt::EditRole).toInt(), 40);
   EXPECT_EQ(opacityIndex.data(Qt::DisplayRole).toString(), "40%");

   const types::LayerInfo info =
      model_->GetLayerInfo(types::LayerType::Radar, std::monostate {});
   EXPECT_FLOAT_EQ(info.opacity_, 0.4f);
}

TEST_F(LayerModelOpacityTest, MapStyleOpacityIsNotEditable)
{
   const int mapRow = FindRow(model_, types::LayerType::Map);
   ASSERT_GE(mapRow, 0);

   const QModelIndex opacityIndex =
      model_->index(mapRow, static_cast<int>(LayerModel::Column::Opacity));

   EXPECT_FALSE(model_->flags(opacityIndex) & Qt::ItemIsEditable);
   EXPECT_EQ(opacityIndex.data(Qt::DisplayRole).toString(), "Opaque");
   EXPECT_FALSE(model_->setData(opacityIndex, 25, Qt::ItemDataRole::EditRole));
}

TEST_F(LayerModelOpacityTest, SetLayerOpacityIgnoresMapLayers)
{
   EXPECT_FALSE(model_->SetLayerOpacity(
      types::LayerType::Map, types::MapLayer::MapUnderlay, 0.2f));

   EXPECT_TRUE(model_->SetLayerOpacity(
      types::LayerType::Radar, std::monostate {}, 0.55f));
   const types::LayerInfo info =
      model_->GetLayerInfo(types::LayerType::Radar, std::monostate {});
   EXPECT_FLOAT_EQ(info.opacity_, 0.55f);
}

TEST_F(LayerModelOpacityTest, ResetRestoresDefaultOpacity)
{
   ASSERT_TRUE(model_->SetLayerOpacity(
      types::LayerType::Radar, std::monostate {}, 0.3f));
   model_->ResetLayers();

   const types::LayerInfo info =
      model_->GetLayerInfo(types::LayerType::Radar, std::monostate {});
   EXPECT_FLOAT_EQ(info.opacity_, 1.0f);
}

// A profile saved before a built-in data layer existed has no entry for it.
// Loading it must add the layer, or the layer could never be shown for anyone
// with existing settings (Custom Models arrived after most profiles were made).
TEST_F(LayerModelOpacityTest, ExistingProfilesGainNewBuiltInDataLayers)
{
   std::ostringstream defaults;
   model_->WriteLayerSettings(defaults);
   const auto saved = boost::json::parse(defaults.str()).as_array();

   boost::json::array withoutCustomModels;
   for (const auto& layer : saved)
   {
      if (layer.as_object().at("description").as_string() != "Custom Models")
      {
         withoutCustomModels.push_back(layer);
      }
   }
   ASSERT_LT(withoutCustomModels.size(), saved.size())
      << "the default layers should include Custom Models";

   std::istringstream oldProfile(boost::json::serialize(withoutCustomModels));
   model_->ReadLayerSettings(oldProfile);

   std::ostringstream reloaded;
   model_->WriteLayerSettings(reloaded);
   const auto after = boost::json::parse(reloaded.str()).as_array();

   EXPECT_EQ(after.size(), saved.size());
   bool found = false;
   for (const auto& layer : after)
   {
      found = found || layer.as_object().at("description").as_string() ==
                          "Custom Models";
   }
   EXPECT_TRUE(found) << "Custom Models was not added to the old profile";
}

} // namespace scwx::qt::model
