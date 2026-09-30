# Adding your own GRIB models

Any model published as GRIB2 on a public AWS S3 bucket with a wgrib2-style
`.idx` file next to each GRIB2 can be added without rebuilding the app. Each
model is a folder with two files:

```
my-model/
  model.json     where the files are and when they exist (rarely edited)
  products.csv   one row per field to offer (edited in a spreadsheet)
```

Use **Custom Models > Import model...** in the GRIB dock and pick the folder.
The app checks both files first and tells you exactly what is wrong; only a
clean model is copied into its own folder under `grib-models/` in the app's
local data directory. Importing the same model name again replaces it.
`hrrr-conus/` here is a working example.

## model.json

| key | meaning |
| --- | --- |
| `model.name` | Shown in the dock. Must be unique. |
| `source.bucket` | A bare S3 bucket name, not a URL. |
| `source.region` | The bucket's AWS region. Default `us-east-1`. |
| `source.key_pattern` | The object key, with `{yyyymmdd}`, `{hh}` (cycle hour), `{fh2}` and `{fh3}` (forecast hour, 2 or 3 digits). |
| `source.cycle_hours` | Hours (UTC) the model starts a run, e.g. `[0, 6, 12, 18]`. |
| `source.min_forecast_hour` | First hour that exists. Default `0` (NBM has no hour 0). |
| `source.max_forecast_hour` | Last hour that exists. |
| `source.forecast_hour_step` | Spacing between published hours. Default `1`. |
| `source.availability_lag_hours` | How old a cycle must be before its files are all there. Default `3`. |
| `defaults` | Display settings a product inherits when its own cell is blank (see below). |

Only `.idx` index files are supported. Key patterns may contain only letters,
digits, `.`, `_`, `-`, `/` and the four placeholders.

## products.csv

Required columns: `name`, `parameter`, `level`, `short_name`. Optional:
`qualifier`, `type`, `units`, `quantity`, `color_offset`, `color_scale`,
`no_data_threshold`, `contour_interval`. Column order doesn't matter; a
blank display cell uses the `defaults` from `model.json`. Lines starting with
`#` are ignored. A bad row is skipped with a warning that names the row and
column; the rest of the model still loads.

* `parameter`, `level`, `qualifier` are matched literally against the `.idx`
  (`TMP` / `2 m above ground`). Look them up in any one `.idx` file.
* `short_name` is **eccodes' own name** for that field, which is often not the
  idx's: TMP at 2 m is `2t`, DPT is `2d`. Get it with
  `grib_ls -p shortName file.grib2` on a downloaded record.
* `quantity` is one of `none`, `temperature_kelvin`,
  `speed_meters_per_second`, `accumulation_millimeters`, `pressure_pascals`;
  the app converts those to your unit settings. `none` shows the raw value with
  `units`.
* Colour: a value maps to the palette at `(value - color_offset) / color_scale`,
  so the palette spans `color_offset` to `color_offset + color_scale`.
* `no_data_threshold`: cells below it are transparent. **Set this for any field
  where zero means "nothing here"** (rain, CAPE, reflectivity); otherwise the
  whole domain is painted the palette's bottom colour.
* `type` is `fill`, or `contour` (which needs `contour_interval`).

The forecast hour in an `.idx` STEP column changes for every file, so it is not
a column: the app always fetches the hour you selected.
