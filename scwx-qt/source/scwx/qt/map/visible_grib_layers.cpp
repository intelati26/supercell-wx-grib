// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#include <scwx/qt/map/visible_grib_layers.hpp>

#include <algorithm>
#include <variant>

namespace scwx::qt::map
{

std::optional<GribCategory>
GribCategoryOf(const types::LayerDescription& description)
{
   if (!std::holds_alternative<types::DataLayer>(description))
   {
      return std::nullopt;
   }

   switch (std::get<types::DataLayer>(description))
   {
   case types::DataLayer::GribMrms:
      return GribCategory::Mrms;
   case types::DataLayer::GribRtma:
      return GribCategory::Rtma;
   case types::DataLayer::GribRrfs:
      return GribCategory::Rrfs;
   case types::DataLayer::GribNbm:
      return GribCategory::Nbm;
   case types::DataLayer::GribUser:
      return GribCategory::User;
   default:
      return std::nullopt;
   }
}

std::string GribCategoryDisplayName(GribCategory category)
{
   switch (category)
   {
   case GribCategory::Mrms:
      return "MRMS";
   case GribCategory::Rtma:
      return "RTMA";
   case GribCategory::Rrfs:
      return "RRFS";
   case GribCategory::User:
      return "Custom Models";
   case GribCategory::Nbm:
   default:
      return "NBM";
   }
}

std::vector<VisibleGribLayer>
VisibleGribLayers(const types::LayerVector& layers, std::size_t mapId)
{
   std::vector<VisibleGribLayer> visible;

   for (const auto& layer : layers)
   {
      const auto category = GribCategoryOf(layer.description_);
      if (layer.type_ != types::LayerType::Data || !category.has_value() ||
          mapId >= layer.displayed_.size() || !layer.displayed_[mapId] ||
          layer.opacity_ <= 0.0f)
      {
         continue;
      }

      const bool seen = std::any_of(visible.cbegin(),
                                    visible.cend(),
                                    [&category](const VisibleGribLayer& v)
                                    { return v.category == *category; });
      if (!seen)
      {
         visible.push_back({*category, layer.opacity_});
      }
   }

   return visible;
}

} // namespace scwx::qt::map
