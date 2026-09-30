#include <scwx/qt/manager/user_model_registry.hpp>
#include <scwx/qt/ui/checkable_combo_box.hpp>
#include <scwx/qt/ui/grib_dock_widget.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
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

} // namespace scwx::qt::ui
