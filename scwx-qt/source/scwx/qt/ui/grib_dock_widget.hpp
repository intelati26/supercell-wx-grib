#pragma once

#include <scwx/qt/map/grib_frame_info.hpp>

#include <memory>
#include <vector>

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

   // RRFS forecast hour on the section's slider (0 for a category with none),
   // and moving it -- which selects that hour exactly as the user dragging it
   // would. Used to step through a loop when exporting it.
   [[nodiscard]] int ForecastHour(map::GribCategory category) const;
   void              SetForecastHour(map::GribCategory category, int hour);

signals:
   // Emitted by the "Map View" panel's preset buttons -- MainWindow owns
   // the actual MapWidget instances (this dock never has a direct
   // reference to one, same as every other signal below), so it connects
   // this to MapWidget::SetMapBounds() on the active pane.
   void MapBoundsRequested(double southLatitude,
                           double westLongitude,
                           double northLatitude,
                           double eastLongitude);

   // Emitted by a category section's "Export image" button. `categoryLabel`
   // is that section's own display name (e.g. "RRFS") -- passed through
   // rather than recomputed on the receiving end.
   void ExportSnapshotRequested(map::GribCategory category,
                                QString            categoryLabel);

   // Emitted by RRFS's "Export loop" button with the forecast hours to save,
   // in order (Play has already been stopped and their downloads started).
   void ExportLoopRequested(map::GribCategory category,
                            QString           categoryLabel,
                            std::vector<int>  hours);

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::ui
