# Formats, integrity and resource handling

[Back to the README](../README.md)

## Input integrity and output publication

Inputs are read-only. SQLite projects preserve selection, metadata edits,
measurements, catalogs, transforms and recovery manifests. SHA-256 dependencies
invalidate stale stages; stacking refuses inputs changed after analysis. Image
checksums and exact decoded lengths are checked. Export uses verified temporary
files, fsync and atomic publication without replacement.

## Formats and numerical values

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

## Memory, disk and parallel processing

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

