#!/usr/bin/env python3
"""Works out which .idx records each RRFS/RTMA product reads, and emits the table.

GribManager downloads only the idx records a product needs (see
scwx::util::grib_idx::FieldSelector and manager/grib_field_selectors.cpp).
decode_grib picks its message by eccodes keys (shortName, typeOfLevel, topLevel,
...), the idx names fields in wgrib2's vocabulary, and the two do not map by
rule. But both list a file's messages in the same order, so the idx row at the
position of the message decode_grib would pick *is* that product's record.

Usage:
  derive_idx_selectors.py <qt/manager dir> <2dfld.grib2> <2dfld.idx> \
      <prslev.grib2> <prslev.idx> <rtma.grib2> <rtma.idx> > grib_field_selectors.cpp

Pass one real file of each family (the idx must belong to its file). Writes the
whole of grib_field_selectors.cpp to stdout and a size report to stderr, and
exits non-zero if anything could not be placed. Needs eccodes' grib_get on PATH.
Reads the product tables from grib_manager.cpp, the hodograph levels from
hodograph_manager.cpp and the wind-barb fields from wind_barb_manager.cpp, so
re-run it after adding a product or changing what those read.
"""
import os
import re
import subprocess
import sys

# What decode_grib's `--derived <name>` reads, per file family: (shortName,
# typeOfLevel, topLevel, bottomLevel), copied from decode_grib.cpp's Compute*()
# and RunDerived(). Keep in step with it.
DERIVED_INPUTS = {
    "stp": {"2dfld": [
        ("orog", "surface", 0, 0), ("cape", "surface", 0, 0),
        ("cin", "surface", 0, 0), ("hlcy", "heightAboveGroundLayer", 1000, 0),
        ("gh", "adiabaticCondensation", 0, 0),
        ("vucsh", "heightAboveGroundLayer", 0, 6000),
        ("vvcsh", "heightAboveGroundLayer", 0, 6000)]},
    "ship": {"2dfld": [
        ("orog", "surface", 0, 0),
        ("cape", "pressureFromGroundLayer", 18000, 0),
        ("2sh", "heightAboveGround", 2, 2),
        ("vucsh", "heightAboveGroundLayer", 0, 6000),
        ("vvcsh", "heightAboveGroundLayer", 0, 6000),
        ("gh", "isothermZero", 0, 0)],
        "prslev": [
        ("t", "isobaricInhPa", 500, 500), ("t", "isobaricInhPa", 700, 700),
        ("gh", "isobaricInhPa", 500, 500), ("gh", "isobaricInhPa", 700, 700)]},
    "shear6": {"2dfld": [
        ("vucsh", "heightAboveGroundLayer", 0, 6000),
        ("vvcsh", "heightAboveGroundLayer", 0, 6000)]},
    "wind10": {"2dfld": [("10u", "", -1, -1), ("10v", "", -1, -1)]},
    "wind500": {"prslev": [
        ("u", "isobaricInhPa", 500, 500), ("v", "isobaricInhPa", 500, 500)]},
}


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def table_text(src, marker):
    start = src.index(marker)
    start = src.index("{", start)
    depth, i = 0, start
    while True:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start + 1:i]
        i += 1


def split_top(text, sep=","):
    parts, depth, cur, instr = [], 0, "", False
    for ch in text:
        if ch == '"':
            instr = not instr
        if not instr:
            if ch in "{(":
                depth += 1
            elif ch in "})":
                depth -= 1
            elif ch == sep and depth == 0:
                parts.append(cur.strip())
                cur = ""
                continue
        cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def entries(src, table):
    body = strip_comments(table_text(src, "ProductConfig> " + table))
    out = []
    for e in split_top(body):
        e = e.strip()
        if e.startswith("{"):
            out.append(split_top(e[1:-1]))
    return out


def val(fields, i, default):
    return fields[i] if i < len(fields) else default


def unq(s):
    return s.strip().strip('"')


class Family:
    """One real file of a family: its messages (eccodes keys) beside its idx."""

    def __init__(self, grib, idxfile):
        rows = subprocess.run(
            ["grib_get", "-f", "-p",
             "shortName,typeOfLevel,topLevel,bottomLevel,startStep,"
             "lengthOfTimeRange", grib],
            capture_output=True, text=True, check=True).stdout.splitlines()
        self.msgs = [r.split() for r in rows]
        self.idx = [l.rstrip("\n").split(":") for l in open(idxfile) if l.strip()]
        assert len(self.msgs) == len(self.idx), (len(self.msgs), len(self.idx))
        offsets = [int(r[1]) for r in self.idx]
        total = os.path.getsize(grib)
        self.sizes = [(offsets[i + 1] if i + 1 < len(offsets) else total)
                      - offsets[i] for i in range(len(offsets))]

    def find(self, short, tol="", top=-1, bot=-1, ss=-1, lt=-1, subset=None):
        """decode_grib's FindMessage: the first message that fits."""
        for i in (range(len(self.msgs)) if subset is None else subset):
            m = self.msgs[i]
            if m[0] != short or (tol and m[1] != tol):
                continue
            if top >= 0 or bot >= 0:
                t = int(m[2]) if m[2].lstrip("-").isdigit() else -1
                b = int(m[3]) if m[3].lstrip("-").isdigit() else -1
                if (top >= 0 and t != top) or (bot >= 0 and b != bot):
                    continue
            if ss >= 0 and not (m[4].isdigit() and int(m[4]) == ss):
                continue
            if lt >= 0 and not (m[5].isdigit() and int(m[5]) == lt):
                continue
            return i
        return None

    def selector(self, i):
        """The (parameter, level) for message i, and the records that share it."""
        sel = (self.idx[i][3], self.idx[i][4])
        group = [j for j, r in enumerate(self.idx) if (r[3], r[4]) == sel]
        return sel, group

    def bytes_of(self, group):
        return sum(self.sizes[j] for j in group)


def cpp_selectors(sels):
    return ", ".join('{"%s", "%s"}' % s for s in sels)


def group_selectors(ff, finds, report, label):
    """Selectors (deduplicated) for each eccodes find() in `finds`."""
    sels, nbytes, problems = [], 0, 0
    for args in finds:
        i = ff.find(*args)
        if i is None:
            print(f"MISSING {label}: {args}", file=sys.stderr)
            problems += 1
            continue
        sel, group = ff.selector(i)
        if sel not in sels:
            sels.append(sel)
            nbytes += ff.bytes_of(group)
    print(f"{label}: {len(sels)} field(s), {nbytes / 1e6:.1f} MB", file=sys.stderr)
    return sels, problems


def hodograph_finds(text):
    """(shortName, typeOfLevel, top, bottom) for every field HodographManager
    decodes: each level's u and v, and terrain."""
    body = table_text(text, "HodographManager::Level> levels")
    finds = []
    for e in split_top(strip_comments(body)):
        e = e.strip()
        if not e.startswith("{"):
            continue
        f = split_top(e[1:-1])
        tol, level = unq(f[0]), int(f[1])
        finds.append((unq(f[4]), tol, level, level))
        finds.append((unq(f[5]), tol, level, level))
    finds.append(("orog",))
    return finds


def wind_barb_finds(text):
    """The short names WindBarbManager decodes (decodeField("...", ...))."""
    return [(n,) for n in re.findall(r'decodeField\("([^"]+)"', text)]


def main():
    mdir = sys.argv[1]
    fam = {"2dfld": Family(sys.argv[2], sys.argv[3]),
           "prslev": Family(sys.argv[4], sys.argv[5]),
           "rtma": Family(sys.argv[6], sys.argv[7])}
    src = open(os.path.join(mdir, "grib_manager.cpp")).read()
    problems = 0
    lines = []

    for category, table in (("Rtma", "kRtmaProducts_"), ("Rrfs", "kRrfsProducts_")):
        lines.append(f"   // {category}")
        for f in entries(src, table):
            name = unq(f[0])
            derived = unq(val(f, 9, '""'))
            fname = ("rtma" if category == "Rtma" else
                     "prslev" if "PressureLevel" in val(f, 15, "") else "2dfld")

            if derived:
                spec = DERIVED_INPUTS.get(derived)
                if spec is None:
                    lines.append(f"   // UNKNOWN derived index {derived} for {name}")
                    print(f"{name}: unknown derived index {derived}", file=sys.stderr)
                    problems += 1
                    continue
                parts = {}
                for which, inputs in spec.items():
                    sels, bad = group_selectors(
                        fam[which], [(a, b, c, d) for a, b, c, d in inputs],
                        None, f"{name} ({which})")
                    problems += bad
                    parts[which] = sels
                # `primary` is the product's own file family, `secondary` the
                # other file SHIP also reads
                other = [w for w in parts if w != fname]
                lines.append('   {{map::GribCategory::%s, "%s"},\n    {{%s}, {%s}}},' % (
                    category, name, cpp_selectors(parts.get(fname, [])),
                    cpp_selectors(parts[other[0]]) if other else ""))
                continue

            if not unq(f[2]):
                lines.append(f"   // SKIP {name}: no shortName")
                continue

            ff = fam[fname]
            find_args = (unq(f[2]), unq(val(f, 10, '""')),
                         int(val(f, 11, "-1")), int(val(f, 12, "-1")),
                         int(val(f, 13, "-1")), int(val(f, 14, "-1")))
            i = ff.find(*find_args)
            if i is None:
                lines.append(f"   // MISSING {name}: shortName {unq(f[2])}")
                print(f"{name}: not found in the {fname} file", file=sys.stderr)
                problems += 1
                continue
            sel, group = ff.selector(i)
            if ff.find(*find_args, subset=group) != i:
                print(f"{name}: the records sharing {sel} pick a different "
                      f"message than the whole file does", file=sys.stderr)
                problems += 1
            lines.append('   {{map::GribCategory::%s, "%s"},\n    {{%s}, {}}},' % (
                category, name, cpp_selectors([sel])))
            print(f"{name}: {sel[0]} / {sel[1]}, {len(group)} record(s), "
                  f"{ff.bytes_of(group) / 1e6:.2f} MB", file=sys.stderr)

    hod, bad = group_selectors(
        fam["2dfld"],
        [a if len(a) == 4 else (a[0],) for a in hodograph_finds(
            open(os.path.join(mdir, "hodograph_manager.cpp")).read())],
        None, "hodograph")
    problems += bad
    barbs, bad = group_selectors(
        fam["rtma"],
        wind_barb_finds(open(os.path.join(mdir, "wind_barb_manager.cpp")).read()),
        None, "wind barbs")
    problems += bad

    sys.stdout.write(TEMPLATE % ("\n".join(lines), cpp_selectors(hod),
                                 cpp_selectors(barbs)))
    sys.exit(1 if problems else 0)


TEMPLATE = """// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

// GENERATED by tools/derive_idx_selectors.py from real RRFS and RTMA files --
// do not edit by hand; re-run the tool after adding a product (a unit test fails
// for an RRFS or RTMA product missing from the table).

#include <scwx/qt/manager/grib_field_selectors.hpp>

#include <map>
#include <utility>

namespace scwx::qt::manager::grib_fields
{

namespace
{

using Key = std::pair<map::GribCategory, std::string>;

// Positional {parameter, level} pairs: naming each member would only add noise
// to a generated table.
// NOLINTBEGIN(modernize-use-designated-initializers)
const std::map<Key, ProductFields> kProductFields_ {
%s
};

const std::vector<scwx::util::grib_idx::FieldSelector> kHodographFields_ {
   %s};

const std::vector<scwx::util::grib_idx::FieldSelector> kWindBarbFields_ {
   %s};
// NOLINTEND(modernize-use-designated-initializers)

} // namespace

const ProductFields* FieldsFor(map::GribCategory   category,
                               const std::string& displayName)
{
   const auto it = kProductFields_.find({category, displayName});
   return it == kProductFields_.cend() ? nullptr : &it->second;
}

const std::vector<scwx::util::grib_idx::FieldSelector>& HodographFields()
{
   return kHodographFields_;
}

const std::vector<scwx::util::grib_idx::FieldSelector>& WindBarbFields()
{
   return kWindBarbFields_;
}

} // namespace scwx::qt::manager::grib_fields
"""


main()
