#pragma once

#include <scwx/common/products.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <QDockWidget>

namespace Ui
{
class VolumeDockWidget;
}

namespace scwx::qt::config
{
class RadarSite;
} // namespace scwx::qt::config

namespace scwx::qt::ui
{

// The 3D volume pane: every tilt of the active map's Level 2 volume as stacked
// translucent cones over a square region (see volume::RadarVolume and
// VolumeViewWidget). It follows the active map's radar site, product and time;
// the region is picked on the map ("3D view here") or is the whole radar.
//
// Data is only fetched while the pane is visible, on its own worker thread,
// and is released when the pane is hidden.
class VolumeDockWidget : public QDockWidget
{
   Q_OBJECT
   Q_DISABLE_COPY_MOVE(VolumeDockWidget)

public:
   struct Source
   {
      std::shared_ptr<config::RadarSite>    radarSite {};
      common::RadarProductGroup             group {};
      std::string                           product {};
      std::chrono::system_clock::time_point time {};
   };

   explicit VolumeDockWidget(QWidget* parent = nullptr);
   ~VolumeDockWidget() override;

   // What the active map shows; the pane redraws when this changes.
   void SetSource(const Source& source);

   // Centre the region on a map point and show the pane.
   void ShowRegion(double latitude, double longitude);

protected:
   void showEvent(QShowEvent* event) override;
   void hideEvent(QHideEvent* event) override;

private:
   class Impl;
   std::unique_ptr<Impl> p;
   Ui::VolumeDockWidget* ui;
};

} // namespace scwx::qt::ui
