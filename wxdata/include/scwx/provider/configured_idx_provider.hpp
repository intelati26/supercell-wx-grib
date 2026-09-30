#pragma once

#include <scwx/provider/idx_model_provider.hpp>
#include <scwx/util/grib_model_config.hpp>

#include <chrono>
#include <string>

namespace scwx::provider
{

// An idx-style GRIB model defined by a user-imported model.json (see
// scwx::util::grib_model_config): the bucket, key pattern, cycle hours and
// forecast-hour range all come from the config instead of being compiled in,
// the way NbmDataProvider's are. Fields are fetched one at a time through the
// bucket's .idx sidecar, exactly like NBM's.
class ConfiguredIdxProvider : public IdxModelProvider
{
public:
   explicit ConfiguredIdxProvider(
      const util::grib_model_config::SourceSpec& source);
   ~ConfiguredIdxProvider() override;

   ConfiguredIdxProvider(const ConfiguredIdxProvider&)            = delete;
   ConfiguredIdxProvider& operator=(const ConfiguredIdxProvider&) = delete;
   ConfiguredIdxProvider(ConfiguredIdxProvider&&)                 = delete;
   ConfiguredIdxProvider& operator=(ConfiguredIdxProvider&&)      = delete;

   // The model's rules as pure functions of its source spec, so they can be
   // checked without a network or a provider instance.

   // Newest cycle whose hour is in cycleHours and that is at least
   // availabilityLagHours old at `now`.
   static std::chrono::system_clock::time_point
   LatestCycle(const util::grib_model_config::SourceSpec& source,
               std::chrono::system_clock::time_point      now);

   // `hour` clamped to [minForecastHour, maxForecastHour], then rounded up to
   // the next published hour (minForecastHour + n * forecastHourStep).
   static int SnapHour(const util::grib_model_config::SourceSpec& source,
                       int                                        hour);

   // key_pattern expanded for `cycle` (its UTC date and hour) and the
   // snapped-in-range `hour`.
   static std::string
   BuildKey(const util::grib_model_config::SourceSpec& source,
            std::chrono::system_clock::time_point      cycle,
            int                                        hour);

   void SetCycle(std::chrono::system_clock::time_point cycleTime) override;
   void UseLatestCycle() override;
   [[nodiscard]] bool IsUsingLatestCycle() const override;
   [[nodiscard]] std::chrono::system_clock::time_point
   CurrentCycle() const override;

   void              SetForecastHour(int hour) override;
   [[nodiscard]] int ForecastHour() const override;

   [[nodiscard]] std::string
   BuildKeyFor(std::chrono::system_clock::time_point cycle,
               int                                    hour) const override;
   [[nodiscard]] int MinForecastHourFor() const override;
   [[nodiscard]] int MaxForecastHourFor(
      std::chrono::system_clock::time_point cycle) const override;
   [[nodiscard]] int
   SnapForecastHourFor(std::chrono::system_clock::time_point cycle,
                       int                                    hour) const override;

   // Key patterns aren't invertible in general, so the generic
   // list-and-find API can't recover a time from a key; GribManager never
   // uses it for idx models (it builds keys with BuildKeyFor()).
   [[nodiscard]] std::chrono::system_clock::time_point
   GetTimePointByKey(const std::string& key) const override;

   std::shared_ptr<wsr88d::NexradFile>
   LoadObjectByKey(const std::string& key) override;
   std::shared_ptr<wsr88d::NexradFile>
   LoadObjectByTime(std::chrono::system_clock::time_point time) override;

   std::optional<std::string>
   FetchField(const std::string&              key,
              const std::string&              parameter,
              const std::string&              level,
              const std::string&              qualifier,
              const std::string&              outputPath,
              const DownloadProgressCallback& progressCallback =
                 nullptr) override;

protected:
   std::string GetPrefix(std::chrono::system_clock::time_point date) override;

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::provider
