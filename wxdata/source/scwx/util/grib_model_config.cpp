#include <scwx/util/grib_model_config.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

#include <boost/json.hpp>

namespace scwx::util::grib_model_config
{

namespace json = boost::json;

namespace
{

constexpr std::string_view kSupportedKind = "idx";

// One table drives both front ends, so the JSON "defaults" block and a CSV
// cell can never disagree about what a legal value is.
struct NumericField
{
   const char* key;
   double      lo;
   double      hi;
   float DisplaySpec::*member;
};

constexpr NumericField kNumericFields[] = {
   {"color_offset", -1e9, 1e9, &DisplaySpec::colorOffset},
   {"color_scale", 1e-9, 1e9, &DisplaySpec::colorScale},
   {"no_data_threshold", -1e9, 1e9, &DisplaySpec::noDataThreshold},
   {"contour_interval", 0.0, 1e9, &DisplaySpec::contourInterval}};

std::string RangeText(const NumericField& f)
{
   return "out of range [" + std::to_string(f.lo) + ", " +
          std::to_string(f.hi) + "]";
}

struct DisplayProblem
{
   const char* field;
   const char* message;
};

// Cross-field rules shared by both front ends.
std::optional<DisplayProblem> CheckDisplay(const DisplaySpec& d)
{
   if (d.type != "fill" && d.type != "contour")
   {
      return DisplayProblem {"type", "must be \"fill\" or \"contour\""};
   }
   if (d.type == "contour" && d.contourInterval <= 0.0f)
   {
      return DisplayProblem {"contour_interval",
                             "required (> 0) when type is contour"};
   }
   return std::nullopt;
}

// ---------------------------------------------------------------- JSON

struct Reader
{
   std::vector<std::string>& errors;

   void Error(const std::string& path, const std::string& message)
   {
      errors.push_back(path + ": " + message);
   }

   const json::object* Object(const json::object& parent,
                              const char*         key,
                              bool                required)
   {
      auto it = parent.find(key);
      if (it == parent.end())
      {
         if (required)
         {
            Error(key, "missing required object");
         }
         return nullptr;
      }
      if (!it->value().is_object())
      {
         Error(key, "expected an object");
         return nullptr;
      }
      return &it->value().as_object();
   }

   bool String(const json::object& o,
               const std::string&  path,
               const char*         key,
               bool                required,
               std::string&        out)
   {
      auto it = o.find(key);
      if (it == o.end())
      {
         if (required)
         {
            Error(path + key, "missing required string");
         }
         return false;
      }
      if (!it->value().is_string())
      {
         Error(path + key, "expected a string");
         return false;
      }
      out = std::string(it->value().as_string());
      if (required && out.empty())
      {
         Error(path + key, "must not be empty");
         return false;
      }
      return true;
   }

   void Bool(const json::object& o,
             const std::string&  path,
             const char*         key,
             bool&               out)
   {
      auto it = o.find(key);
      if (it == o.end())
      {
         return;
      }
      if (!it->value().is_bool())
      {
         Error(path + key, "expected true or false");
         return;
      }
      out = it->value().as_bool();
   }

   void Display(const json::object& o, const std::string& path, DisplaySpec& d)
   {
      String(o, path, "type", false, d.type);
      String(o, path, "units", false, d.units);
      String(o, path, "quantity", false, d.quantity);

      for (const auto& f : kNumericFields)
      {
         auto it = o.find(f.key);
         if (it == o.end())
         {
            continue;
         }
         if (!it->value().is_number())
         {
            Error(path + f.key, "expected a number");
            continue;
         }
         double value = it->value().to_number<double>();
         if (!(value >= f.lo && value <= f.hi))
         {
            Error(path + f.key, RangeText(f));
            continue;
         }
         d.*(f.member) = static_cast<float>(value);
      }

      // Only when the fields above were individually fine, otherwise the
      // cross-check would just repeat an error already reported.
      if (errors.empty())
      {
         if (auto problem = CheckDisplay(d))
         {
            Error(path + problem->field, problem->message);
         }
      }
   }
};

// ----------------------------------------------------------------- CSV

using Record = std::vector<std::string>;

// RFC 4180: quoted fields may hold delimiters, quotes ("") and newlines.
// Returns false on an unterminated quote.
bool ReadCsv(std::string_view text, char delimiter, std::vector<Record>& out)
{
   Record      record;
   std::string field;
   bool        quoted    = false;
   bool        fieldOpen = false; // distinguishes "" from no field at all

   auto endField = [&]
   {
      record.push_back(std::move(field));
      field.clear();
      fieldOpen = false;
   };
   auto endRecord = [&]
   {
      if (fieldOpen || !record.empty())
      {
         endField();
         out.push_back(std::move(record));
         record.clear();
      }
   };

   for (std::size_t i = 0; i < text.size(); ++i)
   {
      const char c = text[i];
      if (quoted)
      {
         if (c == '"')
         {
            if (i + 1 < text.size() && text[i + 1] == '"')
            {
               field += '"';
               ++i;
            }
            else
            {
               quoted = false;
            }
         }
         else
         {
            field += c;
         }
      }
      else if (c == '"' && field.empty())
      {
         quoted    = true;
         fieldOpen = true;
      }
      else if (c == delimiter)
      {
         endField();
      }
      else if (c == '\r' || c == '\n')
      {
         if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
         {
            ++i;
         }
         if (!fieldOpen && record.empty())
         {
            out.push_back({}); // blank line still occupies a row number
         }
         else
         {
            endRecord();
         }
      }
      else
      {
         field += c;
         fieldOpen = true;
      }
   }
   if (quoted)
   {
      return false;
   }
   endRecord();
   return true;
}

char SniffDelimiter(std::string_view text)
{
   bool        quoted = false;
   std::size_t commas = 0;
   std::size_t semis  = 0;
   for (char c : text)
   {
      if (c == '"')
      {
         quoted = !quoted;
      }
      else if (!quoted && (c == '\n' || c == '\r'))
      {
         break;
      }
      else if (!quoted && c == ',')
      {
         ++commas;
      }
      else if (!quoted && c == ';')
      {
         ++semis;
      }
   }
   return semis > commas ? ';' : ',';
}

std::string Trim(const std::string& s)
{
   auto notSpace = [](unsigned char c) { return !std::isspace(c); };
   auto first    = std::find_if(s.begin(), s.end(), notSpace);
   auto last     = std::find_if(s.rbegin(), s.rend(), notSpace).base();
   return first < last ? std::string(first, last) : std::string {};
}

std::string Lower(std::string s)
{
   std::transform(s.begin(),
                  s.end(),
                  s.begin(),
                  [](unsigned char c) { return std::tolower(c); });
   return s;
}

bool IsBlank(const Record& rec)
{
   return std::all_of(
      rec.begin(), rec.end(), [](const std::string& c) { return Trim(c).empty(); });
}

bool IsComment(const Record& rec)
{
   return !rec.empty() && Trim(rec[0]).starts_with('#');
}

std::string RowLabel(std::size_t row, const std::string& column = {})
{
   auto label = "row " + std::to_string(row);
   return column.empty() ? label : label + " (" + column + ")";
}

} // namespace

// ---------------------------------------------------------------- API

namespace
{

// S3 bucket naming rules, close enough to catch a pasted URL or path: the
// real check is the request itself failing, but that shouldn't be how a typo
// in a user's file is found.
bool IsPlausibleBucketName(std::string_view name)
{
   if (name.size() < 3 || name.size() > 63)
   {
      return false;
   }
   const auto alnum = [](char c)
   { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
   if (!alnum(name.front()) || !alnum(name.back()) ||
       name.find("..") != std::string_view::npos)
   {
      return false;
   }
   return std::all_of(name.begin(),
                      name.end(),
                      [&alnum](char c) { return alnum(c) || c == '.' || c == '-'; });
}

bool IsPlausibleRegion(std::string_view region)
{
   return !region.empty() && region.size() <= 32 &&
          std::all_of(region.begin(),
                      region.end(),
                      [](char c)
                      {
                         return (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-';
                      });
}

} // namespace

ParseResult ParseModelSettings(std::string_view jsonText)
{
   ParseResult result;
   Reader      r {result.errors};

   boost::system::error_code ec;
   json::value               root = json::parse(jsonText, ec);
   if (ec)
   {
      result.errors.push_back("$: not valid JSON: " + ec.message());
      return result;
   }
   if (!root.is_object())
   {
      result.errors.push_back("$: top level must be an object");
      return result;
   }

   ModelConfig config;
   const auto& top = root.as_object();

   if (top.contains("products"))
   {
      r.Error("products", "not allowed here; products live in products.csv");
   }

   if (auto* model = r.Object(top, "model", true))
   {
      r.String(*model, "model.", "name", true, config.name);
      r.String(*model, "model.", "kind", false, config.kind);
      r.Bool(*model, "model.", "always_one_active", config.alwaysOneActive);
      if (config.kind != kSupportedKind)
      {
         r.Error("model.kind",
                 "unsupported kind \"" + config.kind + "\" (only \"idx\")");
      }
   }

   if (auto* source = r.Object(top, "source", true))
   {
      auto& s = config.source;
      r.String(*source, "source.", "bucket", true, s.bucket);
      r.String(*source, "source.", "key_pattern", true, s.keyPattern);
      r.String(*source, "source.", "idx_suffix", false, s.idxSuffix);
      r.String(*source, "source.", "region", false, s.region);

      // These become part of a request, so they are checked, not trusted.
      if (!s.bucket.empty() && !IsPlausibleBucketName(s.bucket))
      {
         r.Error("source.bucket",
                 "expected a bare S3 bucket name (3-63 lowercase letters, "
                 "digits, '.' or '-'), not a URL");
      }
      if (!IsPlausibleRegion(s.region))
      {
         r.Error("source.region",
                 "expected an AWS region like \"us-east-1\"");
      }
      if (s.idxSuffix != ".idx")
      {
         r.Error("source.idx_suffix",
                 "only \".idx\" is supported (wgrib2-style index files)");
      }

      if (!s.keyPattern.empty())
      {
         if (auto reason = ValidateKeyPattern(s.keyPattern); !reason.empty())
         {
            r.Error("source.key_pattern", reason);
         }
      }

      if (auto it = source->find("max_forecast_hour"); it != source->end())
      {
         if (!it->value().is_int64() || it->value().as_int64() < 0 ||
             it->value().as_int64() > 384)
         {
            r.Error("source.max_forecast_hour", "expected an integer 0-384");
         }
         else
         {
            s.maxForecastHour = static_cast<int>(it->value().as_int64());
         }
      }

      const auto optionalInt =
         [&](const char* key, int low, int high, int& out)
      {
         auto found = source->find(key);
         if (found == source->end())
         {
            return;
         }
         if (!found->value().is_int64() || found->value().as_int64() < low ||
             found->value().as_int64() > high)
         {
            r.Error(std::string("source.") + key,
                    "expected an integer " + std::to_string(low) + "-" +
                       std::to_string(high));
            return;
         }
         out = static_cast<int>(found->value().as_int64());
      };
      optionalInt("min_forecast_hour", 0, 384, s.minForecastHour);
      optionalInt("forecast_hour_step", 1, 24, s.forecastHourStep);
      optionalInt("availability_lag_hours", 0, 48, s.availabilityLagHours);
      if (s.minForecastHour > s.maxForecastHour)
      {
         r.Error("source.min_forecast_hour",
                 "may not be greater than max_forecast_hour");
      }

      auto it = source->find("cycle_hours");
      if (it == source->end() || !it->value().is_array() ||
          it->value().as_array().empty())
      {
         r.Error("source.cycle_hours", "expected a non-empty array of hours");
      }
      else
      {
         for (const auto& hour : it->value().as_array())
         {
            if (!hour.is_int64() || hour.as_int64() < 0 || hour.as_int64() > 23)
            {
               r.Error("source.cycle_hours", "hours must be integers 0-23");
               break;
            }
            s.cycleHours.push_back(static_cast<int>(hour.as_int64()));
         }
      }
   }

   if (auto* defaults = r.Object(top, "defaults", false))
   {
      r.Display(*defaults, "defaults.", config.defaults);
   }

   if (result.errors.empty())
   {
      result.config = std::move(config);
   }
   return result;
}

ProductsResult ParseProductsCsv(std::string_view csvText,
                                const DisplaySpec& defaults)
{
   ProductsResult result;

   if (csvText.starts_with("\xEF\xBB\xBF")) // UTF-8 BOM from Excel
   {
      csvText.remove_prefix(3);
   }

   const char          delimiter = SniffDelimiter(csvText);
   std::vector<Record> records;
   if (!ReadCsv(csvText, delimiter, records))
   {
      result.errors.push_back("unterminated quote");
      return result;
   }

   // Record i is spreadsheet row i+1. Skipped blank/comment records still
   // count, so row numbers match what the user sees in their editor.
   std::size_t headerIndex = 0;
   while (headerIndex < records.size() &&
          (IsBlank(records[headerIndex]) || IsComment(records[headerIndex])))
   {
      ++headerIndex;
   }
   if (headerIndex == records.size())
   {
      result.errors.push_back("no header row");
      return result;
   }

   std::vector<std::string> columns;
   for (const auto& c : records[headerIndex])
   {
      columns.push_back(Lower(Trim(c)));
   }

   auto columnOf = [&](const std::string& name) -> std::optional<std::size_t>
   {
      auto it = std::find(columns.begin(), columns.end(), name);
      if (it == columns.end())
      {
         return std::nullopt;
      }
      return static_cast<std::size_t>(it - columns.begin());
   };

   for (const char* required : {"name", "parameter", "level", "short_name"})
   {
      if (!columnOf(required))
      {
         result.errors.push_back(
            std::string("header: missing required column \"") + required + "\"");
      }
   }
   if (!result.errors.empty())
   {
      return result;
   }

   auto cell = [&](const Record& rec, const std::string& column)
   {
      auto index = columnOf(column);
      return (index && *index < rec.size()) ? Trim(rec[*index]) : std::string {};
   };

   for (std::size_t i = headerIndex + 1; i < records.size(); ++i)
   {
      const auto& rec = records[i];
      const auto  row = i + 1;

      if (IsBlank(rec) || IsComment(rec))
      {
         continue;
      }

      ProductSpec              product;
      std::vector<std::string> problems;
      product.display = defaults;

      product.name            = cell(rec, "name");
      product.index.parameter = cell(rec, "parameter");
      product.index.level     = cell(rec, "level");
      product.index.qualifier = cell(rec, "qualifier");
      product.shortName       = cell(rec, "short_name");

      const std::pair<const char*, const std::string*> requiredCells[] = {
         {"name", &product.name},
         {"parameter", &product.index.parameter},
         {"level", &product.index.level},
         {"short_name", &product.shortName}};
      for (const auto& [column, value] : requiredCells)
      {
         if (value->empty())
         {
            problems.push_back(RowLabel(row, column) + ": required");
         }
      }

      if (auto v = cell(rec, "type"); !v.empty())
      {
         product.display.type = v;
      }
      if (auto v = cell(rec, "units"); !v.empty())
      {
         product.display.units = v;
      }
      if (auto v = cell(rec, "quantity"); !v.empty())
      {
         product.display.quantity = v;
      }

      for (const auto& f : kNumericFields)
      {
         auto text = cell(rec, f.key);
         if (text.empty())
         {
            continue;
         }
         if (delimiter == ';') // decimal comma only alongside ';'
         {
            std::replace(text.begin(), text.end(), ',', '.');
         }
         errno        = 0;
         char*  end   = nullptr;
         double value = std::strtod(text.c_str(), &end);
         if (end == text.c_str() || *end != '\0' || errno == ERANGE)
         {
            problems.push_back(RowLabel(row, f.key) + ": \"" + text +
                               "\" is not a number");
         }
         else if (!(value >= f.lo && value <= f.hi))
         {
            problems.push_back(RowLabel(row, f.key) + ": " + RangeText(f));
         }
         else
         {
            product.display.*(f.member) = static_cast<float>(value);
         }
      }

      if (problems.empty())
      {
         if (auto problem = CheckDisplay(product.display))
         {
            problems.push_back(RowLabel(row, problem->field) + ": " +
                               problem->message);
         }
      }

      if (problems.empty())
      {
         const bool duplicate = std::any_of(
            result.products.begin(),
            result.products.end(),
            [&](const ProductSpec& p) { return p.name == product.name; });
         if (duplicate)
         {
            problems.push_back(RowLabel(row, "name") + ": duplicate of \"" +
                               product.name + "\"");
         }
      }

      if (!problems.empty())
      {
         for (const auto& p : problems)
         {
            result.warnings.push_back(p + " -- row skipped");
         }
         continue;
      }
      result.products.push_back(std::move(product));
   }

   if (result.products.empty())
   {
      result.errors.push_back("no valid product rows");
   }
   return result;
}

ParseResult LoadModelFolder(const std::string& folder)
{
   auto slurp = [](const std::string& path, std::string& out)
   {
      std::ifstream in(path, std::ios::binary);
      if (!in)
      {
         return false;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      out = ss.str();
      return true;
   };

   ParseResult result;
   std::string jsonText;
   std::string csvText;
   if (!slurp(folder + "/model.json", jsonText))
   {
      result.errors.push_back("model.json: cannot read");
   }
   if (!slurp(folder + "/products.csv", csvText))
   {
      result.errors.push_back("products.csv: cannot read");
   }
   if (!result.errors.empty())
   {
      return result;
   }

   result = ParseModelSettings(jsonText);
   for (auto& e : result.errors)
   {
      e = "model.json: " + e;
   }
   if (!result.errors.empty())
   {
      return result;
   }

   auto products = ParseProductsCsv(csvText, result.config->defaults);
   for (const auto& e : products.errors)
   {
      result.errors.push_back("products.csv: " + e);
   }
   for (const auto& w : products.warnings)
   {
      result.warnings.push_back("products.csv: " + w);
   }
   if (!result.errors.empty())
   {
      result.config.reset();
      return result;
   }

   result.config->products = std::move(products.products);
   return result;
}

std::string ValidateKeyPattern(std::string_view keyPattern)
{
   // The pattern becomes an S3 object key inside the model's own bucket, and
   // comes from a file the user imported, so this is an allow-list rather
   // than a list of known-bad things: anything not explicitly permitted is
   // rejected, with a reason specific enough to fix the file.
   constexpr std::size_t kMaxLength = 512;

   if (keyPattern.empty())
   {
      return "key_pattern is empty";
   }
   if (keyPattern.size() > kMaxLength)
   {
      return "key_pattern is longer than " + std::to_string(kMaxLength) +
             " characters";
   }
   if (keyPattern.front() == '/')
   {
      return "key_pattern must be relative to the bucket (no leading '/')";
   }
   if (keyPattern.back() == '/')
   {
      return "key_pattern must name a file, not a folder (trailing '/')";
   }
   if (keyPattern.find("//") != std::string_view::npos)
   {
      return "key_pattern has an empty path segment ('//')";
   }
   if (keyPattern.find("..") != std::string_view::npos)
   {
      return "key_pattern may not contain '..'";
   }
   if (keyPattern == "." || keyPattern.starts_with("./") ||
       keyPattern.ends_with("/.") ||
       keyPattern.find("/./") != std::string_view::npos)
   {
      return "key_pattern may not contain a '.' path segment";
   }

   constexpr std::string_view kTokens[] = {
      "{yyyymmdd}", "{hh}", "{fh2}", "{fh3}"};

   for (std::size_t i = 0; i < keyPattern.size();)
   {
      const char c = keyPattern[i];

      if (c == '{')
      {
         const auto rest = keyPattern.substr(i);
         const auto token =
            std::find_if(std::begin(kTokens),
                         std::end(kTokens),
                         [&rest](std::string_view t)
                         { return rest.starts_with(t); });
         if (token == std::end(kTokens))
         {
            const auto close = rest.find('}');
            return "unknown or unterminated placeholder '" +
                   std::string(rest.substr(
                      0, close == std::string_view::npos ? rest.size() :
                                                           close + 1)) +
                   "' (allowed: {yyyymmdd}, {hh}, {fh2}, {fh3})";
         }
         i += token->size();
         continue;
      }

      const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                           c == '-' || c == '/';
      if (!allowed)
      {
         // Covers ':' (so no URL scheme), '\\', '%' (no percent-encoding
         // tricks), '?', '#', spaces and control or non-ASCII bytes.
         std::string shown = (c >= 0x20 && c < 0x7f) ?
                                std::string("'") + c + "'" :
                                "byte 0x" + [&]
         {
            char buf[8];
            std::snprintf(
               buf, sizeof(buf), "%02X", static_cast<unsigned char>(c));
            return std::string(buf);
         }();
         return "key_pattern contains a character that isn't allowed: " +
                shown +
                " (letters, digits, '.', '_', '-', '/' and the placeholders "
                "only)";
      }
      ++i;
   }

   return {};
}

std::string ExpandKeyPattern(std::string_view keyPattern,
                             std::string_view yyyymmdd,
                             int              cycleHour,
                             int              forecastHour)
{
   char hh[16];
   char fh2[16];
   char fh3[16];
   std::snprintf(hh, sizeof(hh), "%02d", cycleHour);
   std::snprintf(fh2, sizeof(fh2), "%02d", forecastHour);
   std::snprintf(fh3, sizeof(fh3), "%03d", forecastHour);

   const std::pair<std::string_view, std::string_view> substitutions[] = {
      {"{yyyymmdd}", yyyymmdd}, {"{hh}", hh}, {"{fh2}", fh2}, {"{fh3}", fh3}};

   std::string out(keyPattern);
   for (const auto& [token, value] : substitutions)
   {
      for (std::size_t pos = out.find(token); pos != std::string::npos;
           pos             = out.find(token, pos + value.size()))
      {
         out.replace(pos, token.size(), value);
      }
   }
   return out;
}

} // namespace scwx::util::grib_model_config
