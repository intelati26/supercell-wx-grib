#pragma once

#include <scwx/qt/map/draw_layer.hpp>

namespace scwx::qt::map
{

// Plots 10m wind barbs over a decimated subset of RTMA's Lambert grid,
// driven by manager::WindBarbManager's independently-fetched direction/
// speed frames. Follows MarkerLayer's DrawLayer + GeoIcons pattern rather
// than GribProductLayer's raw-GL mesh approach -- GeoIcons already handles
// per-instance texture selection (speed bucket) and rotation (direction),
// which is all a barb field needs. Hover text goes through GeoIcons' own
// per-icon SetIconHoverText/RunMousePicking, the same pathway MarkerLayer
// uses -- barbs are point features, not an "area" layer, so they don't
// participate in GenericLayer's AddAreaSibling/CombineAreaHoverText.
class WindBarbLayer : public DrawLayer
{
   Q_DISABLE_COPY_MOVE(WindBarbLayer)

public:
   explicit WindBarbLayer(const std::shared_ptr<gl::GlContext>& glContext);
   ~WindBarbLayer();

   void Initialize(const std::shared_ptr<MapContext>& mapContext) final;
   void Render(const std::shared_ptr<MapContext>& mapContext,
               const QMapLibre::CustomLayerRenderParameters&) final;
   void Deinitialize() final;

   bool
   RunMousePicking(const std::shared_ptr<MapContext>&            mapContext,
                   const QMapLibre::CustomLayerRenderParameters& params,
                   const QPointF&                                mouseLocalPos,
                   const QPointF&                                mouseGlobalPos,
                   const glm::vec2&                              mouseCoords,
                   const common::Coordinate&                     mouseGeoCoords,
                   std::shared_ptr<types::EventHandler>& eventHandler) final;

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::map
