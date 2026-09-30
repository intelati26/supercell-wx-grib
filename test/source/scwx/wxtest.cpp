#include <scwx/qt/main/application_paths.hpp>
#include <scwx/util/logger.hpp>

#include <aws/core/Aws.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <spdlog/spdlog.h>

int main(int argc, char** argv)
{
   // A QApplication (a QCoreApplication, so everything that used to work
   // still does) so widget tests can build real widgets. Qt's offscreen
   // platform, unless the caller chose one, so no display is needed and
   // nothing pops up on a developer's desktop.
   if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
   {
      qputenv("QT_QPA_PLATFORM", "offscreen");
   }
   const QApplication app(argc, argv);

   scwx::util::Logger::Initialize();
   spdlog::set_level(spdlog::level::debug);

   scwx::qt::main::ApplicationPaths::Initialize();
   scwx::qt::main::ApplicationPaths::LogErrors();

   Aws::SDKOptions awsSdkOptions;
   Aws::InitAPI(awsSdkOptions);

   ::testing::InitGoogleTest(&argc, argv);
   int result = RUN_ALL_TESTS();

   Aws::ShutdownAPI(awsSdkOptions);

   return result;
}
