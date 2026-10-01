# Developer handoff: setup, push and merge workflow

For a developer (or an AI assistant) picking this project up on a new machine.
It covers what is specific to this fork. For the code base itself read
[`AGENTS.md`](../AGENTS.md) (architecture, namespaces, Conan/CMake, tests, style)
and [`CONTRIBUTING.md`](../CONTRIBUTING.md); this document does not repeat them.

Written 2026-10-01 at the state of `develop` commit `c1b65219`.

## 1. What this is

`intelati26/supercell-wx-grib` is a fork of
[`dpaulat/supercell-wx`](https://github.com/dpaulat/supercell-wx) (a C++20/Qt6
NEXRAD radar viewer) that adds a GRIB layer family on top: MRMS, RTMA, RRFS and
NBM fields, user-imported models, gridded hodographs and wind barbs, WebP export,
and legends on exports. **Upstream is never touched**: nothing here is pushed to,
or opened as a pull request against, `dpaulat/supercell-wx`.

Remotes (this is the layout every command below assumes):

| Remote | URL | Use |
|---|---|---|
| `fork` | `https://github.com/intelati26/supercell-wx-grib` | push here; `develop` is the integration branch |
| `origin` | `https://github.com/dpaulat/supercell-wx` | upstream, fetch only; the base for the format check |

Documents that describe the features: [`grib-field-downloads.md`](grib-field-downloads.md)
(how RRFS/RTMA data is fetched by byte range and how to add a product) and
[`grib-models/README.md`](grib-models/README.md) (user-imported model configs).

## 2. Setting up a machine

### What was used (recorded, not re-derived)

This is read from the working machine's build directory and package list, not
from a fresh install; a clean-machine run has **not** been done, so expect to
fix small things.

- Arch Linux, GCC 16.2.1, CMake 4.4, Ninja 1.13, Qt 6.11.2 from the system
  packages, Conan 2.32 (`pip install --user conan`, in `~/.local/bin`),
  Python 3.14, clang/clang-format/clang-tidy 22.1.8 (CI uses 21), jemalloc
  (profiling only), eccodes 2.48 (needed for `decode_grib`).
- Optional: libwebp's `cwebp`/`img2webp` (`libwebp-utils` on Arch,
  `apt install webp`) for WebP export tests; they are skipped if absent.

### Clone and remotes

```bash
git clone -o fork --recurse-submodules https://github.com/intelati26/supercell-wx-grib.git supercell-wx
cd supercell-wx
git remote add origin https://github.com/dpaulat/supercell-wx   # upstream, fetch only
git fetch origin
gh auth login        # needed for `gh` (CI, PRs); must be the intelati26 account to push
```

`test/data` is a submodule with golden files; keep it at the commit the tree
records (`git submodule update --init test/data`). A stale `test/data` makes
unrelated tests fail.

### Configure and build

`tools/setup-linux-ninja-release.sh` is upstream's route (it assumes GCC 13 and
Qt under `/opt/Qt`). The working machine instead has this recorded in its CMake
cache, which is the whole configuration:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=external/cmake-conan/conan_provider.cmake \
      -DCONAN_HOST_PROFILE="default;auto-cmake" -DCONAN_BUILD_PROFILE=default
conan profile detect     # once, if ~/.conan2/profiles/default does not exist
cd build-release
ninja supercell-wx decode_grib wxtest
```

The first configure runs Conan (`--build missing`) and takes a long time; build
output stays in `build-release/` (about 2 GB). The binary is
`build-release/Release/bin/supercell-wx`, with `decode_grib` beside it (the app
launches it by path; without it GRIB layers never draw).

### Run

```bash
build-release/Release/bin/supercell-wx
```

- Settings and layers: `~/.local/share/Supercell Wx/` (`settings.json`,
  `layers.json`, `layers.json.version`, `maptiler-cache.db`).
- Download cache: `~/.cache/Supercell Wx/grib/` (the tests use `~/.cache/wxtest/`).
- The map needs a map-provider key: set your own MapTiler key in Settings.
  **Never commit a settings file**; the working machine's contains a key.
- If a binary with a different name is lying around from an older build, run the
  one you just built; a stale binary has cost time before.

### Test

```bash
cd build-release
ctest --exclude-regex "test_mln.*|NtpClient.*|UpdateManager.*"     # serial, as CI does
```

About 500 tests, ~2 minutes, and **they use the network** (NOAA S3, NWS): some
are live checks against real RRFS/RTMA/MRMS data. Do not use `ctest -j`: several
suites write the same scratch file and fail intermittently when run in parallel
(`SettingsManagerTest`, `MarkerModelTest` ...). That is a test-isolation quirk,
not a regression. `wxtest` runs on Qt's offscreen platform (no display needed);
set `SCWX_UI_SNAPSHOT_DIR=/some/dir` to have the widget tests save PNGs.

## 3. The push / merge workflow

This is how every change in this fork was made. **Gate first, then push.**

### 3.1 Start

```bash
git fetch fork origin
git switch -c my-change fork/develop        # or stack on an unmerged branch
```

Long pieces of work were done as a **stack of branches**, each created from the
previous, so a later one could build on an earlier one before it merged. The
whole stack merges by pushing the tip (3.4).

### 3.2 The gate (all three, in this order, before every push)

```bash
# 1. Format only the lines this fork changed relative to upstream
git add -A && git clang-format $(git merge-base HEAD origin/develop)   # fixes in place
git add -u && git clang-format --diff $(git merge-base HEAD origin/develop)  # must print nothing

# 2. Build everything that ships plus the tests
cd build-release && ninja supercell-wx decode_grib wxtest

# 3. Serial test run, exit code and "100% tests passed"
ctest --exclude-regex "test_mln.*|NtpClient.*|UpdateManager.*"
```

- `git clang-format` refuses with unstaged changes; stage (or commit) first.
  It checks only changed lines, which is also how CI checks, so code that was
  never touched again after being added escaped it for a long time; the base is
  the merge-base with **upstream** (`origin/develop`), not the fork.
- **Never put the push after the tests with `;`**: `ctest; git push` pushes even
  when the tests fail (a failing run once went out that way). Read the result,
  then push.
- clang-tidy is advisory (CI posts review comments; there is no failing
  tidy job). When you run it locally: `build-release/compile_commands.json` is
  maplibre's only, so make a real database with `ninja -t compdb`, strip the
  GCC-only `-mno-direct-extern-access` (clang rejects it and then **silently
  skips analysis**: an early "0 warnings" here was wrong for that reason), and
  ignore `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` and
  `bugprone-throwing-static-initialization` (new in clang-tidy 22, absent from
  CI's 21). Real findings are in the `bugprone-`, `clang-analyzer-` and
  `performance-` families; the rest has been style noise.

### 3.3 Commit and push

```bash
git commit -m "..."        # descriptive; see below
git push fork my-change
```

Commits end with an attribution trailer when an assistant wrote them
(`Co-Authored-By: ...`). Pushing a branch does **not** start CI.

### 3.4 Merge into `develop`

`develop` moves while you work (other sessions merge PRs into it), so:

```bash
git fetch fork
git merge fork/develop            # into your branch; resolve conflicts here
# re-run the whole gate on the merged result (3.2)
git push fork my-change
git push fork my-change:develop   # fast-forward: this is the merge
```

No pull request was used for the stack (earlier work used PRs #12-#16 on the
fork). Either is fine; a PR to `develop` triggers CI automatically.

**Conflict hot spot:** `scwx-qt/source/scwx/qt/manager/grib_manager.cpp`. Two
rules that came out of the last merge:

- Any place that reuses a cached download must call
  `GribManager::UseCachedDownload(path)` (it checks existence **and** touches the
  file so eviction leaves it alone), never a bare `std::filesystem::exists()`.
- Cache paths go through `CacheKeyFor(category, product, s3Key)`: a product that
  downloads only its own fields has its own cache entry
  (`<s3 key>.fields-<product>`), not the S3 object's path. Anything that tracks
  "what is showing / wanted" holds that cache key, not the S3 key.

### 3.5 CI

`.github/workflows/ci.yml` runs on **push to `develop`**, **pull requests to
`develop`**, and **manual dispatch**; three jobs (Windows x64, Windows arm64,
Ubuntu GCC 16), about 90 minutes in all. Pushing a feature branch runs nothing.

```bash
gh workflow run ci.yml --ref my-change -R intelati26/supercell-wx-grib
gh run list -R intelati26/supercell-wx-grib --limit 5
gh run view <id> -R intelati26/supercell-wx-grib --json jobs
```

- The Windows x64 and Linux jobs run the unit tests (with the same exclusions
  as above); arm64 is a cross build and only builds. Live-network tests can fail
  on a flaky external service (an NWS/IEM API test did) without any code fault.
- Pushing to `develop` starts a run, so a documentation-only commit should carry
  `[skip ci]` in its message (GitHub then skips the push-triggered run). Do not
  dispatch extra runs on branches unless asked; the owner builds and runs locally
  and does not wait on Windows CI.
- A run can fail at **Checkout** (a GitHub-side flake) or at **Post Comments**
  (the clang-tidy comment poster, which fails when there is no pull request to
  comment on). Neither is a code failure; read which step failed before reacting.
- Logs expire; fetch what you need soon after a run ends (`gh run view <id>
  --log-failed`).
- Flatpak packaging was removed; AppImage and the Windows MSI/zips remain.

### 3.6 After merging

Branches that were merged are redundant with `develop` and can be deleted from
the fork (`git push fork --delete <branch>`); the five branches of the last stack
(`webp-export`, `visible-legend`, `hodograph-migration`, `idx-slices`,
`rrfs-availability`) have not been.

## 4. Design rules worth knowing before you touch GRIB code

- **Never download a whole RRFS/RTMA object.** An RRFS 2D file is ~350 MB, the
  field a layer reads is 0.5-3 MB. Products read byte ranges named by the
  object's `.idx` (`AwsNexradDataProvider::DownloadGribFieldsByIndex`). The
  selector table `grib_field_selectors.cpp` is **generated**
  (`tools/derive_idx_selectors.py`, usage in `grib-field-downloads.md`); a unit
  test fails for an RRFS/RTMA product that is missing from it.
- **"Latest" RRFS cycle = the newest 3-hourly cycle that really has files**
  (found by listing S3, not a clock guess). Never an hourly cycle: its 2D file
  has half the records (157 vs 315: no CAPE, helicity, clouds, satellite bands,
  and only the 10 m wind of the hodograph's levels) and there is no pressure-level
  file or F000. Hourly cycles can be chosen explicitly and start at F001.
- **RRFS hour picker = hours actually published** (grib2 object *and* its `.idx`),
  not the cycle's nominal horizon: a run still being written has published only
  part of its 84 hours. `RrfsDataProvider::RefreshAvailability()`.
- **The gridded hodograph is a pick in the RRFS product list**, not a layer
  switch (`HodographSelection`); the layer-manager row only places it. Sized by
  screen pixels (`hodograph_zoom.hpp`), not metres.
- **Layer order matters**: GRIB layers sit below "Map Underlay" by default; a
  profile with a GRIB layer under the underlay shows nothing for it.
- **Adding a setting breaks tests**: `test/data/json/settings/*.json` are golden
  files in the `test/data` submodule and every setting is in them. Avoid new
  settings unless you also update that submodule (a separate repository).
- **`scwx::qt::util` shadows `scwx::util`**: after including a `scwx/qt/util/`
  header, `util::grib_idx::...` fails to compile; write `scwx::util::...`.
- **Windows cannot rename over an open file**: swap files with
  `util::ReplaceFileWithRetry`, and close a file as soon as you have read it.
- **Status bar**: `StatusManager::ReportProgress` (downloads, 3 s staleness) and
  `ReportMessage` (notices, 15 s). The line shown is the longest-running item, not
  the latest, so concurrent downloads do not make it flicker.

## 5. Checking the UI without a screen

Qt's offscreen platform cannot draw the OpenGL layers. To see a real map render
and capture it from a terminal, run the app in a private virtual compositor with
a scratch profile (nothing touches your own settings or desktop):

```bash
S=/tmp/scwx-shot; mkdir -p "$S"/{data/"Supercell Wx",cache,config,out}
cp ~/.local/share/"Supercell Wx"/{layers.json,settings.json} "$S/data/Supercell Wx/"
# edit the copies: in settings.json set general.screen_capture_on_refresh=true,
# general.screen_capture_folder="$S/out", general.grid_width/grid_height=1;
# in layers.json show only the layers you want in pane 0 ("displayed"[0]).
cat > "$S/run.sh" <<EOF
#!/bin/bash
export XDG_DATA_HOME=$S/data XDG_CACHE_HOME=$S/cache XDG_CONFIG_HOME=$S/config QT_QPA_PLATFORM=wayland
cd $PWD/build-release/Release/bin && exec timeout 400 ./supercell-wx > $S/app.log 2>&1
EOF
chmod +x "$S/run.sh"
dbus-run-session -- kwin_wayland --virtual --no-lockscreen --no-global-shortcuts \
    --width 2560 --height 1440 --socket scwx-virt --exit-with-session "$S/run.sh"
# a PNG appears in $S/out when radar data arrives (about a minute)
```

There is no input device in that session, so it can capture but not click. Use
a unique `--socket` per run and let a run finish before starting the next.
`xdotool` cannot see or drive Wayland-native windows, and on a locked desktop
`spectacle` captures only the lock screen.

## 6. State at handoff and what is open

Everything below is merged into `fork/develop` unless noted.

Done this stretch: WebP export (`cwebp`/`img2webp`, bundled in release packages),
a legend on every export of a map picture (dock exports, Ctrl+S, capture on
refresh, copy), hodograph as a product pick with pixel sizing and a layers
migration (`layers.json.version`), whole-object downloads replaced by `.idx` byte
ranges (RRFS, RTMA, hodograph, wind barbs), "Latest"/hour picker driven by what
is published, a steady and clickable status line with a pending-downloads window.

Open items:

- **Release**: no tag or GitHub release exists. The artifacts say version 0.6.1;
  the owner has also said "0.0.1". Needs a decision.
- **Windows is unverified on real hardware** for the recent work (the
  Windows file-swap hardening and the range-join download code are covered only by CI).
- **Hodograph**: ticking it with no other RRFS product works, but it needs a
  3-hourly cycle (an hourly cycle's file lacks its levels). "Latest" is always
  suitable; choosing an hourly cycle explicitly leaves the hodograph mostly empty
  and logs `decode_grib failed` lines, which is expected.
- **Loop export** (animated WebP) exists for RRFS only; NBM and custom models
  have no equivalent. The gridded loop export waits for each hour's frame to load.
- Not started: ECMWF open data (bucket is public; its index format differs from
  wgrib2's `.idx`), SHARPpy-style point soundings, per-pane independent GRIB
  products, a separate models window.
- MSI installer contents for `decode_grib` and the bundled WebP tools were never
  inspected on a Windows machine (zips, tarball and AppImage were).
- Stale merged branches on the fork (3.6).
