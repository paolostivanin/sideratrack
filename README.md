# Stellastack

Linux astrophotography stacking, from camera exposures to linear masters.

Stellastack provides a Qt 6 desktop application and a command-line interface for
**Import → Calibrate → Analyze → Review → Stack → Export**. Each project covers one target,
camera and optical setup, with exposures from one or more nights.

**Project status:** version 0.2.0 is an initial implementation for manual
validation. Native Wayland/manual GUI validation and comparison with Siril
remain to be completed. See [validation results](docs/VALIDATION.md).

## What it does

- Calibrates lights with biases, darks, dark-flats and flats, and creates reusable
  calibration masters.
- Measures star quality and alignment, with previews, star overlays, blinking
  and configurable frame grading.
- Stacks one-shot color (OSC) exposures into a linear RGB master, or monochrome
  LRGBSHO exposures into separate filter masters on a shared reference grid.
- Reads FITS, Rice-compressed FITS and mono/RGB XISF; exports FITS, lossless
  compressed FITS and XISF, with optional coverage, weight and rejection maps.
- Preserves original inputs and supports cancellation and recovery.

Color composition and image finishing happen in your image editor. Mosaics,
GPU processing, drizzle, live stacking and proprietary RAW support are deferred.

## Build and launch

This repository distributes source; downstream distributions provide binary
packages. Building requires:

- CMake ≥3.25, Ninja, pkg-config and a C++20 compiler.
- Qt ≥6.5 Core and Widgets; Qt Test is also required by the default GUI build,
  which enables tests.
- CFITSIO, libxml2, OpenSSL, zlib, LZ4, Zstandard, SQLite, oneTBB, Eigen and
  librtprocess development packages.

On openSUSE, Qt/demosaicing packages include `qt6-widgets-devel`,
`qt6-test-devel` and `librtprocess-devel`. Install your distribution's development
packages for the remaining dependencies. SEP 1.4.1 is bundled; distribution
maintainers can select their SEP library with `-DSTELLASTACK_SYSTEM_SEP=ON`.

From the source directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/stellastack
```

Open an existing project with `./build/stellastack /data/target.stella`.
For a GUI-free build, add `-DSTELLASTACK_BUILD_GUI=OFF`; Qt Core remains required.
To omit tests and their dependencies, add `-DBUILD_TESTING=OFF`.

Install binaries, desktop entry, icon and license notices with
`cmake --install build --prefix /your/prefix`. Install the GUI and CLI beside
each other.

## Your first desktop stack

1. Create a project and import your lights and calibration files. Check frame
   type, filter, night and camera metadata; correct them in the frame table or
   with **Edit selected**.
2. Open **Calibration assignments**, review automatic assignments and resolve ambiguous
   matches. Mark imported calibration masters correctly.
3. Run **Calibrate** to save prepared lights, then **Analyze**. Review star measurements and previews, and blink frames to
   identify problems. Preview stretching does not change image data.
4. Exclude unwanted frames, or configure quality limits in **Settings & grading**.
   Fix or exclude unreadable and unregistered frames before stacking. Processing
   without required calibration needs an explicit setting.
5. Run **Stack** into a new output directory, then **Export** the completed
   masters in your preferred format. Continue finishing in your image editor.

For calibration matching rules, reusable master creation, grading and stacking
options, see the [desktop workflow guide](docs/WORKFLOW.md).

**Cancellation and recovery:** after cancelled calibration, run **Calibrate** again;
after cancelled analysis, run **Analyze** again.
After cancelled integration, use **Resume** with the same output directory.
Completed stages are saved and verified before reuse; existing unrelated outputs
are never silently overwritten.

## CLI quick start

The CLI uses the same processing engine and runs without a display. Replace the
example paths with your own project, input folders and output directories. Include
calibration frames in the imported inputs and review assignments before analysis.

```sh
./build/stellastack-cli init /data/target.stella
./build/stellastack-cli import /data/target.stella /data/night1 /data/night2
./build/stellastack-cli calibration /data/target.stella
./build/stellastack-cli calibrate /data/target.stella --json
./build/stellastack-cli analyze /data/target.stella --json
./build/stellastack-cli list /data/target.stella
./build/stellastack-cli stack /data/target.stella /data/masters --json
./build/stellastack-cli export /data/target.stella /data/export --format xisf-zstd
```

Use `./build/stellastack-cli --help` for commands and global options. See the
[CLI reference](docs/CLI.md) for frame edits, settings, calibration-master
creation, resume, inspection and conversion.

## Formats and resource requirements

Inputs are read-only. Projects retain metadata edits, selections and processing
state; stacking refuses inputs changed after analysis. Outputs are verified
before publication.

The default RAM budget is 60% of available memory. Preflight conservatively
requires about 2 GiB for a 26 MP mono or color filter array (CFA) exposure, or
4.6 GiB for 61 MP; already-RGB input requires three times those amounts. These
are processing budgets, not OS-enforced limits on resident memory.

The scratch-cache budget defaults to the smaller of 100 GiB and 25% of available
disk space. Recovery checkpoints and staged exports also need space in the output
directory, independently of that budget. CPU thread and resource limits are
configurable.

Working and output samples are float32; negative values are preserved. FITS
compression is lossless. Big-endian XISF blocks are currently rejected, and native
XISF astrometric properties are not converted to FITS WCS. Ambiguous multi-image
containers must be split before import, except supported PixInsight integration
masters with named rejection/slope maps.

See [formats and processing details](docs/PROCESSING.md) for supported encodings,
calibration units, integrity checks and memory/cache behavior.

## Testing and validation

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSTELLASTACK_BUILD_GUI=OFF -DSTELLASTACK_SANITIZERS=ON
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

A real 222-light, 26 MP QHY268M LRGB project from two observing nights completed
with verified outputs. Measured stacking time was 246.9 seconds, with 7.83 GiB
peak RSS and nearly all 36 logical cores active at peak. This excludes preceding
analysis, and no flats were supplied. This result does not establish performance
superiority or release readiness.

See the [real-camera validation](docs/VALIDATION.md),
[benchmark runner and comparison protocol](docs/BENCHMARKS.md) and
[implementation specification](docs/PLAN.md).

## Licensing

Stellastack is GPL-3.0-or-later. Format-handling code adapted from
[xisf2fits](https://github.com/paolostivanin/xisf2fits) retains its MIT license in
`src/xisf/LICENSE`. Bundled SEP is LGPL-3.0-or-later; its licenses, authors,
upstream commit and local patch are under `vendor/sep`.
