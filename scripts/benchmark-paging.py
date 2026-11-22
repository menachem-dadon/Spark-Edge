#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
"""Paired local observations, never a timing-based correctness gate or downloader."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time


def memory(pid):
    if os.name == "nt":
        class Counters(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
                (name, ctypes.c_size_t) for name in ("peak_rss", "rss", "peak_pool", "pool", "peak_nonpaged",
                                                   "nonpaged", "pagefile", "peak_private", "private")]
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
        handle = kernel.OpenProcess(0x410, False, pid)
        if not handle:
            return {}
        try:
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return {"peak_rss_bytes": counters.peak_rss, "peak_private_bytes": counters.peak_private}
        finally:
            kernel.CloseHandle(handle)
    else:
        try:
            values = dict(line.split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines())
            return {"peak_rss_bytes": int(values["VmHWM"].split()[0]) * 1024}
        except (OSError, KeyError):
            pass
    return {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=Path, help="JSON list of {name, args: [arguments]} overrides")
    parser.add_argument("--cache-mib", type=int, default=1500)
    parser.add_argument("--tokens", type=int, default=8)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    if not args.model.is_file():
        parser.error("model must already exist locally")
    cases = json.loads(args.cases.read_text()) if args.cases else [
        {"name": "buffered-1", "args": ["--moe-io-mode", "buffered", "--moe-io-threads", "1"]},
        {"name": "buffered-4", "args": ["--moe-io-mode", "buffered", "--moe-io-threads", "4"]},
        {"name": "direct-4", "args": ["--moe-io-mode", "direct", "--moe-io-threads", "4"]}]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    common = [str(args.bin.resolve()), "-m", str(args.model.resolve()), "-p", "0", "-n", str(args.tokens),
              "-r", "2", "-b", "8", "-ub", "1", "-t", str(args.threads), "-ngl", "0", "-o", "json",
              "--moe-paging", "explicit", "--moe-paging-cache-mib", str(args.cache_mib)]
    report = {"model": str(args.model.resolve()), "model_bytes": args.model.stat().st_size,
              "note": "No OS cache flush. Forward/reverse paired warm-cache observations; no speed threshold.", "runs": []}
    for ordinal, case in enumerate(cases + list(reversed(cases))):
        command = common + case["args"]
        output = args.output.with_suffix(f".{ordinal}.stdout.log")
        errors = args.output.with_suffix(f".{ordinal}.stderr.log")
        observed = {}
        start = time.monotonic()
        with output.open("wb") as stdout, errors.open("wb") as stderr:
            environment = os.environ.copy()
            environment.update(case.get("env", {}))
            proc = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            while proc.poll() is None:
                for key, value in memory(proc.pid).items():
                    observed[key] = max(observed.get(key, 0), value)
                time.sleep(.1)
        if proc.returncode:
            raise RuntimeError(f"{case['name']} failed: {errors}")
        result = {"name": case["name"], "command": command, "wall_seconds": time.monotonic() - start,
                  "environment_overrides": case.get("env", {}),
                  "memory": observed, "bench": json.loads(output.read_text(encoding="utf-8"))}
        for metric, minimum in case.get("min_metrics", {}).items():
            if result["bench"][0].get(metric, 0) < minimum:
                raise RuntimeError(f"{case['name']}: {metric} was not exercised; check the experiment patch/configuration")
        report["runs"].append(result)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(case["name"], result["bench"][0]["samples_ts"], observed, flush=True)


if __name__ == "__main__":
    main()
