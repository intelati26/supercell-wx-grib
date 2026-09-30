#include <scwx/qt/util/image_export.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace scwx::qt::util::image_export
{

namespace
{

QImage Solid(QRgb colour, int width = 32, int height = 16)
{
   QImage image(width, height, QImage::Format_ARGB32);
   image.fill(colour);
   return image;
}

// Runs a test with an empty PATH (and no tools folder beside the test binary),
// so every tool reads as not installed, then restores it.
class NoToolsScope
{
public:
   NoToolsScope() : saved_(qgetenv("PATH")) { qputenv("PATH", ""); }
   ~NoToolsScope() { qputenv("PATH", saved_); }

private:
   QByteArray saved_;
};

} // namespace

TEST(ImageExportTest, MissingToolIsReportedNotSubstituted)
{
   const NoToolsScope noTools;
   QTemporaryDir      dir;
   ASSERT_TRUE(dir.isValid());

   EXPECT_FALSE(CanSaveWebp());
   EXPECT_FALSE(CanSaveAnimation());

   QString    error;
   const auto still = dir.filePath("still.webp");
   EXPECT_FALSE(SaveImage(Solid(0xff336699), still, error));
   EXPECT_TRUE(error.contains("cwebp")) << error.toStdString();
   EXPECT_TRUE(error.contains("libwebp")) << error.toStdString();
   EXPECT_FALSE(QFile::exists(still)) << "no other format may be written";

   const auto loop = dir.filePath("loop.webp");
   EXPECT_FALSE(
      SaveAnimation({Solid(0xffff0000), Solid(0xff00ff00)}, 100, loop, error));
   EXPECT_TRUE(error.contains("img2webp")) << error.toStdString();
   EXPECT_FALSE(QFile::exists(loop));
}

TEST(ImageExportTest, PngNeedsNoExternalTool)
{
   const NoToolsScope noTools;
   QTemporaryDir      dir;
   ASSERT_TRUE(dir.isValid());

   QString    error;
   const auto path = dir.filePath("still.png");
   EXPECT_TRUE(SaveImage(Solid(0xff336699), path, error))
      << error.toStdString();
   EXPECT_EQ(QImage(path).pixel(3, 3), 0xff336699u);
}

TEST(ImageExportTest, RejectsNullImageAndSingleFrame)
{
   QTemporaryDir dir;
   ASSERT_TRUE(dir.isValid());

   QString error;
   EXPECT_FALSE(SaveImage(QImage {}, dir.filePath("x.png"), error));
   EXPECT_FALSE(error.isEmpty());

   EXPECT_FALSE(
      SaveAnimation({Solid(0xff000000)}, 100, dir.filePath("x.webp"), error));
   EXPECT_FALSE(error.isEmpty());
}

// The next two run the real libwebp tools, so they only mean something where
// those are installed (CI does not install them; a developer machine does).
TEST(ImageExportTest, WebpStillIsLossless)
{
   if (!CanSaveWebp())
   {
      GTEST_SKIP() << "cwebp not installed";
   }

   QTemporaryDir dir;
   ASSERT_TRUE(dir.isValid());
   QString    error;
   const auto path = dir.filePath("still.webp");

   ASSERT_TRUE(SaveImage(Solid(0xff336699), path, error))
      << error.toStdString();

   // Qt may not have a WebP reader, so check the container instead: a WebP
   // file holding a VP8L chunk is the lossless codec (lossy would be VP8).
   QFile file(path);
   ASSERT_TRUE(file.open(QIODevice::ReadOnly));
   const QByteArray bytes = file.readAll();
   EXPECT_TRUE(bytes.startsWith("RIFF"));
   EXPECT_EQ(bytes.mid(8, 4), "WEBP");
   EXPECT_TRUE(bytes.contains("VP8L"));
}

TEST(ImageExportTest, AnimatedWebpHasEveryFrame)
{
   if (!CanSaveAnimation())
   {
      GTEST_SKIP() << "img2webp not installed";
   }

   QTemporaryDir dir;
   ASSERT_TRUE(dir.isValid());
   QString    error;
   const auto path = dir.filePath("loop.webp");

   ASSERT_TRUE(
      SaveAnimation({Solid(0xffff0000), Solid(0xff00ff00), Solid(0xff0000ff)},
                    120,
                    path,
                    error))
      << error.toStdString();

   QFile file(path);
   ASSERT_TRUE(file.open(QIODevice::ReadOnly));
   const QByteArray bytes = file.readAll();
   EXPECT_TRUE(bytes.startsWith("RIFF"));
   EXPECT_EQ(bytes.mid(8, 4), "WEBP");
   // An animation is the VP8X container with an ANIM chunk and one ANMF per
   // frame
   EXPECT_TRUE(bytes.contains("ANIM"));
   EXPECT_EQ(bytes.count("ANMF"), 3);
}

} // namespace scwx::qt::util::image_export
