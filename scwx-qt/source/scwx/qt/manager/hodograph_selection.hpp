// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <atomic>

#include <QObject>

namespace scwx::qt::manager
{

// Whether the user has picked the gridded hodograph as a product: the
// "Gridded Hodograph" entry in the RRFS section's product list. That entry is
// the only thing that makes hodographs download and draw -- the layer-manager
// row only says where in the stack, and on which panes, they appear once
// picked.
//
// A process-wide object of its own, not state on HodographManager: that one
// is created and dropped with the layers that use it, but the pick is made in
// the dock whether or not any hodograph layer exists. Like every other
// product pick it is not saved, so each launch starts with no hodographs.
class HodographSelection : public QObject
{
   Q_OBJECT

public:
   static HodographSelection& Instance();

   [[nodiscard]] bool IsEnabled() const;

   // Emits EnabledChanged() only if the pick actually changed.
   void SetEnabled(bool enabled);

signals:
   void EnabledChanged(bool enabled);

private:
   HodographSelection() = default;

   std::atomic<bool> enabled_ {false};
};

} // namespace scwx::qt::manager
