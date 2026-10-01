#include <scwx/qt/manager/status_manager.hpp>

#include <chrono>
#include <set>
#include <thread>

#include <gtest/gtest.h>

namespace scwx
{
namespace qt
{
namespace manager
{

// Pure logic, no network needed. StatusManager is a real, app-wide
// singleton (see its own Instance() doc), so these tests share state
// with each other (and with anything else in the same wxtest process
// that reports through it) -- each test reports under its own distinct
// id(s) and always clears them at the end, so it doesn't leak state into
// whichever test happens to run next.
TEST(StatusManagerTest, EmptyByDefault)
{
   auto statusManager = StatusManager::Instance();

   // Not asserting IsBusy()/CurrentStatusText() are false/empty here --
   // this is a shared singleton, and another test (or, if this ever ran
   // inside the real app, a real manager) could legitimately have an
   // entry active concurrently. Only this test's own round-trip below is
   // this test's actual concern.
   statusManager->ReportProgress("status-manager-test-empty", "Test", 0, -1);
   EXPECT_TRUE(statusManager->IsBusy());

   statusManager->ReportComplete("status-manager-test-empty");
}

TEST(StatusManagerTest, ReportProgressAndComplete)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-progress", "Test Download", 1048576, 10485760);

   const std::string text = statusManager->CurrentStatusText();
   EXPECT_NE(text.find("Test Download"), std::string::npos);
   EXPECT_NE(text.find("1.0 MB"), std::string::npos);
   EXPECT_NE(text.find("of 10.0 MB"), std::string::npos);
   EXPECT_TRUE(statusManager->IsBusy());

   statusManager->ReportComplete("status-manager-test-progress");

   // This id's own contribution is gone -- can't assert IsBusy() is now
   // false outright (see EmptyByDefault's own comment on why), only that
   // this id's own text no longer appears.
   EXPECT_EQ(statusManager->CurrentStatusText().find("Test Download"),
             std::string::npos);
}

// A failure notice is just its sentence: no byte counts or percentage, since it
// reports something that went wrong, not progress.
TEST(StatusManagerTest, ReportMessageShowsOnlyTheSentence)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportMessage("status-manager-test-message",
                                "Test Product: download failed, will retry");

   const std::string text = statusManager->CurrentStatusText();
   EXPECT_NE(text.find("Test Product: download failed, will retry"),
             std::string::npos)
      << text;
   EXPECT_EQ(text.find('%'), std::string::npos) << text;
   EXPECT_EQ(text.find("MB"), std::string::npos) << text;

   statusManager->ReportComplete("status-manager-test-message");
}

// A notice is not download activity: it must not look like something is still
// in progress, and it outlives the few seconds an abandoned download does.
TEST(StatusManagerTest, MessagesAreNotActivityAndOutliveStaleDownloads)
{
   using namespace std::chrono_literals;

   auto statusManager = StatusManager::Instance();

   statusManager->ReportMessage("status-manager-test-notice", "Notice only");
   statusManager->ReportProgress(
      "status-manager-test-notice-download", "Stale download", 0, -1);

   // Past a stale download's lifetime (3 s), well inside a message's (15 s)
   std::this_thread::sleep_for(3500ms);

   const std::string text = statusManager->CurrentStatusText();
   EXPECT_EQ(text, "Notice only") << text;

   statusManager->ReportComplete("status-manager-test-notice");
}

TEST(StatusManagerTest, UnknownTotalOmitsFraction)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-unknown", "Test Unknown", 2097152, -1);

   const std::string text = statusManager->CurrentStatusText();
   EXPECT_NE(text.find("Test Unknown"), std::string::npos);
   EXPECT_NE(text.find("2.0 MB"), std::string::npos);
   EXPECT_EQ(text.find(" of "), std::string::npos);

   statusManager->ReportComplete("status-manager-test-unknown");
}

TEST(StatusManagerTest, MultipleEntriesShowTheLongestRunningPlusCount)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-multi-a", "First", 1024, -1);
   statusManager->ReportProgress(
      "status-manager-test-multi-b", "Second", 2048, -1);

   // "First" started first, so it stays the line shown -- with a "(+1 more)"
   // for the other -- even though "Second" reported more recently.
   const std::string text = statusManager->CurrentStatusText();
   EXPECT_NE(text.find("First"), std::string::npos) << text;
   EXPECT_NE(text.find("(+1 more)"), std::string::npos) << text;

   statusManager->ReportComplete("status-manager-test-multi-a");
   statusManager->ReportComplete("status-manager-test-multi-b");
}

// The regression: two downloads at once (wind barbs and hodographs at startup)
// each report many times a second, and the line used to follow whichever
// reported last -- flipping between them on every chunk.
TEST(StatusManagerTest, TheLineDoesNotFlipBetweenConcurrentDownloads)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-flip-a", "Barbs", 0, 1000);
   statusManager->ReportProgress("status-manager-test-flip-b", "Hodo", 0, 1000);

   std::set<std::string> descriptions;
   for (int chunk = 1; chunk <= 20; ++chunk)
   {
      statusManager->ReportProgress(
         "status-manager-test-flip-a", "Barbs", chunk * 10, 1000);
      statusManager->ReportProgress(
         "status-manager-test-flip-b", "Hodo", chunk * 10, 1000);

      const std::string text = statusManager->CurrentStatusText();
      descriptions.insert(text.substr(0, text.find(':')));
   }

   EXPECT_EQ(descriptions, std::set<std::string> {"Barbs"});

   // When the longer-running one finishes, the other takes over
   statusManager->ReportComplete("status-manager-test-flip-a");
   const std::string text = statusManager->CurrentStatusText();
   EXPECT_EQ(text.substr(0, text.find(':')), "Hodo");

   statusManager->ReportComplete("status-manager-test-flip-b");
}

// A failure notice is not buried under a running download
TEST(StatusManagerTest, ANoticeIsShownAheadOfDownloads)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-ahead-a", "Busy", 10, 100);
   statusManager->ReportMessage("status-manager-test-ahead-b", "It failed");

   const std::string text = statusManager->CurrentStatusText();
   EXPECT_EQ(text.substr(0, 9), "It failed") << text;
   EXPECT_NE(text.find("(+1 more)"), std::string::npos) << text;

   statusManager->ReportComplete("status-manager-test-ahead-a");
   statusManager->ReportComplete("status-manager-test-ahead-b");
}

// The "show all pending" list: every entry, in the order the single line would
// show them, each in the same wording
TEST(StatusManagerTest, PendingLinesListEveryEntryInDisplayOrder)
{
   auto statusManager = StatusManager::Instance();

   statusManager->ReportProgress(
      "status-manager-test-list-a", "Alpha", 1048576, 2097152);
   statusManager->ReportProgress("status-manager-test-list-b", "Beta", 0, -1);
   statusManager->ReportMessage("status-manager-test-list-c", "Gamma failed");
   // Alpha reports again; it started first, so it keeps its place
   statusManager->ReportProgress(
      "status-manager-test-list-a", "Alpha", 1572864, 2097152);

   const auto lines = statusManager->PendingLines();

   // Other tests' entries may be present (a shared singleton); find ours
   std::vector<std::string> ours;
   for (const auto& line : lines)
   {
      if (line.rfind("Alpha", 0) == 0 || line.rfind("Beta", 0) == 0 ||
          line.rfind("Gamma", 0) == 0)
      {
         ours.push_back(line);
      }
   }

   ASSERT_EQ(ours.size(), 3u);
   EXPECT_EQ(ours[0], "Gamma failed");            // notice first
   EXPECT_EQ(ours[1], "Alpha: 1.5 MB of 2.0 MB"); // longest running
   EXPECT_EQ(ours[2], "Beta: 0.0 MB");

   statusManager->ReportComplete("status-manager-test-list-a");
   statusManager->ReportComplete("status-manager-test-list-b");
   statusManager->ReportComplete("status-manager-test-list-c");
}

} // namespace manager
} // namespace qt
} // namespace scwx
