#pragma once

#include <scwx/qt/map/draw_layer.hpp>

namespace scwx::qt::map
{

// Plots a small hodograph (the connected polyline of wind vectors at
// several heights, colored by height band -- see docs/gridded-hodograph-
// plan.md) at a decimated subset of RRFS's Lambert grid, driven by
// manager::HodographManager's independently-fetched per-level u/v +
// terrain frames.
//
// Unlike WindBarbLayer (GeoIcons -- a pre-baked icon texture per bucketed
// speed, since a barb's shape only depends on one value), a hodograph's
// shape depends on the whole per-point wind profile -- effectively
// infinite combinations, so it cannot be a pre-baked icon. Built on
// GeoLines directly instead (the same real, geo-anchored line primitive
// LinkedVectors already uses for NEXRAD Level 3 linked-vector packets --
// found and reused rather than inventing a new draw primitive, since it
// already does exactly "connect these points with colored/width-
// controlled line segments"). One real consequence of that choice:
// GeoLines' endpoints are real lat/lon coordinates, not GeoIcons' screen-
// space pixel offset -- so a hodograph's on-screen size is a real
// geographic distance (see TierForZoom's own comment), unlike a barb
// icon's fixed pixel size.
class HodographLayer : public DrawLayer
{
   Q_DISABLE_COPY_MOVE(HodographLayer)

public:
   explicit HodographLayer(const std::shared_ptr<gl::GlContext>& glContext);
   ~HodographLayer();

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
