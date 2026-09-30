// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#include <scwx/qt/util/image_export.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace scwx::qt::util::image_export
{

namespace
{

constexpr int kToolTimeoutMs_ = 300'000;

struct ToolResult
{
   bool    ok {false};
   QString error;
};

ToolResult RunTool(const QString&     program,
                   const QStringList& arguments,
                   const QString&     workingDirectory)
{
   QProcess process;
   process.setWorkingDirectory(workingDirectory);
   process.setProcessChannelMode(QProcess::MergedChannels);
   process.start(program, arguments);

   if (!process.waitForStarted(10'000))
   {
      return {false, QObject::tr("could not start %1").arg(program)};
   }
   if (!process.waitForFinished(kToolTimeoutMs_))
   {
      process.kill();
      process.waitForFinished(5'000);
      return {false,
              QObject::tr("%1 took too long and was stopped").arg(program)};
   }

   if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
   {
      const QString output =
         QString::fromLocal8Bit(process.readAll()).trimmed();
      return {false,
              QObject::tr("%1 failed (exit code %2)%3")
                 .arg(QFileInfo(program).fileName())
                 .arg(process.exitCode())
                 .arg(output.isEmpty() ? QString {} : ": " + output)};
   }

   return {true, {}};
}

QString FramePath(int index)
{ return QStringLiteral("frame_%1.png").arg(index, 4, 10, QLatin1Char('0')); }

} // namespace

QString FindTool(const QString& name)
{
   QString found = QStandardPaths::findExecutable(name);
   if (found.isEmpty())
   {
      found = QStandardPaths::findExecutable(
         name, {QCoreApplication::applicationDirPath() + "/tools"});
   }
   return found;
}

bool CanSaveWebp()
{ return !FindTool(QStringLiteral("cwebp")).isEmpty(); }

bool CanSaveAnimation()
{ return !FindTool(QStringLiteral("img2webp")).isEmpty(); }

QString InstallHint(const QString& toolName)
{
   // One package provides both tools, named differently per system
   return QObject::tr(
             "%1 is needed to save WebP files but was not found. It comes "
             "with libwebp's command-line tools: \"apt install webp\" "
             "(Debian/Ubuntu), \"pacman -S libwebp-utils\" (Arch), \"dnf "
             "install libwebp-tools\" (Fedora), \"brew install webp\" "
             "(macOS), or download them from developers.google.com/speed/"
             "webp/download and put them on your PATH or in a \"tools\" "
             "folder beside the program.")
      .arg(toolName);
}

bool SaveImage(const QImage& image, const QString& path, QString& error)
{
   error.clear();

   if (image.isNull())
   {
      error = QObject::tr("there is no image to save");
      return false;
   }

   if (!path.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive))
   {
      if (!image.save(path))
      {
         error = QObject::tr("could not write %1").arg(path);
         return false;
      }
      return true;
   }

   const QString tool = FindTool(QStringLiteral("cwebp"));
   if (tool.isEmpty())
   {
      error = InstallHint(QStringLiteral("cwebp"));
      return false;
   }

   QTemporaryDir temp;
   if (!temp.isValid())
   {
      error = QObject::tr("could not create a temporary folder");
      return false;
   }
   if (!image.save(temp.filePath(FramePath(0)), "PNG"))
   {
      error = QObject::tr("could not prepare the image for WebP");
      return false;
   }

   // cwebp writes nothing on failure but may leave an old file in place
   QFile::remove(path);
   const ToolResult result = RunTool(tool,
                                     {QStringLiteral("-lossless"),
                                      QStringLiteral("-quiet"),
                                      FramePath(0),
                                      QStringLiteral("-o"),
                                      QDir::toNativeSeparators(path)},
                                     temp.path());
   error                   = result.error;
   if (result.ok && !QFile::exists(path))
   {
      error = QObject::tr("cwebp did not write %1").arg(path);
      return false;
   }
   return result.ok;
}

bool SaveAnimation(const std::vector<QImage>& frames,
                   int                        frameDelayMs,
                   const QString&             path,
                   QString&                   error)
{
   error.clear();

   if (frames.size() < 2)
   {
      error = QObject::tr("an animation needs at least two frames");
      return false;
   }

   const QString tool = FindTool(QStringLiteral("img2webp"));
   if (tool.isEmpty())
   {
      error = InstallHint(QStringLiteral("img2webp"));
      return false;
   }

   QTemporaryDir temp;
   if (!temp.isValid())
   {
      error = QObject::tr("could not create a temporary folder");
      return false;
   }

   // Frames go to the tool as relative file names from inside the temp
   // folder, which keeps the command line far under Windows' length limit
   // however long the loop is.
   QStringList arguments {QStringLiteral("-loop"),
                          QStringLiteral("0"),
                          QStringLiteral("-lossless"),
                          QStringLiteral("-d"),
                          QString::number(std::max(1, frameDelayMs))};
   for (int i = 0; i < static_cast<int>(frames.size()); ++i)
   {
      if (!frames[i].save(temp.filePath(FramePath(i)), "PNG"))
      {
         error = QObject::tr("could not prepare frame %1 for WebP").arg(i + 1);
         return false;
      }
      arguments << FramePath(i);
   }
   arguments << QStringLiteral("-o") << QDir::toNativeSeparators(path);

   QFile::remove(path);
   const ToolResult result = RunTool(tool, arguments, temp.path());
   error                   = result.error;
   if (result.ok && !QFile::exists(path))
   {
      error = QObject::tr("img2webp did not write %1").arg(path);
      return false;
   }
   return result.ok;
}

} // namespace scwx::qt::util::image_export
