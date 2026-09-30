#include <scwx/provider/mrms_data_provider.hpp>
#include <scwx/util/logger.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <system_error>

#if defined(_MSC_VER)
#   pragma warning(push, 0)
#endif

#if defined(__GNUC__)
// Same known issue nexrad_file_factory.cpp already works around: boost::
// iostreams::copy's internal copy_operation has an implicit copy ctor this
// GCC/Boost combination flags as deprecated (-Werror promotes it).
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>

#if defined(__GNUC__)
#   pragma GCC diagnostic pop
#endif

#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

namespace scwx::provider
{

static const std::string logPrefix_ = "scwx::provider::mrms_data_provider";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

class MrmsDataProvider::Impl
{
public:
   explicit Impl(std::string bucketName) : bucketName_ {std::move(bucketName)}
   {
   }
   ~Impl() = default;

   // AwsNexradDataProvider stores its own copy privately with no accessor,
   // so DownloadAndDecompress() (added by this subclass, not part of the
   // base interface) needs its own.
   std::string bucketName_;
};

MrmsDataProvider::MrmsDataProvider(const std::string& product) :
    MrmsDataProvider(product, "noaa-mrms-pds", "us-east-1")
{
}

MrmsDataProvider::MrmsDataProvider(const std::string& product,
                                   const std::string& bucketName,
                                   const std::string& region) :
    AwsNexradDataProvider(product, bucketName, region),
    p(std::make_unique<Impl>(bucketName))
{
}

MrmsDataProvider::~MrmsDataProvider() = default;

std::string
MrmsDataProvider::GetPrefix(std::chrono::system_clock::time_point date)
{
   if (date < std::chrono::system_clock::time_point {})
   {
      date = std::chrono::system_clock::time_point {};
   }

   // MRMS's S3 layout: CONUS/<product>/<YYYYMMDD>/<file>.grib2.gz -- no
   // per-site subfolder, unlike NEXRAD's <site>/<product>/... layout.
   return fmt::format("CONUS/{0}/{1:%Y%m%d}/", radar_site(), fmt::gmtime(date));
}

std::chrono::system_clock::time_point
MrmsDataProvider::GetTimePointByKey(const std::string& key) const
{
   std::chrono::system_clock::time_point time {};

   // Filename format: MRMS_<Product>_<Level>_<YYYYMMDD>-<HHMMSS>.grib2.gz
   static const std::regex kTimeRegex {R"((\d{8})-(\d{6})\.grib2)"};
   std::smatch             match;

   if (std::regex_search(key, match, kTimeRegex))
   {
      const std::string dateStr = match[1].str() + " " + match[2].str();

      using namespace std::chrono;
#if (__cpp_lib_chrono < 201907L)
      using namespace date;
#endif

      std::istringstream in {dateStr};
      in >> parse("%Y%m%d %H%M%S", time);

      if (in.fail())
      {
         logger_->warn("Invalid time: \"{}\"", dateStr);
      }
   }
   else
   {
      logger_->warn("Time not parsable from key: \"{}\"", key);
   }

   return time;
}

std::shared_ptr<wsr88d::NexradFile>
MrmsDataProvider::LoadObjectByKey(const std::string& /* key */)
{
   // Not applicable: MRMS files are GRIB2, not a WSR-88D binary format.
   // Use DownloadAndDecompress() instead.
   logger_->warn(
      "LoadObjectByKey() is not applicable to MRMS data, use "
      "DownloadAndDecompress() instead");
   return nullptr;
}

std::shared_ptr<wsr88d::NexradFile> MrmsDataProvider::LoadObjectByTime(
   std::chrono::system_clock::time_point /* time */)
{
   logger_->warn(
      "LoadObjectByTime() is not applicable to MRMS data, use "
      "DownloadAndDecompress() instead");
   return nullptr;
}

std::optional<std::string> MrmsDataProvider::DownloadAndDecompress(
   const std::string&              key,
   const std::string&              outputPath,
   const DownloadProgressCallback& progressCallback)
{
   Aws::S3::Model::GetObjectRequest request;
   request.SetBucket(p->bucketName_);
   request.SetKey(key);

   // Stop mid-download once the app starts shutting down, same as the base
   // class's own downloads.
   request.SetContinueRequestHandler([this](const Aws::Http::HttpRequest*)
                                     { return IsRunning(); });

   // Same progress-reporting idiom as AwsNexradDataProvider::
   // DownloadObject() (which this doesn't call directly -- that helper
   // writes the raw response straight to a file, but this method needs
   // the response body itself, to decompress before ever touching disk).
   if (progressCallback)
   {
      auto bytesReceived = std::make_shared<std::int64_t>(0);

      request.SetDataReceivedEventHandler(
         [bytesReceived, progressCallback](const Aws::Http::HttpRequest*,
                                           Aws::Http::HttpResponse* response,
                                           long long                chunkSize)
         {
            *bytesReceived += chunkSize;

            std::int64_t totalBytes = -1;
            if (response != nullptr &&
                response->HasHeader(Aws::Http::CONTENT_LENGTH_HEADER))
            {
               try
               {
                  totalBytes = std::stoll(
                     response->GetHeader(Aws::Http::CONTENT_LENGTH_HEADER));
               }
               catch (const std::exception&)
               {
                  // Malformed/unparseable header -- report unknown
                  // rather than a wrong total.
               }
            }

            progressCallback(*bytesReceived, totalBytes);
         });
   }

   auto outcome = client()->GetObject(request);

   if (!outcome.IsSuccess())
   {
      if (IsRunning())
      {
         logger_->warn(
            "Failed to download {}: {}", key, outcome.GetError().GetMessage());
      }
      else
      {
         logger_->debug("Download cancelled for key: {}", key);
      }
      return std::nullopt;
   }

   auto& body = outcome.GetResultWithOwnership().GetBody();

   // Decompress straight into a partial file next to outputPath, then
   // rename it into place -- never holds the whole decompressed grid in
   // memory, and a failed/interrupted decompress never leaves a truncated
   // file at outputPath for a later cache check to mistake for a complete
   // one. Streams via operator<<(streambuf*) rather than boost::iostreams::
   // copy(), which trips -Wdeprecated-copy with a file sink in this
   // Boost/GCC combination (see the pragma above).
   static std::atomic<std::uint64_t> partialCounter {0};
   const std::string                 partialPath =
      fmt::format("{}.{}.part", outputPath, partialCounter.fetch_add(1));

   bool written = false;
   {
      std::ofstream out(partialPath, std::ios::binary | std::ios::trunc);
      if (!out)
      {
         logger_->warn("Could not open {} for writing", partialPath);
         return std::nullopt;
      }

      // MRMS objects are always gzip-compressed (.grib2.gz keys), so
      // decompress unconditionally.
      boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
      in.push(boost::iostreams::gzip_decompressor());
      in.push(body);

      // A decompression error thrown mid-stream is caught by operator<<
      // and surfaces as failbit on `out`.
      out << &in;
      out.flush();
      written = !out.fail();
   }

   std::error_code ec;
   if (!written)
   {
      logger_->warn("Failed to decompress {}", key);
      std::filesystem::remove(partialPath, ec);
      return std::nullopt;
   }

   std::filesystem::rename(partialPath, outputPath, ec);
   if (ec)
   {
      logger_->warn(
         "Could not move {} into place: {}", outputPath, ec.message());
      std::filesystem::remove(partialPath, ec);
      return std::nullopt;
   }

   logger_->debug("Decompressed {}", key);

   return outputPath;
}

} // namespace scwx::provider
