// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <optional>
#include <set>

namespace scwx::qt::ui::rrfs_hours
{

struct Range
{
   int first;
   int last;
};

// The forecast hours the RRFS hour picker covers. With `published` known -- the
// hours really on S3 for the selected cycle -- that is the first to the last of
// them: an hourly cycle starts at F001 (it has no F000), and a cycle still
// being written has not reached its nominal horizon, so offering buttons up to
// it only shows hours that cannot be used. Before S3 has been listed (or if the
// cycle has nothing published), the cycle's nominal horizon, 0 to `nominalMax`.
[[nodiscard]] inline Range
Covered(const std::optional<std::set<int>>& published, int nominalMax)
{
   if (published.has_value() && !published->empty())
   {
      return {*published->begin(), *published->rbegin()};
   }
   return {0, nominalMax};
}

// Whether `hour` can be shown: published, or nothing is known yet (a button is
// not disabled on a guess).
[[nodiscard]] inline bool
Available(const std::optional<std::set<int>>& published, int hour)
{ return !published.has_value() || published->contains(hour); }

} // namespace scwx::qt::ui::rrfs_hours
