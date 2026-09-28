#pragma once

#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>

#include <string>

#include <QImage>

namespace scwx::qt::util::grib_legend
{

// Composites a color-scale legend bar plus model/valid-time text onto
// `image` in place, for `gribManager`'s current product -- reads that
// product's own colorOffset/colorScale straight from its decoded frame
// file (see map::ReadGribFrameColorRange()) and samples the same shared
// palette GribProductLayer's shader does (see LoadPalette()'s own doc),
// so the legend always matches what's actually drawn on the map.
// `categoryLabel` is the short display name (e.g. "RRFS", "NBM") shown
// alongside the product name -- GribManager itself has no user-facing
// category label, only GribDockWidget's own CategoryDisplayName() does.
// A no-op (image left untouched) if this category has no decoded frame
// yet (colorScale == 0).
void DrawLegend(QImage&                     image,
               manager::GribManager&        gribManager,
               map::GribCategory            category,
               const std::string&           categoryLabel);

} // namespace scwx::qt::util::grib_legend
