# GRIB field downloads (.idx byte ranges)

RRFS and RTMA publish one S3 object per forecast hour holding every field: about
350 MB for an RRFS `2dfld` file, 590 MB for `prslev`, 84 MB for an RTMA analysis.
A map layer reads one field of that -- under a megabyte or a few megabytes -- so
the app downloads only the messages a product reads and decodes them with
`decode_grib` exactly as it would from the whole file.

| Source | Whole object | A product's fields |
|---|---|---|
| RRFS reflectivity (1 km) | 361 MB | 0.57 MB |
| RRFS STP | 361 MB | 9.7 MB |
| RRFS SHIP (two files) | 950 MB | 12.9 MB |
| Hodograph (17 levels, u and v, terrain) | 361 MB | 55 MB |
| RTMA 2 m temperature | 84 MB | 6.1 MB |
| RTMA wind barbs (direction, speed, gust) | 84 MB | 17 MB |

MRMS objects are small and gzipped and have no idx; NBM and custom models already
select a single record by idx (`ConfiguredIdxProvider`).

## How it works

1. `AwsNexradDataProvider::DownloadGribFieldsByIndex()` fetches `<key>.idx`
   (kept in memory afterwards, so fetching several fields of one file reads it
   once), selects every record whose parameter and level match
   (`scwx::util::grib_idx::SelectRecords`), merges records that sit next to each
   other into one byte range (`MergedRanges`), range-downloads them and joins them
   in file order into one file.
2. Selection is by parameter and level only, *any step*. A file can hold the same
   field at several steps (a sub-hourly file's 15/30/45/60 minute records, or an
   hour's accumulation next to the run total); `decode_grib` already picks the
   right message from what it is given, so the slice decodes the same as the
   whole file. Verified: all 48 RRFS/RTMA products decode **byte-identical** from
   slices and from whole files.
3. `GribManager` keeps each product's bytes as its own cache entry,
   `<key>.fields-<product>`, because a product's slice cannot be shared with
   another product reading the same object (`CacheKeyFor`).
4. If the idx is not there yet, or does not list a field, or the transfer fails,
   the attempt fails visibly (status bar) and is retried at the next poll or hour
   change. The whole object is never downloaded as a fallback for a product that
   has fields.

## Adding or changing a product

The idx names fields in wgrib2's vocabulary (`REFD:1000 m above ground`),
`decode_grib` selects by eccodes keys (`rare`, `heightAboveGround`, 1000), and the
two do not map by rule. `tools/derive_idx_selectors.py` finds each product's
records by aligning the message `decode_grib` would pick with the idx row at the
same position, and writes all of `grib_field_selectors.cpp`:

```
derive_idx_selectors.py scwx-qt/source/scwx/qt/manager \
    2dfld.grib2 2dfld.idx prslev.grib2 prslev.idx rtma.grib2 rtma.idx \
    > scwx-qt/source/scwx/qt/manager/grib_field_selectors.cpp
```

Give it one real file of each family with its idx (RRFS 2dfld and prslev from a
3-hourly cycle, and an RTMA analysis). It reads the product tables from
`grib_manager.cpp`, hodograph levels from `hodograph_manager.cpp` and wind-barb
fields from `wind_barb_manager.cpp`, and exits non-zero if a product cannot be
placed. Derived products' inputs are listed in the script (copied from
`decode_grib.cpp`); keep them in step with it.

A unit test fails for any RRFS or RTMA product missing from the table.
