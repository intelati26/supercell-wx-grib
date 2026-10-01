# GRIB idx byte-range fetch: shared primitive + model survey

## Why this exists

RRFS's own onboarding (see `rrfs-source-plan.md`) assumed one file = one
whole-object download = one decode -- true for RRFS/RTMA/MRMS, whose
files are already small and single-purpose. GFS and NBM (National
Blend of Models) break that assumption: both publish every field for a
cycle/hour in *one* object, hundreds of MB in size (GFS 0.25deg pgrb2:
~500MB; NBM core: ~160MB). Downloading the whole thing per product
would blow past the existing 40GB cache cap in a couple of frames.

Both (and, as the survey below found, most other NOAA model sources)
ship a NOMADS/wgrib2-style `.idx` text sidecar alongside the real file
-- one line per GRIB2 message, naming its byte offset:
`N:byteOffset:d=YYYYMMDDHH:PARAM:LEVEL:STEP[:QUALIFIER]:`. Since a GRIB2
message is self-delimited (`GRIB`...`7777`), the byte range between one
record's offset and the next's is a complete, independently decodable
file. Reading the small idx, finding one record, and range-fetching
just that slice turns a 500MB download into a few-hundred-KB one.

## Built (2026-09-26): the shared primitive

- `scwx::util::grib_idx` (`wxdata/include/scwx/util/grib_idx.hpp` +
  `source/scwx/util/grib_idx.cpp`) -- pure, network-free parsing:
  `ParseIdx()`, `RangeForRecord()`, `FindRecord()`, `ToRangeHeader()`.
  No AWS/network dependency, fully unit-tested against real captured
  idx text (including NBM's real ambiguous-shortName case below).
- `AwsNexradDataProvider` (the base every S3-backed provider --
  Mrms/Rtma/Rrfs today -- already derives from) gained:
  - `DownloadObjectRange()` -- ranged `GetObjectRequest` via the AWS
    SDK's own `SetRange()`, sharing its request-building/cancellation/
    progress-callback logic with the existing `DownloadObject()` through
    a new private `DownloadObjectImpl()` (refactored, not duplicated).
  - `DownloadObjectString()` -- small sidecar files (the idx text) into
    memory, no throwaway temp file.
  - `DownloadGribMessageByIndex()` -- the actual one-call primitive:
    fetch `key + ".idx"`, parse, find the record, range-download it.
    Any current or future subclass gets this for free.

**Real wrinkle already found and handled, not just anticipated**: NBM's
own idx has several records sharing one parameter+level, differing only
by a trailing qualifier (`CAPE:surface:1 hour fcst:` vs.
`CAPE:surface:1 hour fcst:ens std dev`) -- eccodes shortName alone can't
tell them apart (both are `cape`). `FindRecord()`'s `qualifier` argument
matches the idx's own literal trailing text, solved at the byte-range
layer before eccodes is ever involved, so `decode_grib` never sees the
ambiguity at all -- a cleaner fix than extending `FindMessage()` again
(the RRFS APCP precedent) since here the file being decoded contains
only the one message you asked for.

**Verified live** against two independently-run, differently-shaped
real sources, not mocked: `AwsNexradDataProviderTest.
DownloadGribMessageByIndexGfs` (real GFS PRMSL field, confirms output
starts `GRIB`/ends `7777` and is under 5MB, not a fallback to the whole
~500MB file) and `...NbmQualifier` (the exact CAPE/ens-std-dev case
above). Plus 6 pure-parsing unit tests. Full suite otherwise unaffected.

## Survey: what else is out there (live-checked 2026-09-26)

Checked every candidate against the real bucket, not assumed from a
model's name. Bucket names below are exact, confirmed live.

| Model | Bucket | Layout | Cadence | Idx? | Notes |
|---|---|---|---|---|---|
| GFS | `noaa-gfs-bdp-pds` | `gfs.YYYYMMDD/HH/atmos/gfs.tHHz.pgrb2.0p25.fFFF` | 00/06/12/18z, f000-f384 | Yes | Global, ~500MB/file. Non-uniform hour step (hourly to f120, 3h to f240, 12h to f384) -- the existing hour-slider assumes a flat range. |
| NBM (Blend) | `noaa-nbm-grib2-pds` | `blend.YYYYMMDD/HH/core/blend.tHHz.core.fFFF.co.grib2` | hourly, all 24 | Yes | CONUS, ~160MB/file. Ambiguous-qualifier case above. |
| HRRR | `noaa-hrrr-bdp-pds` | `hrrr.YYYYMMDD/conus/hrrr.tHHz.{wrfnat,wrfsfc}fFF.grib2` | hourly | Yes | `wrfnat` (native levels) ~700MB; `wrfsfc` (surface/common pressure levels, the practically useful one) ~145MB. Also an `alaska/` domain. |
| NAM | `noaa-nam-pds` | `nam.YYYYMMDD/nam.tHHz.awphysFF.tm00.grib2` (+ several AFWA/other sub-products) | 00/06/12/18z | Yes | CONUS 12km, ~55MB/file for the main product; many sub-products exist, not all explored. |
| GEFS | `noaa-gefs-pds` | `gefs.YYYYMMDD/HH/atmos/pgrb2ap5/gepNN.tHHz.pgrb2a.0p50.fFFF` | 00/06/12/18z, out to f384+ | Yes | 30 members + control (`gec00`), 0.5deg, ~13-15MB/member/hour -- an actual ensemble, not just one more deterministic model; also has `chem/`, `wave/` sub-trees. |
| CFS | `noaa-cfs-pds` | `cfs.YYYYMMDD/HH/6hrly_grib_NN/flxfYYYYMMDDHH.NN.<init>.grb2` | 4 members, 6-hourly | Yes | Seasonal climate model (months out) -- real, idx-fetchable, but not an obvious fit for a severe-weather radar companion app. Low priority. |
| URMA | `noaa-urma-pds` | -- | -- | -- | **Bucket exists but is stale**: only `akurma.*` (Alaska) present, and only through 2018-10-01. Real CONUS URMA is not live in this bucket today -- don't build against it without re-verifying where it actually lives now. |
| NDFD | `noaa-ndfd-pds` | `opnl/AR.<region>/VP.NNN-NNN/ds.<elem>.bin` | continuous | **No** | Real GRIB2 despite the `.bin` extension, but no idx sidecar seen -- doesn't fit this primitive as-is; would need whole-file download or a different slicing approach. |
| NBM GeoTIFF | `noaa-nbm-pds` | pre-rendered GeoTIFF, not GRIB2 | -- | N/A | Already noted in `supercell-wx-nbm-open-data` memory -- wrong bucket for this pipeline, GDAL path not eccodes. |

## Not done / next steps

- No new `GribCategory` has been activated yet for any of these --
  this pass built and proved the *shared* fetch primitive only. Each
  model still needs its own onboarding pass (provider class,
  `ProductConfig` table, dock-widget tab, `MakeProvider()` arm --
  the same 5 touch points RRFS's own onboarding hit), one at a time.
- GFS's non-uniform forecast-hour step and NAM's many sub-products
  are real wrinkles flagged above, not yet designed around.
- URMA needs re-verification of where (if anywhere) it's actually
  published live before it's worth pursuing further.
- NDFD doesn't fit this primitive and wasn't pursued further.

See [[supercell-wx-grib-idx-primitive]] (memory) for the full story.
