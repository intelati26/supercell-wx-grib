#include <scwx/qt/manager/status_manager.hpp>
#include <scwx/qt/ui/clickable_label.hpp>
#include <scwx/qt/ui/pending_status_dialog.hpp>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace scwx::qt::ui
{

namespace
{

QApplication* Application()
{ return qobject_cast<QApplication*>(QCoreApplication::instance()); }

void Click(QWidget&        widget,
           QPoint          where,
           Qt::MouseButton button = Qt::LeftButton)
{
   QMouseEvent press(QEvent::MouseButtonPress,
                     where,
                     widget.mapToGlobal(where),
                     button,
                     button,
                     Qt::NoModifier);
   QMouseEvent release(QEvent::MouseButtonRelease,
                       where,
                       widget.mapToGlobal(where),
                       button,
                       Qt::NoButton,
                       Qt::NoModifier);
   QApplication::sendEvent(&widget, &press);
   QApplication::sendEvent(&widget, &release);
}

} // namespace

TEST(ClickableLabelTest, LeftClickInsideEmitsClicked)
{
   if (Application() == nullptr)
   {
      GTEST_SKIP() << "no QApplication in this process";
   }

   ClickableLabel label;
   label.setText("Wind Barbs: 1.0 MB (+1 more)");
   label.resize(200, 20);

   int clicks = 0;
   QObject::connect(&label, &ClickableLabel::Clicked, [&]() { ++clicks; });

   Click(label, {10, 10});
   EXPECT_EQ(clicks, 1);

   // A right click, or a release that is not over the label, is not a click
   Click(label, {10, 10}, Qt::RightButton);
   EXPECT_EQ(clicks, 1);

   Click(label, {500, 500});
   EXPECT_EQ(clicks, 1);

   EXPECT_EQ(label.cursor().shape(), Qt::PointingHandCursor);
}

TEST(PendingStatusDialogTest, ListsEveryPendingItemAndSaysWhenThereAreNone)
{
   if (Application() == nullptr)
   {
      GTEST_SKIP() << "no QApplication in this process";
   }

   auto statusManager = manager::StatusManager::Instance();

   // A shared singleton: other tests may leave entries up briefly, so find ours
   const auto countOurs = [](const QListWidget& list)
   {
      int count = 0;
      for (int row = 0; row < list.count(); ++row)
      {
         const QString text = list.item(row)->text();
         count += text.startsWith("PendingDialog") ? 1 : 0;
      }
      return count;
   };

   PendingStatusDialog dialog;
   auto*               list = dialog.findChild<QListWidget*>();
   ASSERT_NE(list, nullptr);

   statusManager->ReportProgress(
      "pending-dialog-test-a", "PendingDialog Alpha", 1048576, 4194304);
   statusManager->ReportProgress(
      "pending-dialog-test-b", "PendingDialog Beta", 0, -1);
   statusManager->ReportMessage("pending-dialog-test-c",
                                "PendingDialog Gamma failed");

   dialog.Refresh();
   EXPECT_EQ(countOurs(*list), 3);

   // If SCWX_UI_SNAPSHOT_DIR is set, save what the dialog looks like there
   if (const char* dir = std::getenv("SCWX_UI_SNAPSHOT_DIR"))
   {
      dialog.grab().save(QString("%1/pending-dialog.png").arg(dir));
   }

   // Notice first, then the downloads longest-running first, same wording as
   // the status bar line
   std::vector<QString> ours;
   for (int row = 0; row < list->count(); ++row)
   {
      if (list->item(row)->text().startsWith("PendingDialog"))
      {
         ours.push_back(list->item(row)->text());
      }
   }
   ASSERT_EQ(ours.size(), 3u);
   EXPECT_EQ(ours[0], "PendingDialog Gamma failed");
   EXPECT_EQ(ours[1], "PendingDialog Alpha: 1.0 MB of 4.0 MB");
   EXPECT_EQ(ours[2], "PendingDialog Beta: 0.0 MB");

   statusManager->ReportComplete("pending-dialog-test-a");
   statusManager->ReportComplete("pending-dialog-test-b");
   statusManager->ReportComplete("pending-dialog-test-c");

   dialog.Refresh();
   EXPECT_EQ(countOurs(*list), 0);
}

// Open, it follows what is happening without being asked: a download that
// appears while it is showing is listed after a moment
TEST(PendingStatusDialogTest, ChangesAppearWhileItIsOpen)
{
   if (Application() == nullptr)
   {
      GTEST_SKIP() << "no QApplication in this process";
   }

   auto statusManager = manager::StatusManager::Instance();

   PendingStatusDialog dialog;
   dialog.show();
   auto* list = dialog.findChild<QListWidget*>();
   ASSERT_NE(list, nullptr);

   statusManager->ReportProgress(
      "pending-dialog-test-live", "PendingDialog Live", 2097152, -1);

   // The refresh is coalesced (a quarter of a second) and queued onto the GUI
   // thread; let the event loop run for it
   bool found = false;
   for (int i = 0; i < 100 && !found; ++i) // up to ~2.5 s
   {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds {25});
      for (int row = 0; row < list->count(); ++row)
      {
         found =
            found || list->item(row)->text() == "PendingDialog Live: 2.0 MB";
      }
   }
   EXPECT_TRUE(found);

   statusManager->ReportComplete("pending-dialog-test-live");
}

} // namespace scwx::qt::ui
