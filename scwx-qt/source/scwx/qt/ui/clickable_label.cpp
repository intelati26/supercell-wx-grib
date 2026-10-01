// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#include <scwx/qt/ui/clickable_label.hpp>

#include <QMouseEvent>

namespace scwx::qt::ui
{

ClickableLabel::ClickableLabel(QWidget* parent) : QLabel(parent)
{ setCursor(Qt::PointingHandCursor); }

void ClickableLabel::mouseReleaseEvent(QMouseEvent* event)
{
   // Released inside the label, not dragged off it first
   if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
   {
      Q_EMIT Clicked();
   }

   QLabel::mouseReleaseEvent(event);
}

} // namespace scwx::qt::ui
