# SideraStack

Linux astrophotography stacking, from camera exposures to linear masters.

C++20, Qt 6 Widgets, GPL-3.0-or-later. The workflow is
**Import → Analyze → Review → Stack → Export**. One target, camera and optical
setup per project, with multiple nights and OSC or mono LRGBSHO filters.

## Build and launch

Development dependencies: CMake ≥3.25, a C++20 compiler, Qt ≥6.5 Core/Widgets,
CFITSIO, libxml2, OpenSSL, zlib, LZ4, Zstandard, SQLite, oneTBB, Eigen and
librtprocess. SEP 1.4.1 is bundled with documented correctness fixes. Distribution
maintainers can select their SEP library with `-DSIDERASTACK_SYSTEM_SEP=ON`.

On openSUSE, Qt/demosaicing packages include `qt6-widgets-devel`,
`qt6-test-devel` and `librtprocess-devel`. Use your distribution's development
packages for the remaining dependencies.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/siderastack
```

Open an existing project with `./build/siderastack /data/target.sidera`.
Install binaries, desktop entry, icon and license notices with
`cmake --install build --prefix /your/prefix`. The GUI and CLI must be installed
beside each other. This repository distributes source; downstream distributions
provide binary packages.

For a GUI-free build, set `-DSIDERASTACK_BUILD_GUI=OFF`. Disable test dependencies
with `-DBUILD_TESTING=OFF` when building distribution packages.

## Desktop workflow

1. Create a project and import files or a folder recursively. Check frame type,
   filter, night, Bayer pattern, camera/exposure metadata and calibration state.
   Double-click type/filter/night to correct them, or use **Edit selected** for
   bulk acquisition metadata corrections. Mark imported calibration masters and
   indicate whether a dark master already has its bias removed.
2. Open **Calibration** to review assignments. Automatic matching checks camera,
   dimensions, CFA, binning, gain/offset, readout mode, exposure, temperature and
   flat filter. Same-night groups take priority. Resolve ambiguous masters or
   flat sessions with explicit overrides. Flats use a matching dark-flat or
   bias; dark scaling is disabled. CFA calibration happens before RCD debayering.
   **Create masters** exports stacked biases, darks, dark-flats and normalized
   flats, independently of light stacking. It uses selected calibration frames,
   or all included raw calibration frames when none are selected. Choose **Bias**
   or **Dark-flat** explicitly for flats; Automatic prefers dark-flat. Missing
   chosen calibrations cause an error instead of silently using the other method.
   Calibration-only projects work without lights or prior analysis. Import the
   exported masters into a project when you want to reuse them.
3. **Analyze**, then review FWHM, HFR, eccentricity, star count, background,
   noise, transparency and alignment residuals. Sort numeric metrics, click the
   metric plot, zoom/pan previews, show star overlays and blink selected frames.
   Previews use calibrated cached exposures when available and otherwise show
   the raw exposure; the label identifies which is displayed. Stretching affects
   the preview only.
4. Exclude unwanted frames or set FWHM/eccentricity limits and a per-filter
   FWHM retention percentage in **Settings & grading**. Manual inclusion overrides
   grading thresholds. Unreadable or unregistered included frames stop stacking
   until fixed or excluded. Missing calibration requires an explicit setting.
5. **Stack** into a new output directory. OSC produces a linear RGB master;
   mono produces separate filter masters on the same reference grid. Inverse
   variance weighting, same-filter stellar/background normalization and iterative
   sigma clipping are configurable. Statistical rejection is disabled wherever
   fewer than ten valid exposures contribute. Uncovered/invalid samples are NaN.
6. **Export** completed masters as FITS, lossless compressed FITS, XISF, or
   Zstandard/shuffled XISF. Coverage, weight and rejection maps are optional.
   Continue finishing/color composition in your image editor.

**Cancel** saves completed stages/bands. After cancelled analysis, run Analyze
again; after cancelled integration, use Resume with the same output directory.
The last stack directory is saved in the project. Completed masters and maps
are verified before reuse; an interrupted output publication is recoverable.
Existing unrelated outputs are never silently overwritten.

## CLI

The same engine runs without a display. Paths and IDs are passed as arguments:

```sh
./build/siderastack-cli init /data/target.sidera
./build/siderastack-cli import /data/target.sidera /data/night1 /data/night2
./build/siderastack-cli calibration /data/target.sidera
./build/siderastack-cli masters /data/target.sidera /data/calibration-masters --flat-calibration bias
./build/siderastack-cli analyze /data/target.sidera --json
./build/siderastack-cli list /data/target.sidera
./build/siderastack-cli edit /data/target.sidera --ids 17,24 --set '{"selection":-1}'
./build/siderastack-cli stack /data/target.sidera /data/masters --json
./build/siderastack-cli resume /data/target.sidera /data/masters --json
./build/siderastack-cli export /data/target.sidera /data/export --format xisf-zstd
```

`settings PROJECT` prints processing settings. Use `--set` to merge a JSON
object; budgets are bytes. `edit --set` supports type (`kind`), filter, session,
master, biasSubtracted, selection and acquisition `header` overrides. Selection
is -1 excluded, 0 automatic, 1 manually included. Header edits merge into existing
metadata; null removes a key. Calibration overrides use `SS_BIAS`, `SS_DARK`,
`SS_FLAT`, or `SS_DARKFLAT` with comma-separated IDs or `"none"`.

```sh
./build/siderastack-cli settings /data/target.sidera \
  --set '{"memory":8589934592,"scratch":107374182400,"threads":18}'
./build/siderastack-cli edit /data/target.sidera --ids 31 \
  --set '{"header":{"BAYERPAT":"RGGB","EXPTIME":60}}'
./build/siderastack-cli convert exposure.fits.fz exposure.xisf --format xisf-zstd
```

Use `inspect FILE` to inspect decoded geometry, metadata and source SHA-256.
SIGINT/SIGTERM cancel work at processing checkpoints (exit code 130).

## Integrity and resource handling

Inputs are read-only. SQLite projects preserve selection, metadata edits,
measurements, catalogs, transforms and recovery manifests. SHA-256 dependencies
invalidate stale stages; stacking refuses inputs changed after analysis. Image
checksums and exact decoded lengths are checked. Export uses verified temporary
files, fsync and atomic publication without replacement.

Read FITS/Rice FITS and mono/RGB XISF with UInt8/16/32 or Float32/64,
planar/interleaved pixels, zlib, LZ4/LZ4HC, Zstandard, shuffling and subblocks.
The converter preserves stored numerical values. Calibration converts images
with a known nominal range to common relative units, including integer camera
exposures and normalized PixInsight float masters. Unknown physical-unit images
retain their values; mixing unknown and normalized units requires an explicit
**Scale to relative units** correction. Working/output samples are float32;
accumulation and rejection statistics use float64, and negative values are
preserved. FITS export compression is lossless GZIP_2 without float
quantization. FITS scaling, CFA offsets, long strings and reference WCS are
handled. PixInsight masters containing one `integration` image plus named
rejection/slope maps are supported; other ambiguous multiple-image containers
must be split before import. Big-endian
XISF blocks are currently rejected, and native XISF astrometric properties are
not converted into FITS WCS; use FITS WCS keywords for astrometric metadata.

Default RAM budget is 60% of available memory; scratch is the smaller of
100 GiB and 25% of available disk space. All detected logical CPU threads are
enabled by default, with a configurable limit. TBB parallelizes calibration,
independent frame measurements, resampling and per-pixel statistics. Reading,
verification and publication include serial work, so every stage cannot keep
every core busy continuously. Processing uses bounded bands and sequential
full-frame readers. A RAM cache retains calibrated frames for repeated rejection
passes after reserving space for accumulators, masters and decode workspaces;
the disk cache is separately bounded. Cache shortages repeat reads with the same
arithmetic and fixed contribution order. RAM is allocated for useful work, not
filled artificially. Large compressed XISF blocks need full decode buffers. Preflight
conservatively requires approximately 80 bytes per input sample (about 2 GiB
for 26 MP mono/CFA, 4.6 GiB for 61 MP mono/CFA, three times those amounts for
already-RGB input). This is a processing budget, not an OS-enforced RSS limit.
Recovery checkpoints and staged exports use space in the output directory,
independently of the scratch-cache budget. Non-reentrant CFITSIO builds are
serialized for safety. No nested RCD thread pool is enabled.

## Testing and performance status

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSIDERASTACK_BUILD_GUI=OFF -DSIDERASTACK_SANITIZERS=ON
cmake --build build-sanitize --parallel 2
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitize --output-on-failure
```

The GUI test requires Qt Test. Independent format/workflow tests require Python,
NumPy, Astropy, `lz4`, and `zstandard`; install these in a virtual environment and
set `-DPython3_EXECUTABLE=/path/to/venv/bin/python` when configuring. CMake skips
that suite if the modules are missing. Tests cover independently encoded formats,
known-truth calibration/registration, OSC/mono outputs, multiple nights,
cache/thread equivalence, corruption, cancellation, interrupted publication,
failed output writes and a 10,000-frame GUI model.

This is an initial implementation for manual validation, not a claim of release
readiness or performance superiority. A real 222-light, 26 MP QHY268M LRGB project
from two observing nights completed with verified outputs. Its measured stack
time was 246.9 seconds, with 7.83 GiB peak RSS and nearly all 36 logical cores
active at peak; this excludes the preceding analysis. No flats were supplied.
Native Wayland/manual GUI and Siril comparison validation remain to be completed.
See the [real-camera validation](docs/VALIDATION.md), the
[benchmark runner and comparison protocol](docs/BENCHMARKS.md) and the
[implementation specification](docs/PLAN.md). Mosaics, GPU processing, drizzle,
live stacking, proprietary RAW and image finishing are deferred.

## Licensing

SideraStack is GPL-3.0-or-later. Format-handling code adapted from
[xisf2fits](https://github.com/paolostivanin/xisf2fits) retains its MIT license in
`src/xisf/LICENSE`. Bundled SEP is LGPL-3.0-or-later; its licenses, authors,
upstream commit and local patch are under `vendor/sep`.
