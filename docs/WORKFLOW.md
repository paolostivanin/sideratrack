# Desktop workflow

[Back to the README](../README.md)

1. Create a project and import files or a folder recursively. Check frame type,
   filter, night, Bayer pattern, camera/exposure metadata and calibration state.
   Double-click type/filter/night to correct them, or use **Edit selected** for
   bulk acquisition metadata corrections. Mark imported calibration masters and
   indicate whether a dark master already has its bias removed.
2. Open **Calibration assignments** to review assignments. Automatic matching checks camera,
   dimensions, CFA, binning, gain/offset, readout mode, exposure, temperature and
   flat filter. Known conflicts are rejected. Missing gain, temperature and exposure
   are recovered from named filename tokens when possible; embedded metadata and
   manual edits take precedence. Unique compatible groups may match with missing
   fields, which are disclosed in the assignment table. More complete metadata
   takes priority, followed by the same night and then masters over raw groups.
   Resolve ambiguous masters or
   flat sessions with explicit overrides. Flats use a matching dark-flat or
   bias; dark scaling is disabled. CFA calibration happens before RCD debayering.
   **Create masters** exports stacked biases, darks, dark-flats and normalized
   flats, independently of light stacking. It uses selected calibration frames,
   or all included raw calibration frames when none are selected. Choose **Bias**
   or **Dark-flat** explicitly for flats; Automatic prefers dark-flat. Missing
   chosen calibrations cause an error instead of silently using the other method.
   Calibration-only projects work without lights or prior analysis. Import the
   exported masters into a project when you want to reuse them.
3. Run **Calibrate** to save prepared lights, then **Analyze** and review FWHM, HFR, eccentricity, star count, background,
   noise, transparency and alignment residuals. Sort numeric metrics, click the
   metric plot, zoom/pan previews, show star overlays and blink selected frames.
   Previews use prepared exposures when available and otherwise show
   the raw exposure; the label identifies which is displayed. Stretching affects
   the preview only. **Start blink / Stop blink** plays selected visible frames in
   table order. Set seconds per frame (default 1 s; 0.1–60 s). Playback uses cached
   previews up to 2048 pixels on the long edge; stopping restores full resolution.
   Star overlays remain aligned, and changing selection/filter stops playback.
   Preview caches are bounded in RAM and share the scratch budget with processing.
   Analysis metrics, registration results and errors appear in the table and plot
   while the worker is still running.
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

Prepared lights default to `<project path>.calibrated/`; change the directory in
**Calibration assignments**. These verified float FITS intermediates remain after
closing the application and are separate from evictable scratch caches. Calibrate
shows per-frame progress and reuses unchanged prepared images. Analyze and Stack
require current prepared images; changed inputs, assignments or missing/corrupt
intermediates require Calibrate followed by Analyze. Source exposures are preserved; recursive import skips managed prepared images.
Existing projects keep metadata edits and overrides, but older derived results need
Calibrate followed by Analyze before stacking with this processing version.
The supplied 222-light, 26 MP mono dataset needs about 22 GiB for intermediates.

**Cancel** saves completed stages/bands. After cancelled calibration, run Calibrate
again to continue from completed images. After cancelled analysis, run Analyze
again; after cancelled integration, use Resume with the same output directory.
The last stack directory is saved in the project. Completed masters and maps
are verified before reuse; an interrupted output publication is recoverable.
Existing unrelated outputs are never silently overwritten.

