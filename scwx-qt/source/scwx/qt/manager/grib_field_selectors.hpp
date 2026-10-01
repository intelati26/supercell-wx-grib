// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/util/grib_idx.hpp>

#include <string>
#include <vector>

namespace scwx::qt::manager::grib_fields
{

// Which of a source file's GRIB2 messages a product reads, named the way the
// file's ".idx" sidecar names them (see scwx::util::grib_idx::FieldSelector).
// RRFS and RTMA publish one object per hour holding every field -- ~350MB and
// ~84MB -- of which a product reads well under a megabyte or a few megabytes,
// so only those messages are downloaded (AwsNexradDataProvider::
// DownloadGribFieldsByIndex()) and decode_grib reads them from there exactly as
// it would from the whole file.
struct ProductFields
{
   // Fields in the file the product's own family reads (RRFS 2dfld or prslev,
   // RTMA's analysis file)
   std::vector<scwx::util::grib_idx::FieldSelector> primary;

   // Fields in RRFS's *other* file, for the one product that reads both (SHIP)
   std::vector<scwx::util::grib_idx::FieldSelector> secondary;
};

// The product's fields, or nullptr for a product with no entry (MRMS, NBM,
// custom models -- they download whole objects or select one record by their
// own rules). Keyed by display name, which GribManager already treats as a
// product's identity.
//
// The table is generated from real files by tools/derive_idx_selectors.py:
// eccodes keys do not map to idx names by rule, so each product's record is
// found by aligning the message decode_grib would pick with the idx row at the
// same position. Re-run it when a product is added; a unit test fails for an
// RRFS/RTMA product missing here.
[[nodiscard]] const ProductFields* FieldsFor(map::GribCategory  category,
                                             const std::string& displayName);

// What HodographManager reads from an RRFS 2dfld file: u and v at every
// hodograph level, and terrain height. Derived from its level table.
[[nodiscard]] const std::vector<scwx::util::grib_idx::FieldSelector>&
HodographFields();

// What WindBarbManager reads from an RTMA analysis file: wind direction, speed
// and gust at 10 m.
[[nodiscard]] const std::vector<scwx::util::grib_idx::FieldSelector>&
WindBarbFields();

} // namespace scwx::qt::manager::grib_fields
