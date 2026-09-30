#include <scwx/qt/manager/hodograph_manager.hpp>
#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/status_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/provider/rrfs_data_provider.hpp>
#include <scwx/util/logger.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <fmt/format.h>

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

namespace scwx::qt::manager
{

static const std::string logPrefix_ = "scwx::qt::manager::hodograph_manager";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// Same 4 minute cadence GribManager/WindBarbManager use.
static constexpr int kPollIntervalMs_ = 4 * 60 * 1000;

namespace
{

// Shares GribManager's own decode_grib binary and download/cache
// directory -- same reasoning as WindBarbManager's own DecodeGribPath()/
// DownloadDir(): a file GribManager(Rrfs) already downloaded this hour
// (or WindBarbManager's own RTMA one -- different bucket, same cache
// root) is reused here for free when the S3 key matches.
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

std::string CachedDownloadPath(const std::string& key)
{
   std::string path = key;
   if (path.ends_with(".gz"))
   {
      path.resize(path.size() - 3);
   }
   return CacheDir() + "/" + path;
}

const std::string& TerrainFramePath()
{
   static const std::string path = DownloadDir() + "/hodograph_orog.frame";
   return path;
}

std::string UFramePath(std::size_t levelIndex)
{ return DownloadDir() + fmt::format("/hodograph_u_{:02d}.frame", levelIndex); }

std::string VFramePath(std::size_t levelIndex)
{ return DownloadDir() + fmt::format("/hodograph_v_{:02d}.frame", levelIndex); }

// 17 real discrete wind levels RRFS's 2dfld file carries, verified live
// (2026-09-25) -- see docs/gridded-hodograph-plan.md. Ordered by
// increasing height, matching this header's own Levels() doc.
const std::vector<HodographManager::Level>& LevelTable()
{
   // clang-format off
   static const std::vector<HodographManager::Level> levels {
      {"heightAboveGround", 10,   10.0f,   false, "10u", "10v"},
      {"heightAboveGround", 30,   30.0f,   false, "u",   "v"},
      {"heightAboveGround", 50,   50.0f,   false, "u",   "v"},
      {"heightAboveGround", 80,   80.0f,   false, "u",   "v"},
      {"heightAboveGround", 100,  100.0f,  false, "u",   "v"},
      {"heightAboveGround", 160,  160.0f,  false, "u",   "v"},
      {"heightAboveGround", 320,  320.0f,  false, "u",   "v"},
      {"heightAboveSea",    305,  305.0f,  true,  "u",   "v"},
      {"heightAboveSea",    457,  457.0f,  true,  "u",   "v"},
      {"heightAboveSea",    610,  610.0f,  true,  "u",   "v"},
      {"heightAboveSea",    914,  914.0f,  true,  "u",   "v"},
      {"heightAboveSea",    1524, 1524.0f, true,  "u",   "v"},
      {"heightAboveSea",    1829, 1829.0f, true,  "u",   "v"},
      {"heightAboveSea",    2134, 2134.0f, true,  "u",   "v"},
      {"heightAboveSea",    2743, 2743.0f, true,  "u",   "v"},
      {"heightAboveSea",    3658, 3658.0f, true,  "u",   "v"},
      {"heightAboveSea",    4572, 4572.0f, true,  "u",   "v"},
   };
   // clang-format on
   return levels;
}

const std::vector<HodographManager::HeightBand>& HeightBandTable()
{
   static const std::vector<HodographManager::HeightBand> bands {
      {1000.0f, "0-1 km AGL", {255, 0, 0}},
      {3000.0f, "1-3 km AGL", {0, 190, 0}},
      {4600.0f, "3+ km AGL", {30, 100, 255}},
   };
   return bands;
}

} // namespace

class HodographManager::Impl
{
public:
   explicit Impl() : provider_ {std::make_shared<provider::RrfsDataProvider>()}
   {
   }

   // See HodographManager::DataGeneration(). Written from a fetchPool_
   // thread, read from the GUI thread.
   std::atomic<std::uint64_t> dataGeneration_ {0};

   // Owners currently drawing hodographs -- see SetDrawing().
   std::mutex                drawersMutex_;
   std::set<const void*>     drawers_;

   // Same idiom GribManager::Impl/WindBarbManager::Impl use: stop+join in
   // the destructor's *body*, since fetchPool_'s posted lambdas capture
   // `this`.
   ~Impl()
   {
      fetchPool_.stop();
      fetchPool_.join();
   }

   std::shared_ptr<provider::RrfsDataProvider> provider_;
   QTimer*                                     timer_ {nullptr};

   std::mutex  fetchMutex_;
   std::string lastKey_;

   boost::asio::thread_pool fetchPool_ {1};
};

HodographManager::HodographManager() : p(std::make_unique<Impl>())
{
   p->timer_ = new QTimer(this);
   connect(p->timer_, &QTimer::timeout, this, &HodographManager::Poll);
   p->timer_->start(kPollIntervalMs_);

   // Deferred for the same reason as GribManager's/WindBarbManager's own
   // first poll -- this manager is constructed during
   // HodographLayer::Initialize(), and Poll() does blocking network I/O.
   QTimer::singleShot(0, this, &HodographManager::Poll);
}

HodographManager::~HodographManager() = default;

namespace
{
std::weak_ptr<HodographManager> instanceRef_ {};
std::mutex                      instanceMutex_ {};
} // namespace

std::shared_ptr<HodographManager> HodographManager::Instance()
{
   std::unique_lock lock(instanceMutex_);

   std::shared_ptr<HodographManager> instance = instanceRef_.lock();
   if (instance == nullptr)
   {
      instance     = std::make_shared<HodographManager>();
      instanceRef_ = instance;
   }

   return instance;
}

std::shared_ptr<HodographManager> HodographManager::InstanceIfExists()
{
   std::unique_lock lock(instanceMutex_);
   return instanceRef_.lock();
}

void HodographManager::SetDrawing(const void* owner, bool drawing)
{
   bool wasDrawing = false;
   bool nowDrawing = false;
   {
      std::lock_guard lock(p->drawersMutex_);
      wasDrawing = !p->drawers_.empty();
      if (drawing)
      {
         p->drawers_.insert(owner);
      }
      else
      {
         p->drawers_.erase(owner);
      }
      nowDrawing = !p->drawers_.empty();
   }

   if (!wasDrawing && nowDrawing)
   {
      // Deferred: this is called from a layer's Render(), and Poll() does
      // blocking network I/O that must not stall painting.
      QTimer::singleShot(0, this, &HodographManager::Poll);
   }
}

const std::vector<HodographManager::Level>& HodographManager::Levels()
{ return LevelTable(); }

std::uint64_t HodographManager::DataGeneration() const
{ return p->dataGeneration_; }

std::string HodographManager::GetUFramePath(std::size_t levelIndex)
{ return UFramePath(levelIndex); }

std::string HodographManager::GetVFramePath(std::size_t levelIndex)
{ return VFramePath(levelIndex); }

std::string HodographManager::GetTerrainFramePath()
{ return TerrainFramePath(); }

void HodographManager::SetCycle(std::chrono::system_clock::time_point cycleTime)
{
   p->provider_->SetCycle(cycleTime);
   Poll();
}

void HodographManager::UseLatestCycle()
{
   p->provider_->UseLatestCycle();
   Poll();
}

void HodographManager::SetForecastHour(int hour)
{
   p->provider_->SetForecastHour(hour);
   Poll();
}

const std::vector<HodographManager::HeightBand>& HodographManager::HeightBands()
{ return HeightBandTable(); }

const HodographManager::HeightBand&
HodographManager::BandForHeight(float heightMeters)
{
   const auto& bands = HeightBandTable();
   for (const auto& band : bands)
   {
      if (heightMeters <= band.maxHeightMeters)
      {
         return band;
      }
   }
   return bands.back();
}

void HodographManager::Poll()
{
   {
      // Nothing is drawing hodographs (no layer yet, or every one is zoomed
      // out / hidden): don't list S3, download or decode. SetDrawing() polls
      // as soon as one starts.
      std::lock_guard lock(p->drawersMutex_);
      if (p->drawers_.empty())
      {
         return;
      }
   }

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

   logger_->info("New RRFS file for hodographs: {}", latestKey);

   const std::string cachedPath = CachedDownloadPath(latestKey);
   if (std::filesystem::exists(cachedPath))
   {
      // Already cached, quite possibly by GribManager(Rrfs) fetching the
      // exact same file -- decode is cheap, do it right here rather than
      // bouncing to the background pool.
      if (ApplyCachedDownload(latestKey))
      {
         std::lock_guard lock(p->fetchMutex_);
         p->lastKey_ = latestKey;
      }
      return;
   }

   std::shared_ptr<provider::RrfsDataProvider> provider = p->provider_;
   auto statusManager = manager::StatusManager::Instance();

   boost::asio::post(
      p->fetchPool_,
      [this, latestKey, provider, statusManager]()
      {
         std::filesystem::create_directories(
            std::filesystem::path(CachedDownloadPath(latestKey)).parent_path());

         auto downloaded = provider->DownloadRaw(
            latestKey,
            CachedDownloadPath(latestKey),
            [&statusManager](std::int64_t bytesReceived,
                             std::int64_t totalBytes)
            {
               statusManager->ReportProgress(
                  "hodograph", "Hodograph", bytesReceived, totalBytes);
            });
         statusManager->ReportComplete("hodograph");

         if (!downloaded.has_value())
         {
            logger_->warn("Failed to download {}", latestKey);
            return;
         }

         // Shares GribManager's download cache -- without this, these
         // downloads were never evicted.
         GribManager::NoteCachedDownload(*downloaded);

         if (ApplyCachedDownload(latestKey))
         {
            std::lock_guard lock(p->fetchMutex_);
            p->lastKey_ = latestKey;
         }
      });
}

bool HodographManager::ApplyCachedDownload(const std::string& key)
{
   const std::string cachedPath = CachedDownloadPath(key);

   // colorOffset/colorScale/noDataThreshold are meaningless here (raw u/v/
   // terrain values are read directly by HodographLayer, never colorized),
   // same placeholder reasoning as WindBarbManager's own decodeField.
   // contourInterval (0.0, unused) must still be passed whenever
   // typeOfLevel/topLevel/bottomLevel are, per decode_grib's own CLI
   // ordering -- see its usage string.
   auto decodeField = [&](const std::string& shortName,
                          const std::string& typeOfLevel,
                          long               level,
                          const std::string& outputPath) -> bool
   {
      const std::string tmpPath = outputPath + ".tmp";

      QStringList args {QString::fromStdString(cachedPath),
                        QString::fromStdString(tmpPath),
                        QString::number(0.0),
                        QString::number(1.0),
                        QString::number(-999.0),
                        QString::fromStdString(shortName),
                        QString::number(0.0),
                        QString::fromStdString(typeOfLevel),
                        QString::number(level),
                        QString::number(level)};

      QProcess decodeProcess;
      decodeProcess.start(QString::fromStdString(DecodeGribPath()), args);

      if (!decodeProcess.waitForFinished(10000) ||
          decodeProcess.exitCode() != 0)
      {
         logger_->warn("decode_grib failed for {} ({} @ {} {}): {}",
                       cachedPath,
                       shortName,
                       typeOfLevel,
                       level,
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

   bool allOk = true;

   const auto& levels = LevelTable();
   for (std::size_t i = 0; i < levels.size(); ++i)
   {
      const auto& level = levels[i];
      allOk &= decodeField(
         level.uShortName, level.typeOfLevel, level.level, UFramePath(i));
      allOk &= decodeField(
         level.vShortName, level.typeOfLevel, level.level, VFramePath(i));
   }

   // Terrain height, metres -- always "surface" (no disambiguation
   // needed, single message in the file), read once regardless of level
   // count. colorOffset/colorScale/noDataThreshold placeholders same as
   // above.
   {
      const std::string tmpPath = TerrainFramePath() + ".tmp";

      QProcess decodeProcess;
      decodeProcess.start(QString::fromStdString(DecodeGribPath()),
                          {QString::fromStdString(cachedPath),
                           QString::fromStdString(tmpPath),
                           QString::number(0.0),
                           QString::number(1.0),
                           QString::number(-999.0),
                           QString::fromStdString("orog")});

      if (!decodeProcess.waitForFinished(10000) ||
          decodeProcess.exitCode() != 0)
      {
         logger_->warn("decode_grib failed for {} (orog): {}",
                       cachedPath,
                       decodeProcess.readAllStandardError().toStdString());
         std::filesystem::remove(tmpPath);
         allOk = false;
      }
      else
      {
         std::error_code ec;
         util::ReplaceFileWithRetry(tmpPath, TerrainFramePath(), ec);
         if (ec)
         {
            logger_->warn(
               "Could not replace {}: {}", TerrainFramePath(), ec.message());
            std::filesystem::remove(tmpPath);
            allOk = false;
         }
      }
   }

   if (allOk)
   {
      logger_->info("Updated hodograph frames ({} levels)", levels.size());
      ++p->dataGeneration_;
      Q_EMIT HodographDataReady();
      return true;
   }

   return false;
}

} // namespace scwx::qt::manager
