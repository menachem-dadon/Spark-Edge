#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
"""Local-model HTTP/MTP smoke. No downloads, resident baseline or timing gate."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request


def request(base, path, body=None):
    req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=180) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache-mib", type=int, default=4096)
    parser.add_argument("--mtp", action="store_true", help="also require a successful built-in MTP run")
    args = parser.parse_args()
    if not args.model.is_file():
        parser.error("model must already exist locally")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"model": str(args.model.resolve()), "model_bytes": args.model.stat().st_size, "runs": []}
    for mtp in ([False, True] if args.mtp else [False]):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        command = [str(args.bin.resolve()), "-m", str(args.model.resolve()), "--host", "127.0.0.1", "--port", str(port),
                   "-c", "256", "-b", "16", "-ub", "8", "-t", "4", "-ngl", "0", "--parallel", "1",
                   "--fit", "off", "--no-warmup", "--metrics", "--moe-paging", "explicit",
                   "--moe-cache-mib", str(args.cache_mib), "--moe-io-threads", "4"]
        if mtp:
            command += ["--spec-type", "draft-mtp", "--spec-draft-n-max", "3", "--spec-draft-ngl", "0",
                        "--spec-draft-threads", "4"]
        log_path = args.output.with_suffix(".mtp.log" if mtp else ".normal.log")
        with log_path.open("wb") as log:
            env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
            proc = subprocess.Popen(command, stdout=log, stderr=log, env=env,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                deadline = time.monotonic() + 180
                while True:
                    if proc.poll() is not None:
                        raise RuntimeError(f"server exited {proc.returncode}: {log_path}")
                    try:
                        if request(base, "/health")["status"] == "ok":
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"startup: {log_path}")
                    time.sleep(.2)
                config = request(base, "/sparkmoe/v1/config")
                assert config["mtp_enabled"] == mtp and config["indexed_expert_tensors"] > 0
                result = request(base, "/completion", {"prompt": "The capital of France is", "n_predict": 12,
                                 "temperature": 0, "seed": 42, "ignore_eos": True})
                assert result["tokens_predicted"] == 12 and result["content"].strip()
                stats = request(base, "/sparkmoe/v1/paging")
                assert stats["enabled"] and stats["bytes_read"] > 0 and stats["cache_misses"] > 0
                assert stats["process_rss_bytes"] > 0 and proc.poll() is None
                if mtp:
                    assert result["timings"].get("draft_n", 0) > 0, "MTP did not draft tokens"
                report["runs"].append({"mtp": mtp, "command": command, "config": config, "stats": stats,
                                       "content": result["content"], "timings": result["timings"]})
                args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
                print(f"PASS real server mtp={mtp}: 12 tokens, active paging, {result['timings']}", flush=True)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
