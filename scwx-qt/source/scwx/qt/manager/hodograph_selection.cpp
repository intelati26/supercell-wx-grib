// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#include <scwx/qt/manager/hodograph_selection.hpp>

namespace scwx::qt::manager
{

HodographSelection& HodographSelection::Instance()
{
   // Never destroyed: layers and the dock may outlive static destruction
   static auto* instance = new HodographSelection();
   return *instance;
}

bool HodographSelection::IsEnabled() const
{
   return enabled_.load();
}

void HodographSelection::SetEnabled(bool enabled)
{
   if (enabled_.exchange(enabled) != enabled)
   {
      Q_EMIT EnabledChanged(enabled);
   }
}

} // namespace scwx::qt::manager
