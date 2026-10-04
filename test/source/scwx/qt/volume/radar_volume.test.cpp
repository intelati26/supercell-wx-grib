#include <scwx/qt/volume/radar_volume.hpp>
#include <scwx/wsr88d/ar2v_file.hpp>

#include <cmath>
#include <numbers>

#include <gtest/gtest.h>

namespace scwx::qt::volume
{

static const std::string kKlsxVolume_ =
   std::string(SCWX_TEST_DATA_DIR) +
   "/nexrad/level2/Level2_KLSX_20210527_1757.ar2v";

static std::array<std::uint8_t, 4> Opaque(float /* value */)
{
   return {255, 255, 255, 255};
}

TEST(RadarVolume, BeamHeightFollowsFourThirdsEarth)
{
   EXPECT_DOUBLE_EQ(BeamHeightKm(0.0, 0.5), 0.0);
   // 0.5 degrees at 100 km is about 1.46 km above the antenna.
   EXPECT_NEAR(BeamHeightKm(100.0, 0.5), 1.461, 0.005);
   // A flat beam still rises with range as the earth curves away.
   EXPECT_NEAR(BeamHeightKm(100.0, 0.0), 0.589, 0.005);
   EXPECT_GT(BeamHeightKm(100.0, 19.5), 33.0);
}

TEST(RadarVolume, GroundRangeIsShorterThanSlantRange)
{
   EXPECT_NEAR(GroundRangeKm(100.0, 0.0), 100.0, 0.01);
   EXPECT_NEAR(GroundRangeKm(100.0, 19.5),
               100.0 * std::cos(19.5 * std::numbers::pi / 180),
               0.5);
   EXPECT_LT(GroundRangeKm(100.0, 19.5), GroundRangeKm(100.0, 0.5));
}

TEST(RadarVolume, LocalOffset)
{
   const auto north = LocalOffsetKm(38.0, -90.0, 39.0, -90.0);
   EXPECT_NEAR(north[0], 0.0, 1e-9);
   EXPECT_NEAR(north[1], 111.19, 0.01);

   const auto east = LocalOffsetKm(38.0, -90.0, 38.0, -89.0);
   EXPECT_NEAR(east[0], 111.19 * std::cos(38.0 * std::numbers::pi / 180), 0.01);
   EXPECT_NEAR(east[1], 0.0, 1e-9);

   // Across the antimeridian the short way round is taken.
   const auto wrap = LocalOffsetKm(52.0, 179.5, 52.0, -179.5);
   EXPECT_GT(wrap[0], 0.0);
   EXPECT_LT(wrap[0], 100.0);
}

TEST(RadarVolume, RegionContains)
{
   const VolumeRegion region {
      .centerXKm = 10.0, .centerYKm = -20.0, .halfSizeKm = 5.0};
   EXPECT_TRUE(region.Contains(10.0, -20.0));
   EXPECT_TRUE(region.Contains(15.0, -25.0));
   EXPECT_FALSE(region.Contains(15.1, -20.0));
   EXPECT_FALSE(region.Contains(10.0, -14.9));
}

TEST(RadarVolume, CellSizeCoarsensWithRegion)
{
   EXPECT_DOUBLE_EQ(ChooseCellSize(30.0).gateKm, 0.25);
   EXPECT_DOUBLE_EQ(ChooseCellSize(80.0).gateKm, 0.5);
   EXPECT_DOUBLE_EQ(ChooseCellSize(150.0).azimuthDeg, 1.0);
   EXPECT_DOUBLE_EQ(ChooseCellSize(460.0).gateKm, 2.0);
}

TEST(RadarVolume, DistinctElevationsDropsRepeatedCuts)
{
   const std::vector<float> cuts =
      DistinctElevations({0.5f, 0.48f, 0.9f, 0.5f, 1.3f, 0.88f, 19.5f});
   ASSERT_EQ(cuts.size(), 4u);
   EXPECT_NEAR(cuts[0], 0.5f, 0.05f);
   EXPECT_NEAR(cuts[1], 0.9f, 0.05f);
   EXPECT_FLOAT_EQ(cuts[2], 1.3f);
   EXPECT_FLOAT_EQ(cuts[3], 19.5f);
}

TEST(RadarVolume, MeshSkipsCellsBelowThresholdAndTransparent)
{
   RadarVolume volume {};
   volume.siteHeightKm = 0.2;
   volume.region       = {.centerXKm = 0.0, .centerYKm = 0.0, .halfSizeKm = 50};
   volume.tilts.push_back(
      {.elevationDeg = 1.0f,
       .cells        = {
          // Due east, 20-21 km out.
          {.az0Deg = 89.5f,
                  .az1Deg = 90.5f,
                  .r0Km   = 20,
                  .r1Km   = 21,
                  .value  = 45},
          {.az0Deg = 0.0f, .az1Deg = 1.0f, .r0Km = 20, .r1Km = 21, .value = 10},
          {.az0Deg = 180.0f,
                  .az1Deg = 181.0f,
                  .r0Km   = 5,
                  .r1Km   = 6,
                  .value  = 60},
       }});
   // A lower tilt listed second is still drawn first.
   volume.tilts.push_back({.elevationDeg = 0.5f,
                           .cells        = {{.az0Deg = 270.0f,
                                             .az1Deg = 271.0f,
                                             .r0Km   = 30,
                                             .r1Km   = 31,
                                             .value  = 30}}});

   const VolumeMesh mesh = BuildMesh(
      volume,
      [](float value) -> std::array<std::uint8_t, 4>
      {
         // 60 dBZ is transparent in this table.
         return {255, 0, 0, static_cast<std::uint8_t>(value >= 60 ? 0 : 255)};
      },
      {.threshold = 20.0f, .signedProduct = false});

   ASSERT_EQ(mesh.tilts.size(), 2u);
   EXPECT_FLOAT_EQ(mesh.tilts[0].elevationDeg, 0.5f);
   EXPECT_EQ(mesh.tilts[0].firstIndex, 0u);
   EXPECT_EQ(mesh.tilts[0].indexCount, 6u);
   EXPECT_FLOAT_EQ(mesh.tilts[1].elevationDeg, 1.0f);
   EXPECT_EQ(mesh.tilts[1].firstIndex, 6u);
   EXPECT_EQ(mesh.tilts[1].indexCount, 6u);
   ASSERT_EQ(mesh.vertices.size(), 8u);
   ASSERT_EQ(mesh.indices.size(), 12u);

   // The 0.5 degree cell is due west, the 1.0 degree cell due east.
   for (std::size_t i = 0; i < 4; ++i)
   {
      EXPECT_LT(mesh.vertices[i].x, -29.0f);
      EXPECT_NEAR(mesh.vertices[i].y, 0.0f, 0.6f);
   }
   for (std::size_t i = 4; i < 8; ++i)
   {
      EXPECT_GT(mesh.vertices[i].x, 19.0f);
      EXPECT_NEAR(mesh.vertices[i].y, 0.0f, 0.2f);
      EXPECT_GT(mesh.vertices[i].z, 0.2f + 0.3f);
   }
   // Far edge higher than near edge.
   EXPECT_GT(mesh.vertices[6].z, mesh.vertices[4].z);
}

TEST(RadarVolume, MeshThresholdUsesMagnitudeForSignedProducts)
{
   RadarVolume volume {};
   volume.tilts.push_back(
      {.elevationDeg = 0.5f,
       .cells        = {
          {.az0Deg = 0, .az1Deg = 1, .r0Km = 10, .r1Km = 11, .value = -30},
          {.az0Deg = 1, .az1Deg = 2, .r0Km = 10, .r1Km = 11, .value = 5},
          {.az0Deg = 2, .az1Deg = 3, .r0Km = 10, .r1Km = 11, .value = 25},
       }});

   const VolumeMesh mesh =
      BuildMesh(volume, Opaque, {.threshold = 20.0f, .signedProduct = true});
   EXPECT_EQ(mesh.vertices.size(), 8u);
}

TEST(RadarVolume, MeshIsCentredOnRegion)
{
   RadarVolume volume {};
   volume.region = {.centerXKm = 0.0, .centerYKm = 50.0, .halfSizeKm = 10};
   volume.tilts.push_back({.elevationDeg = 0.5f,
                           .cells        = {{.az0Deg = 359.5f,
                                             .az1Deg = 360.5f,
                                             .r0Km   = 49.5f,
                                             .r1Km   = 50.5f,
                                             .value  = 1}}});

   const VolumeMesh mesh = BuildMesh(volume, Opaque, {});
   ASSERT_EQ(mesh.vertices.size(), 4u);
   for (const auto& v : mesh.vertices)
   {
      EXPECT_NEAR(v.x, 0.0f, 0.5f);
      EXPECT_NEAR(v.y, 0.0f, 0.6f);
   }
}

class RadarVolumeFileTest : public testing::Test
{
protected:
   static void SetUpTestSuite()
   {
      file_ = std::make_shared<wsr88d::Ar2vFile>();
      ASSERT_TRUE(file_->LoadFile(kKlsxVolume_));
   }
   static void TearDownTestSuite() { file_.reset(); }

   static std::shared_ptr<wsr88d::Ar2vFile> file_;
};

std::shared_ptr<wsr88d::Ar2vFile> RadarVolumeFileTest::file_ {};

TEST_F(RadarVolumeFileTest, ExtractsCellsInsideRegion)
{
   auto [scan, elevation, cuts] =
      file_->GetElevationScan(wsr88d::rda::DataBlockType::MomentRef, 0.5f, {});
   ASSERT_NE(scan, nullptr);
   EXPECT_GT(DistinctElevations(cuts).size(), 5u);

   const VolumeRegion region {
      .centerXKm = 0.0, .centerYKm = 0.0, .halfSizeKm = 50.0};
   const VolumeTilt tilt = ExtractTilt(*scan,
                                       wsr88d::rda::DataBlockType::MomentRef,
                                       elevation,
                                       region,
                                       ChooseCellSize(region.halfSizeKm),
                                       false);
   ASSERT_FALSE(tilt.cells.empty());

   for (const VolumeCell& cell : tilt.cells)
   {
      const double az =
         (cell.az0Deg + cell.az1Deg) / 2.0 * std::numbers::pi / 180.0;
      const double s =
         GroundRangeKm((cell.r0Km + cell.r1Km) / 2.0, tilt.elevationDeg);
      // Cell centres are inside, give or take half a cell.
      EXPECT_TRUE(
         region.Contains(s * std::sin(az) * 0.99, s * std::cos(az) * 0.99));
      EXPECT_GE(cell.value, -33.0f);
      EXPECT_LE(cell.value, 95.0f);
      EXPECT_LT(cell.az0Deg, cell.az1Deg);
      EXPECT_LT(cell.r0Km, cell.r1Km);
   }
}

TEST_F(RadarVolumeFileTest, CoarserCellsMeanFewerCells)
{
   auto [scan, elevation, cuts] =
      file_->GetElevationScan(wsr88d::rda::DataBlockType::MomentRef, 0.5f, {});
   ASSERT_NE(scan, nullptr);

   const VolumeRegion region {
      .centerXKm = 0.0, .centerYKm = 0.0, .halfSizeKm = 150.0};
   const VolumeTilt fine   = ExtractTilt(*scan,
                                       wsr88d::rda::DataBlockType::MomentRef,
                                       elevation,
                                       region,
                                         {.gateKm = 0.25, .azimuthDeg = 0.5},
                                       false);
   const VolumeTilt coarse = ExtractTilt(*scan,
                                         wsr88d::rda::DataBlockType::MomentRef,
                                         elevation,
                                         region,
                                         {.gateKm = 1.0, .azimuthDeg = 1.0},
                                         false);
   ASSERT_FALSE(coarse.cells.empty());
   EXPECT_LT(coarse.cells.size() * 3, fine.cells.size());

   // Combining keeps the strongest echo.
   float fineMax   = -100.0f;
   float coarseMax = -100.0f;
   for (const auto& c : fine.cells)
   {
      fineMax = std::max(fineMax, c.value);
   }
   for (const auto& c : coarse.cells)
   {
      coarseMax = std::max(coarseMax, c.value);
   }
   EXPECT_FLOAT_EQ(fineMax, coarseMax);
}

TEST_F(RadarVolumeFileTest, RegionAwayFromDataIsEmpty)
{
   auto [scan, elevation, cuts] =
      file_->GetElevationScan(wsr88d::rda::DataBlockType::MomentRef, 0.5f, {});
   ASSERT_NE(scan, nullptr);

   // Well beyond the 460 km range.
   const VolumeRegion region {
      .centerXKm = 900.0, .centerYKm = 0.0, .halfSizeKm = 50.0};
   EXPECT_TRUE(ExtractTilt(*scan,
                           wsr88d::rda::DataBlockType::MomentRef,
                           elevation,
                           region,
                           ChooseCellSize(region.halfSizeKm),
                           false)
                  .cells.empty());
}

} // namespace scwx::qt::volume
