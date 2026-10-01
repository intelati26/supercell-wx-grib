# User-importable model config files: scope

Status (2026-09-30): implemented end to end on branch `user-models` (stacked on `hour-picks`): parse/validate, a config-driven provider, a registry, a `Custom Models` GRIB category and dock section with Import. See "As built" at the end.

## Ask

Let users add GRIB model sources without a rebuild, via plain-text config
files, one per model. Each file states, at minimum:

1. **Model name** (what the dock shows; also the category label).
2. **Index-file summary**: how to find one field inside the model's bundle,
   i.e. the `.idx` vocabulary (wgrib2 `PARAM`/`LEVEL`/`STEP`/`QUALIFIER`).
3. **How to display the type**: color scale, units, contour vs fill.

## What exists today (the thing to externalize)

`GribCategory` is a closed enum (`Mrms, Rtma, Rrfs, Nbm`) and every product
is a C++ row in `kMrmsProducts_/kRtmaProducts_/kRrfsProducts_/kNbmProducts_`
(`grib_manager.cpp`, struct `ProductConfig`). Fields split cleanly:

| Concern | `ProductConfig` fields | Config-file section |
| --- | --- | --- |
| Identity | `displayName` | `[model]` / `[[product]] name` |
| Locate in bundle | `s3Product`, `nbmParameter`, `nbmLevel`, `nbmQualifier`, `rrfsFileFamily` | `[source]`, `[product.index]` |
| Decode selection | `shortName`, `typeOfLevel`, `topLevel`, `bottomLevel`, `startStep`, `lengthOfTimeRange`, `derivedIndex` | `[product.decode]` |
| Display | `colorOffset`, `colorScale`, `noDataThreshold`, `quantity`, `units`, `contourInterval` | `[product.display]` |

Existing primitives that already do the hard part for idx-style sources:
`scwx::util::grib_idx::{ParseIdx, FindRecord, RangeForRecord}` and
`AwsNexradDataProvider::DownloadGribMessageByIndex()`. Any S3 bucket that
ships a `.idx` sidecar needs no new C++ to be fetched.

## Proposed format (DECIDED: a folder per model = `model.json` + `products.csv`)

Boost.JSON is already a dependency, and fields are edited in bulk in a
spreadsheet, so the config is split by how often each half changes:

```
grib-models/nbm-conus/
  model.json      settings; rarely edited
  products.csv    one row per field; edited in Excel/LibreOffice
```

`model.json`:

```json
{
  "model":  { "name": "NBM CONUS", "kind": "idx", "always_one_active": false },
  "source": {
    "bucket": "noaa-nbm-grib2-pds",
    "key_pattern": "blend.{yyyymmdd}/{hh}/core/blend.t{hh}z.core.f{fh3}.co.grib2",
    "cycle_hours": [0, 6, 12, 18],
    "max_forecast_hour": 36,
    "idx_suffix": ".idx"
  },
  "defaults": { "type": "fill", "units": "K", "quantity": "temperature_kelvin",
                "color_offset": 250, "color_scale": 70 }
}
```

`products.csv` (columns matched by header name, any order, case-insensitive;
required `name,parameter,level,short_name`; optional `qualifier,type,units,
quantity,color_offset,color_scale,no_data_threshold,contour_interval`; a blank
display cell inherits `defaults`):

```
name,parameter,level,qualifier,short_name,units,color_offset,color_scale
2m Temperature,TMP,2 m above ground,,2t,,,
CAPE Ens Std Dev,CAPE,surface,ens std dev,cape,J/kg,0,4000
```

Rules: `#` rows and blank rows are ignored (but still count for row numbers);
UTF-8 BOM, CRLF/LF and RFC 4180 quoting accepted; `,` vs `;` delimiter is
sniffed and a decimal comma is accepted only with `;`; a bad row is skipped
with a warning naming `row N (column)`; a duplicate `name` skips the later
row; zero valid rows is fatal; a `products` key in the JSON is an error.
The idx STEP text is intentionally not a column: it changes per forecast hour
so it can never match more than one file. Placeholders in `key_pattern`:
`{yyyymmdd}`, `{hh}`, `{fh2}`, `{fh3}`.

Planned: `DraftConfigFromIdx(idxText, url)` -- the user supplies a sample `.idx`
and gets both files drafted (one CSV row per record, `short_name` left blank
for the user to fill; cells starting `=+-@` sanitized on write).

## Design decisions to settle before building

1. **Enum to registry.** `GribCategory` and the `switch`es in `Products()`,
   `MakeProvider()`, `StartsWithAllProductsInactive()`, and the
   `GribManager::Instance(category)` map assume four fixed values. User
   models need a dynamic id (e.g. `struct GribCategoryId { int builtin;
   std::string custom; }` or widening the enum to an int-backed handle
   allocated at load). This is the biggest change; the rest is plumbing.
   The "any new models start with no active product" rule is already the
   default (`StartsWithAllProductsInactive` is true for everything except
   Mrms/Rtma), so custom models inherit it.
2. **Provider genericization.** `NbmDataProvider` is effectively a generic
   "S3 key pattern + idx" provider already; extract a
   `ConfiguredIdxProvider(bucket, key_pattern, cycles, max_fh)` and re-express
   NBM (and GFS/HRRR/NAM/GEFS from the survey in
   `grib-idx-model-sources-plan.md`) as config. Built-in NBM could then ship
   as a bundled config file, which proves the format.
3. **Scope of `kind`.** Recommend v1 = `idx` only. Whole-file sources (MRMS
   gzip per product, RTMA, RRFS prslev/2dfld) stay built-in: their fetch
   rules (gzip, per-product folders, two-file families) are code, not data.
   `derivedIndex` (STP/SHIP) also stays built-in; config can reference a
   built-in derived name at most.
4. **Where files live / import UX.** `<appdata>/grib-models/<model>/`, scanned
   at startup, plus an "Import model..." button (copies the file in and
   hot-reloads). Validation errors surface in the status bar, per file, and
   never abort startup.
5. **Validation & safety.** Files are untrusted text: schema-validate
   (required keys, numeric ranges, `key_pattern` only allows the known
   placeholders), restrict `bucket` to public S3 and `https` URLs, cap idx
   size, never shell out with config strings (decode_grib gets `shortName`
   etc. via argv, already the case, but must stay validated).
6. **Persistence.** Active-product selection per model is keyed by model
   name; renaming a file orphans saved state (acceptable; document it).
7. **Tests.** Loader unit tests (good/bad files, placeholder expansion),
   plus one live-marked test that a bundled idx config resolves the same
   byte range as the hard-coded NBM row.

## Suggested phasing

1. Parse + validate a config into `ProductConfig` rows (pure function, no
   UI); round-trip existing NBM table through it as a test oracle.
2. Introduce the dynamic category handle; migrate built-ins onto it.
3. `ConfiguredIdxProvider` + NBM re-expressed as bundled config.
4. Dock UI: list custom models, Import button, per-file error display.
5. Docs + example files (HRRR, GFS) shipped in the release.

## Open questions for the user

- Should a config be able to declare a *new display kind* (e.g. wind barbs
  from two fields), or only fill/contour for scalar fields in v1?
- Should users be able to override built-in product display settings
  (color ranges) with the same mechanism?

## Progress

Branch `model-config-files` (worktree `/home/mitch/Claude/wt/model-config`): phase 1 -- `wxdata/.../util/grib_model_config.{hpp,cpp}`: `ParseModelSettings`, `ParseProductsCsv`, `LoadModelFolder`, `ExpandKeyPattern` (pure, no throw, no network) + 14 tests (`test/.../grib_model_config.test.cpp`). `ValidateKeyPattern` deliberately left for the user to write; its reject-list test fails until they do. Next: `DraftConfigFromIdx`, then the enum-to-registry change.


## As built (2026-09-30, branch `user-models`)

Decisions made while building, where they differ from or settle the plan above:

- **Enum vs registry (decision 1): neither.** One new category,
  `GribCategory::User` ("Custom Models"), serves whichever model is
  selected, with a model picker in its dock section. No enum-to-registry
  refactor; a model is data. Switching models is `GribManager::ReloadUserModel()`
  (deactivate everything, reset cycle/hour, rebuild the product table).
  Product tables are never freed (a deque), since fetch threads may still
  hold references across a switch.
- **Provider genericization (decision 2).** `IdxModelProvider` is the common
  interface (cycle/hour selection plus the model's rules: `BuildKeyFor`,
  `MinForecastHourFor`, `MaxForecastHourFor`, `SnapForecastHourFor`,
  `RunsCycleAt`). `NbmDataProvider` implements it with its compiled-in rules
  (unchanged); `ConfiguredIdxProvider` implements it from a model.json. The
  idx code paths in `GribManager` were renamed `Nbm*` -> `Idx*`. Oracle test:
  NBM's real short-cycle rules as a config give identical keys and hour
  snapping to the hard-coded provider; a live test shows a user config
  describing NBM's bucket decodes the same field (same mean) as built-in NBM.
- **model.json additions.** Optional `region`, `min_forecast_hour`,
  `forecast_hour_step`, `availability_lag_hours`. `bucket` must be a bare S3
  name, `region` an AWS region, `idx_suffix` must be `.idx` (the fetch path
  only builds that), `quantity` one of five known names; all rejected with a
  reason at import.
- **Where files live / import UX (decision 4).** `<local app data>/grib-models/<slug>/`.
  `UserModelRegistry` scans it and imports: validate first, then copy
  `model.json` + `products.csv` only, into a folder named from a sanitised
  slug of the model name; refuses symlinks and files over 1 MB; copies nothing
  on error. The dock shows errors/warnings in dialogs and a "Not loaded" note
  for broken folders.
- **Verification.** Shipped example `docs/grib-models/hrrr-conus` was built
  from a real HRRR file (idx names, eccodes short names, ranges, no-data
  cutoffs) and a live test imports that exact folder (no warnings allowed) and
  decodes a real field. The dock is tested by rendering the real widget
  offscreen (`wxtest` now runs a `QApplication` on the offscreen platform).

### Still open

- **Selection isn't persisted.** The selected model and which products are
  checked reset each launch (the plan's decision 6). The first model
  alphabetically is selected at startup.
- **Only wgrib2-style `.idx`.** ECMWF's open-data bucket
  (`s3://ecmwf-forecasts`, eu-central-1) uses a JSON-lines `.index`; its
  format was not confirmed (the bucket throttled), so a new index parser
  would be needed. eccodes itself decodes ECMWF GRIB fine.
- **`DraftConfigFromIdx`** (draft both files from a sample `.idx`) not built.
- **Native file dialog untested.** The import wiring after the dialog is
  tested; the `QFileDialog` call itself is not, and Windows paths with
  non-ASCII characters are unverified.
- **No built-in-NBM-as-config** (phase 3's "prove the format" step): NBM stays
  compiled in because its extended cycles use a non-uniform hour step a single
  `forecast_hour_step` can't express.
