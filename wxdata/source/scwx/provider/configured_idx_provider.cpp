#include <scwx/provider/configured_idx_provider.hpp>
#include <scwx/util/logger.hpp>
#include <scwx/util/time.hpp>

#include <algorithm>

#include <fmt/chrono.h>

namespace scwx::provider
{

static const std::string logPrefix_ =
   "scwx::provider::configured_idx_provider";
static const auto logger_ = scwx::util::Logger::Create(logPrefix_);

using util::grib_model_config::SourceSpec;

class ConfiguredIdxProvider::Impl
{
public:
   explicit Impl(SourceSpec source) : source_ {std::move(source)} {}

   SourceSpec source_;

   bool                                  useLatestCycle_ {true};
   std::chrono::system_clock::time_point cycleOverride_ {};
   int                                   forecastHour_ {0};
};

ConfiguredIdxProvider::ConfiguredIdxProvider(const SourceSpec& source) :
    // The radarSite slot is just a label for this provider family.
    IdxModelProvider("configured-idx", source.bucket, source.region),
    p(std::make_unique<Impl>(source))
{
   p->forecastHour_ = source.minForecastHour;
}

ConfiguredIdxProvider::~ConfiguredIdxProvider() = default;

std::chrono::system_clock::time_point
ConfiguredIdxProvider::LatestCycle(const SourceSpec&                    source,
                                   std::chrono::system_clock::time_point now)
{
   using namespace std::chrono;

   const auto newest =
      floor<hours>(now - hours {std::max(0, source.availabilityLagHours)});

   // Walk back hour by hour to the newest cycle the model actually runs. A
   // model with one cycle a day is at most 23 steps from any hour.
   auto candidate = newest;
   for (int i = 0; i < 48; ++i, candidate -= hours {1})
   {
      const int hour =
         static_cast<int>(duration_cast<hours>(candidate - floor<days>(candidate))
                             .count());
      if (std::find(source.cycleHours.begin(), source.cycleHours.end(), hour) !=
          source.cycleHours.end())
      {
         return candidate;
      }
   }

   return newest;
}

int ConfiguredIdxProvider::SnapHour(const SourceSpec& source, int hour)
{
   const int low  = source.minForecastHour;
   const int high = std::max(low, source.maxForecastHour);
   const int step = std::max(1, source.forecastHourStep);

   hour = std::clamp(hour, low, high);

   const int steps = (hour - low + step - 1) / step;
   return std::min(high, low + steps * step);
}

std::string
ConfiguredIdxProvider::BuildKey(const SourceSpec&                    source,
                                std::chrono::system_clock::time_point cycle,
                                int                                   hour)
{
   using namespace std::chrono;

   const auto cycleDay  = floor<days>(cycle);
   const int  cycleHour = static_cast<int>(
      duration_cast<hours>(cycle - cycleDay).count());

   return util::grib_model_config::ExpandKeyPattern(
      source.keyPattern,
      fmt::format("{0:%Y%m%d}", fmt::gmtime(cycleDay)),
      cycleHour,
      SnapHour(source, hour));
}

void ConfiguredIdxProvider::SetCycle(
   std::chrono::system_clock::time_point cycleTime)
{
   p->cycleOverride_  = std::chrono::floor<std::chrono::hours>(cycleTime);
   p->useLatestCycle_ = false;
}

void ConfiguredIdxProvider::UseLatestCycle()
{
   p->useLatestCycle_ = true;
}

bool ConfiguredIdxProvider::IsUsingLatestCycle() const
{
   return p->useLatestCycle_;
}

std::chrono::system_clock::time_point ConfiguredIdxProvider::CurrentCycle() const
{
   return p->useLatestCycle_ ?
             LatestCycle(p->source_, util::time::now()) :
             p->cycleOverride_;
}

void ConfiguredIdxProvider::SetForecastHour(int hour)
{
   p->forecastHour_ = hour;
}

int ConfiguredIdxProvider::ForecastHour() const
{
   return p->forecastHour_;
}

std::string ConfiguredIdxProvider::BuildKeyFor(
   std::chrono::system_clock::time_point cycle, int hour) const
{
   return BuildKey(p->source_, cycle, hour);
}

bool ConfiguredIdxProvider::RunsCycleAt(
   std::chrono::system_clock::time_point cycle) const
{
   using namespace std::chrono;
   const int hour = static_cast<int>(
      duration_cast<hours>(cycle - floor<days>(cycle)).count());
   return std::find(p->source_.cycleHours.begin(),
                    p->source_.cycleHours.end(),
                    hour) != p->source_.cycleHours.end();
}

int ConfiguredIdxProvider::MinForecastHourFor() const
{
   return p->source_.minForecastHour;
}

int ConfiguredIdxProvider::MaxForecastHourFor(
   std::chrono::system_clock::time_point /* cycle */) const
{
   return std::max(p->source_.minForecastHour, p->source_.maxForecastHour);
}

int ConfiguredIdxProvider::SnapForecastHourFor(
   std::chrono::system_clock::time_point /* cycle */, int hour) const
{
   return SnapHour(p->source_, hour);
}

std::chrono::system_clock::time_point
ConfiguredIdxProvider::GetTimePointByKey(const std::string& key) const
{
   logger_->debug("Cannot recover a time from a configured key: \"{}\"", key);
   return {};
}

std::shared_ptr<wsr88d::NexradFile>
ConfiguredIdxProvider::LoadObjectByKey(const std::string& /* key */)
{
   logger_->warn("LoadObjectByKey() is not applicable to GRIB models");
   return nullptr;
}

std::shared_ptr<wsr88d::NexradFile> ConfiguredIdxProvider::LoadObjectByTime(
   std::chrono::system_clock::time_point /* time */)
{
   logger_->warn("LoadObjectByTime() is not applicable to GRIB models");
   return nullptr;
}

std::optional<std::string>
ConfiguredIdxProvider::FetchField(const std::string&              key,
                                  const std::string&              parameter,
                                  const std::string&              level,
                                  const std::string&              qualifier,
                                  const std::string&              outputPath,
                                  const DownloadProgressCallback& progressCallback)
{
   return DownloadGribMessageByIndex(p->source_.bucket,
                                     key,
                                     parameter,
                                     level,
                                     qualifier,
                                     outputPath,
                                     progressCallback);
}

std::string ConfiguredIdxProvider::GetPrefix(
   std::chrono::system_clock::time_point /* date */)
{
   // The exact object for the current selection; only reached through the
   // generic listing API, which GribManager doesn't use for idx models.
   return BuildKeyFor(CurrentCycle(), p->forecastHour_);
}

} // namespace scwx::provider
