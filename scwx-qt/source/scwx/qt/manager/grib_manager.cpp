#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/hodograph_selection.hpp>
#include <scwx/qt/manager/grib_field_download.hpp>
#include <scwx/qt/manager/grib_field_selectors.hpp>
#include <scwx/qt/manager/status_manager.hpp>
#include <scwx/qt/manager/timeline_manager.hpp>
#include <scwx/qt/manager/user_model_registry.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/settings/unit_settings.hpp>
#include <scwx/qt/types/unit_types.hpp>
#include <scwx/provider/mrms_data_provider.hpp>
#include <scwx/provider/configured_idx_provider.hpp>
#include <scwx/provider/idx_model_provider.hpp>
#include <scwx/provider/nbm_data_provider.hpp>
#include <scwx/provider/rrfs_data_provider.hpp>
#include <scwx/provider/rtma_data_provider.hpp>
#include <scwx/util/logger.hpp>
#include <scwx/util/time.hpp>

#include <algorithm>
#include <iterator>
#include <chrono>
#include <deque>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <fmt/format.h>

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

namespace scwx::qt::manager
{

static const std::string logPrefix_ = "scwx::qt::manager::grib_manager";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

namespace
{

// Path to the out-of-process eccodes decode helper (see
// grib-helper/README.md for why it isn't linked directly into wxdata).
// Built alongside supercell-wx and installed next to it when eccodes was
// found at configure time (see grib-helper/CMakeLists.txt) -- resolved
// lazily, not at static-init time, since QCoreApplication::
// applicationDirPath() needs the QApplication instance main() constructs
// before this is ever actually called.
const std::string& DecodeGribPath()
{
   static const std::string path = (QCoreApplication::applicationDirPath() +
#if defined(_WIN32)
                                    "/decode_grib.exe"
#else
                                    "/decode_grib"
#endif
                                    )
                                      .toStdString();
   return path;
}

// See map::GetGribDataDirectory() -- same lazy-static reasoning applies
// here (ApplicationPaths::Initialize() must have already run).
const std::string& DownloadDir()
{
   static const std::string dir = map::GetGribDataDirectory().string();
   return dir;
}

const std::string& CacheDir()
{
   static const std::string dir = DownloadDir() + "/cache";
   return dir;
}

// decode_grib runs off the GUI thread (see GribManager::QueueCachedDecode),
// so a generous bound costs nothing in responsiveness -- SHIP's two-file
// decode has been measured at several minutes on slow CI runners, and a
// 10s limit silently dropped it there.
constexpr int kDecodeTimeoutMs_ = 10 * 60 * 1000;

// A unique temporary name next to `framePath` for decode_grib to write to
// before the result is renamed into place -- unique per decode, since two
// decodes for the same product can overlap (the fetch pool runs two jobs,
// plus the decode pool) and must not write the same temporary file. Seen
// live: during the RRFS hour-picks Play loop, a prefetch finishing just as
// playback reaches that hour decoded the same key twice at once, and on
// Windows the two writers to one fixed `<frame>.tmp` corrupted it -- the
// loop "advanced but showed stale/wrong frames". Ends in ".tmp" so
// GribManager::Impl::RemoveStaleTmpFrames() finds leftovers.
std::string UniqueTmpFramePath(const std::string& framePath)
{
   static std::atomic<std::uint64_t> counter {0};
   return fmt::format("{}.{}.tmp", framePath, counter.fetch_add(1));
}

} // namespace

// Unlike MRMS -- one S3 object per product -- RTMA and RRFS both bundle
// many fields into one file, so there's no per-field S3 path to select;
// every product in a given category's table resolves to the *same* S3
// key for a given time, and field selection happens downstream, by GRIB
// shortName, once decode_grib has the file (see decode_grib's shortName
// CLI argument). Unlike RTMA though, RRFS bundles fields into *two* files
// (2dfld/prslev, see RrfsDataProvider's class comment) -- kRrfsProducts_
// entries are 2dfld-only for now (RrfsDataProvider only ever resolves
// the 2dfld file); prslev support is a real follow-up, not built yet.
// (A `Source` enum used to
// live here to pick download mechanics per product; removed once
// map::GribCategory itself became one-per-family -- category alone
// determines it now, see MakeProvider.)

// What physical quantity a product's raw value represents, so the
// Shift-hover data tooltip can convert it to whatever the user has
// configured in Settings > Units, the same way RadarProductLayer's own
// tooltip already does for distance/height. None means there's no
// user-configurable alternative (dBZ is dBZ; the same is true of MRMS's
// rotation track, which isn't even in a clean physical unit to begin
// with) -- shown as ProductConfig::units natively, unconverted.
enum class PhysicalQuantity
{
   None,
   TemperatureKelvin,
   SpeedMetersPerSecond,
   AccumulationMillimeters,
   PressurePascals,
};

// Curated set of MRMS/RTMA/RRFS products (confirmed against the real
// noaa-mrms-pds/noaa-rtma-pds/noaa-rrfs-ops-pds S3 buckets), not any
// source's full catalog -- MRMS entries picked to include products with
// no per-site-radar equivalent (rotation track, hail, precip), so this
// doesn't just read as redundant with radar. displayName is what
// GribDockWidget's dropdown shows; s3Product is the literal
// CONUS/<s3Product>/ folder name
// (Mrms only); shortName is the GRIB shortName decode_grib should select
// out of the bundled file (Rtma/Rrfs only). Which table a product lives
// in (kMrmsProducts_/kRtmaProducts_/kRrfsProducts_) is itself what used
// to be the `source` field -- see Products(GribCategory).
struct ProductConfig
{
   std::string      displayName;
   std::string      s3Product;
   std::string      shortName;
   float            colorOffset;
   float            colorScale;
   float            noDataThreshold;
   PhysicalQuantity quantity;
   std::string      units; // native GRIB physical unit -- the tooltip
                           // shows this as-is only when quantity is None;
                           // otherwise GribManager::FormatValue() converts
                           // and picks the unit's own abbreviation instead
                           // (see quantity's own comment).

   // 0 (default) means the normal palette-fill rendering; a nonzero value
   // tells decode_grib (see its own contourInterval CLI arg) to bake an
   // isoline-mode header into the frame instead, in this product's own
   // physical unit (e.g. 400 for MSLP's 400 Pa / 4 hPa synoptic
   // convention). Only meaningful alongside a non-empty shortName --
   // decode_grib's CLI requires shortName before contourInterval, which
   // every current/likely contour candidate (RTMA/RRFS bundled fields)
   // already passes anyway.
   float contourInterval = 0.0f;

   // Empty (default): the normal single-message decode, selecting
   // `shortName` out of the bundled file. Non-empty (e.g. "stp"): this
   // product is computed from several messages in the same file via
   // decode_grib's `--derived <name>` mode instead (see decode_grib.cpp's
   // own RunDerived/ComputeStp) -- `shortName` is unused in that case,
   // decode_grib's derived path has its own fixed, internally-hardcoded
   // field list per index name.
   std::string derivedIndex;

   // Empty typeOfLevel (default) / -1 topLevel/bottomLevel (default)
   // means "shortName alone is unambiguous," matching every product
   // that predates these fields. Several real RRFS fields (cape/cin/
   // hlcy/vucsh/vvcsh/tcc/rare) share a shortName with another level/
   // layer in the same 2dfld file and need these set -- see decode_grib's
   // own FindMessage for the exact mechanism, confirmed against a real
   // downloaded file for every product that sets them, not guessed.
   std::string typeOfLevel;
   long        topLevel    = -1;
   long        bottomLevel = -1;

   // -1 (default, "don't care") for every product above. RRFS's two
   // "tp" (Total Precipitation) messages share identical typeOfLevel/
   // topLevel/bottomLevel, so those three can't disambiguate them --
   // startStep/lengthOfTimeRange are the actual discriminators (see
   // decode_grib's FindMessage for the exact mechanism and "1-Hour
   // Precipitation"/"Total Precipitation" below for the only two
   // products that set them).
   long startStep         = -1;
   long lengthOfTimeRange = -1;

   // Rrfs-only (meaningless for Mrms/Rtma, left at the default) --
   // TwoDField (default) reads the 2dfld file every existing RRFS product
   // came from; PressureLevel reads the *other* per-cycle file RRFS
   // publishes (isobaric-level fields -- 500mb/700mb/etc. HGT/TMP/wind),
   // a genuinely different file with its own, different hourly-
   // availability rule (see provider::RrfsFileFamily's own doc). Read by
   // MakeProvider() once, at provider construction -- unlike cycle/
   // forecast-hour, which the user can change live via the run/hour
   // picker, this is fixed for a product's whole lifetime.
   provider::RrfsFileFamily rrfsFileFamily =
      provider::RrfsFileFamily::TwoDField;

   // Nbm-only (meaningless for every other category, left empty) -- NBM's
   // own per-cycle file bundles every field for the whole CONUS domain in
   // one ~160MB object, too large to download whole per product the way
   // Rtma/Rrfs do (see provider::NbmDataProvider's own class comment).
   // Every Nbm product instead downloads exactly one GRIB2 message via
   // AwsNexradDataProvider::DownloadGribMessageByIndex(), selected by
   // these three fields against the file's own ".idx" sidecar --
   // wgrib2's own PARAM/LEVEL/QUALIFIER vocabulary (e.g. "TMP"/
   // "2 m above ground"), *not* eccodes shortName (that's what the
   // existing `shortName` field above is for instead: once the one
   // matching message is downloaded, decode_grib still decodes it
   // normally by shortName -- real, live-confirmed defense against a
   // wrong idx match, not redundant, since the two vocabularies don't
   // always agree). nbmQualifier's default ("") matches only a record
   // with no qualifier at all -- see scwx::util::grib_idx::FindRecord()
   // for the exact semantics, including the real ambiguous case
   // (CAPE:surface:1 hour fcst: sharing its parameter+level with a
   // second record differing only by an "ens std dev" qualifier) this
   // mechanism exists to resolve.
   std::string nbmParameter {};
   std::string nbmLevel {};
   std::string nbmQualifier {};
};

// clang-format off

// MRMS: radar mosaic products, map::GribCategory::Mrms.
static const std::vector<ProductConfig> kMrmsProducts_ {
   // dBZ, matches res/palettes/wct/DR.pal's defined range (-20 to 75).
   // Below 0 dBZ (real calm-air returns, and MRMS's -999 "no coverage"
   // sentinel) is not rendered.
   {"Composite Reflectivity",
    "MergedReflectivityQCComposite_00.50", "", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},

   // Each is a real MRMS product with no per-site-radar equivalent
   // (unlike composite reflectivity, which single-site NEXRAD already
   // shows) -- that's deliberate, see the comment above this table.
   // Rotation Track's "no rotation" is a real, meaningful 0 (not "no
   // data"), so its noDataThreshold sits at -1.0 rather than at 0 like
   // reflectivity's does -- verified against live data (2026-09-20) that
   // its sentinel is -3/-999, comfortably below -1.0.
   //
   // MESH does NOT share that: a fresh live survey (2026-09-29) found its
   // real sentinel split in two -- -3 for "no radar coverage" but -1 for
   // "in coverage, no hail detected" (the vast majority of the domain on
   // a quiet day). A -1.0 noDataThreshold only excludes values strictly
   // *below* -1.0 (see grib.frag's `value < uNoDataThreshold`), so it let
   // that literal -1 sentinel through as real data, rendering as a dark
   // wash across the entire composite radar footprint -- visible live as
   // a "grey shading over the radar area" baked into the product itself.
   // MESH's noDataThreshold is 0.0 instead (hail size can't be
   // meaningfully negative, and the same -1/-3 split shows up across
   // most of the MRMS entries added below -- see the block comment
   // there for the general rule this became).

   // Rotational velocity difference. Verified against live data
   // (2026-09-20, an active severe weather day) that the file's raw
   // values are pre-scaled by ~1000 from the textbook s^-1 unit -- an
   // observed real max of 22 corresponds to an extreme ~0.022 s^-1
   // event, not 22 s^-1. Scale to 30 (raw units) so that real event
   // fills most of the ramp with a little headroom, not clipped near
   // the top.
   {"Rotation Track (30 min)",
    "RotationTrackML30min_00.50", "", 0.0f, 30.0f, -1.0f,
    PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "", "", -1, -1},

   // Maximum Estimated Size of Hail, mm. Severe criteria (1 in) is
   // 25.4mm, significant severe (2 in) is 50.8mm; extreme record cases
   // exceed 100mm. Verified against live data: real current max 61.8mm,
   // comfortably inside this range. Quantity is Accumulation (ties to the
   // same Settings > Units > Accumulation the user already has for radar
   // precip totals) rather than a dedicated "hail size" unit -- both are
   // just "millimeters of something", and Accumulation's Inches/
   // Millimeters options are exactly the right pair either way.
   {"Max Hail Size (60 min)",
    "MESH_Max_60min_00.50", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},

   // 1-hour radar-only precip estimate, mm. Verified against live data:
   // real current max was 71.4mm, already exceeding an initial 50mm
   // guess -- scaled to 80 for headroom above an observed real event
   // rather than clipping it.
   {"1-Hour Precip Estimate",
    "RadarOnly_QPE_01H_00.00", "", 0.0f, 80.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},

   // Everything below was added from a live 2026-09-29 survey (grib_ls +
   // grib_get_data against a real file per product): observed min/max/
   // most-common-values informed every offset/scale/noDataThreshold below
   // the same way the entries above were tuned by hand. MRMS GRIB2 files
   // carry no eccodes name/units metadata (grib_ls reports "unknown" for
   // all three) since they use local, non-WMO parameter tables, so units
   // and physical meaning below come from NSSL/MRMS product documentation,
   // not the file itself -- noted per group where that matters.
   //
   // Sentinel convention seen across nearly all of these: -1 means "below
   // this product's detection threshold" (a real, meaningful "none"), -3
   // or -999 means "no radar/model coverage at all". Both are always <0,
   // and every field below is a non-negative physical magnitude. Where a
   // product's own -1 "below threshold" sentinel is a dominant value
   // (MESH/POSH/SHI/EchoTop/VIL/VII/HeightCompositeReflectivity/
   // BrightBand -- verified per-product against the live survey, not
   // assumed), noDataThreshold is 0.0f rather than -1.0f: grib.frag's
   // cutoff is strict `value < uNoDataThreshold`, so -1.0f would let a
   // literal -1 sentinel through as real data, painting the entire
   // composite radar footprint a dark "no echo" wash (see the MESH
   // comment above this table for how that showed up live). 0.0f drops
   // both -1 and -3/-999 while still rendering a genuine 0. Where a
   // product's -1 is instead real signal, not a sentinel (rotation, gauge
   // influence, azimuthal shear all have legitimate negative/zero
   // values), the old -1.0f convention is kept and noted inline.

   // FLASH (NSSL flash-flood system) Average Recurrence Interval: how
   // rare (in years) the accumulated precip is vs. NOAA Atlas 14. Capped
   // at 200 years in the source data itself (verified live: 3h+ windows
   // all hit exactly 200.0 at their max), hence colorScale 200 for those;
   // shorter windows scaled to their own observed max with headroom.
   {"FLASH ARI (30 min)", "FLASH_QPE_ARI30M_00.00", "", 0.0f, 25.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (1 hr)", "FLASH_QPE_ARI01H_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (3 hr)", "FLASH_QPE_ARI03H_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (6 hr)", "FLASH_QPE_ARI06H_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (12 hr)", "FLASH_QPE_ARI12H_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (24 hr)", "FLASH_QPE_ARI24H_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},
   {"FLASH ARI (Max)", "FLASH_QPE_ARIMAX_00.00", "", 0.0f, 200.0f, -1.0f,
    PhysicalQuantity::None, "years", 0.0f, "", "", -1, -1},

   // FLASH Flood Guidance: the rainfall depth (mm) needed in this window
   // to cause flooding. Observed live max (215-310mm depending on window)
   // is too large to be the QPE/FFG ratio some NSSL docs also call this --
   // treated as the guidance depth itself, using the same Accumulation
   // quantity as other rainfall-depth fields above.
   {"FLASH Flood Guidance (1 hr)", "FLASH_QPE_FFG01H_00.00", "", 0.0f,
    250.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},
   {"FLASH Flood Guidance (3 hr)", "FLASH_QPE_FFG03H_00.00", "", 0.0f,
    350.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},
   {"FLASH Flood Guidance (6 hr)", "FLASH_QPE_FFG06H_00.00", "", 0.0f,
    330.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},
   {"FLASH Flood Guidance (Max)", "FLASH_QPE_FFGMAX_00.00", "", 0.0f,
    350.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},

   // FLASH hydrologic model output: three independent models (CREST,
   // SAC-SMA, HP) each produce max simulated streamflow (m^3/s), max
   // streamflow per unit basin area (m^3/s/km^2), and (CREST/SAC only)
   // max soil saturation (%). Live max streamflow hit tens of thousands
   // of m^3/s at a handful of major-river cells -- colorScale is set for
   // useful contrast on flash-flood-relevant (not major-river) flows and
   // deliberately clips that rare extreme tail rather than compressing
   // everything else toward black.
   {"FLASH CREST Max Streamflow", "FLASH_CREST_MAXSTREAMFLOW_00.00", "",
    0.0f, 500.0f, -1.0f, PhysicalQuantity::None, "m³/s", 0.0f, "", "",
    -1, -1},
   {"FLASH CREST Max Unit Streamflow",
    "FLASH_CREST_MAXUNITSTREAMFLOW_00.00", "", 0.0f, 5.0f, -1.0f,
    PhysicalQuantity::None, "m³/s/km²", 0.0f, "", "", -1, -1},
   {"FLASH CREST Max Soil Saturation", "FLASH_CREST_MAXSOILSAT_00.00", "",
    0.0f, 100.0f, -1.0f, PhysicalQuantity::None, "%", 0.0f, "", "", -1,
    -1},
   {"FLASH SAC Max Streamflow", "FLASH_SAC_MAXSTREAMFLOW_00.00", "", 0.0f,
    500.0f, -1.0f, PhysicalQuantity::None, "m³/s", 0.0f, "", "", -1,
    -1},
   {"FLASH SAC Max Unit Streamflow", "FLASH_SAC_MAXUNITSTREAMFLOW_00.00",
    "", 0.0f, 5.0f, -1.0f, PhysicalQuantity::None, "m³/s/km²",
    0.0f, "", "", -1, -1},
   {"FLASH SAC Max Soil Saturation", "FLASH_SAC_MAXSOILSAT_00.00", "",
    0.0f, 100.0f, -1.0f, PhysicalQuantity::None, "%", 0.0f, "", "", -1,
    -1},
   {"FLASH HP Max Streamflow", "FLASH_HP_MAXSTREAMFLOW_00.00", "", 0.0f,
    500.0f, -1.0f, PhysicalQuantity::None, "m³/s", 0.0f, "", "", -1,
    -1},
   {"FLASH HP Max Unit Streamflow", "FLASH_HP_MAXUNITSTREAMFLOW_00.00", "",
    0.0f, 5.0f, -1.0f, PhysicalQuantity::None, "m³/s/km²", 0.0f,
    "", "", -1, -1},

   // Radar-only QPE at other durations (01H is already above). Each
   // colorScale is its own observed live max plus ~10-15% headroom.
   {"15-Min Precip Estimate", "RadarOnly_QPE_15M_00.00", "", 0.0f, 30.0f,
    -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"3-Hour Precip Estimate", "RadarOnly_QPE_03H_00.00", "", 0.0f, 110.0f,
    -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"6-Hour Precip Estimate", "RadarOnly_QPE_06H_00.00", "", 0.0f, 180.0f,
    -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"12-Hour Precip Estimate", "RadarOnly_QPE_12H_00.00", "", 0.0f, 270.0f,
    -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"24-Hour Precip Estimate", "RadarOnly_QPE_24H_00.00", "", 0.0f, 550.0f,
    -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"48-Hour Precip Estimate", "RadarOnly_QPE_48H_00.00", "", 0.0f,
    1200.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},
   {"72-Hour Precip Estimate", "RadarOnly_QPE_72H_00.00", "", 0.0f,
    1800.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},
   {"Precip Estimate Since 12Z", "RadarOnly_QPE_Since12Z_00.00", "", 0.0f,
    450.0f, -1.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f,
    "", "", -1, -1},

   // Multi-sensor QPE (radar + gauge bias correction, Pass 2 = the final,
   // most gauge-corrected pass). Each window's colorScale set the same
   // way as radar-only QPE above.
   {"1-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_01H_Pass2_00.00", "", 0.0f, 75.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"3-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_03H_Pass2_00.00", "", 0.0f, 115.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"6-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_06H_Pass2_00.00", "", 0.0f, 190.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"12-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_12H_Pass2_00.00", "", 0.0f, 190.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"24-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_24H_Pass2_00.00", "", 0.0f, 330.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"48-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_48H_Pass2_00.00", "", 0.0f, 430.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"72-Hour Precip Estimate (Multi-Sensor)",
    "MultiSensor_QPE_72H_Pass2_00.00", "", 0.0f, 700.0f, -1.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},

   // Precip rate (instantaneous, mm/hr -- not a depth, so PhysicalQuantity
   // stays None rather than the Accumulation depth setting) and QC/
   // diagnostic layers.
   {"Instantaneous Precip Rate", "PrecipRate_00.00", "", 0.0f, 200.0f,
    -1.0f, PhysicalQuantity::None, "mm/hr", 0.0f, "", "", -1, -1},
   // Categorical precip-type code (0=none, 1=warm stratiform, 3=snow,
   // 6=convective, 7=hail mix, 10=cold stratiform, 91/96=tropical), not a
   // smooth physical quantity -- colorScale covers the common 0-10 code
   // range; rare tropical codes above that clip to the top color.
   {"Precip Type (MRMS)", "PrecipFlag_00.00", "", 0.0f, 10.0f, -1.0f,
    PhysicalQuantity::None, "code", 0.0f, "", "", -1, -1},
   {"Radar Quality Index", "RadarQualityIndex_00.00", "", 0.0f, 1.0f,
    -1.0f, PhysicalQuantity::None, "", 0.0f, "", "", -1, -1},
   {"Warm Rain Probability", "WarmRainProbability_00.50", "", 0.0f, 100.0f,
    -1.0f, PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   // Gauge influence on the multi-sensor bias correction, -1 (pure radar)
   // to +1 (pure gauge) -- colorOffset/-Scale map that full range, and
   // -1.0f still correctly drops the deeper -3 "no coverage" sentinel
   // since it's strictly below every real value here.
   {"Gauge Influence Index (1 hr)", "GaugeInflIndex_01H_Pass2_00.00", "",
    -1.0f, 2.0f, -1.0f, PhysicalQuantity::None, "", 0.0f, "", "", -1, -1},
   {"Gauge Influence Index (24 hr)", "GaugeInflIndex_24H_Pass2_00.00", "",
    -1.0f, 2.0f, -1.0f, PhysicalQuantity::None, "", 0.0f, "", "", -1, -1},

   // Hail: MESH at other windows (60min is already above), plus POSH
   // (probability of severe hail, %) and SHI (Severe Hail Index, an
   // unitless intensity score). colorScale kept at 100 across every MESH
   // window to match the existing 60min entry, even though shorter
   // windows' live max is smaller -- consistent sizes make windows
   // visually comparable to each other. noDataThreshold is 0.0f, not
   // -1.0f -- see the MESH comment above this table and the general-rule
   // comment at the top of this block for why (all four share the same
   // -1/-3 sentinel split).
   {"Max Hail Size (Instant)", "MESH_00.50", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},
   {"Max Hail Size (30 min)", "MESH_Max_30min_00.50", "", 0.0f, 100.0f,
    0.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"Max Hail Size (120 min)", "MESH_Max_120min_00.50", "", 0.0f, 100.0f,
    0.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"Max Hail Size (240 min)", "MESH_Max_240min_00.50", "", 0.0f, 100.0f,
    0.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"Max Hail Size (360 min)", "MESH_Max_360min_00.50", "", 0.0f, 100.0f,
    0.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"Max Hail Size (24 hr)", "MESH_Max_1440min_00.50", "", 0.0f, 100.0f,
    0.0f, PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "",
    -1, -1},
   {"Probability of Severe Hail", "POSH_00.50", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   {"Severe Hail Index", "SHI_00.50", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::None, "", 0.0f, "", "", -1, -1},

   // Rotation Track at other windows (ML 30min is already above) --
   // colorScale set per-window from its own live observed max, same
   // "raw units (x1000/s)" pre-scaling as the shipped entry. AzShear
   // (0-2km and 3-6km layers) is signed, so unlike everything else in
   // this table its colorOffset is negative and its noDataThreshold
   // can't use the -1.0f convention -- real values run well negative
   // (observed live to -34), so it's set far enough below that to only
   // catch the actual -999 "no coverage" sentinel.
   {"Rotation Track (30 min)", "RotationTrack30min_00.50", "", 0.0f, 50.0f,
    -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "", "",
    -1, -1},
   {"Rotation Track (60 min)", "RotationTrack60min_00.50", "", 0.0f, 50.0f,
    -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "", "",
    -1, -1},
   {"Rotation Track (120 min)", "RotationTrack120min_00.50", "", 0.0f,
    60.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track (240 min)", "RotationTrack240min_00.50", "", 0.0f,
    60.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track (360 min)", "RotationTrack360min_00.50", "", 0.0f,
    60.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track (24 hr)", "RotationTrack1440min_00.50", "", 0.0f,
    130.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f,
    "", "", -1, -1},
   {"Rotation Track ML (60 min)", "RotationTrackML60min_00.50", "", 0.0f,
    30.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track ML (120 min)", "RotationTrackML120min_00.50", "", 0.0f,
    50.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track ML (240 min)", "RotationTrackML240min_00.50", "", 0.0f,
    50.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track ML (360 min)", "RotationTrackML360min_00.50", "", 0.0f,
    50.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Rotation Track ML (24 hr)", "RotationTrackML1440min_00.50", "", 0.0f,
    55.0f, -1.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f, "",
    "", -1, -1},
   {"Azimuthal Shear (0-2km)", "MergedAzShear_0-2kmAGL_00.50", "", -40.0f,
    80.0f, -900.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f,
    "", "", -1, -1},
   {"Azimuthal Shear (3-6km)", "MergedAzShear_3-6kmAGL_00.50", "", -20.0f,
    40.0f, -900.0f, PhysicalQuantity::None, "raw units (x1000 /s)", 0.0f,
    "", "", -1, -1},

   // Reflectivity/storm-structure family. Every dBZ product here reuses
   // Composite Reflectivity's -20/95 color range and 0.0f noDataThreshold
   // from the top of this table (dBZ is never legitimately negative
   // below about -30, so 0.0f cleanly drops MRMS's -99/-999 sentinels the
   // same way it does for Composite Reflectivity).
   {"Reflectivity at Lowest Altitude", "ReflectivityAtLowestAltitude_00.50",
    "", -20.0f, 95.0f, 0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "",
    -1, -1},
   {"Composite Reflectivity (1-hr Max)", "CREF_1HR_MAX_00.50", "", -20.0f,
    95.0f, 0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Base Reflectivity (1-hr Max)", "BREF_1HR_MAX_00.50", "", -20.0f,
    95.0f, 0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Low-Level Composite Reflectivity", "LowLevelCompositeReflectivity_00.50",
    "", -20.0f, 95.0f, 0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "",
    -1, -1},
   {"Layer Composite Reflectivity (Low)",
    "LayerCompositeReflectivity_Low_00.50", "", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Layer Composite Reflectivity (High)",
    "LayerCompositeReflectivity_High_00.50", "", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Layer Composite Reflectivity (Super)",
    "LayerCompositeReflectivity_Super_00.50", "", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Reflectivity at 0C", "Reflectivity_0C_00.50", "", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Reflectivity at -10C", "Reflectivity_-10C_00.50", "", -20.0f, 95.0f,
    0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Reflectivity at -20C", "Reflectivity_-20C_00.50", "", -20.0f, 95.0f,
    0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},
   {"Seamless Hybrid Scan Reflectivity", "SeamlessHSR_00.00", "", -20.0f,
    95.0f, 0.0f, PhysicalQuantity::None, "dBZ", 0.0f, "", "", -1, -1},

   // Heights and vertically-integrated fields -- all non-negative
   // magnitudes. Every one of these except Seamless HSR Height showed the
   // same -1/-3 sentinel split as MESH in the live survey (Seamless HSR
   // Height's survey sample never showed -1, so it keeps the -1.0f
   // convention), so they get the same 0.0f fix.
   {"Composite Reflectivity Height", "HeightCompositeReflectivity_00.50",
    "", 0.0f, 20000.0f, 0.0f, PhysicalQuantity::None, "m", 0.0f, "", "",
    -1, -1},
   {"Seamless HSR Height", "SeamlessHSRHeight_00.00", "", 0.0f, 18.0f,
    -1.0f, PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"Echo Top (18 dBZ)", "EchoTop_18_00.50", "", 0.0f, 20.0f, 0.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"Echo Top (30 dBZ)", "EchoTop_30_00.50", "", 0.0f, 20.0f, 0.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"Echo Top (50 dBZ)", "EchoTop_50_00.50", "", 0.0f, 10.0f, 0.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"Echo Top (60 dBZ)", "EchoTop_60_00.50", "", 0.0f, 5.0f, 0.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"Vertically Integrated Liquid", "VIL_00.50", "", 0.0f, 35.0f, 0.0f,
    PhysicalQuantity::None, "kg/m²", 0.0f, "", "", -1, -1},
   {"VIL Density", "VIL_Density_00.50", "", 0.0f, 3.0f, 0.0f,
    PhysicalQuantity::None, "g/m³", 0.0f, "", "", -1, -1},
   {"VIL (2-hr Max)", "VIL_Max_120min_00.50", "", 0.0f, 50.0f, 0.0f,
    PhysicalQuantity::None, "kg/m²", 0.0f, "", "", -1, -1},
   {"VIL (24-hr Max)", "VIL_Max_1440min_00.50", "", 0.0f, 70.0f, 0.0f,
    PhysicalQuantity::None, "kg/m²", 0.0f, "", "", -1, -1},
   {"Vertically Integrated Ice", "VII_00.50", "", 0.0f, 20.0f, 0.0f,
    PhysicalQuantity::None, "kg/m²", 0.0f, "", "", -1, -1},

   // Melting-layer / freezing-height diagnostics. H50/H60 use a different
   // sentinel pair (-99/-999, not -1/-3) that -1.0f already excludes
   // correctly, so they're unaffected by the MESH-style fix below. H60
   // pair had zero real signal on survey day (a quiet day for extreme
   // 60 dBZ echoes aloft -- the product itself is legitimate, just
   // unverified live); scaled the same as their H50 counterparts since
   // both share the same "height above a temperature level" physical
   // meaning.
   {"50 dBZ Height Above 0C", "H50_Above_0C_00.50", "", 0.0f, 8.0f, -1.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"50 dBZ Height Above -20C", "H50_Above_-20C_00.50", "", 0.0f, 3.0f,
    -1.0f, PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"60 dBZ Height Above 0C", "H60_Above_0C_00.50", "", 0.0f, 8.0f, -1.0f,
    PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   {"60 dBZ Height Above -20C", "H60_Above_-20C_00.50", "", 0.0f, 3.0f,
    -1.0f, PhysicalQuantity::None, "km", 0.0f, "", "", -1, -1},
   // Bright Band Top/Bottom Height DO show the -1/-3 split (-1 here means
   // "no bright band detected", still a real MESH-style sentinel), so
   // 0.0f like the rest of this fix.
   {"Bright Band Top Height", "BrightBandTopHeight_00.00", "", 0.0f,
    7000.0f, 0.0f, PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},
   {"Bright Band Bottom Height", "BrightBandBottomHeight_00.00", "", 0.0f,
    6000.0f, 0.0f, PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},

   // Lightning. Probability fields (%) use a plain 0.0f noDataThreshold
   // since the sentinel here is an enormous -99900 (far below any real
   // percentage or density); the jump flag is a simple 0/1 boolean so
   // -0.5f sits cleanly between "no jump" (0) and the negative sentinels.
   {"Lightning Probability (30 min)",
    "LightningProbabilityNext30minGrid_scale_1", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   {"Lightning Probability (60 min)",
    "LightningProbabilityNext60minGrid_scale_1", "", 0.0f, 100.0f, 0.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   {"Lightning Jump (5 min)", "LtgJumpGrid_Max_005min_scale_1", "", 0.0f,
    1.0f, -0.5f, PhysicalQuantity::None, "", 0.0f, "", "", -1, -1},
   {"CG Lightning Density (1 min)", "NLDN_CG_001min_AvgDensity_00.00", "",
    0.0f, 2.0f, 0.0f, PhysicalQuantity::None, "strikes/min/km²", 0.0f,
    "", "", -1, -1},
   {"CG Lightning Density (5 min)", "NLDN_CG_005min_AvgDensity_00.00", "",
    0.0f, 2.0f, 0.0f, PhysicalQuantity::None, "strikes/min/km²", 0.0f,
    "", "", -1, -1},
   {"CG Lightning Density (15 min)", "NLDN_CG_015min_AvgDensity_00.00", "",
    0.0f, 4.0f, 0.0f, PhysicalQuantity::None, "strikes/min/km²", 0.0f,
    "", "", -1, -1},
   {"CG Lightning Density (30 min)", "NLDN_CG_030min_AvgDensity_00.00", "",
    0.0f, 5.5f, 0.0f, PhysicalQuantity::None, "strikes/min/km²", 0.0f,
    "", "", -1, -1},

   // Model-derived background fields feeding MRMS's own precip-type/
   // melting-layer algorithms. Temperatures are already in Celsius in the
   // raw file (live range -4 to 31C, nowhere near Kelvin magnitudes), so
   // these use PhysicalQuantity::None with an explicit "C" unit rather
   // than TemperatureKelvin -- that enum assumes a raw Kelvin value and
   // would apply the wrong conversion here.
   {"Model Freezing Level Height", "Model_0degC_Height_00.50", "", 0.0f,
    6500.0f, -1.0f, PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},
   {"Model Surface Temp", "Model_SurfaceTemp_00.50", "", -10.0f, 45.0f,
    -100.0f, PhysicalQuantity::None, "°C", 0.0f, "", "", -1, -1},
   {"Model Wet Bulb Temp", "Model_WetBulbTemp_00.50", "", -10.0f, 40.0f,
    -100.0f, PhysicalQuantity::None, "°C", 0.0f, "", "", -1, -1},
};

// RTMA: rolling surface analysis, map::GribCategory::Rtma. noDataThreshold
// sits far below every real value (rather than at a sentinel like MRMS's)
// because RTMA is a QC'd, full-CONUS surface analysis with no coverage
// gaps -- confirmed via grib_ls against a real file: numberOfMissing=0
// for every field -- so there's no sentinel to filter, just headroom
// under legitimately-always-positive values (Kelvin, wind speed).
static const std::vector<ProductConfig> kRtmaProducts_ {
   // 2m temperature, Kelvin. Verified against live data: real range
   // 270.15-314.64K; scaled to 260-325K for seasonal/regional headroom.
   {"2m Temperature", "", "2t", 260.0f, 65.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},

   // 2m dewpoint, Kelvin. Verified against live data: real range
   // 242.07-302.49K; scaled to 235-310K for headroom (Gulf Coast summer
   // dewpoints can approach the top of this range).
   {"2m Dewpoint", "", "2d", 235.0f, 75.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},

   // 10m wind speed, m/s. Verified against live data: real range
   // 0-26 m/s; scaled to 0-30 for headroom above an observed calm day.
   {"10m Wind Speed", "", "10si", 0.0f, 30.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "", "", -1, -1},

   // 10m wind gust, m/s (shortName i10fg). Flagged as the most
   // operationally useful unsurfaced RTMA field -- gusts are what
   // actually matter for damage/warning criteria, not sustained speed.
   // Verified against live data: real range 0-28.4 m/s; scaled to 0-40
   // for headroom toward severe/damaging gust events (that one snapshot
   // wasn't an active severe day), same reasoning as 10m Wind Speed above.
   {"10m Wind Gust", "", "i10fg", 0.0f, 40.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "", "", -1, -1},

   // Surface visibility, metres (shortName vis). Verified against live
   // data: real range 1-16000 m -- 16000 is the field's own ceiling
   // (NWP's standard "unlimited visibility" cap, not this particular
   // day's actual max), so colorScale is set to that physical cap
   // directly rather than adding headroom above an observed value like
   // every other field here does. Quantity is None (raw metres) for now,
   // not a proper distance-unit conversion -- no PhysicalQuantity::
   // Distance category exists yet (UnitSettings has no visibility/range
   // unit either), scoped as a follow-up rather than added tonight.
   {"Visibility", "", "vis", 0.0f, 16000.0f, -999.0f,
    PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},
};

// RRFS: forecast model, map::GribCategory::Rrfs. Phase 1 scope: all
// 2dfld, fixed at RrfsDataProvider's resolved synoptic-cycle F000, no
// forecast-hour selection yet. Same no-coverage-gaps convention as RTMA
// above -- RRFS's fields are full model grids too.
// RRFS's fields are full model grids with no missing cells, so unlike MRMS
// there is no "no coverage" sentinel to drop -- but a field whose zero means
// "nothing here" (no rain, no CAPE, clear sky, no echo) still can't be drawn
// at zero: every such cell would be painted the bottom colour of the shared
// reflectivity palette, a grey blanket over the whole domain (in a real F000
// file 92% of simulated-reflectivity cells are <= 0 dBZ, 88% of precip-rate
// and 51% of CAPE cells are 0). Those products set noDataThreshold just above
// the meaningless floor so that part is transparent. Cutoffs were read off a
// live file's distributions where the field is in F000 (reflectivity, VIL,
// precip rate, CAPE, cloud cover); accumulated precipitation and lightning
// threat aren't in F000, so theirs are conservative guesses. Continuous
// fields (temperature, wind, pressure, heights, satellite bands) are meant
// to fill the domain and are unchanged.
static const std::vector<ProductConfig> kRrfsProducts_ {
   // Mean sea level pressure (ETA reduction), Pa. Verified against live
   // data on two separate days: 100203-103017 Pa (2026-09-20) and
   // 99345.6-103127 Pa (2026-09-25), both comfortably inside a
   // 97000-107000 Pa (970-1070 hPa) headroom range toward a deep
   // continental low and a strong winter high. Rendered as isolines every
   // 400 Pa (4 hPa, the standard synoptic MSLP contour interval), not a
   // solid fill -- Phase 1 shipped as a plain fill deliberately, to prove
   // the provider/S3-key/decode slice end to end first; now that it has,
   // this is the "revisit the deferred contour work" phase. colorOffset/
   // colorScale below are vestigial in contour mode (the shader ignores
   // them once contourInterval is set) but left as-is rather than zeroed,
   // in case a fill/contour toggle is ever added.
   {"Mean Sea Level Pressure", "", "mslet", 97000.0f, 10000.0f, -999.0f,
    PhysicalQuantity::PressurePascals, "Pa", 400.0f, "", "", -1, -1},

   // Composite lightning threat, dimensionless (NCEP's own composite
   // index, not a literal flash count) -- "entire atmosphere, 0-1 hour
   // max fcst". Verified against live data: real range 0-36.46; scaled
   // to 0-50 for headroom above an observed active day. Note: RRFS also
   // carries LTNGSD (lightning strike density, two near-surface levels)
   // in the same file, but its shortName resolves to "unknown" in
   // eccodes (no definition table entry for this parameter combination
   // yet) -- decode_grib selects by shortName, so that field isn't
   // usable this way; LTNG is the one that works.
   {"Lightning Threat", "", "ltng", 0.0f, 50.0f, 0.1f,
    PhysicalQuantity::None, "index", 0.0f, "", "", -1, -1},

   // Significant Tornado Parameter, fixed-layer form (Thompson et al.
   // 2003) -- SPC's documented predecessor to today's effective-layer
   // default; true effective-layer STP needs a full vertical-profile
   // parcel test RRFS doesn't ship as ready fields, deliberately out of
   // scope here (see decode_grib.cpp's ComputeStp for the full formula
   // and field-selection notes). Computed, not decoded -- shortName is
   // unused (empty); decode_grib's `--derived stp` mode has its own
   // fixed, internally-hardcoded field list. Verified against live data:
   // real range -1.11 to 2.26 on a quiet (non-severe) day; scaled to
   // -2 to 6 for headroom toward a genuinely favorable severe-weather
   // setup, which this one observed day wasn't.
   {"STP (Fixed-Layer)", "", "", -2.0f, 8.0f, 0.1f,
    PhysicalQuantity::None, "index", 0.0f, "stp", "", -1, -1},

   // Everything below verified against the same real downloaded 2dfld
   // file as MSLET/STP above (2026-09-25) -- a full field inventory,
   // 262 messages, confirmed how few of them were actually exposed
   // before this pass. Several share a shortName with another level/
   // layer in the same file (see ProductConfig::typeOfLevel's own
   // comment) -- every entry below that sets typeOfLevel/topLevel/
   // bottomLevel needed it to resolve to the intended message, confirmed
   // by decoding each one directly and checking its range matched.

   // Model-derived composite reflectivity, three fixed levels -- RRFS's
   // `rare` field, not an MRMS-style true composite (max across all
   // tilts); each level is its own product since eccodes has no single
   // "composite" message for it. Same -20/95 range as MRMS reflectivity
   // (kMrmsProducts_ above) so it reads on the same visual scale.
   {"Simulated Reflectivity (1km AGL)", "", "rare", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "heightAboveGround", 1000, 1000},
   {"Simulated Reflectivity (4km AGL)", "", "rare", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "heightAboveGround", 4000, 4000},
   {"Simulated Reflectivity (-10C level)", "", "rare", -20.0f, 95.0f, 0.0f,
    PhysicalQuantity::None, "dBZ", 0.0f, "", "isothermal", 263, 263},

   // Vertically Integrated Liquid, kg/m^2. Verified: real range
   // 0.001-197.3; scaled to 0-220 for headroom.
   {"VIL", "", "veril", 0.0f, 220.0f, 0.5f,
    PhysicalQuantity::None, "kg/m^2", 0.0f, "", "", -1, -1},

   // RRFS's own forecast surface visibility, metres -- distinct from
   // RTMA's analysis-only vis (kRtmaProducts_ above). Verified: real
   // range 23-86586 m, notably not capped at 16000m the way RTMA's own
   // vis is -- this field's own ceiling is higher, so scaled to that
   // observed max plus headroom rather than reusing RTMA's constant.
   {"Visibility", "", "vis", 0.0f, 90000.0f, -999.0f,
    PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},

   // RRFS's own forecast 10m wind gust, m/s -- distinct from RTMA's
   // analysis-only gust (kRtmaProducts_ above, shortName i10fg).
   // Verified: real range 0-39.8; scaled to 0-45 for headroom.
   {"Wind Gust", "", "gust", 0.0f, 45.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "", "", -1, -1},

   // Surface pressure, Pa -- station pressure, not MSLP (see MSLET
   // above for that). Verified: real range 64432-102960 Pa (terrain
   // drives this range far more than weather does, same as RTMA's own
   // unsurfaced sp field); scaled to 60000-105000 for headroom.
   {"Surface Pressure", "", "sp", 60000.0f, 45000.0f, -999.0f,
    PhysicalQuantity::PressurePascals, "Pa", 0.0f, "", "", -1, -1},

   // Instantaneous precipitation rate, kg/m^2/s (== mm/s). Verified:
   // real range 0-0.0791; scaled to 0-0.1 for headroom. Quantity is None
   // (raw rate), not AccumulationMillimeters -- that setting is for
   // totals, converting a rate through it would be wrong.
   {"Precipitation Rate", "", "prate", 0.0f, 0.1f, 0.000028f,
    PhysicalQuantity::None, "kg/m^2/s", 0.0f, "", "", -1, -1},

   // 1-hour accumulated precipitation, kg/m^2 (== mm). RRFS's 2dfld file
   // carries *two* "tp" messages sharing identical surface/level-0
   // metadata -- this one and "Total Precipitation" below -- disambiguated
   // by lengthOfTimeRange (always 1 here, regardless of forecast hour;
   // see decode_grib's FindMessage for the full mechanism, confirmed live
   // via grib_ls against a real forecast-hour-5 file, not assumed).
   // Verified: real range 0-107.8 at that hour; scaled to 0-120 for
   // headroom.
   {"1-Hour Precipitation", "", "tp", 0.0f, 120.0f, 0.1f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1,
    -1, 1},

   // Accumulated precipitation since the run's own start (F000), kg/m^2
   // (== mm) -- the *other* "tp" message, disambiguated by startStep
   // (always 0 here, the run-total's own defining trait, vs. the 1-hour
   // field's forecast-hour-dependent startStep). Verified: real range
   // 0-139.8 at forecast hour 5 -- scaled to 0-300 for headroom, but
   // real cost not silently absorbed: this keeps growing with lead time
   // (it is a running total over the whole forecast, up to 84h), so a
   // long-enough run's true max could still exceed this; only checked at
   // one (early) forecast hour, not the full range.
   {"Total Precipitation", "", "tp", 0.0f, 300.0f, 0.1f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1,
    0, -1},

   // -- prslev-backed products below (RRFS's *other* per-cycle file,
   // isobaric-level fields -- see provider::RrfsFileFamily's own doc for
   // its real, different hourly-availability rule from every product
   // above). Every field below shares typeOfLevel "isobaricInhPa" with
   // topLevel==bottomLevel==level (confirmed live: a single isobaric
   // level, not a layer, reports identical top/bottom -- so the existing
   // typeOfLevel/topLevel/bottomLevel disambiguation already handles
   // this with no decode_grib changes). eccodes' own shortNames here
   // ("gh"/"t"/"u"/"v") differ from 2dfld's surface-field naming
   // convention ("hgt" doesn't exist at all here -- confirmed live, not
   // assumed from wxqt's own docs, which named a different field "hgt"
   // too, see [[supercell-wx-rrfs-plan]] for that same lesson learned
   // once already with STP's LCL height).

   // 500mb geopotential height, metres -- rendered as isolines every 60m
   // (the standard US synoptic 500mb contour interval, 6 decameters),
   // matching MSLET's own contour treatment above for the same
   // "troughs/ridges read as lines, not a color wash" reasoning.
   // Verified: real range 5493.75-5952.81m at one forecast hour;
   // colorOffset/colorScale below are vestigial in contour mode (see
   // MSLET's own comment), left non-zero anyway.
   {"500mb Height", "", "gh", 5000.0f, 1500.0f, -999.0f,
    PhysicalQuantity::None, "m", 60.0f, "", "isobaricInhPa", 500, 500, -1, -1,
    provider::RrfsFileFamily::PressureLevel},

   // 500mb temperature, K. Verified: real range 248.1-273.8K; scaled to
   // 240-290K for headroom.
   {"500mb Temperature", "", "t", 240.0f, 50.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "isobaricInhPa",
    500, 500, -1, -1, provider::RrfsFileFamily::PressureLevel},

   // 700mb temperature, K -- SHIP's own T700 input (see ComputeStp's
   // class-comment-adjacent notes on SHIP being blocked on this exact
   // field); useful as its own product regardless of whether SHIP itself
   // gets built on top of it. Verified: real range 264.7-289.4K; scaled
   // to 255-300K for headroom.
   {"700mb Temperature", "", "t", 255.0f, 45.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "isobaricInhPa",
    700, 700, -1, -1, provider::RrfsFileFamily::PressureLevel},

   // 500mb wind speed, m/s -- sqrt(u^2+v^2) via decode_grib's `--derived
   // wind500`, same shape as shear6/wind10 above (see
   // ComputeVectorMagnitude). Raw u/v components aren't exposed as their
   // own products here -- a fill of signed velocity components isn't
   // meaningfully readable the way a scalar speed is; u/v as wind-barb
   // inputs is the eventual point of having this level at all (see
   // [[supercell-wx-grib-extension]]'s "wind-barb default-pairing" idea),
   // not yet built. Verified: real range 0.004-44.5 m/s; scaled to 0-60
   // for headroom.
   {"500mb Wind Speed", "", "", 0.0f, 60.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "wind500", "", -1,
    -1, -1, -1, provider::RrfsFileFamily::PressureLevel},

   // Significant Hail Parameter (SHIP), SPC mesoanalysis form -- the one
   // product that needed prslev to exist at all (T500/T700, see
   // decode_grib.cpp's ComputeShip for the full formula/field-selection
   // notes). Computed, not decoded -- shortName is unused (empty);
   // decode_grib's `--derived ship` mode has its own fixed field list,
   // spanning *two* input files (2dfld + prslev), which
   // FetchShipSelection()/ApplyShipDownload() handle -- this product
   // never reaches RequestFrame()/QueueDownload()/ApplyCachedDownload().
   // rrfsFileFamily is left at its TwoDField default -- meaningless here,
   // since FetchShipSelection() bypasses GetPrefix()/FindKey() entirely
   // for both of its own inputs. Verified against live data: real range
   // -0.0007 to 1.24 on a quiet (non-severe) day; scaled to 0-6 for
   // headroom toward SPC's own documented "very high" (>4) ceiling,
   // which this one observed day wasn't close to, same reasoning as
   // STP's own headroom above.
   {"SHIP", "", "", 0.0f, 6.0f, 0.1f, PhysicalQuantity::None, "index",
    0.0f, "ship", "", -1, -1},

   // Planetary boundary layer height, metres AGL. Verified: real range
   // 19-2247 m; scaled to 0-2500 for headroom.
   {"Boundary Layer Height", "", "blh", 0.0f, 2500.0f, -999.0f,
    PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},

   // Precipitable water, kg/m^2 -- numerically equals mm of liquid
   // water equivalent (1 kg/m^2 == 1 mm), so AccumulationMillimeters is
   // physically correct here, not just a unit-name coincidence.
   // Verified: real range 1.6-83.7; scaled to 0-90 for headroom.
   {"Precipitable Water", "", "pwat", 0.0f, 90.0f, -999.0f,
    PhysicalQuantity::AccumulationMillimeters, "mm", 0.0f, "", "", -1, -1},

   // Cloud ceiling, metres AGL. Verified: real range 28-15993; scaled to
   // 0-16000, matching RTMA's own ceiling convention (kRtmaProducts_
   // doesn't have one yet, but this mirrors its vis field's cap).
   {"Ceiling", "", "ceil", 0.0f, 16000.0f, -999.0f,
    PhysicalQuantity::None, "m", 0.0f, "", "", -1, -1},

   // Total cloud cover, %. Two "tcc" messages exist in the same file
   // (boundaryLayerCloudLayer and atmosphereSingleLayer) -- picked
   // atmosphereSingleLayer as the more standard "whole column" total,
   // disambiguated explicitly rather than trusting message order.
   {"Total Cloud Cover", "", "tcc", 0.0f, 100.0f, 5.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "atmosphereSingleLayer", 0, 0},
   {"Low Cloud Cover", "", "lcc", 0.0f, 100.0f, 5.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   {"Mid Cloud Cover", "", "mcc", 0.0f, 100.0f, 5.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},
   {"High Cloud Cover", "", "hcc", 0.0f, 100.0f, 5.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1},

   // Surface-based CAPE/CIN, J/kg -- the file also carries 3 mixed-layer
   // CAPE/CIN variants (90/180/255 mb) sharing the same "cape"/"cin"
   // shortName; "surface" disambiguates to the SB (not MU) variant.
   // Verified: CAPE 0-5006, CIN -874-0; scaled with headroom.
   {"SBCAPE", "", "cape", 0.0f, 5500.0f, 50.0f,
    PhysicalQuantity::None, "J/kg", 0.0f, "", "surface", 0, 0},
   {"SBCIN", "", "cin", -1000.0f, 1000.0f, -999.0f,
    PhysicalQuantity::None, "J/kg", 0.0f, "", "surface", 0, 0},

   // Most-unstable CAPE/CIN (180mb mixed layer -- the variant SHIP's own
   // formula uses, see decode_grib.cpp; SHIP itself stays deferred until
   // prslev support exists, but MUCAPE/MUCIN are useful fields alone).
   // Verified: CAPE 0-4224, CIN -1364-0.
   {"MUCAPE", "", "cape", 0.0f, 4600.0f, 50.0f,
    PhysicalQuantity::None, "J/kg", 0.0f, "", "pressureFromGroundLayer",
    18000, 0},
   {"MUCIN", "", "cin", -1500.0f, 1500.0f, -999.0f,
    PhysicalQuantity::None, "J/kg", 0.0f, "", "pressureFromGroundLayer",
    18000, 0},

   // Storm-relative helicity, m^2/s^2 -- "hlcy" appears at both 0-1km and
   // 0-3km in the same file, disambiguated by bottomLevel (topLevel is
   // always the surface, 0). Verified: 0-1km range -210.4-433.7, 0-3km
   // range -460-1160.
   {"0-1km SRH", "", "hlcy", -250.0f, 750.0f, -999.0f,
    PhysicalQuantity::None, "m^2/s^2", 0.0f, "", "heightAboveGroundLayer",
    1000, 0},
   {"0-3km SRH", "", "hlcy", -500.0f, 1700.0f, -999.0f,
    PhysicalQuantity::None, "m^2/s^2", 0.0f, "", "heightAboveGroundLayer",
    3000, 0},

   // 0-6km bulk shear magnitude, m/s -- computed, not decoded (see
   // decode_grib's `--derived shear6`, sqrt(vucsh^2+vvcsh^2) at the
   // 0-6000m heightAboveGroundLayer, the same input SHIP/STP's own
   // shear term uses). Verified against live data: real range 0-54.1;
   // scaled to 0-60 for headroom.
   {"0-6km Shear", "", "", 0.0f, 60.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "shear6", "", -1,
    -1},

   // RRFS's own forecast 10m wind speed, m/s -- computed from the
   // native 10u/10v components (no native speed field exists the way
   // RTMA's 10si does), distinct from RTMA's analysis-only 10m Wind
   // Speed (kRtmaProducts_ above). Verified: real range 0-32; scaled to
   // 0-35 for headroom.
   {"10m Wind Speed", "", "", 0.0f, 35.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "wind10", "", -1,
    -1},

   // Simulated GOES-16 ABI brightness temperature, one product per band
   // (7-16, all "unknown"-free unique shortNames, no disambiguation
   // needed). Verified against live data: each band's own real range
   // used directly as colorOffset/colorScale (no added headroom -- these
   // are already full observed ranges for a real day, revisit if a
   // future day is seen to exceed them).
   {"Sat Band 7 (3.9um)", "", "SBTA167", 190.0f, 120.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 8 (6.2um)", "", "SBTA168", 190.0f, 75.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 9 (6.9um)", "", "SBTA169", 190.0f, 80.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 10 (7.3um)", "", "SBTA1610", 190.0f, 90.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 11 (8.4um)", "", "SBTA1611", 190.0f, 115.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 12 (9.6um)", "", "SBTA1612", 205.0f, 75.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 13 (10.3um)", "", "SBTA1613", 190.0f, 115.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 14 (11.2um)", "", "SBTA1614", 190.0f, 115.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 15 (12.3um)", "", "SBTA1615", 190.0f, 112.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
   {"Sat Band 16 (13.3um)", "", "SBTA1616", 190.0f, 92.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1},
};

// Categories whose fields are fetched one at a time through a wgrib2-style
// .idx sidecar (see provider::IdxModelProvider), sharing one set of
// cycle/forecast-hour and per-product fetch paths: NBM, whose rules are
// compiled in, and user-imported models, whose rules come from a config.
static bool IsIdxCategory(map::GribCategory category)
{
   return category == map::GribCategory::Nbm ||
          category == map::GribCategory::User;
}

// NBM (National Blend of Models): map::GribCategory::Nbm. A first,
// deliberately small set -- every entry's own nbmParameter/nbmLevel/
// nbmQualifier confirmed live (2026-09-26) against a real downloaded
// blend.t12z.core.f024.co.grib2 idx, and shortName confirmed live via
// grib_ls against the actual single-message file each one range-
// downloads to (see AwsNexradDataProvider::DownloadGribMessageByIndex(),
// scwx::util::grib_idx). Precipitation/accumulation fields are
// deliberately not included yet -- NBM's own idx step text ("N-1 to N
// hour acc fcst") embeds the forecast hour itself, so picking "the
// 1-hour amount" needs a per-request dynamic step match this table's
// static nbmQualifier alone can't express; a real follow-up, not
// forgotten.
static const std::vector<ProductConfig> kNbmProducts_ {
   // shortName "2t" (not "tmp"/"t") -- confirmed live via grib_ls, same
   // "eccodes' own shortName isn't the obvious guess" lesson this
   // project has hit repeatedly (STP's LCL height, RRFS prslev's
   // isobaric fields, ...).
   {"2m Temperature", "", "2t", 250.0f, 70.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1, -1, -1,
    provider::RrfsFileFamily::TwoDField, "TMP", "2 m above ground", ""},
   {"2m Dewpoint", "", "2d", 230.0f, 70.0f, -999.0f,
    PhysicalQuantity::TemperatureKelvin, "K", 0.0f, "", "", -1, -1, -1, -1,
    provider::RrfsFileFamily::TwoDField, "DPT", "2 m above ground", ""},
   // "WIND" in the idx decodes to shortName "10si" (wind *speed*
   // directly, not raw u/v components) -- confirmed live, another real
   // shortName surprise.
   {"10m Wind Speed", "", "10si", 0.0f, 30.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "", "", -1, -1, -1,
    -1, provider::RrfsFileFamily::TwoDField, "WIND", "10 m above ground",
    ""},
   {"10m Wind Gust", "", "i10fg", 0.0f, 40.0f, -999.0f,
    PhysicalQuantity::SpeedMetersPerSecond, "m/s", 0.0f, "", "", -1, -1, -1,
    -1, provider::RrfsFileFamily::TwoDField, "GUST", "10 m above ground",
    ""},
   // "surface" (not the several "reserved"-level TCDC records that share
   // that exact parameter+level+qualifier combination with each other in
   // the real idx -- a genuinely ambiguous group this table avoids
   // entirely rather than picking one arbitrarily).
   {"Total Cloud Cover", "", "tcc", 0.0f, 100.0f, -999.0f,
    PhysicalQuantity::None, "%", 0.0f, "", "", -1, -1, -1, -1,
    provider::RrfsFileFamily::TwoDField, "TCDC", "surface", ""},
};
// clang-format on

// GribCategory::User has no compiled-in table: its products are the rows of
// whichever model UserModelRegistry has selected, converted here.
PhysicalQuantity QuantityFromName(std::string_view name)
{
   if (name == "temperature_kelvin")
   {
      return PhysicalQuantity::TemperatureKelvin;
   }
   if (name == "speed_meters_per_second")
   {
      return PhysicalQuantity::SpeedMetersPerSecond;
   }
   if (name == "accumulation_millimeters")
   {
      return PhysicalQuantity::AccumulationMillimeters;
   }
   if (name == "pressure_pascals")
   {
      return PhysicalQuantity::PressurePascals;
   }
   return PhysicalQuantity::None;
}

std::vector<ProductConfig>
BuildUserProducts(const std::optional<manager::UserModelEntry>& model)
{
   std::vector<ProductConfig> products;
   if (!model)
   {
      return products;
   }

   for (const auto& spec : model->config.products)
   {
      const auto& d = spec.display;

      // Same field order as the built-in idx (NBM) rows above.
      products.push_back({spec.name,
                          "",
                          spec.shortName,
                          d.colorOffset,
                          d.colorScale,
                          d.noDataThreshold,
                          QuantityFromName(d.quantity),
                          d.units,
                          d.type == "contour" ? d.contourInterval : 0.0f,
                          "",
                          "",
                          -1,
                          -1,
                          -1,
                          -1,
                          provider::RrfsFileFamily::TwoDField,
                          spec.index.parameter,
                          spec.index.level,
                          spec.index.qualifier});
   }
   return products;
}

// Every table ever built is kept, never freed: Products() hands out
// references that background fetch threads may still be reading when the
// user switches models, and a deque never moves existing elements. A table
// is a few KB and one is added per model switch.
std::mutex                             gUserProductsMutex;
std::deque<std::vector<ProductConfig>> gUserProducts(1);

const std::vector<ProductConfig>& CurrentUserProducts()
{
   std::lock_guard lock(gUserProductsMutex);
   return gUserProducts.back();
}

void RebuildUserProducts(const std::optional<manager::UserModelEntry>& model)
{
   auto            products = BuildUserProducts(model);
   std::lock_guard lock(gUserProductsMutex);
   gUserProducts.push_back(std::move(products));
}

const std::vector<ProductConfig>& Products(map::GribCategory category)
{
   switch (category)
   {
   case map::GribCategory::Mrms:
      return kMrmsProducts_;
   case map::GribCategory::Rtma:
      return kRtmaProducts_;
   case map::GribCategory::Nbm:
      return kNbmProducts_;
   case map::GribCategory::User:
      return CurrentUserProducts();
   case map::GribCategory::Rrfs:
   default:
      return kRrfsProducts_;
   }
}

// User asked for a 4 minute default -- roughly half MRMS's native ~2
// minute cadence, meant for "glance at the national picture" rather than
// chasing every update.
static constexpr int kPollIntervalMs_ = 4 * 60 * 1000;

// Size-based, not count-based: a cached download ranges from ~1.5-2MB
// (MRMS) through ~84MB (RTMA, one file for all 13 fields) up to ~320MB
// (RRFS's 2dfld file) -- a fixed file-count cap sized for the small end
// would let the large end blow past any reasonable disk budget, and a cap
// sized for the large end would barely bound the small end at all. Must
// stay comfortably above one full RRFS forecast-hour prefetch's own
// footprint (see GribManager::PrefetchRrfsForecastHourRange(), up to ~27GB
// for a 6-hourly cycle's 84 hours) -- otherwise later hours in the same
// prefetch pass would evict earlier ones before playback ever reaches
// them, defeating the whole point of prefetching. 40GB gives that pass
// headroom to complete plus room for MRMS/RTMA's own loop ranges
// alongside it, while still being a real, finite bound rather than
// unbounded growth across a long-running session. Oldest-by-mtime entries
// are evicted first once the total exceeds this -- or the smaller budget
// free disk space allows, see DownloadCacheBudgetBytes().
static constexpr std::uintmax_t kMaxCacheSizeBytes_ =
   40ULL * 1024 * 1024 * 1024;

// Floor for that budget on a nearly-full disk: still room for a few RRFS
// files plus MRMS/RTMA loops, so the cache keeps working at all (a download
// that doesn't fit then fails on its own rather than evicting everything).
static constexpr std::uintmax_t kMinCacheSizeBytes_ = 2ULL * 1024 * 1024 * 1024;

// Free space the cache leaves on its disk -- the larger of this and 10% of
// the disk.
static constexpr std::uintmax_t kMinFreeDiskBytes_ = 5ULL * 1024 * 1024 * 1024;

// How often NoteCachedDownload() re-walks the whole cache even when its
// running estimate says it's under budget -- the estimate drifts (files
// overwritten in place, other processes) and the budget moves with free
// disk space.
static constexpr auto kCacheRescanInterval_ = std::chrono::minutes {15};

// Mrms/Rtma/RrfsDataProvider share every method GribManager's Poll()/
// FetchArchiveFrame() paths need (Refresh/FindLatestKey/FindKey/
// IsDateCached/GetTimePointsByDate all come from AwsNexradDataProvider),
// so provider_ is held as that common base -- only the actual download
// step differs per subclass (DownloadAndDecompress vs. DownloadRaw, since
// MRMS objects are gzipped and RTMA's/RRFS's aren't, see RtmaDataProvider's
// class comment), so that's the one place a fetch needs to know which
// concrete type it has.
std::shared_ptr<provider::AwsNexradDataProvider>
MakeProvider(map::GribCategory category, const ProductConfig& product)
{
   switch (category)
   {
   case map::GribCategory::Mrms:
      return std::make_shared<provider::MrmsDataProvider>(product.s3Product);
   case map::GribCategory::Rrfs:
   {
      auto rrfsProvider = std::make_shared<provider::RrfsDataProvider>();
      rrfsProvider->SetFileFamily(product.rrfsFileFamily);
      return rrfsProvider;
   }
   case map::GribCategory::Nbm:
      return std::make_shared<provider::NbmDataProvider>();
   case map::GribCategory::User:
   {
      const auto model =
         manager::UserModelRegistry::Instance()->SelectedModel();
      return std::make_shared<provider::ConfiguredIdxProvider>(
         model ? model->config.source :
                 scwx::util::grib_model_config::SourceSpec {});
   }
   case map::GribCategory::Rtma:
   default:
      return std::make_shared<provider::RtmaDataProvider>();
   }
}

namespace
{

// providers_ holds the common AwsNexradDataProvider base, but each
// category only ever constructs one concrete type (see MakeProvider()), so
// every fetch path has to get that concrete type back. One checked cast
// here instead of a hand-written static_cast at each of ~15 call sites:
// clang-tidy flagged those (cppcoreguidelines-pro-type-static-cast-
// downcast), and a mismatched category would have been silent undefined
// behavior -- this throws std::bad_cast instead.
template<typename Derived>
Derived& ProviderAs(provider::AwsNexradDataProvider& base)
{ return dynamic_cast<Derived&>(base); }

void EnsureDateListed(provider::AwsNexradDataProvider&      provider,
                      std::chrono::system_clock::time_point date)
{
   if (!provider.IsDateCached(date))
   {
      provider.GetTimePointsByDate(date, /* update */ true);
   }
}

// Mirrors the S3 key's own directory structure under the cache root
// (not flattened into one directory), and strips ".gz" since a cached
// file's bytes are always already-decompressed. Both matter for
// correctness, not just tidiness: decode_grib's ProductInfoFromPath (see
// grib-helper/src/decode_grib.cpp) derives the product label/valid time
// from the final path component's filename, expecting it to look exactly
// like the original S3 basename (no trailing ".gz", no directory
// components merged into it) -- and for RTMA specifically, the date only
// lives in the *directory* portion of the key (rtma2p5.<date>/...), not
// the basename, which repeats identically every day; flattening would
// have made every date collide on the same cache filename.
//
// Keyed by the S3 key alone (not by product), since the same downloaded
// bytes are reused across every RTMA field that shares one bundled file,
// and across every loop playthrough that revisits the same time.
std::string CachedDownloadPath(const std::string& key)
{
   std::string path = key;
   if (path.ends_with(".gz"))
   {
      path.resize(path.size() - 3);
   }
   return CacheDir() + "/" + path;
}

// Where `product`'s bytes for S3 object `key` live in the cache. A product that
// downloads only its own fields (see grib_fields::FieldsFor()) holds a small
// file of just those, so it cannot share the whole-object path with other
// products reading the same object -- each gets its own entry. Everything else
// still caches the object itself under its key.
std::string CacheKeyFor(map::GribCategory    category,
                        const ProductConfig& product,
                        const std::string&   key)
{
   if (grib_fields::FieldsFor(category, product.displayName) == nullptr)
   {
      return key;
   }

   std::string slug;
   for (const char c : product.displayName)
   {
      const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9');
      const char out =
         alnum ?
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))) :
            '-';
      if (out != '-' || (!slug.empty() && slug.back() != '-'))
      {
         slug += out;
      }
   }
   while (!slug.empty() && slug.back() == '-')
   {
      slug.pop_back();
   }

   return key + ".fields-" + slug;
}

// StatusManager's own doc names this gap directly: a slow fetch already
// reports byte progress, but "a stuck decode_grib call... look[s]
// identical to nothing happening" once the download itself finishes --
// true today because every QueueDownload()/QueueIdxDownload() completion
// handler calls ReportComplete() right before handing off to
// ApplyCachedDownload()/ApplyShipDownload(), which report nothing of
// their own. That gap matters more than it used to: a decode that's
// normally ~25-30ms (see ApplyCachedDownload()'s own doc) can stall for
// seconds under real memory pressure -- exactly the low-RAM scenario a
// blank status bar is least helpful for.
//
// This is the decode-side entry: same status id space as the download
// that fed it (see ApplyCachedDownload()/ApplyShipDownload()'s own call
// sites), so the one status-bar line effectively continues from
// "downloading" into "decoding" rather than needing a second slot.
// RAII rather than an explicit ReportComplete() at every return, since
// both apply functions have multiple early-return failure paths and a
// missed one would leave a stale "still decoding" line behind.
class ScopedDecodeStatus
{
public:
   ScopedDecodeStatus(std::string id, const std::string& displayName) :
       id_ {std::move(id)}
   {
      manager::StatusManager::Instance()->ReportProgress(
         id_, displayName + " (decoding)", 0, -1);
   }

   ~ScopedDecodeStatus()
   { manager::StatusManager::Instance()->ReportComplete(id_); }

   ScopedDecodeStatus(const ScopedDecodeStatus&)            = delete;
   ScopedDecodeStatus& operator=(const ScopedDecodeStatus&) = delete;

private:
   std::string id_;
};

// Shared by every manager downloading into CacheDir() (GribManager,
// HodographManager, WindBarbManager -- see GribManager::
// NoteCachedDownload()), so one running estimate covers the whole cache.
struct DownloadCacheState
{
   std::mutex                            mutex;
   bool                                  scanned {false};
   std::uintmax_t                        estimatedBytes {0};
   std::chrono::steady_clock::time_point lastScan {};
};

DownloadCacheState& CacheState()
{
   static DownloadCacheState state;
   return state;
}

std::uintmax_t CurrentCacheBudgetBytes(std::uintmax_t cacheBytes)
{
   std::error_code ec;
   const auto      space = std::filesystem::space(CacheDir(), ec);
   if (ec)
   {
      return kMaxCacheSizeBytes_;
   }
   return GribManager::DownloadCacheBudgetBytes(
      cacheBytes, space.available, space.capacity);
}

// Walks the whole cache, evicts oldest-first down to the current budget,
// and resets the running estimate to what's actually left. Caller holds
// state.mutex.
void PruneDownloadCacheLocked(DownloadCacheState& state)
{
   namespace fs = std::filesystem;

   // Recursive: cached files live under per-product/per-date
   // subdirectories now (see CachedDownloadPath), not flat in CacheDir().
   std::error_code                  ec;
   std::vector<fs::directory_entry> entries;
   std::uintmax_t                   totalSize = 0;
   for (const auto& entry : fs::recursive_directory_iterator(CacheDir(), ec))
   {
      if (entry.is_regular_file())
      {
         entries.push_back(entry);
         totalSize += entry.file_size();
      }
   }
   if (ec)
   {
      return;
   }

   state.scanned        = true;
   state.lastScan       = std::chrono::steady_clock::now();
   state.estimatedBytes = totalSize;

   const std::uintmax_t budget = CurrentCacheBudgetBytes(totalSize);
   if (totalSize <= budget)
   {
      return;
   }

   std::sort(entries.begin(),
             entries.end(),
             [](const auto& a, const auto& b)
             { return a.last_write_time() < b.last_write_time(); });

   // Remove oldest-first until back under the budget, rather than a fixed
   // eviction count -- how many files that takes depends entirely on
   // which mix (many small MRMS frames vs. a few huge RRFS ones) is
   // actually over the line.
   for (const auto& entry : entries)
   {
      if (totalSize <= budget)
      {
         break;
      }

      const std::uintmax_t size = entry.file_size();
      fs::remove(entry.path(), ec);
      if (!ec)
      {
         totalSize -= size;
      }
   }

   state.estimatedBytes = totalSize;
   logger_->info("Download cache pruned to {:.1f} GB (budget {:.1f} GB)",
                 static_cast<double>(totalSize) / (1024.0 * 1024 * 1024),
                 static_cast<double>(budget) / (1024.0 * 1024 * 1024));
}

// Mrms/Rtma keep this class's original "always exactly one active
// product" behavior (auto-activated at construction) -- Rrfs/Nbm instead
// start with none active, so an idle instance never kicks off a network
// fetch/decode until the user opts into a specific product. Checked by
// category rather than an explicit "these are opt-in" list, so a
// category added after Rrfs/Nbm defaults to opt-in too without this
// needing an update.
bool StartsWithAllProductsInactive(map::GribCategory category)
{
   return category != map::GribCategory::Mrms &&
          category != map::GribCategory::Rtma;
}

} // namespace

// The model's own rules (which keys and forecast hours exist), answered
// without needing any product to be active yet -- the hour slider needs them
// before the first provider is created.
static std::shared_ptr<provider::IdxModelProvider>
MakeIdxRulesProvider(map::GribCategory category)
{
   switch (category)
   {
   case map::GribCategory::Nbm:
      return std::make_shared<provider::NbmDataProvider>();
   case map::GribCategory::User:
   {
      const auto model =
         manager::UserModelRegistry::Instance()->SelectedModel();
      return model ? std::make_shared<provider::ConfiguredIdxProvider>(
                        model->config.source) :
                     nullptr;
   }
   default:
      return nullptr;
   }
}

class GribManager::Impl
{
public:
   explicit Impl(map::GribCategory category) :
       category_ {category},
       // Held for the same reason GribProductLayer holds GribManager
       // itself: TimelineManager::Instance() only caches a weak_ptr, so a
       // discarded shared_ptr would let it be destroyed out from under our
       // signal connections.
       timelineManager_ {manager::TimelineManager::Instance()}
   {
      if (category_ == map::GribCategory::User)
      {
         RebuildUserProducts(userModels_->SelectedModel());
      }

      if (IsIdxCategory(category_))
      {
         idxRules_        = MakeIdxRulesProvider(category_);
         idxForecastHour_ = idxRules_ ? idxRules_->MinForecastHourFor() : 1;
      }

      // Mrms/Rtma start with just the first entry active, matching this
      // class's pre-multi-select behavior exactly -- activeProducts_ must
      // never become empty for these two (see SetProductActive), so this
      // can't be done as a default member initializer the way a single
      // productIndex_ once was; it needs the provider constructed
      // alongside it. Rrfs/Nbm (and anything added after them) instead
      // start with activeProducts_/providers_ both empty -- see
      // StartsWithAllProductsInactive()'s own doc.
      if (!StartsWithAllProductsInactive(category_))
      {
         activeProducts_.insert(0);
         activeSnapshot_.insert(0);
         providers_[0] = MakeProvider(category_, Products(category_)[0]);
      }

      RemoveStaleTmpFrames();
   }

   // Decodes interrupted by a crash or kill leave their uniquely-named
   // temporary frame behind (see UniqueTmpFramePath()) -- each up to a
   // full decoded grid. Only this category's own, so another category's
   // in-flight decode is never touched.
   void RemoveStaleTmpFrames() const
   {
      const std::filesystem::path firstFrame =
         map::GetGribFramePath(category_, 0);
      std::string prefix =
         firstFrame.filename().string(); // "mrms_product0.frame"
      prefix.resize(prefix.size() - std::string_view {"0.frame"}.size());

      std::error_code ec;
      for (const auto& entry :
           std::filesystem::directory_iterator(firstFrame.parent_path(), ec))
      {
         const std::string name = entry.path().filename().string();
         if (name.starts_with(prefix) && name.ends_with(".tmp"))
         {
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
         }
      }
   }

   // Same idiom TimelineManager::Impl uses for its own thread pools:
   // explicitly stop+join here, in this destructor's *body*, before any
   // implicit member teardown begins -- fetchPool_'s posted lambdas
   // capture `this` (the outer GribManager) and touch this Impl's own
   // members, so they must fully finish before either starts being torn
   // down, not just before fetchPool_'s own destructor runs.
   ~Impl()
   {
      // Cancel in-flight S3 requests first -- otherwise the joins below
      // would wait out a download that can run for minutes (RRFS files are
      // ~320MB), blocking whichever thread is destroying this manager.
      // Jobs hold their own shared_ptr to the same provider objects, so
      // this reaches them too (a deactivated product's still-running
      // download aside -- its provider is no longer in providers_).
      for (auto& [index, provider] : providers_)
      {
         provider->Shutdown();
      }

      // Stop every pool before joining any: a lookup job can post to the
      // fetch/decode pools and a fetch job can post to the decode pool,
      // so none may still be accepting-and-running work while another is
      // being joined.
      lookupPool_.stop();
      fetchPool_.stop();
      decodePool_.stop();
      lookupPool_.join();
      fetchPool_.join();
      decodePool_.join();
   }

   map::GribCategory category_;

   // Which products (indices into Products(category_)) are currently
   // fetched/decoded/rendered -- see SetProductActive(). A std::set (not
   // e.g. std::vector<bool>) both for the ordered "lowest = current/
   // primary" convention CurrentProductIndex() relies on, and because
   // insert/erase by index is simpler to reason about than an always-
   // full-size boolean vector. Non-empty after construction for Mrms/
   // Rtma; empty until the user opts in for Rrfs/Nbm (see
   // StartsWithAllProductsInactive()).
   std::set<std::size_t> activeProducts_;

   // One provider per active product, not one shared provider -- needed
   // for correctness with MRMS (each product is a genuinely different S3
   // prefix/provider instance, see MrmsDataProvider's constructor), and
   // kept uniform for RTMA/RRFS too even though every product in either
   // of those categories would construct an identical provider (a few
   // redundant provider objects, not a redundant *download* -- see
   // QueueDownload's own comment on that).
   std::map<std::size_t, std::shared_ptr<provider::AwsNexradDataProvider>>
      providers_;

   std::shared_ptr<manager::TimelineManager> timelineManager_;
   QTimer*                                   timer_ {nullptr};

   // fetchMutex_ guards every field below it, since a background fetch
   // (see QueueDownload) reads/writes them from a fetchPool_ thread while
   // the GUI thread reads/writes them too (SetProductActive,
   // HandleSelectedTimeUpdated, etc.) -- unlike before RTMA/loop-prefetch
   // support, this manager is no longer single-threaded in practice.
   std::mutex fetchMutex_;
   std::map<std::size_t, std::string>
      lastKeys_; // currently displayed, per product
   std::map<std::size_t, std::string>
      lastRequestedKeys_; // most recently asked for, per product

   // Keyed by (productIndex, key) rather than just key -- see
   // QueueDownload's own comment for why two products sharing a category
   // (and so, often, a download) each still need their own in-flight
   // entry.
   std::set<std::pair<std::size_t, std::string>> inFlightKeys_;

   // QueueDownload()'s own in-flight set: each file being downloaded, and
   // every product waiting on it -- keyed by file alone, so products that
   // share a file (RTMA/RRFS bundle every field into one) download it once
   // and each get their own decode when it lands.
   std::map<std::string, std::set<std::size_t>> downloadWaiters_;

   // Mirror of activeProducts_ for background threads (activeProducts_
   // itself is GUI-thread-only) -- lets a lookup, download or decode that
   // outlived its product's deactivation notice and drop its result
   // instead of writing a frame for a product that's no longer shown.
   std::set<std::size_t> activeSnapshot_;

   // Per-product count of lookups queued (see QueueLookup()) -- a lookup
   // only proceeds if it's still the newest one for its product, so rapid
   // archive scrubbing doesn't list S3 once per intermediate step. Never
   // erased, so a re-activated product can't reuse an old job's number.
   std::map<std::size_t, std::uint64_t> lookupSeq_;

   // Serializes committing a decoded frame against deactivating its
   // product -- see CommitDecodedFrame(). Always taken before fetchMutex_,
   // never while holding it.
   std::mutex commitMutex_;

   // Background download pool (2: enough to overlap a couple of prefetch
   // downloads without hammering S3/local bandwidth much harder than a
   // single live/archive fetch already did). Same boost::asio::thread_pool
   // + boost::asio::post pattern TimelineManager itself uses for its own
   // async work, not QtConcurrent/raw threads.
   boost::asio::thread_pool fetchPool_ {2};

   // S3 listing/key resolution for MRMS/RTMA (see QueueLookup()) -- one
   // thread, so lookups for a product run in the order they were asked
   // for, and the (thread-safe but not cheap) provider listing isn't run
   // concurrently against itself.
   boost::asio::thread_pool lookupPool_ {1};

   // Decodes of already-downloaded files (see QueueCachedDecode()) --
   // kept off the GUI thread, and off fetchPool_ so a decode never waits
   // behind a multi-hundred-MB download.
   boost::asio::thread_pool decodePool_ {1};

   // Mirrors TimelineManager's own live/archive state (see
   // LiveStateUpdated/SelectedTimeUpdated) rather than querying it fresh
   // each time -- cheap, and matches how PlacefileLayer/AlertLayer cache
   // it (see timeline_manager.hpp).
   bool                                  isLive_ {true};
   std::chrono::system_clock::time_point selectedTime_ {};

   // RRFS-only (see SetRrfsCycle()/SetRrfsForecastHour() in the header) --
   // mirrors RrfsDataProvider::Impl's own three fields exactly, but kept
   // here too (not just read back from one provider) because this manager
   // can hold several independent RrfsDataProvider instances at once (one
   // per active product, see providers_ above) that all need to agree, and
   // because SetProductActive() can construct a brand new one at any time
   // (always at auto/latest/F000 defaults) that needs to be brought in
   // line with whatever selection is already in effect -- see
   // SyncRrfsProviderState(). Meaningless (left at defaults) for Mrms/Rtma.
   bool                                  rrfsUseLatestCycle_ {true};
   std::chrono::system_clock::time_point rrfsCycleOverride_ {};
   int                                   rrfsForecastHour_ {0};

   // See RrfsSelectionProvider(): only exists while the hodograph is picked
   // with no product checked.
   std::shared_ptr<provider::AwsNexradDataProvider> rrfsSelectionProvider_;

   // Bounds PrefetchRrfsForecastHourRange() and (via GribDockWidget's own
   // Play/pause loop) where the animation wraps back to -- see
   // SetRrfsLoopRange()'s own doc. rrfsLoopEndHour_ < 0 means "unset, use
   // [0, MaxRrfsForecastHour()]" -- the original, unbounded behavior --
   // distinct from an explicit 0 upper bound.
   int rrfsLoopStartHour_ {0};
   int rrfsLoopEndHour_ {-1};

   // Nbm-only -- same shape and reason as the RRFS trio above (each
   // active Nbm product holds its own NbmDataProvider instance that
   // needs to agree with the others, and a freshly-constructed one
   // starts at defaults). No loop-range equivalent: each Nbm product's
   // own cached download is a single range-fetched field (~1-2MB, see
   // ProductConfig's own nbmParameter/nbmLevel/nbmQualifier doc), nowhere
   // near the cache-budget pressure a whole RRFS forecast-hour prefetch
   // creates.
   // GribCategory::User only: held (Instance() keeps just a weak_ptr) so the
   // registry outlives this manager and its selection stays readable.
   std::shared_ptr<manager::UserModelRegistry> userModels_ {
      manager::UserModelRegistry::Instance()};

   // The idx model's own rules -- see MakeIdxRulesProvider().
   std::shared_ptr<provider::IdxModelProvider> idxRules_;

   bool                                  idxUseLatestCycle_ {true};
   std::chrono::system_clock::time_point idxCycleOverride_ {};
   // 1, not 0 -- confirmed live (2026-09-26): unlike RRFS/GFS, NBM
   // doesn't publish an F000 file at all for the CONUS core product, so
   // F001 is the real minimum (see NbmDataProvider::kMinForecastHour_'s
   // own comment). Matches that class's own default so a freshly
   // constructed provider and this manager's own idea of "the current
   // hour" never disagree.
   int idxForecastHour_ {1};
};

GribManager::GribManager(map::GribCategory category) :
    p(std::make_unique<Impl>(category))
{
   p->timer_ = new QTimer(this);
   connect(p->timer_, &QTimer::timeout, this, &GribManager::Poll);
   p->timer_->start(kPollIntervalMs_);

   connect(p->timelineManager_.get(),
           &manager::TimelineManager::LiveStateUpdated,
           this,
           &GribManager::HandleLiveStateUpdated);
   connect(p->timelineManager_.get(),
           &manager::TimelineManager::SelectedTimeUpdated,
           this,
           &GribManager::HandleSelectedTimeUpdated);
   connect(p->timelineManager_.get(),
           &manager::TimelineManager::AnimationStateUpdated,
           this,
           &GribManager::HandleAnimationStateUpdated);

   // Defer the first poll rather than doing it synchronously in the
   // constructor -- Poll() does blocking network I/O, and this manager is
   // constructed from GribProductLayer::Initialize(), which runs during
   // MapLibre's render setup.
   QTimer::singleShot(0, this, &GribManager::Poll);
}

GribManager::~GribManager() = default;

std::uintmax_t
GribManager::DownloadCacheBudgetBytes(std::uintmax_t cacheBytes,
                                      std::uintmax_t availableBytes,
                                      std::uintmax_t capacityBytes)
{
   const std::uintmax_t reserve =
      std::max(kMinFreeDiskBytes_, capacityBytes / 10);

   // What the cache could grow to while still leaving `reserve` free: its
   // own current size (freeing it is always possible) plus free space.
   const std::uintmax_t reachable = cacheBytes + availableBytes;
   const std::uintmax_t allowed = reachable > reserve ? reachable - reserve : 0;

   return std::clamp(allowed, kMinCacheSizeBytes_, kMaxCacheSizeBytes_);
}

void GribManager::NoteCachedDownload(const std::string& path)
{
   std::error_code      ec;
   const std::uintmax_t size = std::filesystem::file_size(path, ec);

   auto&           state = CacheState();
   std::lock_guard lock(state.mutex);

   if (!ec)
   {
      state.estimatedBytes += size;
   }

   // A full walk of the cache (thousands of files once MRMS loops pile
   // up) used to run after every single download. Now only when the
   // running estimate says it's over budget, on the first download of a
   // session, or every kCacheRescanInterval_ to correct the estimate.
   if (!state.scanned ||
       std::chrono::steady_clock::now() - state.lastScan >=
          kCacheRescanInterval_ ||
       state.estimatedBytes > CurrentCacheBudgetBytes(state.estimatedBytes))
   {
      PruneDownloadCacheLocked(state);
   }
}

std::shared_ptr<GribManager> GribManager::Instance(map::GribCategory category)
{
   // One weak_ptr slot per category -- each category is an independent
   // singleton (independent fetch state, product table, frame files), not
   // one shared instance switching what it points to. A real bug lived
   // here until this map replaced it: only two slots existed
   // (mrmsInstance_/modelsInstance_) with a binary `== Mrms` check for
   // which to use, a leftover from when GribCategory only had two values
   // -- once it grew to three (Mrms/Rtma/Rrfs), Rtma and Rrfs silently
   // collapsed onto the same slot, meaning whichever category asked
   // first "won" and the other transparently got the same instance
   // (same products, same fetch state) instead of its own. Caught by
   // code review, not a test -- nothing exercised both categories'
   // Instance() calls closely enough together to notice.
   static std::map<map::GribCategory, std::weak_ptr<GribManager>> instances_;
   static std::mutex instanceMutex_ {};

   std::unique_lock lock(instanceMutex_);

   std::weak_ptr<GribManager>& slot = instances_[category];

   std::shared_ptr<GribManager> gribManager = slot.lock();

   if (gribManager == nullptr)
   {
      gribManager = std::make_shared<GribManager>(category);
      slot        = gribManager;
   }

   return gribManager;
}

std::vector<std::string> GribManager::ProductNames() const
{
   std::vector<std::string> names;
   const auto&              products = Products(p->category_);
   names.reserve(products.size());
   for (const auto& product : products)
   {
      names.push_back(product.displayName);
   }
   return names;
}

std::optional<std::size_t> GribManager::CurrentProductIndex() const
{
   if (p->activeProducts_.empty())
   {
      return std::nullopt;
   }
   return *p->activeProducts_.begin();
}

std::vector<std::size_t> GribManager::ActiveProductIndices() const
{ return {p->activeProducts_.cbegin(), p->activeProducts_.cend()}; }

std::string GribManager::CurrentProductName() const
{
   const auto index = CurrentProductIndex();
   return index ? ProductName(*index) : std::string {};
}

std::string GribManager::ProductName(std::size_t productIndex) const
{
   const auto& products = Products(p->category_);
   return productIndex < products.size() ? products[productIndex].displayName :
                                           std::string {};
}

std::string GribManager::FormatValue(float rawValue) const
{
   const auto index = CurrentProductIndex();
   if (!index)
   {
      // Only reachable for Rrfs/Nbm with nothing active -- insurance
      // against a caller that doesn't check first.
      return fmt::format("{:.2f}", rawValue);
   }
   return FormatValue(*index, rawValue);
}

std::string GribManager::FormatValue(std::size_t productIndex,
                                     float       rawValue) const
{
   const auto& products = Products(p->category_);
   if (productIndex >= products.size())
   {
      return fmt::format("{:.2f}", rawValue);
   }

   const ProductConfig& product      = products[productIndex];
   auto&                unitSettings = settings::UnitSettings::Instance();

   switch (product.quantity)
   {
   case PhysicalQuantity::TemperatureKelvin:
   {
      const auto units = types::GetTemperatureUnitsFromName(
         unitSettings.temperature_units().GetValue());
      const float converted =
         types::ConvertTemperatureFromKelvin(rawValue, units);
      return fmt::format(
         "{:.2f} {}", converted, types::GetTemperatureUnitsAbbreviation(units));
   }
   case PhysicalQuantity::SpeedMetersPerSecond:
   {
      const auto units =
         types::GetSpeedUnitsFromName(unitSettings.speed_units().GetValue());
      return fmt::format("{:.2f} {}",
                         rawValue * types::GetSpeedUnitsScale(units),
                         types::GetSpeedUnitsAbbreviation(units));
   }
   case PhysicalQuantity::AccumulationMillimeters:
   {
      const auto units = types::GetAccumulationUnitsFromName(
         unitSettings.accumulation_units().GetValue());
      return fmt::format("{:.2f} {}",
                         rawValue * types::GetAccumulationUnitsScale(units),
                         types::GetAccumulationUnitsAbbreviation(units));
   }
   case PhysicalQuantity::PressurePascals:
   {
      const auto units = types::GetPressureUnitsFromName(
         unitSettings.pressure_units().GetValue());
      return fmt::format("{:.2f} {}",
                         rawValue * types::GetPressureUnitsScale(units),
                         types::GetPressureUnitsAbbreviation(units));
   }
   case PhysicalQuantity::None:
   default:
      return fmt::format("{:.2f} {}", rawValue, product.units);
   }
}

void GribManager::SetProductActive(const std::string& displayName, bool active)
{
   const auto& products = Products(p->category_);

   for (std::size_t i = 0; i < products.size(); ++i)
   {
      if (products[i].displayName != displayName)
      {
         continue;
      }

      const bool alreadyActive = p->activeProducts_.contains(i);
      if (active == alreadyActive)
      {
         return; // already in the requested state
      }

      if (active)
      {
         p->activeProducts_.insert(i);
         {
            std::lock_guard lock(p->fetchMutex_);
            p->activeSnapshot_.insert(i);
         }
         p->providers_[i] = MakeProvider(p->category_, products[i]);

         if (p->category_ == map::GribCategory::Rrfs)
         {
            // MakeProvider() always starts a fresh RrfsDataProvider at
            // auto/latest/F000 defaults -- bring it in line with whatever
            // cycle/hour selection is already in effect before the
            // Poll()/FetchArchiveFrame() call below runs against it.
            SyncRrfsProviderState(*p->providers_[i]);
         }
         else if (IsIdxCategory(p->category_))
         {
            // Same reason as the Rrfs branch above.
            SyncIdxProviderState(*p->providers_[i]);
         }
      }
      else
      {
         if (p->activeProducts_.size() == 1 &&
             !StartsWithAllProductsInactive(p->category_))
         {
            // Refused, not silently ignored -- Mrms/Rtma need at least
            // one active product to fall back on at all times (unlike
            // Rrfs/Nbm, which are allowed back down to zero -- see
            // StartsWithAllProductsInactive()'s own doc).
            logger_->warn("Refusing to deactivate the only active product: {}",
                          displayName);
            return;
         }

         p->activeProducts_.erase(i);
         p->providers_.erase(i);

         // Same lock CommitDecodedFrame() holds across its check and rename
         // -- see there. Held through the frame file's removal below.
         std::lock_guard commitLock(p->commitMutex_);
         {
            std::lock_guard lock(p->fetchMutex_);
            p->activeSnapshot_.erase(i);
            p->lastKeys_.erase(i);
            p->lastRequestedKeys_.erase(i);
         }

         // Drop this product's decoded frame too, so re-activating it later
         // (possibly in a later session -- frame files persist) never
         // briefly shows a frame that's hours old while the fresh one is
         // fetched. lastKeys_ was just cleared, so the next request
         // re-decodes from the download cache rather than assuming this
         // file is still current.
         std::error_code ec;
         std::filesystem::remove(map::GetGribFramePath(p->category_, i), ec);
      }

      logger_->info(
         "Product {} now {}", displayName, active ? "active" : "inactive");

      Q_EMIT ActiveProductsChanged();

      if (active)
      {
         if (p->category_ == map::GribCategory::Rrfs)
         {
            // Rrfs's own cycle/hour selection (just synced above), not the
            // main timeline's live/selectedTime_ state -- see
            // SetRrfsCycle()'s own doc in grib_manager.hpp for why these
            // are independent axes.
            FetchRrfsSelection();
            RefreshRrfsAvailability();
         }
         else if (IsIdxCategory(p->category_))
         {
            // Same reason as the Rrfs branch above -- Nbm's own cycle/
            // hour selection is a separate axis, and Poll()'s generic
            // "latest" loop deliberately skips Nbm entirely (see its own
            // comment), so nothing else would ever fetch this product.
            FetchIdxSelection();
         }
         else if (p->isLive_)
         {
            Poll();
         }
         else
         {
            FetchArchiveFrame(p->selectedTime_);
         }
      }
      return;
   }

   logger_->warn("Unknown product: {}", displayName);
}

bool GribManager::IsProductActive(const std::string& displayName) const
{
   const auto& products = Products(p->category_);
   for (std::size_t i = 0; i < products.size(); ++i)
   {
      if (products[i].displayName == displayName)
      {
         return p->activeProducts_.contains(i);
      }
   }
   return false;
}

std::vector<std::string> GribManager::ActiveProductNames() const
{
   const auto&              products = Products(p->category_);
   std::vector<std::string> names;
   names.reserve(p->activeProducts_.size());
   for (auto index : p->activeProducts_)
   {
      names.push_back(products[index].displayName);
   }
   return names;
}

void GribManager::SyncRrfsProviderState(
   provider::AwsNexradDataProvider& provider) const
{
   auto& rrfsProvider = ProviderAs<provider::RrfsDataProvider>(provider);

   if (p->rrfsUseLatestCycle_)
   {
      rrfsProvider.UseLatestCycle();
   }
   else
   {
      rrfsProvider.SetCycle(p->rrfsCycleOverride_);
   }
   rrfsProvider.SetForecastHour(p->rrfsForecastHour_);
}

void GribManager::FetchRrfsSelection()
{
   using namespace std::chrono;

   // No-op with nothing active yet -- e.g. the user moved the cycle/hour
   // picker before checking any product. Whatever selection is in effect
   // gets fetched once a product actually activates (SetProductActive()
   // syncs a freshly-constructed provider to it and fetches then).
   const auto index = CurrentProductIndex();
   if (!index)
   {
      return;
   }

   auto& rrfsProvider =
      ProviderAs<provider::RrfsDataProvider>(*p->providers_.at(*index));

   const auto time =
      rrfsProvider.CurrentCycle() + hours {rrfsProvider.ForecastHour()};

   FetchArchiveFrame(time);
}

void GribManager::SetRrfsCycle(std::chrono::system_clock::time_point cycleTime)
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn("SetRrfsCycle() is only meaningful for GribCategory::Rrfs");
      return;
   }

   p->rrfsUseLatestCycle_ = false;
   p->rrfsCycleOverride_  = cycleTime;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncRrfsProviderState(*providerPtr);
   }

   FetchRrfsSelection();
   RefreshRrfsAvailability(true);
}

void GribManager::UseLatestRrfsCycle()
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn(
         "UseLatestRrfsCycle() is only meaningful for GribCategory::Rrfs");
      return;
   }

   p->rrfsUseLatestCycle_ = true;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncRrfsProviderState(*providerPtr);
   }

   FetchRrfsSelection();
   RefreshRrfsAvailability(true);
}

bool GribManager::IsUsingLatestRrfsCycle() const
{ return p->category_ != map::GribCategory::Rrfs || p->rrfsUseLatestCycle_; }

bool GribManager::HasRrfsSelection() const
{
   return p->category_ == map::GribCategory::Rrfs &&
          (CurrentProductIndex().has_value() ||
           HodographSelection::Instance().IsEnabled());
}

std::shared_ptr<provider::AwsNexradDataProvider>
GribManager::RrfsSelectionProvider() const
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      return nullptr;
   }

   if (const auto index = CurrentProductIndex())
   {
      return p->providers_.at(*index);
   }

   if (!HodographSelection::Instance().IsEnabled())
   {
      return nullptr;
   }

   if (!p->rrfsSelectionProvider_)
   {
      p->rrfsSelectionProvider_ =
         std::make_shared<provider::RrfsDataProvider>();
   }
   SyncRrfsProviderState(*p->rrfsSelectionProvider_); // cheap setters
   return p->rrfsSelectionProvider_;
}

std::chrono::system_clock::time_point GribManager::CurrentRrfsCycle() const
{
   // Epoch when there is no selection yet -- "nothing meaningful to report"
   const auto provider = RrfsSelectionProvider();
   if (!provider)
   {
      return {};
   }

   return ProviderAs<provider::RrfsDataProvider>(*provider).CurrentCycle();
}

void GribManager::SetRrfsForecastHour(int hour)
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn(
         "SetRrfsForecastHour() is only meaningful for GribCategory::Rrfs");
      return;
   }

   p->rrfsForecastHour_ = hour;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncRrfsProviderState(*providerPtr);
   }

   FetchRrfsSelection();
}

int GribManager::RrfsForecastHour() const
{ return (p->category_ == map::GribCategory::Rrfs) ? p->rrfsForecastHour_ : 0; }

int GribManager::MaxRrfsForecastHour() const
{
   if (!HasRrfsSelection())
   {
      // The check matters on its own, not just as a guard against
      // CurrentRrfsCycle()'s own epoch fallback below -- an epoch cycle
      // would still resolve to *some* (meaningless) max-hour value rather
      // than the real "nothing selected yet" of 0.
      return 0;
   }

   return provider::RrfsDataProvider::MaxForecastHourForCycle(
      CurrentRrfsCycle());
}

void GribManager::SetRrfsLoopRange(int startHour, int endHour)
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn(
         "SetRrfsLoopRange() is only meaningful for GribCategory::Rrfs");
      return;
   }

   p->rrfsLoopStartHour_ = startHour;
   p->rrfsLoopEndHour_   = endHour;
}

int GribManager::RrfsLoopStartHour() const
{
   return (p->category_ == map::GribCategory::Rrfs) ? p->rrfsLoopStartHour_ : 0;
}

int GribManager::RrfsLoopEndHour() const
{ return (p->category_ == map::GribCategory::Rrfs) ? p->rrfsLoopEndHour_ : -1; }

void GribManager::PrefetchRrfsForecastHourRange()
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn(
         "PrefetchRrfsForecastHourRange() is only meaningful for "
         "GribCategory::Rrfs");
      return;
   }

   const int maxHour   = MaxRrfsForecastHour();
   const int startHour = std::clamp(p->rrfsLoopStartHour_, 0, maxHour);
   const int endHour   = (p->rrfsLoopEndHour_ < 0) ?
                            maxHour :
                            std::clamp(p->rrfsLoopEndHour_, startHour, maxHour);

   std::set<int> hours;
   for (int hour = startHour; hour <= endHour; ++hour)
   {
      hours.insert(hour);
   }
   PrefetchRrfsForecastHours(hours);
}

void GribManager::PrefetchRrfsForecastHours(const std::set<int>& hours)
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      logger_->warn(
         "PrefetchRrfsForecastHours() is only meaningful for "
         "GribCategory::Rrfs");
      return;
   }

   // Nothing to prefetch with no product active (e.g. Play pressed the
   // instant a category is enabled, before any product is checked).
   if (p->activeProducts_.empty())
   {
      return;
   }

   const auto cycle = CurrentRrfsCycle();
   const int  maxHour =
      provider::RrfsDataProvider::MaxForecastHourForCycle(cycle);

   // Every checked product, since they're all drawn during playback.
   // Products in the same file family share each hour's file, which
   // QueueDownload() downloads once for all of them; hours already in the
   // download cache are skipped. SHIP is skipped: its two inputs go through
   // its own dispatch (see FetchShipSelection()).
   int queued = 0;

   // Hours not on S3 (yet) would only fail; skip them when that is known
   const auto published = PublishedRrfsForecastHours();

   for (const std::size_t index : p->activeProducts_)
   {
      const ProductConfig& product = Products(p->category_)[index];
      if (product.derivedIndex == "ship")
      {
         continue;
      }

      const auto productProvider = p->providers_.at(index);
      for (const int hour : hours)
      {
         if (hour < 0 || hour > maxHour ||
             (published.has_value() && !published->contains(hour)))
         {
            continue;
         }

         const std::string key = provider::RrfsDataProvider::BuildKey(
            cycle, hour, product.rrfsFileFamily);
         if (std::filesystem::exists(
                CachedDownloadPath(CacheKeyFor(p->category_, product, key))))
         {
            continue;
         }

         QueueDownload(index, key, productProvider);
         ++queued;
      }
   }

   logger_->debug(
      "Prefetching {} download(s) for {} picked RRFS hour(s) of "
      "cycle {}",
      queued,
      hours.size(),
      scwx::util::TimeString(cycle));
}

std::set<int> GribManager::CachedRrfsForecastHours() const
{
   std::set<int> cached;

   const auto index = CurrentProductIndex();
   if (p->category_ != map::GribCategory::Rrfs || !index)
   {
      return cached;
   }

   const auto           cycle   = CurrentRrfsCycle();
   const ProductConfig& product = Products(p->category_)[*index];
   const int            maxHour =
      provider::RrfsDataProvider::MaxForecastHourForCycle(cycle);

   for (int hour = 0; hour <= maxHour; ++hour)
   {
      const std::string key = provider::RrfsDataProvider::BuildKey(
         cycle, hour, product.rrfsFileFamily);
      if (std::filesystem::exists(
             CachedDownloadPath(CacheKeyFor(p->category_, product, key))))
      {
         cached.insert(hour);
      }
   }

   return cached;
}

void GribManager::RefreshRrfsAvailability(bool force)
{
   if (p->category_ != map::GribCategory::Rrfs)
   {
      return;
   }

   // Copied: it is listed from a pool thread, and the product may be unchecked
   // meanwhile.
   const std::shared_ptr<provider::AwsNexradDataProvider> provider =
      RrfsSelectionProvider();
   if (!provider)
   {
      return; // nothing selected: nothing to size a picker for
   }

   boost::asio::post(p->lookupPool_,
                     [this, provider, force]()
                     {
                        const bool changed =
                           ProviderAs<provider::RrfsDataProvider>(*provider)
                              .RefreshAvailability(force);

                        if (changed || force)
                        {
                           QMetaObject::invokeMethod(
                              this,
                              [this]() { HandleRrfsAvailability(); },
                              Qt::QueuedConnection);
                        }
                     });
}

void GribManager::HandleRrfsAvailability()
{
   // The hour selected may not exist (an hourly cycle has no F000; the cycle
   // may not have reached it yet): move to the nearest one that does, which
   // also fetches it. Otherwise "Latest" may now mean a newer cycle, so fetch
   // what is selected -- already showing it is recognised and costs nothing.
   const auto published = PublishedRrfsForecastHours();
   if (published.has_value() && !published->empty() &&
       !published->contains(p->rrfsForecastHour_))
   {
      const auto next = published->lower_bound(p->rrfsForecastHour_);
      SetRrfsForecastHour(next != published->end() ? *next :
                                                     *published->rbegin());
   }
   else
   {
      FetchRrfsSelection();
   }

   Q_EMIT RrfsAvailabilityChanged();
}

std::optional<std::set<int>> GribManager::PublishedRrfsForecastHours() const
{
   const auto selection = RrfsSelectionProvider();
   if (!selection)
   {
      return std::nullopt;
   }

   const auto  index    = CurrentProductIndex();
   const auto& provider = ProviderAs<provider::RrfsDataProvider>(*selection);
   const auto  cycle    = provider.CurrentCycle();

   // With only the hodograph picked there is no product: it reads RRFS's 2D
   // file
   const ProductConfig* product =
      index ? &Products(p->category_)[*index] : nullptr;

   auto hours = provider.PublishedHours(cycle,
                                        product != nullptr ?
                                           product->rrfsFileFamily :
                                           provider::RrfsFileFamily::TwoDField);
   if (!hours.has_value())
   {
      return std::nullopt;
   }

   if (product != nullptr && product->derivedIndex == "ship")
   {
      // SHIP reads both of RRFS's files: an hour needs both
      const auto pressure = provider.PublishedHours(
         cycle, provider::RrfsFileFamily::PressureLevel);
      if (!pressure.has_value())
      {
         return std::nullopt;
      }

      std::set<int> both;
      std::set_intersection(hours->begin(),
                            hours->end(),
                            pressure->begin(),
                            pressure->end(),
                            std::inserter(both, both.begin()));
      return both;
   }

   return hours;
}

std::vector<std::chrono::system_clock::time_point>
GribManager::PublishedRrfsCycles() const
{
   const auto selection = RrfsSelectionProvider();
   if (!selection)
   {
      return {};
   }

   const auto  index    = CurrentProductIndex();
   const auto& provider = ProviderAs<provider::RrfsDataProvider>(*selection);

   if (!index)
   {
      return provider.PublishedCycles(provider::RrfsFileFamily::TwoDField);
   }

   const ProductConfig& product = Products(p->category_)[*index];
   return provider.PublishedCycles(product.derivedIndex == "ship" ?
                                      provider::RrfsFileFamily::PressureLevel :
                                      product.rrfsFileFamily);
}

void GribManager::Poll()
{
   // RRFS only: keep the picker's idea of what is published current -- listed
   // every poll (the provider rate-limits itself), whether or not the main
   // timeline is live
   if (p->category_ == map::GribCategory::Rrfs)
   {
      RefreshRrfsAvailability();
   }

   // Only the live path polls for "whatever's newest" -- once the user
   // scrubs into archive mode, HandleSelectedTimeUpdated() drives fetches
   // instead, and there's nothing meaningful for a "latest" poll to do.
   if (!p->isLive_)
   {
      return;
   }

   // Copied rather than iterated live: SetProductActive can run on this
   // same (GUI) thread re-entrantly if a fetch below somehow triggered
   // one synchronously, and mutating activeProducts_ mid-range-for would
   // be undefined behavior either way.
   const std::set<std::size_t> activeProducts = p->activeProducts_;

   for (auto index : activeProducts)
   {
      if (Products(p->category_)[index].derivedIndex == "ship")
      {
         // SHIP's own two-file fetch/decode is handled entirely through
         // FetchArchiveFrameForProduct()'s own dedicated dispatch (see
         // its doc) -- this generic "find whatever's latest, RequestFrame
         // it" loop would call decode_grib with the wrong (single-input)
         // CLI form for SHIP's two-input mode.
         continue;
      }

      if (IsIdxCategory(p->category_))
      {
         // Nbm's own per-field idx-based fetch is handled entirely
         // through FetchArchiveFrameForProduct()'s dedicated dispatch
         // (via FetchIdxSelection(), called whenever cycle/hour changes)
         // -- same reason as SHIP's own skip above: this generic "find
         // latest, RequestFrame" loop would call QueueDownload's category
         // switch, which has no Nbm case and would bad-cast this
         // provider to the wrong concrete type.
         continue;
      }

      QueueLookup(index, p->providers_.at(index), std::nullopt);
   }
}

void GribManager::HandleLiveStateUpdated(bool isLive)
{
   p->isLive_ = isLive;

   if (isLive)
   {
      // Snap back to "latest" immediately rather than waiting for the
      // next 4-minute tick.
      Poll();
   }
}

void GribManager::HandleSelectedTimeUpdated(
   std::chrono::system_clock::time_point dateTime)
{
   p->selectedTime_ = dateTime;

   if (!p->isLive_)
   {
      FetchArchiveFrame(dateTime);
   }
}

void GribManager::HandleAnimationStateUpdated(types::AnimationState state)
{
   // Deliberately not gated on !p->isLive_: TimelineManager::Impl::Play()
   // emits AnimationStateUpdated synchronously, before PlaySync() (posted
   // to its own thread pool) ever calls SelectTime() -- the call that
   // actually flips isLive_ false via LiveStateUpdated. Gating on isLive_
   // here would race and likely lose when the user presses Play directly
   // from live view, which is exactly the common case. GetLoopStartAndEnd
   // Times() is well-defined either way (falls back to "now" as the end
   // time when still live), so there's nothing to gate on -- Play always
   // means "about to step through a bounded range."
   if (state == types::AnimationState::Play)
   {
      PrefetchLoopRange();
   }
}

void GribManager::PrefetchLoopRange()
{
   using namespace std::chrono;

   // RRFS has its own forecast-hour prefetch, and NBM/SHIP don't download
   // through QueueDownload() at all (its category switch would cast their
   // provider to the wrong concrete type) -- see this method's doc.
   if (p->category_ != map::GribCategory::Mrms &&
       p->category_ != map::GribCategory::Rtma)
   {
      return;
   }

   auto [startTime, endTime] = p->timelineManager_->GetLoopStartAndEndTimes();
   if (startTime >= endTime)
   {
      return;
   }

   // Every checked product, since they're all drawn during playback --
   // one lookup job each (MRMS products are separate files; RTMA's share
   // one, which QueueDownload() downloads once for all of them).
   for (const std::size_t productIndex : p->activeProducts_)
   {
      PrefetchLoopRangeForProduct(
         productIndex, p->providers_.at(productIndex), startTime, endTime);
   }
}

void GribManager::PrefetchLoopRangeForProduct(
   std::size_t                                      productIndex,
   std::shared_ptr<provider::AwsNexradDataProvider> provider,
   std::chrono::system_clock::time_point            startTime,
   std::chrono::system_clock::time_point            endTime)
{
   using namespace std::chrono;

   // Listing a day and searching it are blocking network I/O -- run them
   // on the lookup pool rather than the GUI thread Play was pressed on.
   boost::asio::post(
      p->lookupPool_,
      [this, productIndex, provider, startTime, endTime]()
      {
         for (auto date = floor<days>(startTime); date <= floor<days>(endTime);
              date += days {1})
         {
            EnsureDateListed(*provider, date);
         }

         // 1-minute ticks match PlaySync's own per-step advance (see
         // TimelineManager::Impl::PlaySync) -- fine enough that FindKey's
         // nearest-match won't skip a real file that lands between ticks,
         // while naturally deduping to the actual (much sparser) set of
         // real files via the std::set below.
         std::set<std::string> neededKeys;
         for (auto t = startTime; t <= endTime; t += minutes {1})
         {
            std::string key = provider->FindKey(t);
            if (!key.empty())
            {
               neededKeys.insert(key);
            }
         }

         {
            std::lock_guard lock(p->fetchMutex_);
            if (!p->activeSnapshot_.contains(productIndex))
            {
               return; // deactivated while this was listing
            }
         }

         logger_->debug("Prefetching {} frame(s) for loop range {} to {}",
                        neededKeys.size(),
                        scwx::util::TimeString(startTime),
                        scwx::util::TimeString(endTime));

         for (const auto& key : neededKeys)
         {
            QueueDownload(productIndex, key, provider);
         }
      });
}

bool GribManager::FetchArchiveFrame(std::chrono::system_clock::time_point time)
{
   // Copied for the same re-entrancy reason as Poll()'s own copy.
   const std::set<std::size_t> activeProducts = p->activeProducts_;

   bool anyRequested = false;
   for (auto index : activeProducts)
   {
      anyRequested |= FetchArchiveFrameForProduct(index, time);
   }
   return anyRequested;
}

bool GribManager::FetchArchiveFrameForProduct(
   std::size_t productIndex, std::chrono::system_clock::time_point time)
{
   using namespace std::chrono;

   auto& provider = p->providers_.at(productIndex);

   if (Products(p->category_)[productIndex].derivedIndex == "ship")
   {
      // SHIP's own two-file dispatch -- see QueueShipInput()/
      // ApplyShipIfReady()'s own docs. Never reaches RequestFrame()/
      // QueueDownload()/ApplyCachedDownload() below, all of which assume
      // one key per product.
      FetchShipSelection(productIndex);
      return true;
   }

   if (IsIdxCategory(p->category_))
   {
      // Nbm's own single-field idx-based dispatch -- see
      // FetchIdxSelectionForProduct()'s own doc. Never reaches
      // RequestFrame()/QueueDownload() below (both assume a whole-file
      // download), and deliberately ignores the `time` argument the same
      // way the Rrfs branch below does -- resolves its own key from the
      // provider's own live cycle/forecast-hour state instead (kept in
      // sync by SyncIdxProviderState() whenever that state changes).
      FetchIdxSelectionForProduct(productIndex);
      return true;
   }

   std::string key;

   if (p->category_ == map::GribCategory::Rrfs)
   {
      // A real correctness gap, found and fixed while adding the prslev
      // file family (see [[supercell-wx-wpc-qpf]] session's own RRFS
      // follow-up): FindKey()/EnsureDateListed() below rely on the base
      // class's day-granularity object cache, which is exactly right for
      // MRMS/RTMA (one listing covers every time within that whole day)
      // but wrong for RRFS -- its own GetPrefix() resolves one *exact*
      // file per cycle/forecast-hour/file-family selection, not a whole
      // day's worth. Confirmed live: GetTimePointsByDate()'s own inner
      // "has this day ever been listed" check ignores its own `update`
      // argument once true, for *any* prior selection that day -- so
      // there is no way to force a correct re-list through that API once
      // the day has been seen once, and changing forecast hour twice on
      // the same calendar day silently kept returning the *first* hour's
      // key both times. RrfsDataProvider::BuildKey() -- already used by
      // PrefetchRrfsForecastHourRange() for the same underlying reason --
      // sidesteps the whole listing path: deterministic, so resolving
      // which file this selection wants needs no network call at all
      // (only downloading it does, and RequestFrame()/QueueDownload()
      // already handle a file that turns out not to exist gracefully).
      auto& rrfsProvider = ProviderAs<provider::RrfsDataProvider>(*provider);
      const auto family  = Products(p->category_)[productIndex].rrfsFileFamily;
      key                = provider::RrfsDataProvider::BuildKey(
         rrfsProvider.CurrentCycle(), rrfsProvider.ForecastHour(), family);
   }
   else
   {
      // MRMS/RTMA need an S3 listing to find the file nearest `time` --
      // blocking network I/O, done on the lookup pool (see QueueLookup()).
      QueueLookup(productIndex, provider, time);
      return true;
   }

   if (key.empty())
   {
      // Nothing found nearby (e.g. archive time predates the source, or a
      // network hiccup) -- leave whatever frame is currently showing
      // rather than clearing it out from under the user for a likely
      // transient gap.
      logger_->warn("No GRIB file found near {} for product {}",
                    scwx::util::TimeString(time),
                    productIndex);
      return false;
   }

   {
      const std::string cacheKey =
         CacheKeyFor(p->category_, Products(p->category_)[productIndex], key);

      std::lock_guard lock(p->fetchMutex_);
      if (cacheKey == p->lastKeys_[productIndex])
      {
         // Already showing this one -- archive scrubbing/playback can
         // re-fire this handler rapidly, don't redo the request each time.
         return true;
      }
   }

   logger_->info("Archive GRIB file for {} (product {}): {}",
                 scwx::util::TimeString(time),
                 productIndex,
                 key);
   RequestFrame(productIndex, key, provider);
   return true;
}

void GribManager::QueueLookup(
   std::size_t                                          productIndex,
   std::shared_ptr<provider::AwsNexradDataProvider>     provider,
   std::optional<std::chrono::system_clock::time_point> time)
{
   std::uint64_t seq;
   {
      std::lock_guard lock(p->fetchMutex_);
      seq = ++p->lookupSeq_[productIndex];
   }

   boost::asio::post(
      p->lookupPool_,
      [this, productIndex, provider, time, seq]()
      {
         using namespace std::chrono;

         // Still active, and still the newest lookup for this product?
         // Must be called with fetchMutex_ held.
         auto stillCurrent = [this, productIndex, seq]()
         {
            return p->activeSnapshot_.contains(productIndex) &&
                   p->lookupSeq_[productIndex] == seq;
         };

         {
            std::lock_guard lock(p->fetchMutex_);
            if (!stillCurrent())
            {
               return; // superseded before it ever ran -- skip the listing
            }
         }

         std::string key;

         if (!time.has_value())
         {
            auto [newObjects, totalObjects] = provider->Refresh();
            logger_->debug("Refresh (product {}): {} new / {} total objects",
                           productIndex,
                           newObjects,
                           totalObjects);

            key = provider->FindLatestKey();
         }
         else
         {
            // MRMS/RTMA's S3 listing is per-UTC-day, so list the day
            // `time` falls on before searching it.
            const auto date = floor<days>(*time);
            EnsureDateListed(*provider, date);

            key = provider->FindKey(*time);

            // A selection near midnight UTC can have its nearest real file
            // on the adjacent day's listing rather than the day `time`
            // itself falls on -- try both neighbors before giving up.
            if (key.empty())
            {
               EnsureDateListed(*provider, date - days {1});
               key = provider->FindKey(*time);
            }
            if (key.empty())
            {
               EnsureDateListed(*provider, date + days {1});
               key = provider->FindKey(*time);
            }

            if (key.empty())
            {
               // Nothing found nearby (e.g. archive time predates the
               // source, or a network hiccup) -- leave whatever frame is
               // currently showing rather than clearing it out from under
               // the user for a likely transient gap.
               logger_->warn("No GRIB file found near {} for product {}",
                             scwx::util::TimeString(*time),
                             productIndex);
               return;
            }
         }

         if (key.empty())
         {
            return;
         }

         {
            std::lock_guard lock(p->fetchMutex_);
            if (!stillCurrent())
            {
               return;
            }

            // Already showing this one -- live polls repeat the same
            // latest key between updates, and archive scrubbing/playback
            // re-fires rapidly over the same file.
            const auto shown = p->lastKeys_.find(productIndex);
            if (shown != p->lastKeys_.cend() &&
                shown->second ==
                   CacheKeyFor(
                      p->category_, Products(p->category_)[productIndex], key))
            {
               return;
            }
         }

         if (time.has_value())
         {
            logger_->info("Archive GRIB file for {} (product {}): {}",
                          scwx::util::TimeString(*time),
                          productIndex,
                          key);
         }
         else
         {
            logger_->info(
               "New GRIB file for product {}: {}", productIndex, key);
         }

         RequestFrame(productIndex, key, provider);
      });
}

// SHIP's own two-file selection: resolves both of its current inputs'
// keys (deterministically, via BuildKey() -- same reasoning as the
// normal RRFS branch above), kicks off a background download for
// whichever isn't already cached, and checks (synchronously, cheap) in
// case both already are. Never touches RequestFrame()/QueueDownload()/
// ApplyCachedDownload() -- those all assume one key decodes one
// product's frame, which doesn't hold for a two-input derived index.
void GribManager::FetchShipSelection(std::size_t productIndex)
{
   auto& rrfsProvider =
      ProviderAs<provider::RrfsDataProvider>(*p->providers_.at(productIndex));

   const auto key2dfld =
      provider::RrfsDataProvider::BuildKey(rrfsProvider.CurrentCycle(),
                                           rrfsProvider.ForecastHour(),
                                           provider::RrfsFileFamily::TwoDField);
   const auto keyPrslev = provider::RrfsDataProvider::BuildKey(
      rrfsProvider.CurrentCycle(),
      rrfsProvider.ForecastHour(),
      provider::RrfsFileFamily::PressureLevel);

   {
      std::lock_guard lock(p->fetchMutex_);
      p->lastRequestedKeys_[productIndex] = key2dfld + "|" + keyPrslev;
   }

   const ProductConfig& ship = Products(p->category_)[productIndex];
   if (!std::filesystem::exists(
          CachedDownloadPath(CacheKeyFor(p->category_, ship, key2dfld))))
   {
      QueueShipInput(productIndex, key2dfld, false);
   }
   if (!std::filesystem::exists(
          CachedDownloadPath(CacheKeyFor(p->category_, ship, keyPrslev))))
   {
      QueueShipInput(productIndex, keyPrslev, true);
   }

   // Covers the case both were already cached (same "show when ready"
   // idea as RequestFrame()'s own already-cached branch, and likewise on
   // the decode pool -- SHIP's decode is the slowest of all) -- if either
   // was just queued above, the job's own "both exist yet?" check will
   // correctly say no for now, and each QueueShipInput() job re-checks
   // this same way once its own download finishes.
   boost::asio::post(
      p->decodePool_,
      [this, productIndex, provider = p->providers_.at(productIndex)]()
      { ApplyShipIfReady(productIndex, provider); });
}

// Called once after queuing (in case both inputs were already cached)
// and again from each QueueShipInput() job's own completion -- checks
// whether SHIP's *current* selection's two inputs are both on disk yet
// and, if so, decodes. Recomputes both keys fresh from the provider's
// own live cycle/forecast-hour state rather than trusting whatever
// triggered this call, so a stale completion (the selection moved on
// while a download was in flight) naturally finds the *new* combination
// still incomplete and does nothing, rather than applying an outdated
// pair.
void GribManager::ApplyShipIfReady(
   std::size_t                                             productIndex,
   const std::shared_ptr<provider::AwsNexradDataProvider>& provider)
{
   auto& rrfsProvider = ProviderAs<provider::RrfsDataProvider>(*provider);

   const auto key2dfld =
      provider::RrfsDataProvider::BuildKey(rrfsProvider.CurrentCycle(),
                                           rrfsProvider.ForecastHour(),
                                           provider::RrfsFileFamily::TwoDField);
   const auto keyPrslev = provider::RrfsDataProvider::BuildKey(
      rrfsProvider.CurrentCycle(),
      rrfsProvider.ForecastHour(),
      provider::RrfsFileFamily::PressureLevel);

   const ProductConfig& ship = Products(p->category_)[productIndex];
   if (!std::filesystem::exists(
          CachedDownloadPath(CacheKeyFor(p->category_, ship, key2dfld))) ||
       !std::filesystem::exists(
          CachedDownloadPath(CacheKeyFor(p->category_, ship, keyPrslev))))
   {
      return; // not both ready yet
   }

   const std::string combinedKey = key2dfld + "|" + keyPrslev;

   {
      std::lock_guard lock(p->fetchMutex_);
      if (combinedKey == p->lastKeys_[productIndex])
      {
         return; // already showing this exact combination
      }
      if (combinedKey != p->lastRequestedKeys_[productIndex])
      {
         return; // superseded by a newer selection since this was queued
      }
   }

   ApplyShipDownload(productIndex, key2dfld, keyPrslev);
}

// Downloads one of SHIP's two inputs (if not already in flight) on the
// background thread pool, then re-checks ApplyShipIfReady() once done --
// mirrors QueueDownload()'s own shape (in-flight dedup, background pool,
// PruneDownloadCache) but doesn't call it directly: its own completion
// unconditionally invokes ApplyCachedDownload() with *one* key, which
// would run decode_grib with the wrong (single-input) CLI form for
// SHIP's two-input mode.
void GribManager::QueueShipInput(std::size_t        productIndex,
                                 const std::string& key,
                                 bool               pressureLevel)
{
   {
      std::lock_guard lock(p->fetchMutex_);
      if (!p->inFlightKeys_.insert({productIndex, key}).second)
      {
         return; // already downloading this specific input
      }
   }

   std::shared_ptr<provider::AwsNexradDataProvider> provider =
      p->providers_.at(productIndex);
   auto                statusManager = manager::StatusManager::Instance();
   const ProductConfig product       = Products(p->category_)[productIndex];
   const std::string   cacheKey      = CacheKeyFor(p->category_, product, key);

   boost::asio::post(
      p->fetchPool_,
      [this,
       productIndex,
       key,
       cacheKey,
       pressureLevel,
       provider,
       product,
       statusManager]()
      {
         const std::string cachedPath = CachedDownloadPath(cacheKey);
         std::filesystem::create_directories(
            std::filesystem::path(cachedPath).parent_path());

         // One status entry per (productIndex, key) pair -- SHIP's two
         // inputs can genuinely be downloading at once, each wanting its
         // own progress entry, same reasoning as QueueDownload()'s own
         // per-(category,productIndex) id.
         const std::string statusId =
            fmt::format("grib-ship-{}-{}", productIndex, key);
         const auto progressCallback =
            [&statusManager, &statusId](std::int64_t bytesReceived,
                                        std::int64_t totalBytes)
         {
            statusManager->ReportProgress(
               statusId, "SHIP", bytesReceived, totalBytes);
         };

         // Each of SHIP's two files holds only the fields it reads from it
         // (the 2dfld ones, or the pressure-level ones)
         const grib_fields::ProductFields* fields =
            grib_fields::FieldsFor(p->category_, product.displayName);

         auto& rrfsProvider = ProviderAs<provider::RrfsDataProvider>(*provider);
         auto  downloaded   = DownloadFieldsOrObject(
            rrfsProvider,
            "SHIP",
            fields == nullptr ?
               std::vector<scwx::util::grib_idx::FieldSelector> {} :
               (pressureLevel ? fields->secondary : fields->primary),
            key,
            cachedPath,
            progressCallback);
         statusManager->ReportComplete(statusId);

         {
            std::lock_guard lock(p->fetchMutex_);
            p->inFlightKeys_.erase({productIndex, key});
         }

         if (!downloaded.has_value())
         {
            logger_->warn("Failed to download SHIP input {}", key);
            return;
         }

         NoteCachedDownload(cachedPath);

         ApplyShipIfReady(productIndex, provider);
      });
}

// Decodes SHIP from its two already-downloaded inputs and atomically
// replaces productIndex's own frame file -- ApplyCachedDownload()'s own
// two-input counterpart; kept separate rather than extending that
// function's signature, since every other product only ever has one
// input.
bool GribManager::ApplyShipDownload(std::size_t        productIndex,
                                    const std::string& key2dfld,
                                    const std::string& keyPrslev)
{
   const ProductConfig& product = Products(p->category_)[productIndex];
   const std::string    framePath =
      map::GetGribFramePath(p->category_, productIndex);
   const std::string        tmpFramePath = UniqueTmpFramePath(framePath);
   const ScopedDecodeStatus decodeStatus(
      fmt::format("grib-{}-{}", static_cast<int>(p->category_), productIndex),
      product.displayName);

   QStringList decodeArgs;
   decodeArgs << "--derived" << "ship"
              << QString::fromStdString(CachedDownloadPath(
                    CacheKeyFor(p->category_, product, key2dfld)))
              << QString::fromStdString(CachedDownloadPath(
                    CacheKeyFor(p->category_, product, keyPrslev)))
              << QString::fromStdString(tmpFramePath)
              << QString::number(product.colorOffset)
              << QString::number(product.colorScale)
              << QString::number(product.noDataThreshold);
   if (product.contourInterval > 0.0f)
   {
      decodeArgs << QString::number(product.contourInterval);
   }

   QProcess decodeProcess;
   decodeProcess.start(QString::fromStdString(DecodeGribPath()), decodeArgs);

   if (!decodeProcess.waitForFinished(kDecodeTimeoutMs_) ||
       decodeProcess.exitCode() != 0)
   {
      logger_->warn("decode_grib failed for SHIP: {}",
                    decodeProcess.readAllStandardError().toStdString());
      decodeProcess.kill();
      decodeProcess.waitForFinished(5000); // release tmpFramePath (Windows)
      std::error_code removeEc;
      std::filesystem::remove(tmpFramePath, removeEc);
      return false;
   }

   if (!CommitDecodedFrame(
          productIndex, key2dfld + "|" + keyPrslev, tmpFramePath, framePath))
   {
      return false;
   }

   logger_->info("Updated {}",
                 map::GetGribFramePath(p->category_, productIndex));
   Q_EMIT FrameReady(productIndex);
   return true;
}

void GribManager::SyncIdxProviderState(
   provider::AwsNexradDataProvider& provider) const
{
   auto& nbmProvider = ProviderAs<provider::IdxModelProvider>(provider);

   if (p->idxUseLatestCycle_)
   {
      nbmProvider.UseLatestCycle();
   }
   else
   {
      nbmProvider.SetCycle(p->idxCycleOverride_);
   }
   nbmProvider.SetForecastHour(p->idxForecastHour_);
}

void GribManager::FetchIdxSelection()
{
   using namespace std::chrono;

   // No-op with nothing active yet -- same reasoning as
   // FetchRrfsSelection()'s identical guard.
   const auto index = CurrentProductIndex();
   if (!index)
   {
      return;
   }

   auto& nbmProvider =
      ProviderAs<provider::IdxModelProvider>(*p->providers_.at(*index));

   // FetchArchiveFrameForProduct()'s own Nbm branch ignores this `time`
   // argument entirely, resolving each product's key from its own
   // provider state instead -- same reasoning as FetchRrfsSelection()'s
   // identical call shape. Computed anyway for readability/logging, not
   // because anything downstream reads it.
   const auto time =
      nbmProvider.CurrentCycle() + hours {nbmProvider.ForecastHour()};

   FetchArchiveFrame(time);
}

// Nbm's own per-product dispatch, called from FetchArchiveFrameForProduct()
// for every active Nbm product (via FetchArchiveFrame()'s loop) and
// directly from FetchIdxSelection()/SetProductActive() when the manager
// only has one product's selection to resolve. Unlike Rrfs/MRMS/RTMA, a
// key alone doesn't say what to download here -- NBM's own per-cycle
// file bundles every field, so the *field* (this product's own
// nbmParameter/nbmLevel/nbmQualifier) has to come along too, and each
// field's cached bytes need their own path (see cacheKey below) since
// several products share the same underlying S3 key.
void GribManager::FetchIdxSelectionForProduct(std::size_t productIndex)
{
   auto& idxProvider =
      ProviderAs<provider::IdxModelProvider>(*p->providers_.at(productIndex));

   const std::string key = idxProvider.BuildKeyFor(idxProvider.CurrentCycle(),
                                                   idxProvider.ForecastHour());

   // Suffixed with this product's own shortName -- distinct products
   // downloading different fields out of the *same* underlying key must
   // not collide on one cache path (CachedDownloadPath() otherwise keys
   // purely off the S3 object path, correct for every other category
   // where one download serves every product sharing that file).
   const std::string cacheKey =
      key + "." + Products(p->category_)[productIndex].shortName;

   {
      std::lock_guard lock(p->fetchMutex_);
      if (cacheKey == p->lastKeys_[productIndex])
      {
         // Already showing this one.
         return;
      }
      p->lastRequestedKeys_[productIndex] = cacheKey;
   }

   if (std::filesystem::exists(CachedDownloadPath(cacheKey)))
   {
      QueueCachedDecode(productIndex, cacheKey);
      return;
   }

   QueueIdxDownload(productIndex, key, cacheKey);
}

// Downloads exactly one field via NbmDataProvider::FetchField() (the
// idx-based range fetch, see AwsNexradDataProvider::
// DownloadGribMessageByIndex()) rather than RequestFrame()/
// QueueDownload()'s whole-file DownloadRaw() path -- those assume the
// downloaded bytes decode by shortName alone out of a multi-field file,
// which is true for Rtma/Rrfs but not for Nbm, where the byte range
// fetched already contains only the one field asked for.
void GribManager::QueueIdxDownload(std::size_t        productIndex,
                                   const std::string& key,
                                   const std::string& cacheKey)
{
   {
      std::lock_guard lock(p->fetchMutex_);
      if (!p->inFlightKeys_.insert({productIndex, cacheKey}).second)
      {
         return;
      }
   }

   std::shared_ptr<provider::AwsNexradDataProvider> provider =
      p->providers_.at(productIndex);
   const ProductConfig product = Products(p->category_)[productIndex];

   auto statusManager = manager::StatusManager::Instance();

   boost::asio::post(
      p->fetchPool_,
      [this, productIndex, key, cacheKey, provider, product, statusManager]()
      {
         const std::string cachedPath = CachedDownloadPath(cacheKey);
         std::filesystem::create_directories(
            std::filesystem::path(cachedPath).parent_path());

         const std::string statusId = fmt::format(
            "grib-{}-{}", static_cast<int>(p->category_), productIndex);
         const auto progressCallback =
            [&statusManager, &statusId, &product](std::int64_t bytesReceived,
                                                  std::int64_t totalBytes)
         {
            statusManager->ReportProgress(
               statusId, product.displayName, bytesReceived, totalBytes);
         };

         auto downloaded =
            ProviderAs<provider::IdxModelProvider>(*provider).FetchField(
               key,
               product.nbmParameter,
               product.nbmLevel,
               product.nbmQualifier,
               cachedPath,
               progressCallback);

         statusManager->ReportComplete(statusId);

         {
            std::lock_guard lock(p->fetchMutex_);
            p->inFlightKeys_.erase({productIndex, cacheKey});
         }

         if (!downloaded.has_value())
         {
            logger_->warn("Failed to download NBM field {} ({}) from {}",
                          product.displayName,
                          product.shortName,
                          key);
            return;
         }

         NoteCachedDownload(cachedPath);

         bool stillWanted;
         {
            std::lock_guard lock(p->fetchMutex_);
            stillWanted = (p->lastRequestedKeys_[productIndex] == cacheKey);
         }

         if (stillWanted)
         {
            ApplyCachedDownload(productIndex,
                                cacheKey,
                                product.shortName,
                                product.colorOffset,
                                product.colorScale,
                                product.noDataThreshold,
                                product.contourInterval,
                                product.derivedIndex,
                                product.typeOfLevel,
                                product.topLevel,
                                product.bottomLevel,
                                product.startStep,
                                product.lengthOfTimeRange);
         }
      });
}

void GribManager::SetIdxCycle(std::chrono::system_clock::time_point cycleTime)
{
   if (!IsIdxCategory(p->category_))
   {
      logger_->warn("SetIdxCycle() is only meaningful for GribCategory::Nbm");
      return;
   }

   p->idxUseLatestCycle_ = false;
   p->idxCycleOverride_  = cycleTime;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncIdxProviderState(*providerPtr);
   }

   FetchIdxSelection();
}

void GribManager::UseLatestIdxCycle()
{
   if (!IsIdxCategory(p->category_))
   {
      logger_->warn(
         "UseLatestIdxCycle() is only meaningful for GribCategory::Nbm");
      return;
   }

   p->idxUseLatestCycle_ = true;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncIdxProviderState(*providerPtr);
   }

   FetchIdxSelection();
}

bool GribManager::IsUsingLatestIdxCycle() const
{ return !IsIdxCategory(p->category_) || p->idxUseLatestCycle_; }

std::chrono::system_clock::time_point GribManager::CurrentIdxCycle() const
{
   if (!IsIdxCategory(p->category_))
   {
      return {};
   }

   // Epoch when nothing is active yet -- same reasoning as
   // CurrentRrfsCycle()'s identical fallback.
   const auto index = CurrentProductIndex();
   if (!index)
   {
      return {};
   }

   return ProviderAs<provider::IdxModelProvider>(*p->providers_.at(*index))
      .CurrentCycle();
}

void GribManager::SetIdxForecastHour(int hour)
{
   if (!IsIdxCategory(p->category_))
   {
      logger_->warn(
         "SetIdxForecastHour() is only meaningful for GribCategory::Nbm");
      return;
   }

   // Snapped to a real, fetchable hour immediately -- NBM's own forecast-
   // hour step is non-uniform beyond F069 for extended cycles (see
   // NbmDataProvider::SnapForecastHour()'s own doc), so a caller driving
   // this from a linear slider would otherwise request hours that simply
   // don't exist for most of the range.
   p->idxForecastHour_ =
      p->idxRules_ ?
         p->idxRules_->SnapForecastHourFor(CurrentIdxCycle(), hour) :
         hour;

   for (auto& [index, providerPtr] : p->providers_)
   {
      SyncIdxProviderState(*providerPtr);
   }

   FetchIdxSelection();
}

int GribManager::IdxForecastHour() const
{ return (IsIdxCategory(p->category_)) ? p->idxForecastHour_ : 0; }

int GribManager::MaxIdxForecastHour() const
{
   if (!IsIdxCategory(p->category_) || !CurrentProductIndex())
   {
      // The second check matters on its own -- same reasoning as
      // MaxRrfsForecastHour()'s identical guard.
      return 0;
   }

   return p->idxRules_ ? p->idxRules_->MaxForecastHourFor(CurrentIdxCycle()) :
                         0;
}

int GribManager::MinIdxForecastHour() const
{
   return (IsIdxCategory(p->category_) && p->idxRules_) ?
             p->idxRules_->MinForecastHourFor() :
             0;
}

void GribManager::ReloadUserModel()
{
   if (p->category_ != map::GribCategory::User)
   {
      logger_->warn(
         "ReloadUserModel() is only meaningful for GribCategory::User");
      return;
   }

   // Every earlier product index belongs to the old model's table.
   for (const auto& name : ActiveProductNames())
   {
      SetProductActive(name, false);
   }

   const auto model = p->userModels_->SelectedModel();
   RebuildUserProducts(model);

   {
      std::lock_guard lock(p->fetchMutex_);
      p->lastKeys_.clear();
      p->lastRequestedKeys_.clear();
   }

   p->idxUseLatestCycle_ = true;
   p->idxCycleOverride_  = {};
   p->idxRules_          = MakeIdxRulesProvider(p->category_);
   p->idxForecastHour_ = p->idxRules_ ? p->idxRules_->MinForecastHourFor() : 0;

   logger_->info("User model is now \"{}\" ({} products)",
                 model ? model->config.name : std::string {"(none)"},
                 Products(p->category_).size());

   Q_EMIT ProductsChanged();
}

std::string GribManager::UserModelName() const
{
   if (p->category_ != map::GribCategory::User)
   {
      return {};
   }
   return p->userModels_->SelectedModelName();
}

std::vector<std::chrono::system_clock::time_point>
GribManager::IdxCycleChoices(int historyHours) const
{
   std::vector<std::chrono::system_clock::time_point> cycles;
   if (!IsIdxCategory(p->category_) || !p->idxRules_)
   {
      return cycles;
   }

   const auto now =
      std::chrono::floor<std::chrono::hours>(std::chrono::system_clock::now());
   for (int i = 0; i < historyHours; ++i)
   {
      const auto cycle = now - std::chrono::hours {i};
      if (p->idxRules_->RunsCycleAt(cycle))
      {
         cycles.push_back(cycle);
      }
   }
   return cycles;
}

int GribManager::MaxIdxForecastHourFor(
   std::chrono::system_clock::time_point cycle) const
{
   return (IsIdxCategory(p->category_) && p->idxRules_) ?
             p->idxRules_->MaxForecastHourFor(cycle) :
             0;
}

void GribManager::RequestFrame(
   std::size_t                                             productIndex,
   const std::string&                                      key,
   const std::shared_ptr<provider::AwsNexradDataProvider>& provider)
{
   // `key` names the S3 object; the cache (and everything that tracks what is
   // showing or wanted) is keyed by where this product's bytes of it live.
   const std::string cacheKey =
      CacheKeyFor(p->category_, Products(p->category_)[productIndex], key);

   {
      std::lock_guard lock(p->fetchMutex_);
      p->lastRequestedKeys_[productIndex] = cacheKey;
   }

   if (std::filesystem::exists(CachedDownloadPath(cacheKey)))
   {
      // Already on disk (a prior fetch, or a prefetch that's since
      // completed) -- just decode it. This is the "show when ready" path:
      // by the time playback actually reaches a prefetched time, this is
      // normally all that runs.
      QueueCachedDecode(productIndex, cacheKey);
      return;
   }

   QueueDownload(productIndex, key, provider);
}

void GribManager::QueueCachedDecode(std::size_t        productIndex,
                                    const std::string& key)
{
   boost::asio::post(
      p->decodePool_,
      [this, productIndex, key]()
      {
         {
            // Skip it if playback/scrubbing has moved on (or the product
            // was deactivated) while this waited in the queue.
            std::lock_guard lock(p->fetchMutex_);
            const auto      wanted = p->lastRequestedKeys_.find(productIndex);
            if (wanted == p->lastRequestedKeys_.cend() || wanted->second != key)
            {
               return;
            }
         }

         const ProductConfig& product = Products(p->category_)[productIndex];
         ApplyCachedDownload(productIndex,
                             key,
                             product.shortName,
                             product.colorOffset,
                             product.colorScale,
                             product.noDataThreshold,
                             product.contourInterval,
                             product.derivedIndex,
                             product.typeOfLevel,
                             product.topLevel,
                             product.bottomLevel,
                             product.startStep,
                             product.lengthOfTimeRange);
      });
}

void GribManager::QueueDownload(
   std::size_t                                      productIndex,
   const std::string&                               key,
   std::shared_ptr<provider::AwsNexradDataProvider> provider)
{
   const std::string cacheKey =
      CacheKeyFor(p->category_, Products(p->category_)[productIndex], key);

   {
      std::lock_guard lock(p->fetchMutex_);
      auto [waiters, firstRequest] = p->downloadWaiters_.try_emplace(cacheKey);
      waiters->second.insert(productIndex);
      if (!firstRequest)
      {
         // Already downloading this file -- for this product (a prefetch
         // and a direct request racing), or for another product sharing
         // it (RTMA/RRFS bundle every field into one file). Either way the
         // running download decodes it for productIndex too when it's done.
         return;
      }
   }

   // Snapshot everything this job needs by value -- it may run well after
   // the user has deactivated this product or moved on, so it must not
   // read p->providers_/p->activeProducts_ live from the pool thread (the
   // provider is passed in for the same reason).
   const ProductConfig product = Products(p->category_)[productIndex];

   // Held by value in the download lambda below, not looked up fresh via
   // Instance() there -- keeps the singleton alive for this download's
   // full duration regardless of whether anything else (e.g. a status
   // bar widget) already holds a longer-lived reference to it yet.
   auto statusManager = manager::StatusManager::Instance();

   boost::asio::post(
      p->fetchPool_,
      [this, productIndex, key, cacheKey, provider, product, statusManager]()
      {
         const std::string cachedPath = CachedDownloadPath(cacheKey);
         std::filesystem::create_directories(
            std::filesystem::path(cachedPath).parent_path());

         // Keyed by (category_, productIndex) -- distinct GribManager
         // instances/products can genuinely download at once (see this
         // class's own multi-select support), each wanting its own status
         // entry rather than clobbering another's.
         const std::string statusId = fmt::format(
            "grib-{}-{}", static_cast<int>(p->category_), productIndex);
         const auto progressCallback =
            [&statusManager, &statusId, &product](std::int64_t bytesReceived,
                                                  std::int64_t totalBytes)
         {
            statusManager->ReportProgress(
               statusId, product.displayName, bytesReceived, totalBytes);
         };

         // MRMS objects are gzipped, RTMA's/RRFS's aren't (see
         // RtmaDataProvider's class comment) -- provider is held as the
         // common AwsNexradDataProvider base, so this is the one place
         // that needs the concrete type back. category_ (unlike
         // providers_/activeProducts_) never changes after construction,
         // so reading it live via `this` from the pool thread is safe.
         const grib_fields::ProductFields* fields =
            grib_fields::FieldsFor(p->category_, product.displayName);

         auto fieldStatus =
            provider::AwsNexradDataProvider::FieldDownloadStatus::Downloaded;

         std::optional<std::string> downloaded;
         switch (p->category_)
         {
         case map::GribCategory::Mrms:
            downloaded =
               ProviderAs<provider::MrmsDataProvider>(*provider)
                  .DownloadAndDecompress(key, cachedPath, progressCallback);
            break;
         case map::GribCategory::Rrfs:
            downloaded = DownloadFieldsOrObject(
               ProviderAs<provider::RrfsDataProvider>(*provider),
               product.displayName,
               fields != nullptr ?
                  fields->primary :
                  std::vector<scwx::util::grib_idx::FieldSelector> {},
               key,
               cachedPath,
               progressCallback,
               &fieldStatus);
            break;
         case map::GribCategory::Rtma:
         default:
            downloaded = DownloadFieldsOrObject(
               ProviderAs<provider::RtmaDataProvider>(*provider),
               product.displayName,
               fields != nullptr ?
                  fields->primary :
                  std::vector<scwx::util::grib_idx::FieldSelector> {},
               key,
               cachedPath,
               progressCallback,
               &fieldStatus);
            break;
         }

         statusManager->ReportComplete(statusId);

         // Every product that asked for this file while it downloaded.
         std::set<std::size_t> waiters;
         {
            std::lock_guard lock(p->fetchMutex_);
            const auto      entry = p->downloadWaiters_.find(cacheKey);
            if (entry != p->downloadWaiters_.end())
            {
               waiters = std::move(entry->second);
               p->downloadWaiters_.erase(entry);
            }
         }

         if (!downloaded.has_value())
         {
            logger_->warn(
               "Failed to download {} for {}", key, product.displayName);

            // A field the cycle's file does not hold (an hourly RRFS cycle's
            // file has half the records of a 3-hourly one) will not appear by
            // trying again; a failed or not-yet-published download may.
            const bool absent = fieldStatus ==
                                provider::AwsNexradDataProvider::
                                   FieldDownloadStatus::NoMatchingRecord;
            statusManager->ReportMessage(
               fmt::format("grib-error-{}-{}",
                           static_cast<int>(p->category_),
                           productIndex),
               absent ? fmt::format("{}: not in this cycle's data (hourly "
                                    "cycles carry fewer fields)",
                                    product.displayName) :
                        fmt::format("{}: download failed, will retry",
                                    product.displayName));
            return;
         }

         NoteCachedDownload(cachedPath);

         // Lets the RRFS hour buttons flip to their downloaded look,
         // whether or not this hour is the one on screen.
         if (p->category_ == map::GribCategory::Rrfs)
         {
            Q_EMIT RrfsCacheChanged();
         }

         // Decode it for each of them -- QueueCachedDecode() skips any whose
         // request has since moved on (playback/scrubbing, or the product
         // was unchecked), which would otherwise show a stale frame. The
         // file stays cached regardless, for a loop repeat or a scrub back.
         for (const std::size_t waiter : waiters)
         {
            QueueCachedDecode(waiter, cacheKey);
         }
      });
}

bool GribManager::ApplyCachedDownload(std::size_t        productIndex,
                                      const std::string& key,
                                      const std::string& shortName,
                                      float              colorOffset,
                                      float              colorScale,
                                      float              noDataThreshold,
                                      float              contourInterval,
                                      const std::string& derivedIndex,
                                      const std::string& typeOfLevel,
                                      long               topLevel,
                                      long               bottomLevel,
                                      long               startStep,
                                      long               lengthOfTimeRange)
{
   const ScopedDecodeStatus decodeStatus(
      fmt::format("grib-{}-{}", static_cast<int>(p->category_), productIndex),
      Products(p->category_)[productIndex].displayName);
   const std::string cachedPath = CachedDownloadPath(key);
   const std::string framePath =
      map::GetGribFramePath(p->category_, productIndex);
   const std::string tmpFramePath = UniqueTmpFramePath(framePath);

   // decode_grib's own runtime is ~25-30ms for a normal single-message
   // decode (measured against a full CONUS MRMS file); a derived index
   // like STP reads several messages and does real per-pixel math, but
   // still comfortably sub-second against a CONUS grid. Blocks whichever
   // thread calls this -- always a background one (decodePool_ for an
   // already-cached hit, a fetchPool_ thread after a download), never the
   // GUI thread.
   QStringList decodeArgs;
   if (!derivedIndex.empty())
   {
      // decode_grib's `--derived <name>` mode has its own fixed field
      // list per index name -- shortName is meaningless here and left
      // out entirely (see ProductConfig::derivedIndex).
      decodeArgs << "--derived" << QString::fromStdString(derivedIndex);
   }
   decodeArgs << QString::fromStdString(cachedPath)
              << QString::fromStdString(tmpFramePath)
              << QString::number(colorOffset) << QString::number(colorScale)
              << QString::number(noDataThreshold);
   if (!shortName.empty())
   {
      // Selects one message out of RTMA's 13-field bundled file (see
      // decode_grib's shortName argument) -- MRMS's single-message files
      // never pass this.
      decodeArgs << QString::fromStdString(shortName);

      // decode_grib's CLI requires shortName before contourInterval
      // before typeOfLevel/topLevel/bottomLevel before startStep/
      // lengthOfTimeRange (see its own usage string) -- so a product
      // needing any qualifier always emits contourInterval too, even
      // when it's 0 (fill mode), and a product needing startStep/
      // lengthOfTimeRange also emits typeOfLevel/topLevel/bottomLevel
      // even when it doesn't need *those* (RRFS's two "tp" messages
      // share identical level metadata -- see ProductConfig's own doc),
      // since the CLI is strictly positional.
      if (startStep >= 0 || lengthOfTimeRange >= 0)
      {
         decodeArgs << QString::number(contourInterval)
                    << QString::fromStdString(typeOfLevel)
                    << QString::number(topLevel) << QString::number(bottomLevel)
                    << QString::number(startStep)
                    << QString::number(lengthOfTimeRange);
      }
      else if (!typeOfLevel.empty())
      {
         decodeArgs << QString::number(contourInterval)
                    << QString::fromStdString(typeOfLevel)
                    << QString::number(topLevel)
                    << QString::number(bottomLevel);
      }
      else if (contourInterval > 0.0f)
      {
         decodeArgs << QString::number(contourInterval);
      }
   }
   else if (!derivedIndex.empty() && contourInterval > 0.0f)
   {
      // The --derived form also accepts a trailing contourInterval, just
      // without a shortName ahead of it (see decode_grib's RunDerived
      // usage string) -- no current derived index actually sets one, but
      // wiring it through now avoids a second special case later.
      decodeArgs << QString::number(contourInterval);
   }

   QProcess decodeProcess;
   decodeProcess.start(QString::fromStdString(DecodeGribPath()), decodeArgs);

   if (!decodeProcess.waitForFinished(kDecodeTimeoutMs_) ||
       decodeProcess.exitCode() != 0)
   {
      logger_->warn("decode_grib failed for {}: {}",
                    cachedPath,
                    decodeProcess.readAllStandardError().toStdString());
      decodeProcess.kill();
      decodeProcess.waitForFinished(5000); // release tmpFramePath (Windows)
      std::error_code removeEc;
      std::filesystem::remove(tmpFramePath, removeEc);
      return false;
   }

   if (!CommitDecodedFrame(productIndex, key, tmpFramePath, framePath))
   {
      return false;
   }

   logger_->info("Updated {}",
                 map::GetGribFramePath(p->category_, productIndex));
   Q_EMIT FrameReady(productIndex);
   return true;
}

bool GribManager::CommitDecodedFrame(std::size_t        productIndex,
                                     const std::string& key,
                                     const std::string& tmpFramePath,
                                     const std::string& framePath)
{
   // Held across the check *and* the rename, so this can't interleave with
   // SetProductActive() deactivating the product (it takes this same lock
   // to clear activeSnapshot_/lastRequestedKeys_ and delete the frame
   // file) or with another commit for the same product: a decode that
   // outlived its product is dropped rather than resurrecting a frame for a
   // product that's no longer shown, and an older decode finishing late
   // can't replace a newer frame. Its own lock, not fetchMutex_, since the
   // rename can retry for a couple of seconds on Windows (see
   // util::ReplaceFileWithRetry()) and fetchMutex_ is taken on the GUI
   // thread for every request.
   std::lock_guard commitLock(p->commitMutex_);

   bool wanted = false;
   {
      std::lock_guard lock(p->fetchMutex_);
      const auto      requested = p->lastRequestedKeys_.find(productIndex);
      wanted                    = p->activeSnapshot_.contains(productIndex) &&
                                  requested != p->lastRequestedKeys_.cend() &&
                                  requested->second == key;
   }

   std::error_code ec;
   if (wanted)
   {
      // Retries while Windows refuses to replace a frame a reader has open
      // (the map layer, the dock, or the PNG export reading it). Several
      // seconds, not the couple the other sites use: this runs on a
      // background decode thread, and giving up drops the hour entirely.
      constexpr int kSwapAttempts = 200;
      util::ReplaceFileWithRetry(tmpFramePath, framePath, ec, kSwapAttempts);
   }

   if (!wanted || ec)
   {
      if (ec)
      {
         logger_->warn("Could not replace frame file: {}", ec.message());

         // A dropped frame leaves the map on the previous hour, which looks
         // like the loop simply stopped -- say so. Nothing clears this id, so
         // the status bar drops it by itself once it goes stale.
         manager::StatusManager::Instance()->ReportMessage(
            fmt::format(
               "grib-swap-{}-{}", static_cast<int>(p->category_), productIndex),
            fmt::format("{}: could not update the frame ({})",
                        ProductName(productIndex),
                        ec.message()));
      }
      std::error_code removeEc;
      std::filesystem::remove(tmpFramePath, removeEc);
      return false;
   }

   {
      std::lock_guard lock(p->fetchMutex_);
      p->lastKeys_[productIndex] = key;
   }
   return true;
}

} // namespace scwx::qt::manager
