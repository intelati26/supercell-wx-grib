#include <scwx/qt/util/grib_legend.hpp>
#include <scwx/qt/settings/palette_settings.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/common/color_table.hpp>
#include <scwx/util/logger.hpp>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

namespace scwx::qt::util::grib_legend
{

namespace
{

static const std::string logPrefix_ = "scwx::qt::util::grib_legend";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

// Every GRIB layer today shares this one palette regardless of product
// (see GribProductLayer::BuildPalette()'s own doc: "a real per-product
// palette is future work") -- the legend has to load the identical
// source, with the identical fallback chain, to actually match what's
// drawn on the map.
const std::string kPaletteKey_ = "BR";

// DR.pal's own native dBZ domain -- must match GribProductLayer's own
// kDefaultDataMomentOffset_/kDefaultDataMomentScale_ exactly, since this
// reproduces that class's own LUT-sampling formula (see its
// BuildPalette() doc: a raw value's fraction across the *product's* own
// colorOffset/colorScale range is used to sample a color from THIS fixed
// domain, not from the product's own range).
constexpr float kPaletteDomainOffset_ = -20.0f;
constexpr float kPaletteDomainScale_  = 95.0f;

constexpr int kMargin     = 16;
constexpr int kBarWidth   = 320;
constexpr int kBarHeight  = 18;
constexpr int kLineHeight = 18;

std::shared_ptr<common::ColorTable> LoadPalette()
{
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

   return colorTable;
}

} // namespace

void DrawLegend(QImage&                image,
               manager::GribManager&   gribManager,
               map::GribCategory       category,
               const std::string&      categoryLabel)
{
   const auto productIndex = gribManager.CurrentProductIndex();
   if (!productIndex)
   {
      // Rrfs/Nbm with nothing checked yet -- same "nothing to show" bail
      // as the no-decoded-frame case just below, just for the other way
      // this category can have nothing yet.
      logger_->info("No product active for {}, skipping legend", categoryLabel);
      return;
   }

   const std::string framePath =
      map::GetGribFramePath(category, *productIndex);

   const auto colorRange = map::ReadGribFrameColorRange(framePath);
   if (colorRange.colorScale == 0.0f)
   {
      logger_->info("No decoded frame yet for {}, skipping legend",
                    categoryLabel);
      return;
   }

   auto colorTable = LoadPalette();
   if (!colorTable->IsValid())
   {
      logger_->warn("Could not load palette, skipping legend");
      return;
   }

   const std::string validTime = map::ReadGribFrameValidTime(framePath);

   QPainter painter(&image);
   painter.setRenderHint(QPainter::Antialiasing, true);

   const int barX = kMargin;
   const int barY = image.height() - kMargin - kBarHeight;

   // Background panel so the legend stays legible over any map content.
   const QRect panelRect(barX - 8,
                         barY - 2 * kLineHeight - 12,
                         kBarWidth + 16,
                         2 * kLineHeight + kBarHeight + 20);
   painter.fillRect(panelRect, QColor(0, 0, 0, 160));

   // Gradient bar -- one column per pixel, reproducing the exact formula
   // GribProductLayer's shader uses to turn a raw value into a color (see
   // kPaletteDomainOffset_/Scale_'s own doc above).
   for (int x = 0; x < kBarWidth; ++x)
   {
      const float t =
         static_cast<float>(x) / static_cast<float>(kBarWidth - 1);
      const float domainValue =
         kPaletteDomainOffset_ + t * kPaletteDomainScale_;
      const auto pixel = colorTable->Color(domainValue);

      painter.setPen(QColor(pixel[0], pixel[1], pixel[2], pixel[3]));
      painter.drawLine(barX + x, barY, barX + x, barY + kBarHeight - 1);
   }

   painter.setPen(QColor(255, 255, 255));
   painter.drawRect(QRect(barX, barY, kBarWidth - 1, kBarHeight - 1));

   const std::string minLabel = gribManager.FormatValue(colorRange.colorOffset);
   const std::string maxLabel = gribManager.FormatValue(
      colorRange.colorOffset + colorRange.colorScale);

   QFont font = painter.font();
   font.setPointSize(10);
   painter.setFont(font);
   painter.setPen(QColor(255, 255, 255));

   painter.drawText(
      barX, barY + kBarHeight + 14, QString::fromStdString(minLabel));

   const QString maxText = QString::fromStdString(maxLabel);
   const int maxTextWidth = painter.fontMetrics().horizontalAdvance(maxText);
   painter.drawText(
      barX + kBarWidth - maxTextWidth, barY + kBarHeight + 14, maxText);

   QFont titleFont = font;
   titleFont.setBold(true);
   painter.setFont(titleFont);
   const std::string title = categoryLabel + " " + gribManager.CurrentProductName();
   painter.drawText(
      barX, barY - kLineHeight - 6, QString::fromStdString(title));

   painter.setFont(font);
   if (!validTime.empty())
   {
      painter.drawText(
         barX, barY - 6, QString::fromStdString("Valid: " + validTime));
   }
}

} // namespace scwx::qt::util::grib_legend
