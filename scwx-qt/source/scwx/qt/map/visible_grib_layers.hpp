// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/types/layer_types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace scwx::qt::map
{

// The GRIB category a layer-manager data layer shows, or nothing for a layer
// that is not one of the GRIB fill layers (radar range, wind barbs, ...).
[[nodiscard]] std::optional<GribCategory>
GribCategoryOf(const types::LayerDescription& description);

// The short name shown for a category in the dock and on exported images.
[[nodiscard]] std::string GribCategoryDisplayName(GribCategory category);

struct VisibleGribLayer
{
   GribCategory category;
   float        opacity {1.0f};
};

// The GRIB fill layers actually visible in map pane `mapId` -- displayed there
// and not fully transparent -- in layer-manager order, which is top-most
// first. One entry per category.
[[nodiscard]] std::vector<VisibleGribLayer>
VisibleGribLayers(const types::LayerVector& layers, std::size_t mapId);

} // namespace scwx::qt::map
