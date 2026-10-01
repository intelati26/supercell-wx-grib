#include <scwx/qt/manager/wind_barb_manager.hpp>
#include <scwx/qt/manager/grib_field_download.hpp>
#include <scwx/qt/manager/grib_field_selectors.hpp>
#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/status_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/provider/rtma_data_provider.hpp>
#include <scwx/util/logger.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

namespace scwx::qt::manager
{

static const std::string logPrefix_ = "scwx::qt::manager::wind_barb_manager";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// Same 4 minute cadence GribManager uses -- plenty frequent for an hourly
// source, just needs to notice a new file reasonably promptly.
static constexpr int kPollIntervalMs_ = 4 * 60 * 1000;

namespace
{

// Shares GribManager's own decode_grib binary and download/cache
// directory deliberately (see map::GetGribDataDirectory) -- a file
// GribManager already downloaded this hour is reused here for free, since
// the cache is keyed by S3 key, same key regardless of which manager
// asks for it. Lazily resolved for the same reason as GribManager's own
// DecodeGribPath()/DownloadDir(): both QCoreApplication::
// applicationDirPath() and ApplicationPaths need main() to have already
// run.
const std::string& DecodeGribPath()
{
   static const std::string path = (QCoreApplication::applicationDirPath() +
#if defined(_WIN32)
                                    "/decode_grib.exe"
#else
                                    "/decode_grib"
#endif
                                    )
                                      .toStdString();
   return path;
}

const std::string& DownloadDir()
{
   static const std::string dir = map::GetGribDataDirectory().string();
   return dir;
}

const std::string& CacheDir()
{
   static const std::string dir = DownloadDir() + "/cache";
   return dir;
}

const std::string& WindDirFramePath()
{
   static const std::string path = DownloadDir() + "/wind_dir_latest.frame";
   return path;
}

const std::string& WindSpeedFramePath()
{
   static const std::string path = DownloadDir() + "/wind_speed_latest.frame";
   return path;
}

const std::string& WindGustFramePath()
{
   static const std::string path = DownloadDir() + "/wind_gust_latest.frame";
   return path;
}

std::string CachedDownloadPath(const std::string& key)
{
   std::string path = key;
   if (path.ends_with(".gz"))
   {
      path.resize(path.size() - 3);
   }
   return CacheDir() + "/" + path;
}

} // namespace

class WindBarbManager::Impl
{
public:
   explicit Impl() : provider_ {std::make_shared<provider::RtmaDataProvider>()}
   {
   }

   // See WindBarbManager::DataGeneration(). Written from a fetchPool_
   // thread, read from the GUI thread.
   std::atomic<std::uint64_t> dataGeneration_ {0};

   // Same idiom GribManager::Impl uses: stop+join in the destructor's
   // *body*, before any implicit member teardown, since fetchPool_'s
   // posted lambdas capture `this`.
   ~Impl()
   {
      fetchPool_.stop();
      fetchPool_.join();
   }

   std::shared_ptr<provider::RtmaDataProvider> provider_;
   QTimer*                                     timer_ {nullptr};

   std::mutex  fetchMutex_;
   std::string lastKey_;

   boost::asio::thread_pool fetchPool_ {1};
};

WindBarbManager::WindBarbManager() : p(std::make_unique<Impl>())
{
   p->timer_ = new QTimer(this);
   connect(p->timer_, &QTimer::timeout, this, &WindBarbManager::Poll);
   p->timer_->start(kPollIntervalMs_);

   // Deferred for the same reason as GribManager's own first poll -- this
   // manager is constructed during WindBarbLayer::Initialize(), and
   // Poll() does blocking network I/O.
   QTimer::singleShot(0, this, &WindBarbManager::Poll);
}

WindBarbManager::~WindBarbManager() = default;

std::shared_ptr<WindBarbManager> WindBarbManager::Instance()
{
   static std::weak_ptr<WindBarbManager> instanceRef_ {};
   static std::mutex                     instanceMutex_ {};

   std::unique_lock lock(instanceMutex_);

   std::shared_ptr<WindBarbManager> instance = instanceRef_.lock();
   if (instance == nullptr)
   {
      instance     = std::make_shared<WindBarbManager>();
      instanceRef_ = instance;
   }

   return instance;
}

std::string WindBarbManager::GetWindDirectionFramePath()
{ return WindDirFramePath(); }

std::string WindBarbManager::GetWindSpeedFramePath()
{ return WindSpeedFramePath(); }

std::string WindBarbManager::GetWindGustFramePath()
{ return WindGustFramePath(); }

std::uint64_t WindBarbManager::DataGeneration() const
{ return p->dataGeneration_; }

void WindBarbManager::Poll()
{
   auto [newObjects, totalObjects] = p->provider_->Refresh();
   logger_->debug(
      "Refresh: {} new / {} total objects", newObjects, totalObjects);

   const std::string latestKey = p->provider_->FindLatestKey();

   std::string currentKey;
   {
      std::lock_guard lock(p->fetchMutex_);
      currentKey = p->lastKey_;
   }

   if (latestKey.empty() || latestKey == currentKey)
   {
      return;
   }

   logger_->info("New RTMA file for wind barbs: {}", latestKey);

   // Only the fields barbs read (see grib_fields::WindBarbFields()), cached as
   // an entry of their own rather than as the ~84MB object.
   const std::string cacheKey   = latestKey + ".fields-windbarbs";
   const std::string cachedPath = CachedDownloadPath(cacheKey);
   if (std::filesystem::exists(cachedPath))
   {
      // Already cached -- decode is cheap, do it right here rather than
      // bouncing to the background pool.
      if (ApplyCachedDownload(cacheKey))
      {
         std::lock_guard lock(p->fetchMutex_);
         p->lastKey_ = latestKey;
      }
      return;
   }

   std::shared_ptr<provider::RtmaDataProvider> provider = p->provider_;
   auto statusManager = manager::StatusManager::Instance();

   boost::asio::post(
      p->fetchPool_,
      [this, latestKey, cacheKey, provider, statusManager]()
      {
         std::filesystem::create_directories(
            std::filesystem::path(CachedDownloadPath(cacheKey)).parent_path());

         auto downloaded = DownloadFieldsOrObject(
            *provider,
            "Wind Barbs",
            grib_fields::WindBarbFields(),
            latestKey,
            CachedDownloadPath(cacheKey),
            [&statusManager](std::int64_t bytesReceived,
                             std::int64_t totalBytes)
            {
               statusManager->ReportProgress(
                  "wind-barbs", "Wind Barbs", bytesReceived, totalBytes);
            });
         statusManager->ReportComplete("wind-barbs");

         if (!downloaded.has_value())
         {
            logger_->warn("Failed to download {}", latestKey);
            statusManager->ReportMessage(
               "wind-barbs-error", "Wind Barbs: download failed, will retry");
            return;
         }

         // Shares GribManager's download cache -- without this, these
         // downloads were never evicted.
         GribManager::NoteCachedDownload(*downloaded);

         if (ApplyCachedDownload(cacheKey))
         {
            std::lock_guard lock(p->fetchMutex_);
            p->lastKey_ = latestKey;
         }
      });
}

bool WindBarbManager::ApplyCachedDownload(const std::string& key)
{
   const std::string cachedPath = CachedDownloadPath(key);

   auto decodeField = [&](const std::string& shortName,
                          const std::string& outputPath) -> bool
   {
      const std::string tmpPath = outputPath + ".tmp";

      // colorOffset/colorScale/noDataThreshold are meaningless for wind
      // barbs (raw direction/speed values are read directly, not
      // colorized), so pass placeholders -- 0/1/-999 keeps the palette
      // math well-defined without discarding any real data.
      QProcess decodeProcess;
      decodeProcess.start(QString::fromStdString(DecodeGribPath()),
                          {QString::fromStdString(cachedPath),
                           QString::fromStdString(tmpPath),
                           QString::number(0.0),
                           QString::number(1.0),
                           QString::number(-999.0),
                           QString::fromStdString(shortName)});

      if (!decodeProcess.waitForFinished(10000) ||
          decodeProcess.exitCode() != 0)
      {
         logger_->warn("decode_grib failed for {} ({}): {}",
                       cachedPath,
                       shortName,
                       decodeProcess.readAllStandardError().toStdString());
         std::filesystem::remove(tmpPath);
         return false;
      }

      std::error_code ec;
      util::ReplaceFileWithRetry(tmpPath, outputPath, ec);
      if (ec)
      {
         logger_->warn("Could not replace {}: {}", outputPath, ec.message());
         std::filesystem::remove(tmpPath);
         return false;
      }

      return true;
   };

   const bool dirOk   = decodeField("10wdir", WindDirFramePath());
   const bool speedOk = decodeField("10si", WindSpeedFramePath());
   const bool gustOk  = decodeField("i10fg", WindGustFramePath());

   if (dirOk && speedOk && gustOk)
   {
      logger_->info("Updated wind barb frames");
      ++p->dataGeneration_;
      Q_EMIT WindDataReady();
      return true;
   }

   return false;
}

} // namespace scwx::qt::manager
