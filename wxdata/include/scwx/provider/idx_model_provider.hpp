#pragma once

#include <scwx/provider/aws_nexrad_data_provider.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace scwx::provider
{

// What GribManager needs from a model whose fields are fetched one at a time
// through a wgrib2-style .idx sidecar (see AwsNexradDataProvider::
// DownloadGribMessageByIndex()): which cycle and forecast hour are selected,
// and the model's own rules for which keys and hours exist.
//
// NbmDataProvider is the built-in implementation, with NBM's hard-coded
// rules; ConfiguredIdxProvider is driven by a user-imported model config
// (scwx::util::grib_model_config). GribManager talks to both through this
// interface, so a new idx-style model is data, not a new code path.
class IdxModelProvider : public AwsNexradDataProvider
{
public:
   using AwsNexradDataProvider::AwsNexradDataProvider;
   ~IdxModelProvider() override = default;

   virtual void SetCycle(std::chrono::system_clock::time_point cycleTime) = 0;
   virtual void UseLatestCycle()                                          = 0;
   [[nodiscard]] virtual bool IsUsingLatestCycle() const                  = 0;
   [[nodiscard]] virtual std::chrono::system_clock::time_point
   CurrentCycle() const = 0;

   virtual void              SetForecastHour(int hour) = 0;
   [[nodiscard]] virtual int ForecastHour() const      = 0;

   // Deterministic key for `cycle`'s `hour` -- no network, no state.
   [[nodiscard]] virtual std::string
   BuildKeyFor(std::chrono::system_clock::time_point cycle, int hour) const = 0;

   // First and last forecast hour that exist for `cycle`.
   [[nodiscard]] virtual int MinForecastHourFor() const = 0;
   [[nodiscard]] virtual int
   MaxForecastHourFor(std::chrono::system_clock::time_point cycle) const = 0;

   // The smallest real forecast hour >= `hour` for `cycle`, clamped to the
   // valid range -- a slider or prefetch loop always gets a fetchable hour.
   [[nodiscard]] virtual int
   SnapForecastHourFor(std::chrono::system_clock::time_point cycle,
                       int                                    hour) const = 0;

   virtual std::optional<std::string>
   FetchField(const std::string&              key,
              const std::string&              parameter,
              const std::string&              level,
              const std::string&              qualifier,
              const std::string&              outputPath,
              const DownloadProgressCallback& progressCallback = nullptr) = 0;
};

} // namespace scwx::provider
