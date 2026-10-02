#!/usr/bin/env python3
"""Benchmark a project copy: sources remain immutable; caches and outputs are isolated."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import sqlite3
import subprocess
import threading
import time


def measure(binary, command, project, extra, directory, cache):
    stop = threading.Event()
    peaks = {"rss_bytes": 0, "scratch_bytes": 0}
    io = {}
    log_path = directory / (command + ".jsonl")
    started = time.monotonic()
    with log_path.open("w") as log:
        proc = subprocess.Popen([str(binary), command, str(project), *map(str, extra), "--json"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        def sample():
            while not stop.is_set():
                try:
                    status = Path(f"/proc/{proc.pid}/status").read_text().splitlines()
                    for line in status:
                        if line.startswith(("VmRSS:", "VmHWM:")):
                            peaks["rss_bytes"] = max(peaks["rss_bytes"], int(line.split()[1]) * 1024)
                    io.update({k: int(v) for k, v in (line.split(":") for line in Path(f"/proc/{proc.pid}/io").read_text().splitlines())})
                except (FileNotFoundError, ProcessLookupError):
                    pass
                peaks["scratch_bytes"] = max(peaks["scratch_bytes"], sum(p.stat().st_size for p in cache.glob("cache-*.fits")))
                stop.wait(.1)
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        stages = {}
        previous_stage, previous_time = "startup", started
        for line in proc.stdout:
            log.write(line)
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            now = time.monotonic()
            stage = event.get("stage", previous_stage)
            stages[previous_stage] = stages.get(previous_stage, 0) + now - previous_time
            previous_stage, previous_time = stage, now
        code = proc.wait()
        ended = time.monotonic()
        stages[previous_stage] = stages.get(previous_stage, 0) + ended - previous_time
        stop.set()
        sampler.join()
    if code:
        raise RuntimeError(f"{command} failed with exit {code}; see {log_path}")
    return {"seconds": ended - started, "stages_seconds": stages, "peak": peaks, "io": io,
            "log": str(log_path), "exit_code": code}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/stellastack-cli"))
    parser.add_argument("--work", required=True, type=Path, help="New empty benchmark directory")
    parser.add_argument("--threads", type=int)
    parser.add_argument("--memory-mib", type=int)
    parser.add_argument("--scratch-gib", type=float)
    parser.add_argument("--description", default="User-supplied dataset")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=False)
    project = args.work / "benchmark.stella"
    cache = args.work / "cache"
    cache.mkdir()
    with sqlite3.connect(f"file:{args.project.resolve()}?mode=ro", uri=True) as source, sqlite3.connect(project) as copy:
        source.backup(copy)
        settings = json.loads(copy.execute("SELECT value FROM records WHERE key='settings'").fetchone()[0])
        settings["cacheDirectory"] = str(cache)
        if args.threads is not None:
            settings["threads"] = args.threads
        if args.memory_mib is not None:
            settings["memory"] = args.memory_mib * 1024**2
        if args.scratch_gib is not None:
            settings["scratch"] = int(args.scratch_gib * 1024**3)
        copy.execute("UPDATE records SET value=? WHERE key='settings'", (json.dumps(settings),))
        count, lights = 0, 0
        dimensions = set()
        for id_, record in copy.execute("SELECT id,record FROM frames").fetchall():
            frame = json.loads(record)
            count += 1
            lights += frame["kind"] == "light"
            dimensions.add((frame["width"], frame["height"], frame["channels"], frame["cfa"]))
            frame.update(analysisKey="", catalog=[], transform={}, error="")
            copy.execute("UPDATE frames SET record=? WHERE id=?", (json.dumps(frame), id_))
        copy.execute("DELETE FROM records WHERE key!='settings'")
    binary = args.binary.resolve()
    result = {"date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "description": args.description, "source_project": str(args.project.resolve()),
              "version": subprocess.check_output([binary, "--version"], text=True).strip(),
              "platform": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
              "inputs": count, "lights": lights, "dimensions": sorted(dimensions), "settings": settings,
              "note": "Cold means a new application cache/catalog. OS page caches are not flushed. Stage times are attributed between progress events.",
              "runs": {}}
    for label in ("cold", "warm"):
        directory = args.work / label
        directory.mkdir()
        result["runs"][label] = {
            "analyze": measure(binary, "analyze", project, [], directory, cache),
            "stack": measure(binary, "stack", project, [directory / "masters"], directory, cache)}
        (args.work / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    # Retain source hashes, selection, transforms and normalization metrics for reproducibility.
    result["frames"] = json.loads(subprocess.check_output([binary, "list", project], text=True))
    result["masters"] = {p.name: p.stat().st_size for p in (args.work / "warm/masters").glob("*") if p.is_file()}
    (args.work / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({label: {stage: round(value["seconds"], 3) for stage, value in run.items()}
                      for label, run in result["runs"].items()}, indent=2))
    print(args.work / "results.json")


if __name__ == "__main__":
    main()
