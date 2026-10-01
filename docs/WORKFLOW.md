# Desktop workflow

[Back to the README](../README.md)

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

