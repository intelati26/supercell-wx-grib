#include <scwx/provider/rrfs_data_provider.hpp>
#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/hodograph_selection.hpp>
#include <scwx/qt/manager/user_model_registry.hpp>
#include <scwx/qt/ui/checkable_combo_box.hpp>
#include <scwx/qt/ui/grib_dock_widget.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSlider>
#include <QStandardItemModel>

#include <gtest/gtest.h>

namespace scwx::qt::ui
{

namespace fs = std::filesystem;

namespace
{

void Write(const fs::path& path, const std::string& text)
{
   fs::create_directories(path.parent_path());
   std::ofstream(path, std::ios::binary) << text;
}

// wxtest's main() makes a QApplication on Qt's offscreen platform. If these
// tests are ever run from a process without one there is no way to build
// widgets, so they skip.
QApplication* Application()
{ return qobject_cast<QApplication*>(QCoreApplication::instance()); }

QGroupBox* FindGroup(QWidget& root, const QString& title)
{
   for (auto* box : root.findChildren<QGroupBox*>())
   {
      if (box->title() == title)
      {
         return box;
      }
   }
   return nullptr;
}

class GribDockWidgetTest : public testing::Test
{
protected:
   void SetUp() override
   {
      app_ = Application();
      if (app_ == nullptr)
      {
         GTEST_SKIP() << "no QApplication in this process";
      }

      root_ = fs::temp_directory_path() /
              ("scwx-dock-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::remove_all(root_);
      fs::create_directories(root_);
      manager::UserModelRegistry::SetModelsDirectoryForTesting(root_ /
                                                               "models");
   }

   void TearDown() override
   {
      manager::UserModelRegistry::SetModelsDirectoryForTesting({});
      if (app_ != nullptr)
      {
         manager::UserModelRegistry::Instance()->Reload();
      }
      std::error_code ec;
      fs::remove_all(root_, ec);
   }

   void InstallModel()
   {
      Write(root_ / "models" / "nbm" / "model.json", R"json({
        "model":  {"name": "NBM Clone (test)"},
        "source": {"bucket": "noaa-nbm-grib2-pds",
          "key_pattern": "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2",
          "cycle_hours": [0, 6, 12, 18], "min_forecast_hour": 1,
          "max_forecast_hour": 36},
        "defaults": {"units": "K", "quantity": "temperature_kelvin",
                     "color_offset": 250, "color_scale": 70}})json");
      Write(root_ / "models" / "nbm" / "products.csv",
            "name,parameter,level,short_name\n"
            "2m Temperature,TMP,2 m above ground,2t\n"
            "2m Dewpoint,DPT,2 m above ground,2d\n");
      Write(root_ / "models" / "broken" / "model.json", "{ not json");
      Write(root_ / "models" / "broken" / "products.csv", "name\nx\n");
      manager::UserModelRegistry::Instance()->Reload();
   }

   // If SCWX_UI_SNAPSHOT_DIR is set, save what the widget looks like there.
   static void MaybeSnapshot(QWidget& widget, const char* name)
   {
      if (const char* dir = std::getenv("SCWX_UI_SNAPSHOT_DIR"))
      {
         widget.grab().save(QString("%1/%2.png").arg(dir, name));
      }
   }

   QApplication* app_ {nullptr};
   fs::path      root_;
};

} // namespace

TEST_F(GribDockWidgetTest, CustomModelsSectionListsTheImportedModel)
{
   InstallModel();

   GribDockWidget dock;
   dock.resize(420, 1500);

   auto* group = FindGroup(dock, "Custom Models");
   ASSERT_NE(group, nullptr);

   // The model picker holds the one good model...
   QComboBox* modelCombo = nullptr;
   for (auto* combo : group->findChildren<QComboBox*>())
   {
      if (combo->findData(QString("NBM Clone (test)")) >= 0)
      {
         modelCombo = combo;
      }
   }
   ASSERT_NE(modelCombo, nullptr);
   EXPECT_EQ(modelCombo->currentData().toString(), "NBM Clone (test)");
   EXPECT_TRUE(modelCombo->isEnabled());

   // ...its two products are in the product picker, unchecked...
   auto* products = group->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);
   auto* itemModel = qobject_cast<QStandardItemModel*>(products->model());
   ASSERT_NE(itemModel, nullptr);
   EXPECT_EQ(itemModel->rowCount(), 2);
   EXPECT_FALSE(products->IsChecked("2m Temperature"));

   // ...the Import button is there, and the broken folder is reported.
   bool haveImport = false;
   for (auto* button : group->findChildren<QPushButton*>())
   {
      haveImport = haveImport || button->text() == "Import model...";
   }
   EXPECT_TRUE(haveImport);

   bool issueShown = false;
   for (auto* label : group->findChildren<QLabel*>())
   {
      issueShown = issueShown || (label->text().contains("Not loaded") &&
                                  label->text().contains("broken"));
   }
   EXPECT_TRUE(issueShown);

   MaybeSnapshot(dock, "dock-with-model");
}

TEST_F(GribDockWidgetTest, WithNoModelsTheSectionExplainsAndDisablesItself)
{
   GribDockWidget dock;
   dock.resize(420, 1500);

   auto* group = FindGroup(dock, "Custom Models");
   ASSERT_NE(group, nullptr);

   bool explained = false;
   for (auto* label : group->findChildren<QLabel*>())
   {
      explained = explained || label->text().contains("no models imported");
   }
   EXPECT_TRUE(explained);

   auto* products = group->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);
   EXPECT_FALSE(products->isEnabled());

   bool importEnabled = false;
   for (auto* button : group->findChildren<QPushButton*>())
   {
      if (button->text() == "Import model...")
      {
         importEnabled = button->isEnabled();
      }
   }
   EXPECT_TRUE(importEnabled); // the one thing you can do

   MaybeSnapshot(dock, "dock-no-models");
}

TEST_F(GribDockWidgetTest, ImportingAModelUpdatesAnOpenDockWithoutARestart)
{
   GribDockWidget dock;
   dock.resize(420, 1500);

   auto* group = FindGroup(dock, "Custom Models");
   ASSERT_NE(group, nullptr);
   auto* products = group->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);
   ASSERT_FALSE(products->isEnabled()); // nothing imported yet

   // A model folder outside the models directory, as a user would have it.
   const fs::path source = root_ / "downloads" / "my-model";
   Write(source / "model.json", R"json({
      "model":  {"name": "Imported Live"},
      "source": {"bucket": "some-bucket", "key_pattern": "m.{yyyymmdd}/{hh}/f{fh3}",
                 "cycle_hours": [0, 12], "max_forecast_hour": 12}})json");
   Write(source / "products.csv",
         "name,parameter,level,short_name\n"
         "Alpha,TMP,2 m above ground,2t\n"
         "Beta,DPT,2 m above ground,2d\n"
         "Gamma,RH,2 m above ground,2r\n");

   const auto result = manager::UserModelRegistry::Instance()->Import(source);
   ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);

   // The dock followed along: the model is listed and selected, its three
   // products are offered, and the controls are usable.
   QComboBox* modelCombo = nullptr;
   for (auto* combo : group->findChildren<QComboBox*>())
   {
      if (combo->findData(QString("Imported Live")) >= 0)
      {
         modelCombo = combo;
      }
   }
   ASSERT_NE(modelCombo, nullptr);
   EXPECT_EQ(modelCombo->currentData().toString(), "Imported Live");
   EXPECT_TRUE(products->isEnabled());
   auto* itemModel = qobject_cast<QStandardItemModel*>(products->model());
   ASSERT_NE(itemModel, nullptr);
   EXPECT_EQ(itemModel->rowCount(), 3);
   EXPECT_EQ(itemModel->item(0)->text(), "Alpha");

   // It landed in the models directory under a slug of its name.
   EXPECT_TRUE(
      fs::is_regular_file(root_ / "models" / "imported-live" / "model.json"));

   MaybeSnapshot(dock, "dock-after-import");
}

TEST_F(GribDockWidgetTest, OnlyRrfsOffersALoopExportAndEveryoneCanExportAnImage)
{
   GribDockWidget dock;
   dock.resize(420, 1500);

   const auto buttonNamed = [](QGroupBox* group, const QString& text)
   {
      QPushButton* found = nullptr;
      for (auto* button : group->findChildren<QPushButton*>())
      {
         if (button->text() == text)
         {
            found = button;
         }
      }
      return found;
   };

   const QString loopText = "Export loop (animated WebP)...";

   for (const char* title : {"MRMS", "RTMA", "RRFS", "NBM", "Custom Models"})
   {
      auto* group = FindGroup(dock, title);
      ASSERT_NE(group, nullptr) << title;
      EXPECT_NE(buttonNamed(group, "Export image..."), nullptr) << title;
      EXPECT_EQ(buttonNamed(group, loopText) != nullptr,
                std::string(title) == "RRFS")
         << title;
   }

   // The hour accessors the export steps through: a category with no hour
   // slider reads as 0, and moving it is a harmless no-op.
   dock.SetForecastHour(map::GribCategory::Mrms, 5);
   EXPECT_EQ(dock.ForecastHour(map::GribCategory::Mrms), 0);
}

// The gridded hodograph is a product in the RRFS list and nowhere else: picking
// it is what turns hodographs on, and it starts unpicked.
TEST_F(GribDockWidgetTest, HodographIsAnRrfsProductThatStartsUnpicked)
{
   auto& selection = manager::HodographSelection::Instance();
   selection.SetEnabled(false);

   GribDockWidget dock;
   dock.resize(420, 1500);

   for (const char* title : {"MRMS", "RTMA", "NBM", "Custom Models"})
   {
      auto* group = FindGroup(dock, title);
      ASSERT_NE(group, nullptr) << title;
      auto* products = group->findChild<CheckableComboBox*>();
      ASSERT_NE(products, nullptr) << title;
      auto* itemModel = qobject_cast<QStandardItemModel*>(products->model());
      ASSERT_NE(itemModel, nullptr) << title;
      for (int row = 0; row < itemModel->rowCount(); ++row)
      {
         EXPECT_NE(itemModel->item(row)->text(), "Gridded Hodograph") << title;
      }
   }

   auto* rrfs = FindGroup(dock, "RRFS");
   ASSERT_NE(rrfs, nullptr);
   auto* products = rrfs->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);
   auto* itemModel = qobject_cast<QStandardItemModel*>(products->model());
   ASSERT_NE(itemModel, nullptr);
   ASSERT_GT(itemModel->rowCount(), 1);
   EXPECT_EQ(itemModel->item(itemModel->rowCount() - 1)->text(),
             "Gridded Hodograph");

   EXPECT_FALSE(products->IsChecked("Gridded Hodograph"));
   EXPECT_FALSE(selection.IsEnabled());

   // SetChecked() only sets the item; a user's click also announces the
   // change, which is what the dock listens to.
   products->SetChecked("Gridded Hodograph", true);
   products->CheckedItemsChanged();
   EXPECT_TRUE(selection.IsEnabled());

   products->SetChecked("Gridded Hodograph", false);
   products->CheckedItemsChanged();
   EXPECT_FALSE(selection.IsEnabled());
}

// Real S3 access, the real dock and the real manager together. Checking an RRFS
// product lists the bucket; the hour grid, slider and cycle list must then be
// what is really published -- not 85 buttons out to a cycle's nominal horizon
// -- with an hour that does not exist disabled, and "Latest" naming the cycle
// it means.
TEST_F(GribDockWidgetTest, RrfsHourGridFollowsWhatIsPublished)
{
   using namespace std::chrono_literals;

   provider::RrfsDataProvider::ResetAvailabilityForTesting();

   const std::string productName = "Simulated Reflectivity (1km AGL)";
   auto gribManager = manager::GribManager::Instance(map::GribCategory::Rrfs);

   GribDockWidget dock;
   dock.resize(420, 1500);

   auto* group = FindGroup(dock, "RRFS");
   ASSERT_NE(group, nullptr);
   auto* products = group->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);

   // What a user's click does
   products->SetChecked(productName, true);
   products->CheckedItemsChanged();

   // The listing comes back through the event loop
   std::optional<std::set<int>> published;
   for (int i = 0; i < 1200 && !published.has_value(); ++i)
   {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
      std::this_thread::sleep_for(50ms);
      published = gribManager->PublishedRrfsForecastHours();
   }
   ASSERT_TRUE(published.has_value()) << "S3 was not listed";
   ASSERT_FALSE(published->empty());
   QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

   // The hour buttons are the three-digit ones
   std::vector<QPushButton*> buttons;
   for (auto* button : group->findChildren<QPushButton*>())
   {
      if (button->text().size() == 3 && button->text()[0].isDigit())
      {
         buttons.push_back(button);
      }
   }
   std::sort(buttons.begin(),
             buttons.end(),
             [](auto* a, auto* b) { return a->text() < b->text(); });

   const int last = *published->rbegin();
   ASSERT_EQ(static_cast<int>(buttons.size()), last + 1)
      << "the grid should end at the last published hour, F" << last;
   for (int hour = 0; hour <= last; ++hour)
   {
      EXPECT_EQ(buttons[static_cast<std::size_t>(hour)]->isEnabled(),
                published->contains(hour))
         << "button F" << hour;
   }

   // The slider covers the same hours
   auto* slider = group->findChild<QSlider*>();
   ASSERT_NE(slider, nullptr);
   EXPECT_EQ(slider->minimum(), *published->begin());
   EXPECT_EQ(slider->maximum(), last);

   // "Latest" says which cycle it is, and the list offers real cycles
   QComboBox* cycles = nullptr;
   for (auto* combo : group->findChildren<QComboBox*>())
   {
      if (combo->itemText(0).startsWith("Latest"))
      {
         cycles = combo;
      }
   }
   ASSERT_NE(cycles, nullptr);
   EXPECT_TRUE(cycles->itemText(0).contains("z)"))
      << cycles->itemText(0).toStdString();
   EXPECT_GT(cycles->count(), 1);

   MaybeSnapshot(dock, "dock-rrfs-published-hours");

   products->SetChecked(productName, false);
   products->CheckedItemsChanged();
   gribManager->UseLatestRrfsCycle();
   provider::RrfsDataProvider::ResetAvailabilityForTesting();
}

// The gridded hodograph is picked like a product but has none of its own: with
// it the only thing ticked, the cycle and hour controls must still work -- they
// used to stay at their empty construction-time range until a normal product
// was checked.
TEST_F(GribDockWidgetTest, HodographAloneSizesTheRrfsHourControls)
{
   using namespace std::chrono_literals;

   provider::RrfsDataProvider::ResetAvailabilityForTesting();
   manager::HodographSelection::Instance().SetEnabled(false);

   auto gribManager = manager::GribManager::Instance(map::GribCategory::Rrfs);
   ASSERT_TRUE(gribManager->ActiveProductIndices().empty());
   EXPECT_FALSE(gribManager->HasRrfsSelection());

   GribDockWidget dock;
   dock.resize(420, 1500);

   auto* group = FindGroup(dock, "RRFS");
   ASSERT_NE(group, nullptr);
   auto* products = group->findChild<CheckableComboBox*>();
   ASSERT_NE(products, nullptr);

   const auto hourButtons = [&]()
   {
      int count = 0;
      for (auto* button : group->findChildren<QPushButton*>())
      {
         count +=
            (button->text().size() == 3 && button->text()[0].isDigit()) ? 1 : 0;
      }
      return count;
   };
   EXPECT_EQ(hourButtons(), 0) << "no selection, no hour grid";

   // Only the hodograph ticked
   products->SetChecked("Gridded Hodograph", true);
   products->CheckedItemsChanged();
   EXPECT_TRUE(gribManager->HasRrfsSelection());

   std::optional<std::set<int>> published;
   for (int i = 0; i < 1200 && !published.has_value(); ++i)
   {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
      std::this_thread::sleep_for(50ms);
      published = gribManager->PublishedRrfsForecastHours();
   }
   ASSERT_TRUE(published.has_value()) << "S3 was not listed";
   ASSERT_FALSE(published->empty());
   QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

   EXPECT_EQ(hourButtons(), *published->rbegin() + 1);

   auto* slider = group->findChild<QSlider*>();
   ASSERT_NE(slider, nullptr);
   EXPECT_EQ(slider->minimum(), *published->begin());
   EXPECT_EQ(slider->maximum(), *published->rbegin());

   // "Latest" is a real, 3-hourly cycle -- the hodograph needs the file an
   // hourly cycle does not have
   const auto cycle = gribManager->CurrentRrfsCycle();
   EXPECT_NE(cycle, std::chrono::system_clock::time_point {});
   EXPECT_FALSE(provider::RrfsDataProvider::UsesSubhVariant(cycle));

   // Unticked again, the grid goes away
   products->SetChecked("Gridded Hodograph", false);
   products->CheckedItemsChanged();
   EXPECT_FALSE(gribManager->HasRrfsSelection());
   EXPECT_EQ(hourButtons(), 0);

   gribManager->UseLatestRrfsCycle();
   provider::RrfsDataProvider::ResetAvailabilityForTesting();
}

} // namespace scwx::qt::ui
