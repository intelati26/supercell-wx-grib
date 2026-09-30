#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A user-importable GRIB model is a folder holding two files:
//
//   model.json    -- what rarely changes: name, source (bucket, key
//                    pattern, cycles), and per-model display defaults.
//   products.csv  -- what people edit in bulk: one row per field, in a
//                    spreadsheet. Blank display cells inherit the JSON's
//                    "defaults" block.
//
// Both files are untrusted text. Nothing here throws or touches the
// network; problems come back as human-readable lines.
namespace scwx::util::grib_model_config
{

// Locates one field inside a model's bundled GRIB2 file: wgrib2's own
// .idx vocabulary (PARAM/LEVEL/QUALIFIER), matched literally by
// grib_idx::FindRecord(). The idx STEP text is deliberately absent: it
// changes per forecast hour ("1 hour fcst", "2 hour fcst"), so a match
// on it would only ever work for one file.
struct IndexSpec
{
   std::string parameter; // e.g. "TMP"
   std::string level;     // e.g. "2 m above ground"
   std::string qualifier; // e.g. "ens std dev"; empty = unqualified only
};

// The values DisplaySpec::quantity may take; the qt layer maps each onto its
// unit-conversion category (see qt::manager::PhysicalQuantity). "none" shows
// the raw value with DisplaySpec::units as-is.
inline constexpr std::array<std::string_view, 5> kQuantityNames {
   "none",
   "temperature_kelvin",
   "speed_meters_per_second",
   "accumulation_millimeters",
   "pressure_pascals"};

// How to draw a decoded field. Mirrors the display half of the built-in
// qt::manager ProductConfig table.
struct DisplaySpec
{
   std::string type = "fill";            // "fill" | "contour"
   std::string units;                    // shown as-is when quantity is "none"
   std::string quantity        = "none"; // "none" | "temperature_kelvin" | ...
   float       colorOffset     = 0.0f;
   float       colorScale      = 1.0f;
   float       noDataThreshold = -999.0f;
   float       contourInterval = 0.0f; // required (> 0) for "contour"
};

struct ProductSpec
{
   std::string name; // dock dropdown label; unique within a model
   IndexSpec   index;

   // eccodes shortName decode_grib selects once the one downloaded message
   // is decoded ("2t", not the idx's "TMP" -- the vocabularies differ).
   std::string shortName;
   DisplaySpec display;
};

struct SourceSpec
{
   std::string      bucket;
   std::string      region = "us-east-1"; // AWS region the bucket lives in
   std::string      keyPattern;           // see ExpandKeyPattern()
   std::vector<int> cycleHours;
   int              maxForecastHour = 0;

   // First forecast hour that exists (NBM, for one, has no F000), and the
   // spacing between published hours after it.
   int minForecastHour  = 0;
   int forecastHourStep = 1;

   // How long after a cycle's nominal hour its files are reliably there;
   // "latest cycle" is the newest one at least this old.
   int availabilityLagHours = 3;

   // Only ".idx" is supported today (the fetch path builds <key>.idx).
   std::string idxSuffix = ".idx";
};

struct ModelConfig
{
   std::string              name;
   std::string              kind            = "idx"; // v1 supports only "idx"
   bool                     alwaysOneActive = false;
   SourceSpec               source;
   DisplaySpec              defaults; // inherited by blank CSV cells
   std::vector<ProductSpec> products;
};

struct ParseResult
{
   // Set only when `errors` is empty.
   std::optional<ModelConfig> config;

   // Fatal: the model can't load. JSON problems are prefixed with the
   // offending JSON path, CSV ones with "row N (column)".
   std::vector<std::string> errors;

   // Non-fatal: a CSV row was skipped. A typo in one of 40 rows shouldn't
   // take the whole model down, but must never pass silently.
   std::vector<std::string> warnings;
};

// model.json -> ModelConfig with no products yet. A "products" key is an
// error: the CSV is the only source of products.
ParseResult ParseModelSettings(std::string_view jsonText);

struct ProductsResult
{
   std::vector<ProductSpec> products;
   std::vector<std::string> errors;   // fatal: no usable header, no rows
   std::vector<std::string> warnings; // skipped rows
};

// products.csv -> rows. Columns are matched by header name (any order,
// case-insensitive); required: name, parameter, level, short_name. Handles
// a UTF-8 BOM, CRLF/LF, RFC 4180 quoting, and sniffs ',' vs ';' (European
// spreadsheets export ';' and write decimal commas, accepted only then).
// Rows starting with '#' and entirely blank rows are ignored. A bad row
// is skipped with a warning; duplicate names skip the later row.
ProductsResult ParseProductsCsv(std::string_view   csvText,
                                const DisplaySpec& defaults);

// Reads <folder>/model.json and <folder>/products.csv and combines them.
ParseResult LoadModelFolder(const std::string& folder);

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
