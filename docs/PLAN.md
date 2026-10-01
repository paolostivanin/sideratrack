# First release specification

## Product

- Independent Linux application: C++20, Qt 6 Widgets, CMake, GPL-3.0-or-later.
- One target, camera, and optical setup per project; multiple nights and filters.
- Up to 10,000 inputs; benchmark 26–61 megapixel mono and OSC data against Siril.
- Output linear RGB OSC masters or separate mono filter masters on a common grid.
- Source distribution; downstream distributions own binary packaging.
- Deferred: mosaics, GPU processing, drizzle, live stacking, proprietary camera
  RAW formats, and finishing/editing.

## Workflow

1. Import files/folders progressively and inspect metadata. Allow bulk corrections
   to frame type, night, filter, calibration state, and selection.
2. Review calibration assignments. Support lights, darks, biases, flats, dark
   flats, and existing masters. Track whether dark masters include bias; never
   subtract it twice. Dark scaling is off. Calibrate CFA before demosaicing.
3. Analyze calibrated exposures: FWHM, HFR, eccentricity, stars, background,
   noise, relative transparency, and registration residuals. Reuse star catalogs.
4. Review sortable metrics, plots, previews, blinking, and star overlays. Support
   manual exclusion and per-filter threshold/percentile rules before integration.
5. Register with geometric star matching, RANSAC, affine transforms, optional
   second-order correction, and a common reference. Resample once with Lanczos-3.
6. Normalize background and stellar flux within each filter. Integrate with
   inverse-variance weights and iterative sigma clipping, or unclipped averaging.
   Disable statistical rejection below ten valid contributors at a pixel.
7. Export float32 linear masters and optional coverage/weight/rejection maps.

## Formats and integrity

- Read FITS and Rice FITS image extensions, and XISF with zlib, LZ4/LZ4HC,
  Zstandard, byte shuffling, and subblocks.
- Preserve scaling, CFA offsets, row orientation, negative calibrated values,
  and acquisition metadata; validate checksums and exact decoded block lengths.
- Export uncompressed FITS, lossless GZIP_2 FITS without float quantization,
  uncompressed XISF, or Zstandard/shuffled XISF.
- Inputs are immutable. Accepted-frame failures stop the affected integration.
  Verify complete outputs before atomic publication.

## Architecture

- Shared processing library, CLI, supervised worker process, and Qt GUI.
- Typed image/tile readers, calibration plans, metrics/selection, transforms,
  immutable run settings, and structured job events.
- SQLite projects with schema versioning, persistent overrides and checkpoints.
- Dependency-aware stage fingerprints, reusable calibration/catalog/alignment
  results, and bounded intermediate caches.
- oneTBB scheduler with global memory accounting and limited concurrent readers;
  independent handles for reentrant CFITSIO. Prevent nested parallelism.
- Default RAM budget: 60% of available memory. Default scratch budget: the smaller
  of 100 GiB or 25% of available disk space. Both are configurable.
- Cache shortages trigger repeated streaming passes using the same algorithm;
  account for whole-block XISF decode memory. Float32 working pixels, float64
  transforms/statistics/accumulations, stable contribution order, safe FP flags.
- SEP extraction/background analysis, Eigen fitting, librtprocess RCD demosaicing.

## Acceptance and delivery

Deliver formats/projects/CLI and fixtures first; then processing; then the full
GUI workflow; then recovery, scale validation, optimization, and documentation.

Test independent compressed-format fixtures, known-truth calibration and star
fields, real OSC/LRGB/SHO data, cached/streamed equivalence, threading, resume,
corruption, cancellation, disk exhaustion, changed sources, and 10,000-file UI
responsiveness. Test Wayland and X11. Record cold/warm complete-workflow and stage
times, peak RAM, scratch space, and I/O against a pinned Siril version at matching
quality. Publish evidence before making performance claims.
