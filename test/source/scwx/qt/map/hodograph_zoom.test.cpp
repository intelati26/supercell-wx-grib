#include <scwx/qt/map/hodograph_zoom.hpp>

#include <cmath>

#include <gtest/gtest.h>

namespace scwx::qt::map::hodograph_zoom
{

namespace
{

constexpr double kPi             = 3.14159265358979323846;
constexpr double kRrfsGridMeters = 3000.0;

// On-screen pixels per metre at `zoom` for the latitude the tiers assume
double PixelsPerMeter(double zoom)
{
   return 512.0 * std::exp2(zoom) /
          (40075016.686 * std::cos(kReferenceLatitudeDegrees * kPi / 180.0));
}

// Length on screen of the reference (30 m/s) vector drawn by `tier` at `zoom`
double GlyphPixels(const ZoomTier& tier, double zoom)
{ return kReferenceWindMs * tier.metersPerMs * PixelsPerMeter(zoom); }

} // namespace

TEST(HodographZoomTest, NothingIsDrawnBelowZoomSix)
{
   EXPECT_FALSE(TierForZoom(5.99).visible);
   EXPECT_FALSE(TierForZoom(0.0).visible);
   EXPECT_TRUE(TierForZoom(6.0).visible);
}

// The regression this guards: sizing by a fixed metres-per-(m/s) per tier made
// hodographs specks when zoomed out and hundreds of pixels across zoomed in.
TEST(HodographZoomTest, HodographsKeepAReadableSizeAtEveryZoom)
{
   for (double zoom = 6.0; zoom <= 14.0; zoom += 0.05)
   {
      const double pixels = GlyphPixels(TierForZoom(zoom), zoom);
      EXPECT_GT(pixels, kGlyphPixels * 0.75) << "zoom " << zoom;
      EXPECT_LT(pixels, kGlyphPixels * 1.25) << "zoom " << zoom;
   }
}

// Zooming in must add hodographs (a smaller stride), never take them away
TEST(HodographZoomTest, ZoomingInNeverMakesThemSparser)
{
   long previousStride = TierForZoom(6.0).stride;
   for (double zoom = 6.0; zoom <= 14.0; zoom += 0.05)
   {
      const long stride = TierForZoom(zoom).stride;
      EXPECT_LE(stride, previousStride) << "zoom " << zoom;
      previousStride = stride;
   }
   EXPECT_GT(TierForZoom(6.0).stride, TierForZoom(12.0).stride);
}

// Neighbouring hodographs stay clear of each other: the glyph is always smaller
// than the gap to the next one
TEST(HodographZoomTest, NeighboursDoNotOverlap)
{
   for (double zoom = 6.0; zoom <= 14.0; zoom += 0.05)
   {
      const ZoomTier tier          = TierForZoom(zoom);
      const double   spacingPixels = static_cast<double>(tier.stride) *
                                     kRrfsGridMeters * PixelsPerMeter(zoom);
      EXPECT_LT(GlyphPixels(tier, zoom) * 1.25, spacingPixels)
         << "zoom " << zoom;
   }
}

// A smooth zoom must not rebuild the geometry every frame
TEST(HodographZoomTest, TheSizeOnlyChangesInSteps)
{
   EXPECT_EQ(TierForZoom(8.01), TierForZoom(8.20));
   EXPECT_NE(TierForZoom(8.01).metersPerMs, TierForZoom(8.30).metersPerMs);
}

} // namespace scwx::qt::map::hodograph_zoom
