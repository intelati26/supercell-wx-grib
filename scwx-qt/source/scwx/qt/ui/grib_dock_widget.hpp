#pragma once

#include <scwx/qt/map/grib_frame_info.hpp>

#include <memory>

#include <QDockWidget>
#include <QString>

namespace scwx::qt::ui
{

// One combined dock (not one per category, see supercell-wx-grib-
// extension memory for that earlier design and why it changed) hosting a
// section per map::GribCategory (MRMS/RTMA/RRFS today) -- each section's
// CheckableComboBox drives that category's GribManager::SetProductActive,
// letting several products be shown at once within one category rather
// than a plain single-select dropdown. Built entirely in code (no .ui
// file) since the three sections are otherwise identical and would
// otherwise triple the same layout in Designer XML.
class GribDockWidget : public QDockWidget
{
   Q_OBJECT

public:
   explicit GribDockWidget(QWidget* parent = nullptr);
   ~GribDockWidget();

signals:
   // Emitted by the "Map View" panel's preset buttons -- MainWindow owns
   // the actual MapWidget instances (this dock never has a direct
   // reference to one, same as every other signal below), so it connects
   // this to MapWidget::SetMapBounds() on the active pane.
   void MapBoundsRequested(double southLatitude,
                           double westLongitude,
                           double northLatitude,
                           double eastLongitude);

   // Emitted by a category section's "Export PNG" button. `categoryLabel`
   // is that section's own display name (e.g. "RRFS") -- passed through
   // rather than recomputed on the receiving end, since CategoryDisplayName()
   // is file-local to grib_dock_widget.cpp.
   void ExportSnapshotRequested(map::GribCategory category,
                                QString            categoryLabel);

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::ui
