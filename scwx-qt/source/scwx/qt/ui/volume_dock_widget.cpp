#include "volume_dock_widget.hpp"
#include "ui_volume_dock_widget.h"

#include <scwx/qt/config/radar_site.hpp>
#include <scwx/qt/manager/radar_product_manager.hpp>
#include <scwx/qt/settings/palette_settings.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/qt/volume/radar_volume.hpp>
#include <scwx/common/color_table.hpp>
#include <scwx/util/logger.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <QDateTime>
#include <QLocale>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QTimer>

namespace scwx::qt::ui
{

static const std::string logPrefix_ = "scwx::qt::ui::volume_dock_widget";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

static constexpr int    kFetchDelayMs_      = 250;
static constexpr int    kMeshDelayMs_       = 60;
static constexpr int    kWholeRadarHalfKm_  = 460;
static constexpr double kKmPerFoot_         = 0.0003048;
static constexpr int    kProductFollowsMap_ = 0;
static constexpr int    kOpacityPercent_    = 100;

// The product combo box after "Map product", in order.
static constexpr std::array<common::Level2Product, 6> kComboProducts_ {
   common::Level2Product::Reflectivity,
   common::Level2Product::Velocity,
   common::Level2Product::SpectrumWidth,
   common::Level2Product::DifferentialReflectivity,
   common::Level2Product::DifferentialPhase,
   common::Level2Product::CorrelationCoefficient};

struct ProductInfo
{
   wsr88d::rda::DataBlockType blockType;
   const char*                units;
   bool                       signedValues;
   bool                       thresholdOn;
   double                     threshold;
   int                        decimals;
};

static ProductInfo GetProductInfo(common::Level2Product product)
{
   using wsr88d::rda::DataBlockType;
   switch (product)
   {
   case common::Level2Product::Velocity:
      return {DataBlockType::MomentVel, " m/s", true, true, 15.0, 1};
   case common::Level2Product::SpectrumWidth:
      return {DataBlockType::MomentSw, " m/s", false, false, 4.0, 1};
   case common::Level2Product::DifferentialReflectivity:
      return {DataBlockType::MomentZdr, " dB", false, false, 2.0, 1};
   case common::Level2Product::DifferentialPhase:
      return {DataBlockType::MomentPhi, "\302\260", false, false, 90.0, 0};
   case common::Level2Product::CorrelationCoefficient:
      return {DataBlockType::MomentRho, "", false, false, 0.8, 2};
   case common::Level2Product::ClutterFilterPowerRemoved:
      return {DataBlockType::MomentCfp, " dB", false, false, 0.0, 1};
   case common::Level2Product::Reflectivity:
   default:
      return {DataBlockType::MomentRef, " dBZ", false, true, 20.0, 1};
   }
}

class VolumeDockWidget::Impl
{
public:
   explicit Impl(VolumeDockWidget* self) : self_ {self} {}
   ~Impl()
   {
      // Jobs post their results back to self_, which outlives them only if
      // they are finished here.
      pool_.join();
   }
   Impl(const Impl&)            = delete;
   Impl& operator=(const Impl&) = delete;
   Impl(Impl&&)                 = delete;
   Impl& operator=(Impl&&)      = delete;

   void ConnectControls();
   void ApplyProductDefaults();
   void ScheduleFetch();
   void ScheduleMesh();
   void StartFetch();
   void StartMesh();
   void Release();
   void UpdateTiltSlider();
   void UpdateCaption();

   [[nodiscard]] common::Level2Product ShownProduct() const;
   [[nodiscard]] volume::VolumeRegion  Region() const;
   [[nodiscard]] std::shared_ptr<common::ColorTable>
   ColorTableFor(common::Level2Product product);

   VolumeDockWidget*     self_;
   Ui::VolumeDockWidget* ui_ {nullptr};

   Source                               source_ {};
   std::optional<std::array<double, 2>> centerLatLon_ {};

   boost::asio::thread_pool pool_ {1};
   std::uint64_t            fetchGeneration_ {0};
   std::uint64_t            meshGeneration_ {0};
   QTimer                   fetchTimer_ {};
   QTimer                   meshTimer_ {};
   bool                     dirty_ {true};

   std::shared_ptr<const volume::RadarVolume> volume_ {};
   common::Level2Product                      volumeProduct_ {};
   bool                                       tiltSliderAtTop_ {true};

   std::string                         colorTablePalette_ {};
   std::shared_ptr<common::ColorTable> colorTable_ {};
};

VolumeDockWidget::VolumeDockWidget(QWidget* parent) :
    QDockWidget(parent),
    p {std::make_unique<Impl>(this)},
    ui(new Ui::VolumeDockWidget)
{
   ui->setupUi(this);
   p->ui_ = ui;

   p->fetchTimer_.setSingleShot(true);
   p->fetchTimer_.setInterval(kFetchDelayMs_);
   p->meshTimer_.setSingleShot(true);
   p->meshTimer_.setInterval(kMeshDelayMs_);

   p->ConnectControls();
   p->ApplyProductDefaults();
   ui->volumeView->SetVerticalScale(
      static_cast<float>(ui->verticalScaleSpinBox->value()));
   ui->volumeView->SetOpacity(static_cast<float>(ui->opacitySlider->value()) /
                              kOpacityPercent_);
}

VolumeDockWidget::~VolumeDockWidget()
{
   // Finish the worker before the widgets its results are posted to go.
   p.reset();
   delete ui;
}

void VolumeDockWidget::Impl::ConnectControls()
{
   QObject::connect(
      &fetchTimer_, &QTimer::timeout, self_, [this]() { StartFetch(); });
   QObject::connect(
      &meshTimer_, &QTimer::timeout, self_, [this]() { StartMesh(); });

   QObject::connect(ui_->productComboBox,
                    &QComboBox::currentIndexChanged,
                    self_,
                    [this]()
                    {
                       ApplyProductDefaults();
                       ScheduleFetch();
                    });
   QObject::connect(ui_->thresholdCheckBox,
                    &QCheckBox::toggled,
                    self_,
                    [this](bool checked)
                    {
                       ui_->thresholdSpinBox->setEnabled(checked);
                       ScheduleMesh();
                    });
   QObject::connect(ui_->thresholdSpinBox,
                    &QDoubleSpinBox::valueChanged,
                    self_,
                    [this]() { ScheduleMesh(); });
   QObject::connect(ui_->sizeSpinBox,
                    &QSpinBox::valueChanged,
                    self_,
                    [this]() { ScheduleFetch(); });
   QObject::connect(ui_->wholeRadarButton,
                    &QPushButton::clicked,
                    self_,
                    [this]()
                    {
                       centerLatLon_.reset();
                       if (ui_->sizeSpinBox->value() == kWholeRadarHalfKm_)
                       {
                          ScheduleFetch();
                       }
                       ui_->sizeSpinBox->setValue(kWholeRadarHalfKm_);
                    });
   QObject::connect(
      ui_->verticalScaleSpinBox,
      &QDoubleSpinBox::valueChanged,
      self_,
      [this](double value)
      { ui_->volumeView->SetVerticalScale(static_cast<float>(value)); });
   QObject::connect(ui_->opacitySlider,
                    &QSlider::valueChanged,
                    self_,
                    [this](int value) {
                       ui_->volumeView->SetOpacity(static_cast<float>(value) /
                                                   kOpacityPercent_);
                    });
   QObject::connect(ui_->tiltSlider,
                    &QSlider::valueChanged,
                    self_,
                    [this](int value)
                    {
                       tiltSliderAtTop_ = value == ui_->tiltSlider->maximum();
                       ui_->volumeView->SetVisibleTiltCount(
                          static_cast<std::size_t>(value));
                       UpdateTiltSlider();
                    });
   QObject::connect(ui_->volumeView,
                    &VolumeViewWidget::ContextLost,
                    self_,
                    [this]() { ScheduleMesh(); });
}

common::Level2Product VolumeDockWidget::Impl::ShownProduct() const
{
   const int index = ui_->productComboBox->currentIndex();
   if (index > kProductFollowsMap_ &&
       static_cast<std::size_t>(index) <= kComboProducts_.size())
   {
      return kComboProducts_.at(static_cast<std::size_t>(index - 1));
   }
   if (source_.group == common::RadarProductGroup::Level2)
   {
      const common::Level2Product product =
         common::GetLevel2Product(source_.product);
      if (product != common::Level2Product::Unknown)
      {
         return product;
      }
   }
   return common::Level2Product::Reflectivity;
}

void VolumeDockWidget::Impl::ApplyProductDefaults()
{
   const ProductInfo    info = GetProductInfo(ShownProduct());
   const QSignalBlocker blockCheck {ui_->thresholdCheckBox};
   const QSignalBlocker blockSpin {ui_->thresholdSpinBox};
   ui_->thresholdSpinBox->setDecimals(info.decimals);
   ui_->thresholdSpinBox->setSuffix(QString::fromUtf8(info.units));
   ui_->thresholdSpinBox->setValue(info.threshold);
   ui_->thresholdCheckBox->setChecked(info.thresholdOn);
   ui_->thresholdSpinBox->setEnabled(info.thresholdOn);
   ui_->thresholdCheckBox->setText(info.signedValues ?
                                      VolumeDockWidget::tr("Hide |v| below") :
                                      VolumeDockWidget::tr("Hide below"));
}

volume::VolumeRegion VolumeDockWidget::Impl::Region() const
{
   volume::VolumeRegion region {
      .centerXKm  = 0.0,
      .centerYKm  = 0.0,
      .halfSizeKm = static_cast<double>(ui_->sizeSpinBox->value())};
   if (centerLatLon_.has_value() && source_.radarSite != nullptr)
   {
      const auto offset = volume::LocalOffsetKm(source_.radarSite->latitude(),
                                                source_.radarSite->longitude(),
                                                centerLatLon_->at(0),
                                                centerLatLon_->at(1));
      region.centerXKm  = offset[0];
      region.centerYKm  = offset[1];
   }
   return region;
}

std::shared_ptr<common::ColorTable>
VolumeDockWidget::Impl::ColorTableFor(common::Level2Product product)
{
   const std::string& palette = common::GetLevel2Palette(product);
   if (palette == colorTablePalette_ && colorTable_ != nullptr)
   {
      return colorTable_;
   }

   // As MapWidgetImpl::UpdateColorTable: the user's table, else the default.
   auto&       setting = settings::PaletteSettings::Instance().palette(palette);
   std::string file    = setting.GetValue();
   if (file.empty())
   {
      file = setting.GetDefault();
   }
   std::unique_ptr<std::istream> stream = util::OpenFile(file);
   if (stream->fail())
   {
      stream = util::OpenFile(setting.GetDefault());
   }
   std::shared_ptr<common::ColorTable> table =
      common::ColorTable::Load(*stream);
   if (!table->IsValid())
   {
      stream = util::OpenFile(setting.GetDefault());
      table  = common::ColorTable::Load(*stream);
   }

   colorTablePalette_ = palette;
   colorTable_        = table;
   return colorTable_;
}

void VolumeDockWidget::Impl::ScheduleFetch()
{
   dirty_ = true;
   if (self_->isVisible())
   {
      fetchTimer_.start();
   }
}

void VolumeDockWidget::Impl::ScheduleMesh()
{
   if (self_->isVisible() && volume_ != nullptr)
   {
      meshTimer_.start();
   }
}

void VolumeDockWidget::Impl::StartFetch()
{
   if (source_.radarSite == nullptr)
   {
      ui_->statusLabel->setText(
         VolumeDockWidget::tr("Waiting for the map to show a radar."));
      return;
   }
   dirty_ = false;

   const std::uint64_t         generation = ++fetchGeneration_;
   const common::Level2Product product    = ShownProduct();
   const ProductInfo           info       = GetProductInfo(product);
   const volume::VolumeRegion  region     = Region();
   const std::string           siteId     = source_.radarSite->id();
   const double                siteLat    = source_.radarSite->latitude();
   const double                siteLon    = source_.radarSite->longitude();
   const double                siteHeightKm =
      source_.radarSite->altitude().value() * kKmPerFoot_;
   const auto time = source_.time;

   ui_->statusLabel->setText(
      VolumeDockWidget::tr("Loading %1 %2...")
         .arg(QString::fromStdString(siteId))
         .arg(QString::fromStdString(common::GetLevel2Description(product))));

   boost::asio::post(
      pool_,
      [=, this]()
      {
         auto volume           = std::make_shared<volume::RadarVolume>();
         volume->siteId        = siteId;
         volume->product       = common::GetLevel2Description(product);
         volume->siteLatitude  = siteLat;
         volume->siteLongitude = siteLon;
         volume->siteHeightKm  = siteHeightKm;
         volume->region        = region;

         try
         {
            const auto manager = manager::RadarProductManager::Instance(siteId);
            auto [scan0, cut0, cuts, time0, status0] =
               manager->GetLevel2Data(info.blockType, 0.0f, time);
            if (scan0 != nullptr)
            {
               volume->time = time0;
               if (const auto first = scan0->cbegin();
                   first != scan0->cend() && first->second != nullptr)
               {
                  volume->vcp = first->second->volume_coverage_pattern_number();
               }
            }

            const volume::CellSize cellSize =
               volume::ChooseCellSize(region.halfSizeKm);
            for (const float elevation : volume::DistinctElevations(cuts))
            {
               auto [scan, cut, unused, scanTime, status] =
                  manager->GetLevel2Data(info.blockType, elevation, time);
               if (scan == nullptr)
               {
                  continue;
               }
               volume->tilts.push_back(volume::ExtractTilt(*scan,
                                                           info.blockType,
                                                           cut,
                                                           region,
                                                           cellSize,
                                                           info.signedValues));
            }
         }
         catch (const std::exception& ex)
         {
            logger_->warn(
               "Could not read the {} volume: {}", siteId, ex.what());
         }

         QMetaObject::invokeMethod(
            self_,
            [this, generation, product, volume]()
            {
               if (generation != fetchGeneration_)
               {
                  return;
               }
               volume_        = volume;
               volumeProduct_ = product;
               UpdateTiltSlider();
               StartMesh();
            },
            Qt::QueuedConnection);
      });
}

void VolumeDockWidget::Impl::StartMesh()
{
   if (volume_ == nullptr)
   {
      return;
   }

   const std::uint64_t generation = ++meshGeneration_;
   const ProductInfo   info       = GetProductInfo(volumeProduct_);
   const std::shared_ptr<common::ColorTable> table =
      ColorTableFor(volumeProduct_);
   const volume::MeshOptions options {
      .threshold     = ui_->thresholdCheckBox->isChecked() ?
                          std::optional<float> {
                         static_cast<float>(ui_->thresholdSpinBox->value())} :
                          std::nullopt,
      .signedProduct = info.signedValues};
   const std::shared_ptr<const volume::RadarVolume> volume = volume_;

   boost::asio::post(
      pool_,
      [=, this]()
      {
         const volume::ColorFunction color =
            [&table](float value) -> std::array<std::uint8_t, 4>
         {
            if (table == nullptr || !table->IsValid())
            {
               constexpr std::uint8_t kGray = 200;
               return {kGray, kGray, kGray, 255};
            }
            const boost::gil::rgba8_pixel_t c = table->Color(value);
            return {c[0], c[1], c[2], c[3]};
         };
         auto mesh = std::make_shared<const volume::VolumeMesh>(
            volume::BuildMesh(*volume, color, options));

         QMetaObject::invokeMethod(
            self_,
            [this, generation, mesh, volume]()
            {
               if (generation != meshGeneration_ || volume != volume_)
               {
                  return;
               }
               const std::size_t bins = mesh->indices.size() / 6;
               ui_->volumeView->SetMesh(mesh, volume->region.halfSizeKm);
               UpdateCaption();

               QString status;
               if (volume->tilts.empty())
               {
                  status =
                     VolumeDockWidget::tr(
                        "No Level 2 volume is loaded for %1 at this time.")
                        .arg(QString::fromStdString(volume->siteId));
               }
               else if (volume->CellCount() == 0)
               {
                  status = VolumeDockWidget::tr(
                     "No echoes in this area (it may be outside the radar's "
                     "range).");
               }
               else
               {
                  status = VolumeDockWidget::tr("%1 tilts, %2 bins drawn")
                              .arg(volume->tilts.size())
                              .arg(QLocale {}.toString(
                                 static_cast<qulonglong>(bins)));
               }
               ui_->statusLabel->setText(status);
            },
            Qt::QueuedConnection);
      });
}

void VolumeDockWidget::Impl::UpdateTiltSlider()
{
   const QSignalBlocker blocker {ui_->tiltSlider};
   const int            count =
      volume_ != nullptr ? static_cast<int>(volume_->tilts.size()) : 0;
   ui_->tiltSlider->setEnabled(count > 1);
   ui_->tiltSlider->setMaximum(std::max(1, count));
   if (tiltSliderAtTop_ || ui_->tiltSlider->value() > count)
   {
      ui_->tiltSlider->setValue(std::max(1, count));
   }
   ui_->volumeView->SetVisibleTiltCount(
      tiltSliderAtTop_ ? std::numeric_limits<std::size_t>::max() :
                         static_cast<std::size_t>(ui_->tiltSlider->value()));

   QString text = QStringLiteral("-");
   if (count > 0)
   {
      std::vector<float> elevations {};
      for (const auto& tilt : volume_->tilts)
      {
         elevations.push_back(tilt.elevationDeg);
      }
      std::ranges::sort(elevations);
      const auto index = static_cast<std::size_t>(
         std::clamp(ui_->tiltSlider->value(), 1, count) - 1);
      text = QString("%1\302\260").arg(elevations.at(index), 0, 'f', 1);
   }
   ui_->tiltValueLabel->setText(text);
}

void VolumeDockWidget::Impl::UpdateCaption()
{
   if (volume_ == nullptr)
   {
      ui_->volumeView->SetCaption({});
      return;
   }

   QString caption = QString("%1  %2")
                        .arg(QString::fromStdString(volume_->siteId))
                        .arg(QString::fromStdString(volume_->product));
   if (volume_->time != std::chrono::system_clock::time_point {})
   {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              volume_->time.time_since_epoch())
                              .count();
      caption += QStringLiteral("\n") +
                 QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::utc())
                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
   }
   if (volume_->vcp != 0)
   {
      caption += VolumeDockWidget::tr("  VCP %1").arg(volume_->vcp);
   }
   ui_->volumeView->SetCaption(caption);
}

void VolumeDockWidget::Impl::Release()
{
   ++fetchGeneration_;
   ++meshGeneration_;
   fetchTimer_.stop();
   meshTimer_.stop();
   volume_.reset();
   dirty_ = true;
   ui_->volumeView->ClearMesh();
}

void VolumeDockWidget::SetSource(const Source& source)
{
   const bool siteChanged =
      (source.radarSite == nullptr) != (p->source_.radarSite == nullptr) ||
      (source.radarSite != nullptr &&
       source.radarSite->id() != p->source_.radarSite->id());
   const bool same = !siteChanged && source.group == p->source_.group &&
                     source.product == p->source_.product &&
                     source.time == p->source_.time;

   const common::Level2Product before = p->ShownProduct();
   p->source_                         = source;
   if (p->ShownProduct() != before)
   {
      p->ApplyProductDefaults();
   }

   // A live map (default time) keeps getting new tilts for the same selection,
   // so it is fetched again; an archive time that has not changed is not.
   if (same && source.time != std::chrono::system_clock::time_point {} &&
       !p->dirty_)
   {
      return;
   }
   p->ScheduleFetch();
}

void VolumeDockWidget::ShowRegion(double latitude, double longitude)
{
   p->centerLatLon_ = std::array<double, 2> {latitude, longitude};
   if (ui->sizeSpinBox->value() == kWholeRadarHalfKm_)
   {
      // A picked point is about a storm, not the whole radar.
      constexpr int        kStormHalfKm = 60;
      const QSignalBlocker blocker {ui->sizeSpinBox};
      ui->sizeSpinBox->setValue(kStormHalfKm);
   }
   show();
   raise();
   p->ScheduleFetch();
}

void VolumeDockWidget::showEvent(QShowEvent* event)
{
   QDockWidget::showEvent(event);
   if (p->dirty_ || p->volume_ == nullptr)
   {
      p->dirty_ = true;
      p->fetchTimer_.start();
   }
}

void VolumeDockWidget::hideEvent(QHideEvent* event)
{
   QDockWidget::hideEvent(event);
   // Closing the pane frees its volume and GPU buffers; reopening fetches
   // again.
   if (!isVisible())
   {
      p->Release();
   }
}

} // namespace scwx::qt::ui
