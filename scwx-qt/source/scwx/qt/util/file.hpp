#pragma once

#include <chrono>
#include <filesystem>
#include <istream>
#include <memory>
#include <string>
#include <system_error>

namespace scwx
{
namespace qt
{
namespace util
{

std::unique_ptr<std::istream>
OpenFile(const std::string&      filename,
         std::ios_base::openmode mode = std::ios_base::in);

// Moves `from` over `to`, retrying briefly if the OS says the destination is
// busy. Windows refuses to replace a file another handle has open ("Access is
// denied") -- e.g. the map layer in the middle of reading the current frame --
// and giving up on the first refusal leaves the old contents in place for good.
// A reader is normally done within milliseconds, so this retries only for
// errors that mean "in use" (permission denied, busy) for up to
// `attempts` x `delay`; any other error, or a reader that never lets go,
// fails as a plain rename would. `ec` is set on failure.
bool ReplaceFileWithRetry(
   const std::filesystem::path& from,
   const std::filesystem::path& to,
   std::error_code&             ec,
   int                          attempts = 40,
   std::chrono::milliseconds    delay    = std::chrono::milliseconds {50});

} // namespace util
} // namespace qt
} // namespace scwx
