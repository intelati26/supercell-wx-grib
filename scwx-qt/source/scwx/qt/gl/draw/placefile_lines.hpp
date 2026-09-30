#pragma once

#include <scwx/qt/gl/gl_context.hpp>
#include <scwx/qt/gl/draw/draw_item.hpp>
#include <scwx/gr/placefile.hpp>

#include <functional>

namespace scwx
{
namespace qt
{
namespace gl
{
namespace draw
{

class PlacefileLines : public DrawItem
{
public:
   explicit PlacefileLines(const std::shared_ptr<GlContext>& context);
   ~PlacefileLines();

   PlacefileLines(const PlacefileLines&)            = delete;
   PlacefileLines& operator=(const PlacefileLines&) = delete;

   PlacefileLines(PlacefileLines&&) noexcept;
   PlacefileLines& operator=(PlacefileLines&&) noexcept;

   void set_selected_time(std::chrono::system_clock::time_point selectedTime);
   void set_thresholded(bool thresholded);

   /**
    * Sets a function called from Render() when the current zoom no longer
    * matches the level of detail, or the visible area, the lines were last
    * built for (only the on-screen part plus a margin is built). It should
    * rebuild the lines (StartLines(), AddLine() for each, FinishLines())
    * on the same thread that normally builds them -- not from within the
    * callback itself. Lines are drawn simplified for the zoom they were
    * built at (see util::SimplifyLine()); the placefile's own line data and
    * hover text are unchanged.
    */
   void set_rebuild_requested_callback(std::function<void()> callback);

   void Initialize() override;
   void Render(const QMapLibre::CustomLayerRenderParameters& params) override;
   void Deinitialize() override;

   bool
   RunMousePicking(const QMapLibre::CustomLayerRenderParameters& params,
                   const QPointF&                                mouseLocalPos,
                   const QPointF&                                mouseGlobalPos,
                   const glm::vec2&                              mouseCoords,
                   const common::Coordinate&                     mouseGeoCoords,
                   std::shared_ptr<types::EventHandler>& eventHandler) override;

   /**
    * Tells the draw item a rebuild it asked for (see
    * set_rebuild_requested_callback()) will not happen -- e.g. the placefile
    * has gone away -- so it may ask again.
    */
   void AbortRebuild();

   /**
    * Resets and prepares the draw item for adding a new set of lines.
    */
   void StartLines();

   /**
    * Adds a placefile line to the internal draw list.
    *
    * @param [in] di Placefile line
    */
   void AddLine(const std::shared_ptr<gr::Placefile::LineDrawItem>& di);

   /**
    * Finalizes the draw item after adding new lines.
    */
   void FinishLines();

private:
   class Impl;

   std::unique_ptr<Impl> p;
};

} // namespace draw
} // namespace gl
} // namespace qt
} // namespace scwx
