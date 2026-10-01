#pragma once

#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/map/visible_grib_layers.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QImage>
#include <QSize>

namespace scwx::qt::util::grib_legend
{

// One GRIB layer whose checked products get a legend.
struct Source
{
   std::shared_ptr<manager::GribManager> gribManager;
   map::GribCategory                     category;
   // Short display name (e.g. "RRFS") shown alongside each product name --
   // GribManager itself has no user-facing category label.
   std::string label;
   // The layer's opacity in the layer manager (1.0 = opaque); noted in the
   // panel title when less, since the colours on the map are then fainter
   // than the bar.
   float opacity {1.0f};
};

struct PanelPlacement
{
   int barX; // left edge of the colour bar
   int barY; // top edge of the colour bar
};

// Where each of `panelCount` legend panels goes on an image of `imageSize`:
// panels listed top first, stacked upward from the bottom-left corner -- the
// last panel lowest -- and continued in a further column to the right when
// the image is too short for them all. A panel that fits in no column (the
// image is too small) gets no placement.
[[nodiscard]] std::vector<std::optional<PanelPlacement>>
LayoutPanels(QSize imageSize, std::size_t panelCount);

// Composites a color-scale legend bar plus model/valid-time text onto `image`
// in place for every checked product of every source, `sources` listed top
// layer first so the legend reads in the same order as the layers stack on the
// map. Reads each product's own colorOffset/colorScale straight from its
// decoded frame file (see map::ReadGribFrameColorRange()) and samples the same
// shared palette GribProductLayer's shader does (see LoadPalette()'s own doc),
// so the legend always matches what's actually drawn on the map.
// Products with no decoded frame yet (colorScale == 0) are skipped; a no-op if
// none has one.
void DrawLegends(QImage& image, const std::vector<Source>& sources);

// DrawLegends() for the GRIB layers visible in one map pane (see
// map::VisibleGribLayers(), top layer first) -- what every export of a map
// picture does: the picture shows those layers, so its legend lists them.
// Nothing is drawn when none is visible.
void DrawVisibleLegends(QImage&                                   image,
                        const std::vector<map::VisibleGribLayer>& visible);

} // namespace scwx::qt::util::grib_legend
