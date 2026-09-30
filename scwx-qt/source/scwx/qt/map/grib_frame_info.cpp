#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/main/application_paths.hpp>

#if defined(_MSC_VER)
#   pragma warning(push, 0)
#endif

#include <glm/gtc/constants.hpp>

#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <fmt/format.h>

namespace scwx::qt::map
{

namespace
{

std::string ReadHeaderLine(const std::string& framePath)
{
   std::ifstream in(framePath, std::ios::binary);
   if (!in)
   {
      return {};
   }

   std::string header;
   std::getline(in, header);
   return header;
}

} // namespace

std::filesystem::path GetGribDataDirectory()
{
   static const std::filesystem::path dir = []
   {
      auto            path = main::ApplicationPaths::GetLocation(
                                main::ApplicationPaths::StandardLocation::Cache) /
                             "grib";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
   }();
   return dir;
}

std::string GetGribFramePath(GribCategory category, std::size_t productIndex)
{
   std::string prefix;
   switch (category)
   {
   case GribCategory::Mrms:
      prefix = "mrms";
      break;
   case GribCategory::Rtma:
      prefix = "rtma";
      break;
   case GribCategory::Rrfs:
      prefix = "rrfs";
      break;
   case GribCategory::Nbm:
      prefix = "nbm";
      break;
   }
   if (prefix.empty())
   {
      return {};
   }

   return (GetGribDataDirectory() /
           fmt::format("{}_product{}.frame", prefix, productIndex))
      .string();
}

std::string ReadGribFrameValidTime(const std::string& framePath)
{
   const std::string header = ReadHeaderLine(framePath);
   return ExtractStringOr(header, "validTime", {});
}

GribFrameColorRange ReadGribFrameColorRange(const std::string& framePath)
{
   const std::string header = ReadHeaderLine(framePath);

   GribFrameColorRange range;
   range.colorOffset =
      static_cast<float>(ExtractNumberOr(header, "colorOffset", 0.0));
   range.colorScale =
      static_cast<float>(ExtractNumberOr(header, "colorScale", 0.0));
   return range;
}

double ExtractNumber(const std::string& header, const std::string& key)
{
   const std::string needle = "\"" + key + "\":";
   const size_t      pos    = header.find(needle);
   if (pos == std::string::npos)
   {
      throw std::runtime_error("Missing key in frame header: " + key);
   }
   return std::stod(header.substr(pos + needle.size()));
}

double ExtractNumberOr(const std::string& header,
                       const std::string& key,
                       double             defaultValue)
{
   try
   {
      return ExtractNumber(header, key);
   }
   catch (const std::runtime_error&)
   {
      return defaultValue;
   }
}

std::string ExtractString(const std::string& header, const std::string& key)
{
   const std::string needle = "\"" + key + "\":\"";
   const size_t      start  = header.find(needle);
   if (start == std::string::npos)
   {
      throw std::runtime_error("Missing key in frame header: " + key);
   }
   const size_t valueStart = start + needle.size();
   const size_t valueEnd   = header.find('"', valueStart);
   if (valueEnd == std::string::npos)
   {
      throw std::runtime_error("Unterminated string for key: " + key);
   }
   return header.substr(valueStart, valueEnd - valueStart);
}

std::string ExtractStringOr(const std::string& header,
                            const std::string& key,
                            const std::string& defaultValue)
{
   try
   {
      return ExtractString(header, key);
   }
   catch (const std::runtime_error&)
   {
      return defaultValue;
   }
}

LambertConstants ComputeLambertConstants(const LambertGrid& grid)
{
   const double phi1 = glm::radians(grid.latin1);
   const double phi2 = glm::radians(grid.latin2);

   double n;
   if (std::abs(grid.latin1 - grid.latin2) < 1e-9)
   {
      n = std::sin(phi1);
   }
   else
   {
      n = std::log(std::cos(phi1) / std::cos(phi2)) /
          std::log(std::tan(glm::quarter_pi<double>() + phi2 / 2.0) /
                   std::tan(glm::quarter_pi<double>() + phi1 / 2.0));
   }

   const double f =
      std::cos(phi1) *
      std::pow(std::tan(glm::quarter_pi<double>() + phi1 / 2.0), n) / n;

   return {n, f};
}

glm::dvec2 LambertForward(const LambertGrid&      grid,
                          const LambertConstants& c,
                          double                  latDeg,
                          double                  lonDeg)
{
   const double phi   = glm::radians(latDeg);
   const double theta = c.n * glm::radians(lonDeg - grid.lov);
   const double rho =
      grid.radius * c.f /
      std::pow(std::tan(glm::quarter_pi<double>() + phi / 2.0), c.n);

   return {rho * std::sin(theta), -rho * std::cos(theta)};
}

glm::dvec2 LambertInverse(const LambertGrid&      grid,
                          const LambertConstants& c,
                          double                  x,
                          double                  y)
{
   const double rho   = std::copysign(std::sqrt(x * x + y * y), c.n);
   const double theta = std::atan2(x, -y);
   const double phi =
      2.0 * std::atan(std::pow(grid.radius * c.f / rho, 1.0 / c.n)) -
      glm::half_pi<double>();

   return {glm::degrees(phi), glm::degrees(theta / c.n) + grid.lov};
}

glm::dvec2 LambertGridToLatLon(const LambertGrid& grid, double i, double j)
{
   const LambertConstants c = ComputeLambertConstants(grid);

   const glm::dvec2 origin = LambertForward(grid, c, grid.lat1, grid.lon1);
   const double     x      = origin.x + i * grid.dx;
   const double     y      = origin.y + j * grid.dy;

   return LambertInverse(grid, c, x, y);
}

bool FitsHalfFloatTexture(const float* values,
                          std::size_t  count,
                          float        noDataThreshold,
                          float        colorScale,
                          float        contourInterval,
                          double       missingValue)
{
   // Largest finite half-precision value.
   constexpr float kHalfMax = 65504.0f;

   if (contourInterval > 0.0f || !(std::abs(colorScale) > 0.0f))
   {
      return false;
   }

   float maxAbs = 0.0f;
   for (std::size_t i = 0; i < count; ++i)
   {
      const float v = values[i];
      if (!(v >= noDataThreshold))
      {
         continue; // not drawn (below the cutoff, or NaN)
      }
      if (static_cast<double>(v) == missingValue)
      {
         if (!(std::abs(v) < kHalfMax))
         {
            return false; // would become infinity
         }
         continue; // drawn, but its exact value doesn't matter
      }
      maxAbs = std::max(maxAbs, std::abs(v));
   }

   if (!(maxAbs < kHalfMax)) // also catches +/-infinity
   {
      return false;
   }
   if (maxAbs == 0.0f)
   {
      return true;
   }

   // Half precision has a 10-bit mantissa, so values in [2^e, 2^(e+1))
   // are spaced 2^(e-10) apart and round by at most half that. The largest
   // drawn value has the coarsest spacing of any of them.
   int exponent = 0;
   std::frexp(maxAbs, &exponent); // maxAbs = m * 2^exponent, m in [0.5, 1)
   const float maxRoundingError = std::ldexp(1.0f, (exponent - 1) - 11);

   return maxRoundingError <= std::abs(colorScale) / 512.0f;
}

bool LatLonBox::Contains(const LatLonBox& other) const
{
   return other.south >= south && other.north <= north && other.west >= west &&
          other.east <= east;
}

LatLonBox ViewportLatLonBox(double centerLat,
                            double centerLon,
                            double zoom,
                            double widthPx,
                            double heightPx,
                            double scale)
{
   // Same constants MapLibre uses (mbgl::util::tileSize_D/LATITUDE_MAX),
   // restated here so this stays free of MapLibre/Qt headers.
   static constexpr double kTileSize    = 512.0;
   static constexpr double kLatitudeMax = 85.051128779806604;

   // Web Mercator x/y in "degrees" (x = longitude; y = the Mercator
   // ordinate scaled to the same units), so one pixel is the same size in
   // both at a given zoom.
   auto toMercatorY = [](double latDeg)
   {
      const double phi =
         glm::radians(std::clamp(latDeg, -kLatitudeMax, kLatitudeMax));
      return glm::degrees(
         std::log(std::tan(glm::quarter_pi<double>() + phi / 2.0)));
   };
   auto fromMercatorY = [](double y)
   {
      return glm::degrees(2.0 * std::atan(std::exp(glm::radians(y))) -
                          glm::half_pi<double>());
   };

   const double pixelsPerDegree = std::pow(2.0, zoom) * kTileSize / 360.0;
   const double halfExtent =
      std::hypot(widthPx, heightPx) / 2.0 / pixelsPerDegree * scale;

   const double centerY = toMercatorY(centerLat);

   LatLonBox box;
   box.south = std::max(fromMercatorY(centerY - halfExtent), -kLatitudeMax);
   box.north = std::min(fromMercatorY(centerY + halfExtent), kLatitudeMax);
   box.west  = std::max(centerLon - halfExtent, -180.0);
   box.east  = std::min(centerLon + halfExtent, 180.0);
   return box;
}

GridIndexBox LambertGridIndexBox(const LambertGrid& grid,
                                 long               nx,
                                 long               ny,
                                 const LatLonBox&   box)
{
   if (nx <= 0 || ny <= 0 || grid.dx == 0.0 || grid.dy == 0.0)
   {
      return {};
   }

   const GridIndexBox wholeGrid {0, nx - 1, 0, ny - 1};

   if (box.east - box.west > 90.0)
   {
      return wholeGrid;
   }

   const LambertConstants c = ComputeLambertConstants(grid);
   const glm::dvec2 origin  = LambertForward(grid, c, grid.lat1, grid.lon1);

   // The projection is continuous and one-to-one over a box this size, so
   // the box's image is bounded by the image of its edges -- sampling the
   // perimeter finely enough to follow the edges' curvature bounds the
   // whole interior without projecting it.
   constexpr int kSamplesPerEdge = 16;

   double iMin = std::numeric_limits<double>::max();
   double iMax = std::numeric_limits<double>::lowest();
   double jMin = std::numeric_limits<double>::max();
   double jMax = std::numeric_limits<double>::lowest();

   auto include = [&](double lat, double lon)
   {
      // Measure longitude on the same side of the cone as the grid, so a
      // grid described in 0-360 degrees still lines up with a -180-180 box.
      lon = grid.lov + std::remainder(lon - grid.lov, 360.0);

      const glm::dvec2 xy = LambertForward(grid, c, lat, lon);
      const double     i  = (xy.x - origin.x) / grid.dx;
      const double     j  = (xy.y - origin.y) / grid.dy;
      iMin                = std::min(iMin, i);
      iMax                = std::max(iMax, i);
      jMin                = std::min(jMin, j);
      jMax                = std::max(jMax, j);
   };

   for (int s = 0; s <= kSamplesPerEdge; ++s)
   {
      const double f   = static_cast<double>(s) / kSamplesPerEdge;
      const double lat = box.south + f * (box.north - box.south);
      const double lon = box.west + f * (box.east - box.west);
      include(box.south, lon);
      include(box.north, lon);
      include(lat, box.west);
      include(lat, box.east);
   }

   GridIndexBox result;
   result.iMin = std::max(static_cast<long>(std::floor(iMin)), 0L);
   result.iMax = std::min(static_cast<long>(std::ceil(iMax)), nx - 1);
   result.jMin = std::max(static_cast<long>(std::floor(jMin)), 0L);
   result.jMax = std::min(static_cast<long>(std::ceil(jMax)), ny - 1);
   return result;
}

} // namespace scwx::qt::map
