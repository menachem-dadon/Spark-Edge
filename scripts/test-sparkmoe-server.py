# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

"""Exercise the real HTTP server with tiny local GGUF fixtures, without downloads."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def request(base, route, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + route, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"{route}: HTTP {error.code}: {error.read().decode(errors='replace')}") from error


def run_case(binary, model, mode, log_dir):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    args = [str(binary), "-m", str(model), "--host", "127.0.0.1", "--port", str(port),
            "-c", "256", "-b", "4", "-ub", "4", "-t", "2", "-ngl", "0", "--parallel", "1",
            "--fit", "off", "--no-warmup", "--chat-template", "chatml", "--moe-paging", mode]
    if mode == "explicit":
        args += ["--moe-slots", "4"]
    env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
    log_path = log_dir / f"server-{mode}.log"
    with log_path.open("wb") as log:
        proc = subprocess.Popen(args, stdout=log, stderr=log, env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 45
            while True:
                if proc.poll() is not None:
                    raise RuntimeError(f"server exited {proc.returncode}: {log_path}")
                try:
                    if request(base, "/health")["status"] == "ok":
                        break
                except (OSError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"server startup: {log_path}")
                time.sleep(.1)
            assert request(base, "/v1/models")["data"], "no model exposed"
            request(base, "/props")
            request(base, "/sparkmoe/v1/config")
            body = {"prompt": "ABC", "n_predict": 4, "temperature": 0, "seed": 42, "ignore_eos": True}
            completion = request(base, "/completion", body)
            assert completion["tokens_predicted"] == 4 and completion["content"], "empty generation"
            chat = request(base, "/v1/chat/completions", {
                "messages": [{"role": "system", "content": "Hi"}, {"role": "user", "content": "ABC"}],
                "max_tokens": 4, "temperature": 0, "ignore_eos": True,
            })
            assert chat["choices"] and chat["usage"]["completion_tokens"] > 0, "chat failed"
            stats = request(base, "/sparkmoe/v1/paging")
            assert stats["enabled"] == (mode == "explicit"), "wrong paging activation"
            if mode == "explicit":
                assert stats["bytes_read"] > 0 and stats["cache_misses"] > 0, "inactive paging"
            reset = request(base, "/sparkmoe/v1/paging/reset-stats", {})
            assert reset["paging"]["bytes_read"] == 0, "reset failed"
            print(f"PASS server {mode}: completion, chat, props, paging, reset, Unicode model path")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    args = parser.parse_args()
    binary_dir = args.bin_dir.resolve()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="sparkmoe-\u05de\u05d5\u05d3\u05dc-") as folder:
        root = Path(folder)
        subprocess.run([str(binary_dir / ("test-moe-regression" + suffix)), "--fixtures-only"],
                       cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run_case(binary_dir / ("llama-server" + suffix), root / "test-dense-regression.gguf", "off", args.log_dir)
        run_case(binary_dir / ("llama-server" + suffix), root / "test-moe-regression.gguf", "explicit", args.log_dir)


if __name__ == "__main__":
    main()
