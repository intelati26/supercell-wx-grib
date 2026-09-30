#pragma once

#include <scwx/qt/map/generic_layer.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>

#include <optional>
#include <string>

namespace scwx::qt::map
{

// Renders every product manager::GribManager has checked in this layer's
// category (see GribManager::ActiveProductIndices()) as a MapLibre
// CustomLayer -- filled fields first, contour products on top -- using the
// same LUT/shader colorizing approach as RadarProductLayer but with a
// subdivided-mesh quad instead of per-vertex polar geometry (see
// grib.vert/grib.frag).
class GribProductLayer : public GenericLayer
{
   Q_DISABLE_COPY_MOVE(GribProductLayer)

public:
   explicit GribProductLayer(std::shared_ptr<gl::GlContext> glContext,
                             GribCategory                   category);
   ~GribProductLayer();

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

   // A "product\nvalue units\nValid: ..." block for each drawn product
   // with data under the cursor, or nullopt if none has any there (out of
   // grid bounds, below noDataThreshold, or no frame loaded yet).
   // GenericLayer's own AddAreaSibling()/CombineAreaHoverText() is what
   // actually combines this with e.g. RadarProductLayer's or another
   // category's GribProductLayer's own text -- see MapWidgetImpl::
   // AddLayer's wiring.
   [[nodiscard]] std::optional<std::string>
   GetHoverText(const std::shared_ptr<MapContext>& mapContext,
                const common::Coordinate& mouseGeoCoords) const override;

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::map
