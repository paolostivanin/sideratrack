# Validation of the initial implementation

## Real camera dataset

On 2026-10-01, a QHY268M mono dataset from two observing-night folders was
processed: 222 XISF lights at 6252 × 4176 pixels (26.11 MP), with six existing
PixInsight calibration masters. Lights use LZ4HC compression and byte shuffling.
The float masters contain a normalized integration image and auxiliary rejection
images; the importer selects the unambiguous integration image.

| Filter | Lights | Exposure | Median registration residual |
| --- | ---: | ---: | ---: |
| L | 51 | 30 s | 0.0812 px |
| R | 58 | 80 s | 0.0867 px |
| G | 58 | 80 s | 0.0494 px |
| B | 55 | 80 s | 0.0626 px |

All lights recorded 0°C, gain 0 and offset 11. Gain and temperature in master
filenames were entered explicitly because their headers omitted those fields.
Masters also omitted some acquisition metadata, so calibration assignments were
made explicitly after inspection: 0°C/30-second dark for L and 0°C/80-second dark
for RGB. These full darks include bias; bias was not subtracted a second time.
No flats were supplied, so this validates dark calibration without dust/vignetting
correction. Flat calibration is covered by independent known-truth tests.

All 222 lights registered successfully, including the rotation between nights.
Integration used the best-FWHM reference, affine registration, Lanczos-3
resampling, within-filter transparency/background normalization, inverse-noise
variance weights and three iterative 3σ rejection passes after initial statistics.
Outputs are four linear float32 FITS masters with coverage, weight and rejection
maps on the common reference grid.

An independent Python/Astropy check verified the checksums of all 16 outputs,
dimensions, filter/NCOMBINE/unit headers, valid-pixel/coverage correspondence,
integer rejection/coverage counts and positive-weight consistency. Between
99.91% and 100% of each output is covered. Gaussian fits to 93–126 bright stars
per master found median FWHM of 2.00–2.10 pixels and median centroid offsets from
the reference catalog below 0.15 pixels. These descriptive fits are a different
estimator from the application metrics and do not establish superiority over
another application. SHA-256 checks verified that all 228 source files remained
unchanged after processing.

## Measured resources

Hardware: Intel Xeon W-2195, 18 physical cores / 36 logical CPUs, approximately
62 GiB RAM. Release build: GCC 16.2, `-O3 -DNDEBUG`, FP contraction disabled;
no fast-math. The environment's CFITSIO build is not reentrant, so FITS access
is serialized. CPU-intensive processing uses oneTBB.

The measured 36-thread stack used a 24 GiB working-memory budget and 30 GiB
scratch-cache budget. Calibrated disk-cache images were already available from
analysis; a new output directory prevented checkpoint/result reuse. OS page
caches were not flushed. Stack time includes input SHA-256 verification,
integration, output checksums and atomic publication, and excludes preceding
import/analysis.

| Threads | RAM budget | Stack wall time | Peak application RSS | Average active CPUs | Peak active CPUs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 36 | 24 GiB | 246.9 s | 7.83 GiB | 26.28 | 35.95 |
| 18 | 16 GiB | 270.3 s | 7.83 GiB | 13.66 | 18.04 |

The second run used an isolated project copy and new outputs with the same
inputs, calibrated disk cache, transforms and numerical settings. Both RAM
budgets allowed all current-filter calibrated frames to remain cached and used
one spatial band. The 36-thread run took 8.7% less wall time in this comparison.
All 16 master/diagnostic pixel buffers were bit-identical between runs. This
single sequential comparison does not establish the best thread count on other
CPUs or workloads; OS-cache state and run-to-run variation are uncontrolled.

Resources are sampled every 0.5 seconds from Linux `/proc`; CPU activity is
reported in logical-CPU equivalents. Serial verification/export and scheduler
overhead reduce the average below the peak. RAM holds useful calibrated frames
and workspaces; unused budget is not allocated artificially. The RAM budget is
managed by the application and is not an OS-enforced RSS ceiling.

Local reproducibility artifacts are in the ignored `validation/tempa-lrgb/`
directory: the project, structured processing logs, resource samples, independent
verification script/report and exported masters. These observations are one
machine/dataset measurement, not a repeatability study or comparison with Siril.

## Automated checks and remaining validation

The release build passes the core, independent-format/workflow and GUI suites.
The GUI suite exercises a 10,000-frame model with five million catalog stars,
numeric sorting, persistent edits, asynchronous import and calibration exports.
The core and independent suites also pass AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer. A temporary installation successfully
launched the real project using Qt's offscreen platform; native desktop interaction
still requires manual validation.

Known-truth tests cover OSC and LRGBSHO, bias/dark/flat/dark-flat calibration,
explicit user choice of bias versus dark-flat for flats, multiple nights and
rotations, rejection, cancellation/resume, corrupt caches, failed writes and
interrupted publication. Independently encoded FITS/Rice and compressed XISF
fixtures test format interoperability. Serial execution with both RAM/disk frame
caches disabled produces identical master, coverage, weight and rejection pixels
to parallel cached execution; independently measured metrics/transforms agree
exactly too.

Real OSC/SHO validation, native Wayland/X11 interaction, a full 10,000-exposure
processing run and matched-quality Siril benchmarks remain outstanding. Mosaics,
GPU processing, drizzle and live stacking are deferred in the accepted scope.
