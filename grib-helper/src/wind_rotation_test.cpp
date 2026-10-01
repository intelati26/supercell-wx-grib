// Pins RotateGridWindToEarth()/LambertConeConstant() (wind_rotation.hpp)
// against known values -- no eccodes dependency, so this builds and runs
// unconditionally (see grib-helper/CMakeLists.txt), unlike decode_grib
// itself. Plain assert + exit code, matching the rest of grib-helper's
// tools rather than pulling in GTest for one small header.
#include "../include/wind_rotation.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using scwx::grib::EarthWind;
using scwx::grib::LambertConeConstant;
using scwx::grib::RotateGridWindToEarth;

namespace
{

bool Near(double a, double b, double tol)
{
   return std::fabs(a - b) <= tol;
}

// Pure geometry, independent of any real grid: at the central meridian
// (lon == lov) theta is always 0 regardless of n, so grid-relative must
// pass through unrotated.
void TestIdentityAtCentralMeridian()
{
   const double n = LambertConeConstant(38.5, 38.5);
   const EarthWind w = RotateGridWindToEarth(7.0, -3.0, -97.5, -97.5, n);
   assert(Near(w.u, 7.0, 1e-9));
   assert(Near(w.v, -3.0, 1e-9));
}

// Exact rotation-matrix check with a round, hand-computable theta: n=1,
// lon-lov=90 => theta=90deg, where cos=0, sin=1, so the formula reduces
// to a known 90-degree swap: u_e = v_grid, v_e = -u_grid.
void TestNinetyDegreeTheta()
{
   const EarthWind w = RotateGridWindToEarth(10.0, 4.0, 90.0, 0.0, 1.0);
   assert(Near(w.u, 4.0, 1e-9));
   assert(Near(w.v, -10.0, 1e-9));
}

// Cone constant sanity: tangent case (latin1==latin2) is just sin(latin1)
// -- RRFS (38.5/38.5) and RTMA (25/25) are both tangent, confirmed live.
void TestConeConstantTangentCase()
{
   assert(Near(LambertConeConstant(38.5, 38.5), std::sin(38.5 * M_PI / 180.0), 1e-12));
   assert(Near(LambertConeConstant(25.0, 25.0), std::sin(25.0 * M_PI / 180.0), 1e-12));
}

// Regression pins from a real RTMA GRIB2 file (rtma2p5.t01z.2dvaranl_ndfd,
// 2026-09-29, lambert grid LoV=-95, Latin1=Latin2=25, n=sin(25deg)):
// rotating that file's own grid-relative 10u/10v with this code reproduces
// the SAME file's independently-reported earth-relative 10si (speed,
// rotation-invariant so also a sanity check on the inputs) and 10wdir
// (direction, true-north-relative per WMO convention) to within GRIB2's
// stored 2-decimal rounding. This is the actual verification of the
// rotation's sign and magnitude against ground truth, not just the
// algebra above -- see wind_rotation.hpp's derivation comment.
struct RtmaPin
{
   double lat, lon, u, v;
   double expectedSpeed, expectedDirDeg; // from that file's own 10si/10wdir
};

void TestAgainstRealRtmaWindDirection()
{
   constexpr double kLov = -95.0;
   const double     n    = LambertConeConstant(25.0, 25.0);

   // clang-format off
   static const RtmaPin pins[] = {
      {19.229, -126.277, 3.48,  -2.79, 4.46, 295.5},
      {27.105,  -90.461, -3.92,  1.88, 4.35, 117.6},
      {34.883,  -73.997, -0.14, -1.64, 1.65,  13.9},
   };
   // clang-format on

   for (const auto& pin : pins)
   {
      const EarthWind w =
         RotateGridWindToEarth(pin.u, pin.v, pin.lon, kLov, n);

      const double speed = std::hypot(w.u, w.v);
      // Meteorological "from" direction, true-north-relative.
      const double dirDeg =
         std::fmod(std::atan2(-w.u, -w.v) * 180.0 / M_PI + 360.0, 360.0);

      if (!Near(speed, pin.expectedSpeed, 0.02) ||
          !Near(dirDeg, pin.expectedDirDeg, 0.5))
      {
         std::fprintf(stderr,
                      "RTMA pin mismatch at (%.3f,%.3f): "
                      "speed=%.3f (want %.3f) dir=%.3f (want %.3f)\n",
                      pin.lat,
                      pin.lon,
                      speed,
                      pin.expectedSpeed,
                      dirDeg,
                      pin.expectedDirDeg);
         std::abort();
      }
   }
}

} // namespace

int main()
{
   TestIdentityAtCentralMeridian();
   TestNinetyDegreeTheta();
   TestConeConstantTangentCase();
   TestAgainstRealRtmaWindDirection();

   std::printf("wind_rotation_test: all checks passed\n");
   return 0;
}
