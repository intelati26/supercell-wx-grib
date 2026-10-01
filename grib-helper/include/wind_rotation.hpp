// Grid-relative -> earth-relative wind rotation for a Lambert Conformal
// Conic grid, in the NCEP/WMO convention (raw U/V components are stored
// relative to the grid's own local x/y axes, not true north/east --
// confirmed via uvRelativeToGrid=1 on both RRFS and RTMA GRIB2 files).
//
// Derivation: this grid's forward projection (grib-helper/src/
// validate_lcc.cpp, matching Snyder's Lambert Conformal Conic) places
// x = rho*sin(theta), y = -rho*cos(theta) with theta = n*(lon - LoV) in
// radians. Differentiating (x,y) with respect to longitude (fixed
// latitude) and latitude (fixed longitude) gives the local map-space
// directions of true east and true north at any point:
//   east  -> (cos theta,  sin theta)
//   north -> (-sin theta, cos theta)
// i.e. (grid_u, grid_v) = R(theta) * (earth_u, earth_v) for
// R(theta) = [[cos theta, -sin theta], [sin theta, cos theta]]. Inverting
// (R(theta)^-1 = R(-theta)) gives the earth-relative components below.
//
// Verified independently (not just derived): applied to real RTMA GRIB2
// grid-relative 10u/10v at several CONUS points and compared against that
// same file's own earth-relative "10wdir" field (a WMO-convention scalar
// that is always true-north-relative, unlike raw U/V) -- matches to
// within GRIB2's stored rounding (~0.1-0.2 deg) at points spanning
// theta from about -13 to +9 deg. See wind_rotation_test.cpp for the
// pinned regression values from that check.
//
// Header-only and dependency-free (no eccodes) so it can be shared by
// decode_grib.cpp (which does depend on eccodes) and a plain unit test
// that doesn't.
#pragma once

#include <cmath>

namespace scwx::grib
{

constexpr double kPi = 3.14159265358979323846;

// Cone constant n for a Lambert Conformal Conic grid. Matches
// validate_lcc.cpp's ConeConstantN -- every NCEP CONUS-nest grid measured
// so far (RRFS, RTMA) is tangent (latin1 == latin2), where this reduces to
// sin(latin1), but the secant form is included for completeness.
inline double LambertConeConstant(double latin1Deg, double latin2Deg)
{
   const double phi1 = latin1Deg * kPi / 180.0;
   const double phi2 = latin2Deg * kPi / 180.0;
   if (std::fabs(latin1Deg - latin2Deg) < 1e-9)
   {
      return std::sin(phi1);
   }
   return std::log(std::cos(phi1) / std::cos(phi2)) /
          std::log(std::tan(kPi / 4 + phi2 / 2) / std::tan(kPi / 4 + phi1 / 2));
}

struct EarthWind
{
   double u;
   double v;
};

// lonDeg/lovDeg must both be normalized to the same range (e.g. both
// -180..180, as decode_grib's GridInfo already keeps them) -- the
// difference is what matters, and an unnormalized 360-degree offset
// between the two would silently corrupt theta.
inline EarthWind RotateGridWindToEarth(double gridU,
                                       double gridV,
                                       double lonDeg,
                                       double lovDeg,
                                       double n)
{
   const double theta = n * (lonDeg - lovDeg) * kPi / 180.0;
   const double c      = std::cos(theta);
   const double s      = std::sin(theta);

   return {gridU * c + gridV * s, -gridU * s + gridV * c};
}

} // namespace scwx::grib
