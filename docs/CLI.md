# CLI reference

[Back to the README](../README.md)

Use `siderastack-cli --help` for available commands and global options.

The same engine runs without a display. Paths and IDs are passed as arguments:

```sh
./build/siderastack-cli init /data/target.sidera
./build/siderastack-cli import /data/target.sidera /data/night1 /data/night2
./build/siderastack-cli calibration /data/target.sidera
./build/siderastack-cli masters /data/target.sidera /data/calibration-masters --flat-calibration bias
./build/siderastack-cli calibrate /data/target.sidera --json
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


`calibration PROJECT` inspects automatic assignments and explains inferred or
missing metadata. `calibrate PROJECT [DIRECTORY]` prepares lights and saves verified
float FITS intermediates, reusing unchanged results. The default directory is
`PROJECT.calibrated/`; an explicit directory is remembered. Analyze and Stack read
these images and do not silently perform calibration. Rerun Calibrate and Analyze
when inputs or assignments change, or a prepared image is missing/corrupt.

With `--json`, newline-delimited `frame-updated` events contain the committed
frame `id`, alongside existing progress/error/completion events. Import, Calibrate
and Analyze emit these updates as work completes. A cancelled calibration retains
completed prepared images; rerun the same command to continue.
