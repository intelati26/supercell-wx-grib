// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <QLabel>

namespace scwx::qt::ui
{

// A label that can be clicked: emits Clicked() when the left button is released
// over it, and shows a pointing-hand cursor so it looks it.
class ClickableLabel : public QLabel
{
   Q_OBJECT

public:
   explicit ClickableLabel(QWidget* parent = nullptr);

signals:
   void Clicked();

protected:
   void mouseReleaseEvent(QMouseEvent* event) override;
};

} // namespace scwx::qt::ui
