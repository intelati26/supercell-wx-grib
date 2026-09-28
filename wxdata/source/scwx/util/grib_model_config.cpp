#include <scwx/util/grib_model_config.hpp>

#include <algorithm>
#include <cstdio>

#include <boost/json.hpp>

namespace scwx::util::grib_model_config
{

namespace json = boost::json;

namespace
{

constexpr std::string_view kSupportedKind = "idx";

struct Reader
{
   std::vector<std::string>& errors;

   void Error(const std::string& path, const std::string& message)
   {
      errors.push_back(path + ": " + message);
   }

   const json::object* Object(const json::object& parent,
                              const std::string&  parentPath,
                              const char*         key,
                              bool                required)
   {
      auto it = parent.find(key);
      if (it == parent.end())
      {
         if (required)
         {
            Error(parentPath + key, "missing required object");
         }
         return nullptr;
      }
      if (!it->value().is_object())
      {
         Error(parentPath + key, "expected an object");
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

   // Accepts any JSON number; rejects NaN/inf and values outside [lo, hi].
   void Number(const json::object& o,
               const std::string&  path,
               const char*         key,
               double              lo,
               double              hi,
               double&             out)
   {
      auto it = o.find(key);
      if (it == o.end())
      {
         return;
      }
      if (!it->value().is_number())
      {
         Error(path + key, "expected a number");
         return;
      }
      double value = it->value().to_number<double>();
      if (!(value >= lo && value <= hi))
      {
         Error(path + key,
               "out of range [" + std::to_string(lo) + ", " +
                  std::to_string(hi) + "]");
         return;
      }
      out = value;
   }
};

void ReadProduct(Reader&             r,
                 const json::object& o,
                 const std::string&  path,
                 ProductSpec&        product)
{
   r.String(o, path, "name", true, product.name);

   if (auto* index = r.Object(o, path, "index", true))
   {
      auto p = path + "index.";
      r.String(*index, p, "parameter", true, product.index.parameter);
      r.String(*index, p, "level", true, product.index.level);
      r.String(*index, p, "qualifier", false, product.index.qualifier);
   }

   if (auto* decode = r.Object(o, path, "decode", true))
   {
      r.String(*decode, path + "decode.", "short_name", true, product.shortName);
   }

   if (auto* display = r.Object(o, path, "display", false))
   {
      auto        p = path + "display.";
      auto&       d = product.display;
      double      v = 0.0;
      r.String(*display, p, "type", false, d.type);
      if (d.type != "fill" && d.type != "contour")
      {
         r.Error(p + "type", "must be \"fill\" or \"contour\"");
      }
      r.String(*display, p, "units", false, d.units);
      r.String(*display, p, "quantity", false, d.quantity);

      v = d.colorOffset;
      r.Number(*display, p, "color_offset", -1e9, 1e9, v);
      d.colorOffset = static_cast<float>(v);
      v = d.colorScale;
      r.Number(*display, p, "color_scale", 1e-9, 1e9, v);
      d.colorScale = static_cast<float>(v);
      v = d.noDataThreshold;
      r.Number(*display, p, "no_data_threshold", -1e9, 1e9, v);
      d.noDataThreshold = static_cast<float>(v);
      v = d.contourInterval;
      r.Number(*display, p, "contour_interval", 0.0, 1e9, v);
      d.contourInterval = static_cast<float>(v);

      if (d.type == "contour" && d.contourInterval <= 0.0f)
      {
         r.Error(p + "contour_interval", "required (> 0) when type is contour");
      }
   }
}

} // namespace

ParseResult ParseModelConfig(std::string_view jsonText)
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

   if (auto* model = r.Object(top, "", "model", true))
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

   if (auto* source = r.Object(top, "", "source", true))
   {
      auto& s = config.source;
      r.String(*source, "source.", "bucket", true, s.bucket);
      r.String(*source, "source.", "key_pattern", true, s.keyPattern);
      r.String(*source, "source.", "idx_suffix", false, s.idxSuffix);

      if (!s.keyPattern.empty())
      {
         if (auto reason = ValidateKeyPattern(s.keyPattern); !reason.empty())
         {
            r.Error("source.key_pattern", reason);
         }
      }

      double fh = 0.0;
      r.Number(*source, "source.", "max_forecast_hour", 0, 384, fh);
      s.maxForecastHour = static_cast<int>(fh);

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

   auto products = top.find("products");
   if (products == top.end() || !products->value().is_array() ||
       products->value().as_array().empty())
   {
      r.Error("products", "expected a non-empty array");
   }
   else
   {
      std::size_t i = 0;
      for (const auto& entry : products->value().as_array())
      {
         auto path = "products[" + std::to_string(i++) + "].";
         if (!entry.is_object())
         {
            r.Error(path, "expected an object");
            continue;
         }
         ProductSpec product;
         ReadProduct(r, entry.as_object(), path, product);
         config.products.push_back(std::move(product));
      }

      // Names key saved per-product state, so duplicates would alias.
      for (std::size_t a = 0; a < config.products.size(); ++a)
      {
         for (std::size_t b = a + 1; b < config.products.size(); ++b)
         {
            if (!config.products[a].name.empty() &&
                config.products[a].name == config.products[b].name)
            {
               r.Error("products[" + std::to_string(b) + "].name",
                       "duplicate of products[" + std::to_string(a) + "]");
            }
         }
      }
   }

   if (result.errors.empty())
   {
      result.config = std::move(config);
   }
   return result;
}

std::string ValidateKeyPattern(std::string_view keyPattern)
{
   // TODO(human): decide what a safe key_pattern is; see the request in chat.
   (void) keyPattern;
   return {};
}

std::string ExpandKeyPattern(std::string_view keyPattern,
                             std::string_view yyyymmdd,
                             int              cycleHour,
                             int              forecastHour)
{
   char hh[8];
   char fh2[8];
   char fh3[8];
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
