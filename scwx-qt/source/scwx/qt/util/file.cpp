#include <scwx/qt/util/file.hpp>
#include <scwx/qt/util/q_file_input_stream.hpp>

#include <algorithm>
#include <fstream>
#include <thread>

#include <QFile>

namespace scwx
{
namespace qt
{
namespace util
{

std::unique_ptr<std::istream> OpenFile(const std::string&      filename,
                                       std::ios_base::openmode mode)
{
   if (filename.starts_with(':'))
   {
      return std::make_unique<QFileInputStream>(filename, mode);
   }
   else
   {
      return std::make_unique<std::ifstream>(filename, mode);
   }
}

bool ReplaceFileWithRetry(const std::filesystem::path& from,
                          const std::filesystem::path& to,
                          std::error_code&             ec,
                          int                          attempts,
                          std::chrono::milliseconds    delay)
{
   for (int attempt = 0; attempt < std::max(1, attempts); ++attempt)
   {
      std::filesystem::rename(from, to, ec);
      if (!ec)
      {
         return true;
      }

      // Only "the destination is in use" is worth waiting out. Anything else
      // (no such file, no space, a bad path) won't fix itself.
      const bool inUse = ec == std::errc::permission_denied ||
                         ec == std::errc::device_or_resource_busy ||
                         ec == std::errc::text_file_busy;
      if (!inUse)
      {
         return false;
      }

      if (attempt + 1 < attempts)
      {
         std::this_thread::sleep_for(delay);
      }
   }

   return false;
}

} // namespace util
} // namespace qt
} // namespace scwx
