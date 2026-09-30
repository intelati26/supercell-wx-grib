#include <scwx/qt/map/grib_product_layer.hpp>
#include <scwx/qt/gl/shader_program.hpp>
#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/settings/palette_settings.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/qt/util/tooltip.hpp>
#include <scwx/common/color_table.hpp>
#include <scwx/util/logger.hpp>

#include <fmt/format.h>

#if defined(_MSC_VER)
#   pragma warning(push, 0)
#endif

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <mbgl/util/constants.hpp>

#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <QGuiApplication>
#include <QTimer>

namespace scwx::qt::map
{

static const std::string logPrefix_ = "scwx::qt::map::grib_product_layer";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// Default/initial values only, used until the first frame loads -- once a
// frame is parsed, its own colorOffset/colorScale/noDataThreshold (set by
// GribManager per product, see grib-helper's decode_grib) take over. These
// match the real base-reflectivity palette's defined range (see
// res/palettes/wct/DR.pal: -20 to 75 dBZ), the same product this layer
// showed before per-product ranges existed.
static constexpr float kDefaultDataMomentOffset_ = -20.0f;
static constexpr float kDefaultDataMomentScale_  = 95.0f;
static constexpr float kDefaultNoDataThreshold_  = 0.0f;

// Same palette key NEXRAD base reflectivity uses (PaletteSettings::palette
// ("BR")) -- reused for every MRMS product for now, not just reflectivity,
// since Supercell has no user-configurable palette for rotation
// track/hail/precip. Visually mismatched hue-wise for those, but shows
// relative intensity correctly; a real per-product palette is future work.
static const std::string kPaletteKey_ = "BR";

namespace
{

// GRIB2's Grid Definition Section is self-describing (see decode_grib's
// GridType detection off eccodes' "gridType" key) -- MRMS is regular_ll
// (uniform degree spacing), RTMA and most other NCEP CONUS-nest models
// (HRRR, RRFS, RAP, NAM, for future sources) are lambert. The two need
// genuinely different mesh math, not just different parameters: on a
// regular_ll grid, a column of constant grid index is a line of constant
// longitude and a row is a line of constant latitude, so the mesh only
// needs to subdivide latitude (see the west/east comment in LoadFrame).
// On a lambert grid neither is true -- rows and columns of constant grid
// index are curves in lat/lon space -- so both axes need subdividing, with
// each vertex's true geographic position computed via the actual
// projection.
enum class GridType
{
   RegularLatLon,
   Lambert
};

// LambertGrid/LambertConstants/ComputeLambertConstants/LambertForward/
// LambertInverse/LambertGridToLatLon live in grib_frame_info.hpp, shared
// with WindBarbLayer/HodographLayer.

// One checked product's decoded frame and the GL objects drawing it --
// GribProductLayer holds one per active product (see
// manager::GribManager::ActiveProductIndices()) and draws them all.
struct ProductFrame
{
   GLuint  vao {GL_INVALID_INDEX};
   GLuint  vbo {GL_INVALID_INDEX};
   GLuint  dataTexture {GL_INVALID_INDEX};
   GLsizei numVertices {0};

   bool loaded {false};

   // Set when GribManager says this product has a new frame (FrameReady),
   // or by the reload timer's fallback stat() check; consumed by Render(),
   // since GL calls only happen there (MapLibre only guarantees this
   // layer's GL context is current inside Initialize()/Render()).
   bool                            needsReload {true};
   std::filesystem::file_time_type lastWriteTime {};

   // Grid geometry, populated by LoadFrame()
   GridType gridType {GridType::RegularLatLon};
   long     nx {};
   long     ny {};
   double   lat1 {};
   double   lon1 {};
   double   di {};
   double   dj {};

   // Lambert only (see GridType/LambertGrid comments above); left
   // zero-initialized and unused for regular_ll frames.
   double lov {};
   double lad {};
   double latin1 {};
   double latin2 {};
   double dx {};
   double dy {};
   double radius {};

   // Where the loaded frame came from, and its header line exactly as
   // read -- the decoded grid itself isn't kept in memory after the GL
   // upload (up to ~98MB for an MRMS CONUS grid, used only by the
   // Shift-hover tooltip). ValueAt() reads the one value it needs back
   // from the file instead, and uses the header to make sure the file
   // still holds the frame on screen (a newer one may have been renamed
   // into place and not reloaded yet).
   std::string framePath;
   std::string frameHeader;

   // GribManager's curated display name, and the frame's valid time
   // (ISO8601 UTC, e.g. "2026-09-19T03:58:38Z") -- kept as two separate
   // fields, not one display string, so the time stays usable data.
   std::string productLabel;
   std::string validTime;

   // Per-frame colorizing range -- set by GribManager per product (see
   // grib_manager.cpp's ProductConfig), read back from the frame header.
   float colorOffset     = kDefaultDataMomentOffset_;
   float colorScale      = kDefaultDataMomentScale_;
   float noDataThreshold = kDefaultNoDataThreshold_;

   // 0 is fill mode; a nonzero value switches the shader to isoline
   // rendering at every multiple of this value (see grib.frag). Contour
   // products draw after fill products, so isolines stay visible on top of
   // a filled field (see Render()).
   float contourInterval = 0.0f;
};

} // namespace

class GribProductLayer::Impl
{
public:
   explicit Impl(GribCategory category) : category_ {category} {}
   ~Impl() = default;

   Impl(const Impl&)             = delete;
   Impl& operator=(const Impl&)  = delete;
   Impl(const Impl&&)            = delete;
   Impl& operator=(const Impl&&) = delete;

   void BuildPalette();

   // Brings frames_ in line with GribManager's active products: frees the
   // GL objects of products no longer checked, and creates (and marks for
   // loading) entries for newly checked ones. GL context must be current.
   void SyncProducts();

   void LoadFrame(std::size_t productIndex, ProductFrame& frame);
   void DrawFrame(const ProductFrame& frame) const;

   static void ClearFrame(ProductFrame& frame);
   static void DeleteFrameGl(ProductFrame& frame);

   // Looks up the raw decoded value nearest (lat, lon) in `frame`, or
   // nullopt if that point falls outside its grid or has no data.
   // Grid-type-aware: regular_ll inverts the linear lat1/lon1/di/dj
   // mapping directly; lambert reuses the same LambertForward/
   // ComputeLambertConstants the mesh builder uses, run the other way
   // (lat/lon -> grid index instead of grid index -> lat/lon).
   static std::optional<float>
   ValueAt(const ProductFrame& frame, double lat, double lon);

   GribCategory category_;

   std::shared_ptr<gl::ShaderProgram> shaderProgram_ {nullptr};

   GLint uMVPMatrixLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uOriginLatLongLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uDataMomentOffsetLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uDataMomentScaleLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uNoDataThresholdLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uDataTextureLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uPaletteLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};
   GLint uContourIntervalLocation_ {static_cast<GLint>(GL_INVALID_INDEX)};

   // Shared by every product's frame: it samples the color table's own
   // domain, not any product's range (see BuildPalette()).
   GLuint paletteTexture_ {GL_INVALID_INDEX};

   // One per checked product, keyed (and drawn, within each pass) by
   // product index.
   std::map<std::size_t, ProductFrame> frames_;

   // Set when GribManager's active products change (see
   // GribManager::ActiveProductsChanged) -- applied in Render() via
   // SyncProducts(), for the same GL-context reason as needsReload.
   bool productsChanged_ {true};

   // Same deferred-to-Render() pattern, set when the user changes their
   // reflectivity palette in Settings.
   bool paletteNeedsRebuild_ {false};

   // Keeps GribManager alive: Instance() only caches a weak_ptr, so
   // discarding the shared_ptr immediately destroys it (and its timer)
   // before it ever gets to poll.
   std::shared_ptr<manager::GribManager> gribManager_;

   // Fallback safety net for noticing a new frame: GribManager::FrameReady
   // (see frameReadyConnection_ below) is the primary, immediate path --
   // this cheap stat()-only poll only matters if a signal were ever
   // somehow missed (e.g. a frame written by some other means entirely).
   QTimer* reloadTimer_ {nullptr};

   boost::signals2::scoped_connection paletteChangedConnection_;

   // Reassigned each Initialize() (see the disconnect-then-connect there,
   // guarding against a second Initialize() without an intervening
   // Deinitialize() leaving two live connections, same reasoning as
   // reloadTimer_'s delete-before-new guard).
   QMetaObject::Connection frameReadyConnection_;
   QMetaObject::Connection activeProductsConnection_;
};

GribProductLayer::GribProductLayer(std::shared_ptr<gl::GlContext> glContext,
                                   GribCategory                   category) :
    GenericLayer(std::move(glContext)), p(std::make_unique<Impl>(category))
{
}
GribProductLayer::~GribProductLayer() = default;

void GribProductLayer::Initialize(
   const std::shared_ptr<MapContext>& /* mapContext */)
{
   logger_->debug("Initialize()");

   // Ensures the poller is constructed/running (singleton, lazy-init on
   // first Instance() call -- see manager::GribManager). Its FrameReady
   // signal (connected below) is what actually tells this layer to
   // reload.
   p->gribManager_ = manager::GribManager::Instance(p->category_);

   // Drain any GL error state inherited from elsewhere (observed: MapLibre's
   // own internal rendering leaves a stray GL_INVALID_VALUE (1281) that has
   // nothing to do with this layer -- confirmed by checking here, before any
   // of this layer's own GL calls run). Without this, that error gets
   // misattributed to whichever layer happens to check glGetError() first.
   SCWX_GL_CHECK_ERROR();

   auto glContext = gl_context();

   p->shaderProgram_ =
      glContext->GetShaderProgram(":/gl/grib.vert", ":/gl/grib.frag");

   p->uMVPMatrixLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uMVPMatrix");
   p->uOriginLatLongLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uOriginLatLong");
   p->uDataMomentOffsetLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uDataMomentOffset");
   p->uDataMomentScaleLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uDataMomentScale");
   p->uNoDataThresholdLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uNoDataThreshold");
   p->uDataTextureLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uDataTexture");
   p->uPaletteLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uPalette");
   p->uContourIntervalLocation_ =
      glGetUniformLocation(p->shaderProgram_->id(), "uContourInterval");

   // Sampler-to-texture-unit bindings don't change per frame; set them once
   // here rather than every Render(). Without this, both samplers default
   // to unit 0 and the palette would sample the raw data texture.
   p->shaderProgram_->Use();
   glUniform1i(p->uDataTextureLocation_, 0);
   glUniform1i(p->uPaletteLocation_, 1);

   glGenTextures(1, &p->paletteTexture_);

   p->BuildPalette();

   // Loads whatever frames the checked products already have on disk.
   p->productsChanged_ = true;
   p->SyncProducts();
   for (auto& [index, frame] : p->frames_)
   {
      p->LoadFrame(index, frame);
   }

   // Live-update when the user changes their reflectivity palette in
   // Settings (mirrors how map_widget.cpp subscribes for RadarProductLayer).
   // Deferred to Render() for the same reason as the reload timer below --
   // this signal can fire at an arbitrary time, not necessarily with this
   // layer's GL context current.
   p->paletteChangedConnection_ = settings::PaletteSettings::Instance()
                                     .palette(kPaletteKey_)
                                     .changed_signal()
                                     .connect(
                                        [this](auto&&...)
                                        {
                                           p->paletteNeedsRebuild_ = true;
                                           Q_EMIT NeedsRendering();
                                        });

   // Primary reload path: GribManager may finish a fetch on its own
   // background thread, so this connection can fire from a thread other
   // than this one -- Qt's queued cross-thread delivery makes that safe,
   // and the slot only ever sets a dirty flag (never touches GL directly),
   // same pattern as paletteChangedConnection_ above.
   QObject::disconnect(p->frameReadyConnection_); // guard against a second
                                                  // Initialize() leaving
                                                  // two live connections
   p->frameReadyConnection_ = connect(p->gribManager_.get(),
                                      &manager::GribManager::FrameReady,
                                      this,
                                      [this](std::size_t productIndex)
                                      {
                                         const auto frame =
                                            p->frames_.find(productIndex);
                                         if (frame != p->frames_.end())
                                         {
                                            frame->second.needsReload = true;
                                            Q_EMIT NeedsRendering();
                                         }
                                      });

   // A product being checked/unchecked changes what this layer draws
   // without any FrameReady -- re-sync now rather than leaving an
   // unchecked product's frame on the map.
   QObject::disconnect(p->activeProductsConnection_);
   p->activeProductsConnection_ =
      connect(p->gribManager_.get(),
              &manager::GribManager::ActiveProductsChanged,
              this,
              [this]()
              {
                 p->productsChanged_ = true;
                 Q_EMIT NeedsRendering();
              });

   // Fallback safety net only -- see reloadTimer_'s comment. Kept slow
   // since FrameReady is the responsive path.
   constexpr int kReloadCheckIntervalMs = 15000;
   delete p->reloadTimer_; // guard against a second Initialize() without an
                           // intervening Deinitialize() leaking a timer
   p->reloadTimer_ = new QTimer(this);
   connect(p->reloadTimer_,
           &QTimer::timeout,
           this,
           [this]()
           {
              bool anyChanged = false;
              for (auto& [index, frame] : p->frames_)
              {
                 std::error_code ec;
                 const auto      writeTime = std::filesystem::last_write_time(
                    GetGribFramePath(p->category_, index), ec);
                 if (!ec && writeTime != frame.lastWriteTime)
                 {
                    frame.needsReload = true;
                    anyChanged        = true;
                 }
              }
              if (anyChanged)
              {
                 Q_EMIT NeedsRendering();
              }
           });
   p->reloadTimer_->start(kReloadCheckIntervalMs);

   SCWX_GL_CHECK_ERROR();
}

void GribProductLayer::Impl::BuildPalette()
{
   // Same palette the user has selected for NEXRAD base reflectivity
   // (Settings > Palettes > Color Tables), not a hardcoded ramp -- see
   // PaletteSettings::palette("BR"). Fallback chain mirrors
   // MapWidgetImpl::UpdateColorTable exactly (map_widget.cpp): configured
   // file, then the key's built-in default, and ColorTable::IsValid() is
   // checked the same way.
   auto& paletteSetting =
      settings::PaletteSettings::Instance().palette(kPaletteKey_);

   std::string colorTableFile = paletteSetting.GetValue();
   if (colorTableFile.empty())
   {
      colorTableFile = paletteSetting.GetDefault();
   }

   std::unique_ptr<std::istream> colorTableStream =
      util::OpenFile(colorTableFile);
   if (colorTableStream->fail())
   {
      logger_->warn("Could not open color table {}", colorTableFile);
      colorTableStream = util::OpenFile(paletteSetting.GetDefault());
   }

   std::shared_ptr<common::ColorTable> colorTable =
      common::ColorTable::Load(*colorTableStream);
   if (!colorTable->IsValid())
   {
      logger_->warn("Could not load color table {}", colorTableFile);
      colorTableStream = util::OpenFile(paletteSetting.GetDefault());
      colorTable       = common::ColorTable::Load(*colorTableStream);
   }

   // Deliberately NOT any product's colorOffset/colorScale here -- those
   // are the *product's* physical range (e.g. 260-325 Kelvin for RTMA
   // temperature), used by the shader to normalize a raw data value into a
   // 0..1 LUT index (see uDataMomentOffset/Scale in DrawFrame()). This loop
   // is a completely different concern: sampling the *color table's own*
   // native gradient to build the 256-entry LUT texture in the first
   // place, which must always use the table's own domain -- DR.pal's
   // breakpoints are dBZ values roughly -20 to 75, regardless of what
   // product is being displayed. Confirmed as a real, live bug: feeding
   // RTMA's Kelvin range in here instead put every LUT entry past DR.pal's
   // highest breakpoint, so ColorTable::Color() clamped all 256 entries to
   // the same final color -- a solid, uninformative fill. That's also why
   // one palette texture serves every product this layer draws.
   std::array<boost::gil::rgba8_pixel_t, 256> palette {};
   for (int i = 0; i < 256; ++i)
   {
      const float t = static_cast<float>(i) / 255.0f;
      const float value =
         kDefaultDataMomentOffset_ + t * kDefaultDataMomentScale_;
      palette[i] = colorTable->Color(value);
   }

   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_1D, paletteTexture_);
   glTexImage1D(GL_TEXTURE_1D,
                0,
                GL_RGBA,
                static_cast<GLsizei>(palette.size()),
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                palette.data());
   glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
}

void GribProductLayer::Impl::SyncProducts()
{
   productsChanged_ = false;

   const std::vector<std::size_t> active = gribManager_->ActiveProductIndices();
   const std::set<std::size_t>    activeSet(active.cbegin(), active.cend());

   for (auto it = frames_.begin(); it != frames_.end();)
   {
      if (!activeSet.contains(it->first))
      {
         DeleteFrameGl(it->second);
         it = frames_.erase(it);
      }
      else
      {
         ++it;
      }
   }

   for (const std::size_t index : active)
   {
      auto [entry, inserted] = frames_.try_emplace(index);
      if (inserted)
      {
         ProductFrame& frame = entry->second;
         glGenVertexArrays(1, &frame.vao);
         glGenBuffers(1, &frame.vbo);
         glGenTextures(1, &frame.dataTexture);
         frame.needsReload = true;
      }
   }
}

void GribProductLayer::Impl::LoadFrame(std::size_t   productIndex,
                                       ProductFrame& frame)
{
   frame.needsReload = false;

   const std::string framePath = GetGribFramePath(category_, productIndex);

   std::ifstream in(framePath, std::ios::binary);
   if (!in)
   {
      // No frame for this product yet (just checked, or its frame was
      // dropped on deactivation -- see GribManager::SetProductActive).
      // Nothing to draw until FrameReady says it's arrived.
      logger_->debug("No frame file yet: {}", framePath);
      ClearFrame(frame);
      return;
   }

   {
      std::error_code ec;
      frame.lastWriteTime = std::filesystem::last_write_time(framePath, ec);
      // On failure, worst case the next reload-timer tick re-reads a frame
      // that hasn't actually changed.
   }

   std::string header;
   std::getline(in, header);

   try
   {
      frame.nx   = static_cast<long>(ExtractNumber(header, "nx"));
      frame.ny   = static_cast<long>(ExtractNumber(header, "ny"));
      frame.lat1 = ExtractNumber(header, "lat1");
      frame.lon1 = ExtractNumber(header, "lon1");
      frame.di   = ExtractNumber(header, "di");
      frame.dj   = ExtractNumber(header, "dj");
      frame.gridType =
         ExtractStringOr(header, "gridType", "regular_ll") == "lambert" ?
            GridType::Lambert :
            GridType::RegularLatLon;
      frame.lov    = ExtractNumberOr(header, "lov", 0.0);
      frame.lad    = ExtractNumberOr(header, "lad", 0.0);
      frame.latin1 = ExtractNumberOr(header, "latin1", 0.0);
      frame.latin2 = ExtractNumberOr(header, "latin2", 0.0);
      frame.dx     = ExtractNumberOr(header, "dx", 0.0);
      frame.dy     = ExtractNumberOr(header, "dy", 0.0);
      frame.radius = ExtractNumberOr(header, "radius", 0.0);
      // GribManager's curated display name rather than the header's own
      // "product" field: decode_grib's label is a clean product+level
      // string for MRMS, but just the bare GRIB shortName for RTMA (e.g.
      // "2t") -- GribManager knows the name shown in the dropdown.
      frame.productLabel = gribManager_->ProductName(productIndex);
      frame.validTime    = ExtractString(header, "validTime");
      frame.colorOffset  = static_cast<float>(
         ExtractNumberOr(header, "colorOffset", kDefaultDataMomentOffset_));
      frame.colorScale = static_cast<float>(
         ExtractNumberOr(header, "colorScale", kDefaultDataMomentScale_));
      frame.noDataThreshold = static_cast<float>(
         ExtractNumberOr(header, "noDataThreshold", kDefaultNoDataThreshold_));
      frame.contourInterval =
         static_cast<float>(ExtractNumberOr(header, "contourInterval", 0.0));
      const auto byteLength =
         static_cast<size_t>(ExtractNumber(header, "byteLength"));
      const double missingValue = ExtractNumberOr(
         header, "missingValue", std::numeric_limits<double>::quiet_NaN());

      // Only held for the GL upload below -- see framePath's comment.
      // make_unique_for_overwrite skips zero-filling a buffer that's about
      // to be overwritten anyway.
      const std::size_t valueCount = byteLength / sizeof(float);
      auto values = std::make_unique_for_overwrite<float[]>(valueCount);
      in.read(reinterpret_cast<char*>(values.get()),
              static_cast<std::streamsize>(byteLength));

      if (!in || valueCount != static_cast<size_t>(frame.nx * frame.ny))
      {
         logger_->warn("Frame payload size mismatch, expected {} got {}",
                       frame.nx * frame.ny,
                       valueCount);
         // nx/ny no longer describe the texture still on the GPU, so the
         // hover tooltip would read the wrong cells -- drop both.
         ClearFrame(frame);
         return;
      }

      // 16-bit when this frame's values allow it without a visible change
      // (see FitsHalfFloatTexture), halving the texture's GPU memory; GL
      // converts the 32-bit upload itself. The tooltip is unaffected --
      // it reads full-precision values back from the frame file.
      const bool halfFloat = FitsHalfFloatTexture(values.get(),
                                                  valueCount,
                                                  frame.noDataThreshold,
                                                  frame.colorScale,
                                                  frame.contourInterval,
                                                  missingValue);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, frame.dataTexture);
      glTexImage2D(GL_TEXTURE_2D,
                   0,
                   halfFloat ? GL_R16F : GL_R32F,
                   static_cast<GLsizei>(frame.nx),
                   static_cast<GLsizei>(frame.ny),
                   0,
                   GL_RED,
                   GL_FLOAT,
                   values.get());
      values.reset();

      frame.framePath   = framePath;
      frame.frameHeader = header;

      // NEAREST for every fill-mode product -- load-bearing, not just a
      // style choice: MRMS's -999 "no coverage" sentinel (and any other
      // product's own noDataThreshold cutoff) must never blend into a
      // real neighboring value at a texel boundary. Contour mode is the
      // one exception: grib.frag's isoline math needs dFdx/dFdy of the
      // sampled value to reflect the real data gradient, and NEAREST
      // filtering makes that spike at every texel edge instead (the
      // sampled value is a step function, not a smooth one) -- LINEAR
      // is safe here specifically because no contour-mode product uses a
      // sentinel value the way MRMS does.
      const GLint filter =
         (frame.contourInterval > 0.0f) ? GL_LINEAR : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      std::vector<float> vertices;

      auto pushVertex =
         [&vertices](double lat, double lon, float texU, float texV)
      {
         vertices.push_back(static_cast<float>(lat));
         vertices.push_back(static_cast<float>(lon));
         vertices.push_back(texU);
         vertices.push_back(texV);
      };

      if (frame.gridType == GridType::RegularLatLon)
      {
         // Build the bounding quad. lat1/lon1 is the north-west corner;
         // grid scans east (+di) and south (-dj), matching MRMS/most
         // GRIB2 default scanning order (see grib-helper/README.md).
         const double west = frame.lon1;
         const double east =
            frame.lon1 + frame.di * static_cast<double>(frame.nx);
         const double north = frame.lat1;
         const double south =
            frame.lat1 - frame.dj * static_cast<double>(frame.ny);

         // A single quad isn't enough: GL interpolates texCoord linearly
         // in screen space, but screen space here is Web Mercator, which
         // is nonlinear in latitude (increasingly stretched toward the
         // poles), while the texture data is a plain linear lat/lon grid.
         // Across a quad spanning CONUS's full ~35 degrees of latitude
         // that mismatch is large -- confirmed empirically at ~2 degrees
         // around 42N, right where Mercator's curvature deviates most
         // from a straight line between the north/south corners.
         // RadarProductLayer never hits this because each of its
         // triangles covers a tiny sliver of the sweep, far too small for
         // the curvature to matter.
         //
         // Fix: subdivide along latitude only (longitude doesn't need it
         // -- Mercator-X is linear in longitude) into enough thin strips
         // that linear interpolation is accurate within each one.
         // Longitude stays 2 columns (west, east); this is a vertical
         // resolution knob, not a full per-data-point mesh like radar's.
         constexpr int kLatSubdivisions = 128;

         vertices.reserve(static_cast<size_t>(kLatSubdivisions + 1) * 2 * 4);

         for (int i = 0; i <= kLatSubdivisions; ++i)
         {
            const double f    = static_cast<double>(i) / kLatSubdivisions;
            const double lat  = north - f * (north - south);
            const auto   texV = static_cast<float>(f);

            // West then east column, in this order, so GL_TRIANGLE_STRIP
            // connects consecutive rows into the correct quads.
            pushVertex(lat, west, 0.0f, texV);
            pushVertex(lat, east, 1.0f, texV);
         }

         frame.numVertices = static_cast<GLsizei>((kLatSubdivisions + 1) * 2);
      }
      else // GridType::Lambert
      {
         // Unlike regular_ll, a row or column of constant grid index here
         // is a curve in lat/lon space, not a straight line -- see
         // GridType's comment above. So both axes need subdividing, each
         // vertex's true position computed via LambertGridToLatLon rather
         // than linear degree interpolation. Resolution chosen to match
         // regular_ll's kLatSubdivisions in order of magnitude; mesh is
         // rebuilt only on frame load, so its cost is a non-issue.
         constexpr int kMeshCols = 96;
         constexpr int kMeshRows = 96;

         const LambertGrid lambertGrid {frame.lov,
                                        frame.lad,
                                        frame.latin1,
                                        frame.latin2,
                                        frame.lat1,
                                        frame.lon1,
                                        frame.dx,
                                        frame.dy,
                                        frame.radius};

         vertices.reserve(
            static_cast<size_t>((kMeshRows + 1) * (kMeshCols + 1) +
                                kMeshRows * 2) *
            4);

         auto vertexAt = [&](int row, int col) -> glm::dvec2
         {
            const double iFrac = static_cast<double>(col) / kMeshCols;
            const double jFrac = static_cast<double>(row) / kMeshRows;
            const double i     = iFrac * static_cast<double>(frame.nx - 1);
            const double j     = jFrac * static_cast<double>(frame.ny - 1);
            return LambertGridToLatLon(lambertGrid, i, j);
         };

         for (int row = 0; row < kMeshRows; ++row)
         {
            const auto texV0 = static_cast<float>(row) / kMeshRows;
            const auto texV1 = static_cast<float>(row + 1) / kMeshRows;

            for (int col = 0; col <= kMeshCols; ++col)
            {
               const auto texU    = static_cast<float>(col) / kMeshCols;
               glm::dvec2 latLon0 = vertexAt(row, col);
               glm::dvec2 latLon1 = vertexAt(row + 1, col);

               pushVertex(latLon0.x, latLon0.y, texU, texV0);
               pushVertex(latLon1.x, latLon1.y, texU, texV1);
            }

            // Degenerate triangles bridging to the next row-band (repeat
            // this band's last vertex, then the next band's first
            // vertex) so the whole mesh still draws with a single
            // GL_TRIANGLE_STRIP call -- same technique the regular_ll
            // path gets for free by only ever having one row-band.
            if (row + 1 < kMeshRows)
            {
               const glm::dvec2 lastOfBand = vertexAt(row + 1, kMeshCols);
               pushVertex(lastOfBand.x, lastOfBand.y, 1.0f, texV1);

               const glm::dvec2 firstOfNext = vertexAt(row + 1, 0);
               pushVertex(firstOfNext.x, firstOfNext.y, 0.0f, texV1);
            }
         }

         frame.numVertices = static_cast<GLsizei>(vertices.size() / 4);
      }

      glBindVertexArray(frame.vao);
      glBindBuffer(GL_ARRAY_BUFFER, frame.vbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                   vertices.data(),
                   GL_STATIC_DRAW);

      constexpr GLsizei stride = 4 * sizeof(float);
      glVertexAttribPointer(
         0, 2, GL_FLOAT, GL_FALSE, stride, static_cast<void*>(0));
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(1,
                            2,
                            GL_FLOAT,
                            GL_FALSE,
                            stride,
                            reinterpret_cast<void*>(2 * sizeof(float)));
      glEnableVertexAttribArray(1);

      frame.loaded = true;

      logger_->info(
         "Loaded GRIB frame: {} valid {} ({} x {} grid, origin "
         "({}, {}), {}-bit texture, {:.1f} MB)",
         frame.productLabel,
         frame.validTime,
         frame.nx,
         frame.ny,
         frame.lat1,
         frame.lon1,
         halfFloat ? 16 : 32,
         static_cast<double>(valueCount) * (halfFloat ? 2 : 4) /
            (1024.0 * 1024.0));
   }
   catch (const std::exception& e)
   {
      logger_->warn("Failed to parse frame header: {}", e.what());
   }
}

void GribProductLayer::Impl::ClearFrame(ProductFrame& frame)
{
   frame.loaded = false;
   frame.framePath.clear();
   frame.frameHeader.clear();
   frame.nx = 0;
   frame.ny = 0;
   frame.productLabel.clear();
   frame.validTime.clear();

   // Release the GPU copy (only ever called with this layer's GL context
   // current: from LoadFrame(), itself only called from Initialize()/
   // Render()).
   if (frame.dataTexture != GL_INVALID_INDEX)
   {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, frame.dataTexture);
      glTexImage2D(
         GL_TEXTURE_2D, 0, GL_R32F, 0, 0, 0, GL_RED, GL_FLOAT, nullptr);
   }
}

void GribProductLayer::Impl::DeleteFrameGl(ProductFrame& frame)
{
   glDeleteVertexArrays(1, &frame.vao);
   glDeleteBuffers(1, &frame.vbo);
   glDeleteTextures(1, &frame.dataTexture);

   frame.vao         = GL_INVALID_INDEX;
   frame.vbo         = GL_INVALID_INDEX;
   frame.dataTexture = GL_INVALID_INDEX;
   frame.numVertices = 0;
   frame.loaded      = false;
}

void GribProductLayer::Impl::DrawFrame(const ProductFrame& frame) const
{
   glUniform1f(uDataMomentOffsetLocation_, frame.colorOffset);
   glUniform1f(uDataMomentScaleLocation_, frame.colorScale);
   glUniform1f(uNoDataThresholdLocation_, frame.noDataThreshold);
   glUniform1f(uContourIntervalLocation_, frame.contourInterval);

   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, frame.dataTexture);

   glBindVertexArray(frame.vao);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, frame.numVertices);
}

void GribProductLayer::Render(
   const std::shared_ptr<MapContext>& /* mapContext */,
   const QMapLibre::CustomLayerRenderParameters& params)
{
   if (p->paletteNeedsRebuild_)
   {
      p->paletteNeedsRebuild_ = false;
      p->BuildPalette();
   }

   if (p->productsChanged_)
   {
      p->SyncProducts();
   }

   bool anyLoaded = false;
   for (auto& [index, frame] : p->frames_)
   {
      if (frame.needsReload)
      {
         p->LoadFrame(index, frame);
      }
      anyLoaded |= frame.loaded;
   }

   if (!anyLoaded)
   {
      return;
   }

   p->shaderProgram_->Use();

   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   const double scale  = std::pow(2.0, params.zoom) * 2.0 *
                         mbgl::util::tileSize_D / mbgl::util::DEGREES_MAX;
   const auto   xScale = static_cast<float>(scale / params.width);
   const auto   yScale = static_cast<float>(scale / params.height);

   glm::mat4 uMVPMatrix(1.0f);
   uMVPMatrix = glm::scale(uMVPMatrix, glm::vec3(xScale, yScale, 1.0f));
   uMVPMatrix = glm::rotate(uMVPMatrix,
                            glm::radians(static_cast<float>(params.bearing)),
                            glm::vec3(0.0f, 0.0f, 1.0f));

   glUniform2fv(p->uOriginLatLongLocation_,
                1,
                glm::value_ptr(glm::vec2 {params.latitude, params.longitude}));
   glUniformMatrix4fv(
      p->uMVPMatrixLocation_, 1, GL_FALSE, glm::value_ptr(uMVPMatrix));

   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_1D, p->paletteTexture_);

   // Every checked product: filled fields first, then contour products on
   // top, so isolines stay visible over a filled field (e.g. MSLP contours
   // over 2m temperature). Within each pass, in product order -- a later
   // fill covers an earlier one where both have data.
   for (const bool contourPass : {false, true})
   {
      for (const auto& [index, frame] : p->frames_)
      {
         if (frame.loaded && (frame.contourInterval > 0.0f) == contourPass)
         {
            p->DrawFrame(frame);
         }
      }
   }

   SCWX_GL_CHECK_ERROR();
}

void GribProductLayer::Deinitialize()
{
   logger_->debug("Deinitialize()");

   delete p->reloadTimer_;
   p->reloadTimer_ = nullptr;

   for (auto& [index, frame] : p->frames_)
   {
      Impl::DeleteFrameGl(frame);
   }
   p->frames_.clear();

   glDeleteTextures(1, &p->paletteTexture_);
   p->paletteTexture_ = GL_INVALID_INDEX;

   QObject::disconnect(p->frameReadyConnection_);
   QObject::disconnect(p->activeProductsConnection_);
}

std::optional<float> GribProductLayer::Impl::ValueAt(const ProductFrame& frame,
                                                     double              lat,
                                                     double              lon)
{
   if (!frame.loaded || frame.framePath.empty() || frame.nx <= 0 ||
       frame.ny <= 0)
   {
      return std::nullopt;
   }

   long gi = 0; // column, west to east, matching the wire format's own
   long gj = 0; // row-major (index = row * nx + col) scan order

   if (frame.gridType == GridType::RegularLatLon)
   {
      // lat1/lon1 is the north-west corner; grid scans east (+di) and
      // south (-dj) -- see LoadFrame's mesh-building comment.
      gi = std::lround((lon - frame.lon1) / frame.di);
      gj = std::lround((frame.lat1 - lat) / frame.dj);
   }
   else // Lambert
   {
      // Exact inverse of LambertGridToLatLon: reuses the same
      // ComputeLambertConstants/LambertForward the mesh builder uses, just
      // run lat/lon -> grid index instead of grid index -> lat/lon.
      const LambertGrid      grid {frame.lov,
                                   frame.lad,
                                   frame.latin1,
                                   frame.latin2,
                                   frame.lat1,
                                   frame.lon1,
                                   frame.dx,
                                   frame.dy,
                                   frame.radius};
      const LambertConstants c = ComputeLambertConstants(grid);

      const glm::dvec2 origin = LambertForward(grid, c, frame.lat1, frame.lon1);
      const glm::dvec2 target = LambertForward(grid, c, lat, lon);

      gi = std::lround((target.x - origin.x) / frame.dx);
      gj = std::lround((target.y - origin.y) / frame.dy);
   }

   if (gi < 0 || gi >= frame.nx || gj < 0 || gj >= frame.ny)
   {
      return std::nullopt;
   }

   // Read just this one value back from the frame file -- see framePath's
   // comment. Opened per lookup rather than held open, since Windows won't
   // let GribManager rename a newer frame over an open file.
   std::ifstream in(frame.framePath, std::ios::binary);
   std::string   header;
   if (!in || !std::getline(in, header) || header != frame.frameHeader)
   {
      // Gone, or already replaced by a frame this layer hasn't loaded yet.
      return std::nullopt;
   }

   const auto index =
      static_cast<std::streamoff>(gj) * static_cast<std::streamoff>(frame.nx) +
      static_cast<std::streamoff>(gi);
   in.seekg(index * static_cast<std::streamoff>(sizeof(float)), std::ios::cur);

   float value = 0.0f;
   in.read(reinterpret_cast<char*>(&value), sizeof(value));
   if (!in)
   {
      return std::nullopt;
   }

   // Same comparison the fragment shader itself uses to discard (see
   // grib.frag) -- keeps the tooltip in agreement with what's actually
   // rendered, rather than a separate, possibly-differing notion of "no
   // data here".
   if (value < frame.noDataThreshold)
   {
      return std::nullopt;
   }

   return value;
}

std::optional<std::string> GribProductLayer::GetHoverText(
   const std::shared_ptr<MapContext>& /* mapContext */,
   const common::Coordinate& mouseGeoCoords) const
{
   // One "product\nvalue units\nValid: ..." block per checked product with
   // data under the cursor, separated by a blank line -- the same way
   // CombineAreaHoverText() separates sibling layers' blocks.
   std::string combined;

   for (const auto& [index, frame] : p->frames_)
   {
      const std::optional<float> value = Impl::ValueAt(
         frame, mouseGeoCoords.latitude_, mouseGeoCoords.longitude_);
      if (!value.has_value())
      {
         continue;
      }

      const std::string formattedValue =
         p->gribManager_ ? p->gribManager_->FormatValue(index, *value) :
                           fmt::format("{:.2f}", *value);

      if (!combined.empty())
      {
         combined += "\n\n";
      }
      combined += fmt::format("{}\n{}\nValid: {}",
                              frame.productLabel,
                              formattedValue,
                              frame.validTime);
   }

   if (combined.empty())
   {
      return std::nullopt;
   }
   return combined;
}

bool GribProductLayer::RunMousePicking(
   const std::shared_ptr<MapContext>& mapContext,
   const QMapLibre::CustomLayerRenderParameters& /* params */,
   const QPointF& /* mouseLocalPos */,
   const QPointF& mouseGlobalPos,
   const glm::vec2& /* mouseCoords */,
   const common::Coordinate& mouseGeoCoords,
   std::shared_ptr<types::EventHandler>& /* eventHandler */)
{
   // Shift-gated, matching the app-wide convention established by
   // RadarProductLayer/RadarSiteLayer: that layer shows nothing at all
   // without Shift, and shows the actual data value under the cursor (plus
   // a distance-from-radar-site measurement, which has no equivalent here)
   // when it's held. Previously this layer showed an unconditional
   // product/time label instead of gating on Shift at all -- that
   // undersold what Shift actually means elsewhere in the app: "show me
   // the data", not just "a modifier radar happens to use".
   if (!(QGuiApplication::keyboardModifiers() &
         Qt::KeyboardModifier::ShiftModifier))
   {
      return false;
   }

   // Combines this layer's own GetHoverText() with every "area" layer
   // wired to it via AddAreaSibling() (see MapWidgetImpl::AddLayer) --
   // e.g. MRMS reflectivity, Models temperature, and radar's own sweep
   // value all in one tooltip. Scoped to just these layers peeking at
   // each other directly; the app's general mouse-picking dispatch
   // (MapWidgetImpl::RunMousePicking, used by polygons/markers/alerts
   // too) still stops at the first hit -- not touched here.
   std::optional<std::string> hoverText =
      CombineAreaHoverText(mapContext, mouseGeoCoords);
   if (!hoverText.has_value())
   {
      return false;
   }

   util::tooltip::Show(*hoverText, mouseGlobalPos);
   return true;
}

} // namespace scwx::qt::map
