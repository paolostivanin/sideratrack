# Measuring performance and quality

Performance targets are goals. Stellastack has no demonstrated speed advantage
against Siril yet. A real 222-light QHY268M LRGB dataset has been processed; see
[validation results](VALIDATION.md). Siril is not installed in the development
environment.

Use the same immutable input dataset, accepted exposures, calibration states,
reference, output dimensions, resampling, normalization, weights and rejection
thresholds for both applications. Record exact versions, build flags, CPU,
RAM, storage/filesystem, thread count and compression formats. Compare 26 MP
and 61 MP mono/OSC projects and multiple nights; include LRGB and SHO. Compare
output background/noise, fitted stellar FWHM/eccentricity, residual alignment,
color ratios, faint-signal photometry, rejected defects and valid coverage.
A speed comparison is meaningful only after these quality checks agree.

The included runner makes an isolated SQLite copy and never changes input files.
It runs a cold application-cache/catalog workflow and a warm workflow, writing
JSON with stage/workflow times, sampled peak RSS/scratch, Linux I/O counters,
settings, input SHA-256 hashes and structured logs:

```sh
python3 tools/benchmark.py /data/project.stella \
  --binary build/stellastack-cli --work /data/benchmarks/run-001 \
  --threads 18 --memory-mib 32768 --scratch-gib 100 \
  --description '61 MP OSC, 240 lights, 3 nights'
```

The work directory must be new. “Cold” describes Stellastack artifacts; the
runner does not flush the operating system's page cache. Use fresh system
sessions for disk-cold comparisons. Linux I/O `read_bytes` includes actual disk
reads while `rchar` includes page-cache reads. RSS is sampled every 100 ms;
very short allocation peaks may be missed. Progress stage attribution is
approximate when calibration stages occur inside analysis/integration.

Run a pinned Siril version separately with matching quality settings and report
its commands/scripts and the same wall-time/RSS/I/O measurements alongside the
Stellastack JSON. The runner does not invent or extrapolate comparison results.
