// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <scwx/provider/aws_nexrad_data_provider.hpp>
#include <scwx/util/grib_idx.hpp>
#include <scwx/util/logger.hpp>

#include <optional>
#include <string>
#include <vector>

namespace scwx::qt::manager
{

// Downloads the GRIB2 messages `selectors` name from S3 object `key` into
// `cachedPath` -- just those, never the whole object: RRFS's is ~350MB and
// RTMA's ~84MB for fields of a few megabytes. If the object's idx is not there
// yet or does not list a field, or the transfer fails, this fails
// (std::nullopt) and the caller tries again at its next poll or hour change;
// falling back to the whole object would turn a briefly lagging idx into
// hundreds of megabytes, and a field the idx lacks is almost certainly not in
// the file either. With no selectors (a product with no entry in
// grib_field_selectors) it downloads the whole object, which decodes
// identically.
//
// `TypedProvider` is RrfsDataProvider or RtmaDataProvider: each exposes the
// idx download and the whole-object DownloadRaw() for its own bucket.
// `label` names what is being fetched in the log. `status`, if given, receives
// how a field download ended (Downloaded for a whole-object download), so a
// caller can tell "this cycle's file does not hold the field" -- permanent, as
// with a product an hourly cycle lacks -- from a download that failed and may
// work later.
template<typename TypedProvider>
std::optional<std::string> DownloadFieldsOrObject(
   TypedProvider&                                          provider,
   const std::string&                                      label,
   const std::vector<scwx::util::grib_idx::FieldSelector>& selectors,
   const std::string&                                      key,
   const std::string&                                      cachedPath,
   const scwx::provider::AwsNexradDataProvider::DownloadProgressCallback&
                                                               progress,
   scwx::provider::AwsNexradDataProvider::FieldDownloadStatus* statusOut =
      nullptr)
{
   using Status = scwx::provider::AwsNexradDataProvider::FieldDownloadStatus;

   if (selectors.empty())
   {
      auto whole = provider.DownloadRaw(key, cachedPath, progress);
      if (statusOut != nullptr)
      {
         *statusOut = whole.has_value() ? Status::Downloaded : Status::Failed;
      }
      return whole;
   }

   const Status status =
      provider.DownloadFields(key, selectors, cachedPath, progress);
   if (statusOut != nullptr)
   {
      *statusOut = status;
   }

   if (status == Status::Downloaded)
   {
      return cachedPath;
   }

   static const auto logger =
      scwx::util::Logger::Create("scwx::qt::manager::grib_field_download");
   logger->warn("{}: {} for {}",
                label,
                status == Status::IndexUnavailable ? "no index yet" :
                status == Status::NoMatchingRecord ? "field not in the index" :
                                                     "download failed",
                key);
   return std::nullopt;
}

} // namespace scwx::qt::manager
