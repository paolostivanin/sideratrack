# Desktop workflow

[Back to the README](../README.md)

1. Create or open a project from the welcome screen. In **Import**, add files,
   add a folder recursively, or drop exposures onto the page. Check frame type,
   filter, night, Bayer pattern, camera/exposure metadata and calibration state.
   Double-click type/filter/night to correct them, or use **Edit metadata** for
   bulk acquisition metadata corrections. Mark imported calibration masters and
   indicate whether a dark master already has its bias removed.
2. Open **Calibration** and select an acquisition group to review its assignments.
   Automatic matching checks camera,
   dimensions, CFA, binning, gain/offset, readout mode, exposure, temperature and
   flat filter. Known conflicts are rejected. Missing gain, temperature and exposure
   are recovered from named filename tokens when possible; embedded metadata and
   manual edits take precedence. Unique compatible groups may match with missing
   fields, which are disclosed in the assignment table. More complete metadata
   takes priority, followed by the same night and then masters over raw groups.
   Resolve ambiguous masters or flat sessions with explicit overrides in the
   selected group’s editor; **Apply to this group** updates its members without
   depending on selection in another screen. Select a raw-flat group to assign
   its bias or dark-flat correction. Flats use a matching dark-flat or
   bias; dark scaling is disabled. CFA calibration happens before RCD debayering.
   **Create calibration masters** exports stacked biases, darks, dark-flats and normalized
   flats, independently of light stacking. It uses selected calibration frames,
   or all included raw calibration frames; its setup dialog makes the source
   selection explicit. Choose **Bias**
   or **Dark-flat** explicitly for flats; Automatic prefers dark-flat. Missing
   chosen calibrations cause an error instead of silently using the other method.
   Calibration-only projects work without lights or prior analysis. In **Results**,
   use **Import calibration masters** to reuse the exported files
   and review their assignments before preparation.
3. Click **Prepare frames** to save calibrated lights, then analyze and align
   them. Analysis starts only if calibration succeeds. In **Review**, inspect
   FWHM, HFR, eccentricity, star count, background,
   noise, transparency and alignment residuals. Sort numeric metrics, click the
   metric plot, zoom/pan previews, show star overlays and blink selected frames.
   Filter by frame type, filter, night and status, or search filenames. The plot
   follows the visible frames. Use **Details** for metadata and status reasons;
   right-click the table header to show additional metric columns.
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
   FWHM retention percentage in **Grading rules**. Preview inclusion counts
   before saving. **Include** and **Exclude** set manual overrides; **Automatic**
   returns selected frames to grading rules. Manual inclusion overrides
   grading thresholds. Unreadable or unregistered included frames stop stacking
   until fixed or excluded. Missing calibration requires an explicit setting.
5. In **Stack**, review the accepted/excluded counts and exposure time by
   filter/night, reference geometry, chosen calibrations and resource budgets.
   Choose the output directory and file format, then click **Stack frames**.
   OSC produces a linear RGB master;
   mono produces separate filter masters on the same reference grid. Inverse
   variance weighting, same-filter stellar/background normalization and iterative
   sigma clipping are exposed under **Integration settings**. Weighting and
   within-filter normalization are applied automatically. Statistical rejection
   is disabled wherever
   fewer than ten valid exposures contribute. Uncovered/invalid samples are NaN.
6. In **Results**, preview completed light/calibration masters and open their
   folders. **Export masters** exports all completed light masters as FITS,
   lossless compressed FITS, XISF, or
   Zstandard/shuffled XISF. Coverage, weight and rejection maps are optional.
   Continue finishing/color composition in your image editor.

Prepared lights default to `<project path>.calibrated/`; change the directory in
**Calibration**. These verified float FITS intermediates remain after
closing the application and are separate from evictable scratch caches. **Prepare frames**
shows per-frame progress and reuses unchanged prepared images. Analyze and Stack
require current prepared images; changed inputs, assignments or missing/corrupt
intermediates require **Prepare frames** (or the separate commands in **Advanced**).
Source exposures are preserved; recursive import skips managed prepared images.
Existing projects keep metadata edits and overrides, but older derived results need
**Prepare frames** before stacking with this processing version.
The supplied 222-light, 26 MP mono dataset needs about 22 GiB for intermediates.

**Cancel** stops safely and saves completed stages/bands. The processing panel
stays visible across pages, with stage progress, elapsed time and expandable
technical details. **Retry preparation** continues calibration and then analysis;
**Resume stack** reuses compatible checkpoints in the same output directory.
The last stack directory is saved in the project, and compatible checkpoints are
also offered on reopening. Completed masters and maps are verified before reuse;
an interrupted publication is recoverable. Existing unrelated outputs are never
silently overwritten. Edits are disabled while processing, but page navigation
and inspection remain available.

Use **Resource preferences** for RAM, scratch and CPU limits, **Registration &
calibration options** for reference/distortion and explicit missing-calibration
permission, **Grading rules** for quality thresholds, and **Integration settings**
for rejection/maps. Changing inputs, assignments or relevant settings updates
readiness; saved masters from a previous selection remain accessible and are
identified as previous results.
