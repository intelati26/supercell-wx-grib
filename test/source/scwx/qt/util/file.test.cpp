#include <scwx/qt/util/file.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace scwx::qt::util
{

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

void Write(const fs::path& path, const std::string& text)
{ std::ofstream(path, std::ios::binary) << text; }

std::string Read(const fs::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return {std::istreambuf_iterator<char>(in), {}};
}

class ReplaceFileTest : public testing::Test
{
protected:
   void SetUp() override
   {
      dir_ = fs::temp_directory_path() /
             ("scwx-replace-" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::remove_all(dir_);
      fs::create_directories(dir_);
   }

   void TearDown() override
   {
      std::error_code ec;
      fs::remove_all(dir_, ec);
   }

   fs::path dir_;
};

} // namespace

TEST_F(ReplaceFileTest, ReplacesAnExistingFile)
{
   Write(dir_ / "frame", "old");
   Write(dir_ / "frame.tmp", "new");

   std::error_code ec;
   EXPECT_TRUE(ReplaceFileWithRetry(dir_ / "frame.tmp", dir_ / "frame", ec));
   EXPECT_FALSE(ec);
   EXPECT_EQ(Read(dir_ / "frame"), "new");
   EXPECT_FALSE(fs::exists(dir_ / "frame.tmp"));
}

TEST_F(ReplaceFileTest, AMissingSourceFailsAtOnceWithoutRetrying)
{
   const auto started = std::chrono::steady_clock::now();

   std::error_code ec;
   EXPECT_FALSE(ReplaceFileWithRetry(dir_ / "nope", dir_ / "frame", ec));
   EXPECT_TRUE(ec);

   // Not an "in use" error, so it must not sit through 40 x 50ms of retries.
   EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

// The scenario behind "Access is denied" on Windows: a reader (the map layer
// loading the current frame) has the destination open at the moment a new frame
// is swapped in. A reader is done within moments, so the swap must land. On
// Linux a rename never conflicts with an open reader, so this passes trivially
// there; on Windows it only passes because of the retry.
TEST_F(ReplaceFileTest, SucceedsOnceABriefReaderLetsGo)
{
   Write(dir_ / "frame", "old");
   Write(dir_ / "frame.tmp", "new");

   std::ifstream reader(dir_ / "frame", std::ios::binary);
   ASSERT_TRUE(reader.is_open());

   std::thread releaser(
      [&reader]()
      {
         std::this_thread::sleep_for(150ms);
         reader.close();
      });

   std::error_code ec;
   const bool      replaced =
      ReplaceFileWithRetry(dir_ / "frame.tmp", dir_ / "frame", ec, 100, 10ms);
   releaser.join();

   EXPECT_TRUE(replaced) << ec.message();
   EXPECT_EQ(Read(dir_ / "frame"), "new");
}

TEST_F(ReplaceFileTest, GivesUpBoundedlyIfTheReaderNeverLetsGo)
{
   Write(dir_ / "frame", "old");
   Write(dir_ / "frame.tmp", "new");

   std::ifstream reader(dir_ / "frame", std::ios::binary);
   ASSERT_TRUE(reader.is_open());

   const auto started = std::chrono::steady_clock::now();

   std::error_code ec;
   const bool      replaced =
      ReplaceFileWithRetry(dir_ / "frame.tmp", dir_ / "frame", ec, 4, 20ms);

   EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

#ifdef _WIN32
   // Windows won't replace a file that stays open: fail, keep the old frame,
   // and leave the new one in place for the caller to clean up.
   EXPECT_FALSE(replaced);
   EXPECT_TRUE(ec);
   EXPECT_EQ(Read(dir_ / "frame"), "old");
#else
   EXPECT_TRUE(replaced) << ec.message(); // POSIX doesn't care about readers
#endif
}

} // namespace scwx::qt::util
