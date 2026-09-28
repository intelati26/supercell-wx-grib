#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scwx::util::grib_model_config
{

// Locates one field inside a model's bundled GRIB2 file: wgrib2's own
// .idx vocabulary (PARAM/LEVEL/STEP/QUALIFIER), matched literally by
// grib_idx::FindRecord(). Not eccodes shortNames -- see ProductSpec.
struct IndexSpec
{
   std::string parameter; // e.g. "TMP"
   std::string level;     // e.g. "2 m above ground"
   std::string qualifier; // e.g. "ens std dev"; empty = unqualified only
};

// How to draw a decoded field. Mirrors the display half of the built-in
// qt::manager ProductConfig table.
struct DisplaySpec
{
   std::string type     = "fill"; // "fill" | "contour"
   std::string units;             // shown as-is when quantity is "none"
   std::string quantity = "none"; // "none" | "temperature_kelvin" | ...
   float       colorOffset      = 0.0f;
   float       colorScale       = 1.0f;
   float       noDataThreshold  = -999.0f;
   float       contourInterval  = 0.0f; // only meaningful for "contour"
};

struct ProductSpec
{
   std::string name; // dock dropdown label
   IndexSpec   index;

   // eccodes shortName decode_grib selects once the one downloaded message
   // is decoded ("2t", not the idx's "TMP" -- the vocabularies differ).
   std::string shortName;
   DisplaySpec display;
};

struct SourceSpec
{
   std::string      bucket;
   std::string      keyPattern; // see ExpandKeyPattern()
   std::vector<int> cycleHours;
   int              maxForecastHour = 0;
   std::string      idxSuffix       = ".idx";
};

struct ModelConfig
{
   std::string              name;
   std::string              kind = "idx"; // v1 supports only "idx"
   bool                     alwaysOneActive = false;
   SourceSpec               source;
   std::vector<ProductSpec> products;
};

// Config files are untrusted text: a malformed file yields errors (one
// human-readable line each, prefixed with the offending JSON path), never
// an exception and never a partially-filled config.
struct ParseResult
{
   std::optional<ModelConfig> config;
   std::vector<std::string>   errors;
};

ParseResult ParseModelConfig(std::string_view jsonText);

// Empty string if `keyPattern` is acceptable, else a reason. Runs before a
// pattern is ever expanded or used to build a request path.
std::string ValidateKeyPattern(std::string_view keyPattern);

// Expands {yyyymmdd}, {hh}, {fh2}, {fh3} (zero-padded to 2/3 digits).
// Assumes ValidateKeyPattern() already passed.
std::string ExpandKeyPattern(std::string_view keyPattern,
                             std::string_view yyyymmdd,
                             int              cycleHour,
                             int              forecastHour);

} // namespace scwx::util::grib_model_config
