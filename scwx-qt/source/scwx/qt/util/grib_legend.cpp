#include <scwx/qt/util/grib_legend.hpp>
#include <scwx/qt/settings/palette_settings.hpp>
#include <scwx/qt/util/file.hpp>
#include <scwx/common/color_table.hpp>
#include <scwx/util/logger.hpp>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include <fmt/format.h>

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
// A panel holds, above the bar, a title line and a valid-time line, and below
// it the min/max value labels -- the labels must be inside the dark backdrop
// or they run into the next panel (and off the bottom of the image).
constexpr int kAboveBar     = 2 * kLineHeight + 12;
constexpr int kBelowBar     = kBarHeight + 20;
constexpr int kPanelHeight  = kAboveBar + kBelowBar;
constexpr int kPanelWidth   = kBarWidth + 16;
constexpr int kPanelSpacing = 8;

// One product's legend, ready to draw
struct Panel
{
   const Source*            source;
   std::size_t              productIndex;
   std::string              validTime;
   map::GribFrameColorRange colorRange;
};

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

std::vector<std::optional<PanelPlacement>> LayoutPanels(QSize       imageSize,
                                                        std::size_t panelCount)
{
   std::vector<std::optional<PanelPlacement>> placements(panelCount);

   // Rows a column holds: each panel needs its height plus the gap above it
   const int usableHeight = imageSize.height() - 2 * kMargin;
   const int rows =
      (usableHeight + kPanelSpacing) / (kPanelHeight + kPanelSpacing);
   if (rows < 1)
   {
      return placements;
   }

   for (std::size_t i = 0; i < panelCount; ++i)
   {
      // Slot 0 is the bottom of the first column; the list's last panel goes
      // there and the first panel ends up highest (or furthest right).
      const std::size_t slot   = panelCount - 1 - i;
      const int         column = static_cast<int>(slot / rows);
      const int         row    = static_cast<int>(slot % rows);

      const int barX = kMargin + column * (kPanelWidth + kPanelSpacing);
      if (barX + kPanelWidth > imageSize.width())
      {
         continue; // no room for another column
      }

      placements[i] = PanelPlacement {barX,
                                      imageSize.height() - kMargin - kBelowBar -
                                         row * (kPanelHeight + kPanelSpacing)};
   }

   return placements;
}

void DrawLegends(QImage& image, const std::vector<Source>& sources)
{
   // Only the products that have something to show
   std::vector<Panel> panels;
   for (const auto& source : sources)
   {
      const auto activeIndices = source.gribManager->ActiveProductIndices();
      if (activeIndices.empty())
      {
         // Rrfs/Nbm with nothing checked -- nothing to show.
         logger_->info("No product active for {}, skipping legend",
                       source.label);
         continue;
      }

      for (const std::size_t productIndex : activeIndices)
      {
         const std::string framePath =
            map::GetGribFramePath(source.category, productIndex);

         const auto colorRange = map::ReadGribFrameColorRange(framePath);
         if (colorRange.colorScale == 0.0f)
         {
            logger_->info("No decoded frame yet for {} {}, skipping its legend",
                          source.label,
                          source.gribManager->ProductName(productIndex));
            continue;
         }

         panels.push_back({&source,
                           productIndex,
                           map::ReadGribFrameValidTime(framePath),
                           colorRange});
      }
   }

   if (panels.empty())
   {
      return;
   }

   auto colorTable = LoadPalette();
   if (!colorTable->IsValid())
   {
      logger_->warn("Could not load palette, skipping legend");
      return;
   }

   const auto placements = LayoutPanels(image.size(), panels.size());

   QPainter painter(&image);
   painter.setRenderHint(QPainter::Antialiasing, true);

   for (std::size_t i = 0; i < panels.size(); ++i)
   {
      const auto& placement = placements[i];
      if (!placement.has_value())
      {
         logger_->info(
            "No room on the image for the {} {} legend",
            panels[i].source->label,
            panels[i].source->gribManager->ProductName(panels[i].productIndex));
         continue;
      }

      const auto&       panel        = panels[i];
      const auto&       gribManager  = *panel.source->gribManager;
      const auto&       colorRange   = panel.colorRange;
      const std::size_t productIndex = panel.productIndex;
      const int         barX         = placement->barX;
      const int         barY         = placement->barY;

      // Background panel so the legend stays legible over any map content.
      const QRect panelRect(
         barX - 8, barY - kAboveBar, kPanelWidth, kPanelHeight);
      painter.fillRect(panelRect, QColor(0, 0, 0, 160));

      // Gradient bar -- one column per pixel, reproducing the exact
      // formula GribProductLayer's shader uses to turn a raw value into a
      // color (see kPaletteDomainOffset_/Scale_'s own doc above).
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

      const std::string minLabel =
         gribManager.FormatValue(productIndex, colorRange.colorOffset);
      const std::string maxLabel = gribManager.FormatValue(
         productIndex, colorRange.colorOffset + colorRange.colorScale);

      QFont font = painter.font();
      font.setPointSize(10);
      painter.setFont(font);
      painter.setPen(QColor(255, 255, 255));

      painter.drawText(
         barX, barY + kBarHeight + 14, QString::fromStdString(minLabel));

      const QString maxText  = QString::fromStdString(maxLabel);
      const int maxTextWidth = painter.fontMetrics().horizontalAdvance(maxText);
      painter.drawText(
         barX + kBarWidth - maxTextWidth, barY + kBarHeight + 14, maxText);

      QFont titleFont = font;
      titleFont.setBold(true);
      painter.setFont(titleFont);
      std::string title =
         panel.source->label + " " + gribManager.ProductName(productIndex);
      if (panel.source->opacity < 1.0f)
      {
         title +=
            fmt::format(" ({:.0f}% opacity)", panel.source->opacity * 100.0f);
      }
      painter.drawText(
         barX, barY - kLineHeight - 6, QString::fromStdString(title));

      painter.setFont(font);
      if (!panel.validTime.empty())
      {
         painter.drawText(barX,
                          barY - 6,
                          QString::fromStdString("Valid: " + panel.validTime));
      }
   }
}

} // namespace scwx::qt::util::grib_legend
