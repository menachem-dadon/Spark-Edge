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


def run_case(binary, model, mode, log_dir, io_mode="buffered", policy="lru", extra=None):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    native_log = model.parent / f"native-{port}.log"
    args = [str(binary), "-m", str(model), "--host", "127.0.0.1", "--port", str(port),
            "-c", "256", "-b", "4", "-ub", "4", "-t", "2", "-ngl", "0", "--parallel", "1",
            "--fit", "off", "--no-warmup", "--chat-template", "chatml", "--moe-paging", mode,
            "--log-file", str(native_log), "--log-timestamps"]
    if mode == "explicit":
        args += ["--moe-slots", "4", "--moe-io-mode", io_mode, "--moe-io-threads", "4", "--moe-cache-policy", policy]
    args += extra or []
    env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
    log_path = log_dir / f"server-{mode}-{io_mode}-{policy}.log"
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
                except (OSError, urllib.error.URLError, RuntimeError):
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
            deadline = time.monotonic() + 5
            while not native_log.exists() or "prompt eval time" not in native_log.read_text(encoding="utf-8", errors="replace"):
                if time.monotonic() > deadline:
                    raise RuntimeError("Native logger did not write generation timings to the Unicode path")
                time.sleep(.1)
            chat = request(base, "/v1/chat/completions", {
                "messages": [{"role": "system", "content": "Hi"}, {"role": "user", "content": "ABC"}],
                "max_tokens": 4, "temperature": 0, "ignore_eos": True,
            })
            assert chat["choices"] and chat["usage"]["completion_tokens"] > 0, "chat failed"
            sse_body = {"messages": [{"role": "user", "content": "ABC"}], "max_tokens": 4,
                        "temperature": 0, "ignore_eos": True, "stream": True}
            req = urllib.request.Request(base + "/v1/chat/completions", data=json.dumps(sse_body).encode(),
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=30) as stream:
                events = [line[6:] for line in stream.read().decode().splitlines() if line.startswith("data: ")]
            assert events and events[-1] == "[DONE]", "incomplete SSE stream"
            assert any(json.loads(event).get("choices") for event in events[:-1]), "empty SSE stream"
            stats = request(base, "/sparkmoe/v1/paging")
            assert stats["enabled"] == (mode == "explicit"), "wrong paging activation"
            if mode == "explicit":
                assert stats["bytes_read"] > 0 and stats["cache_misses"] > 0, "inactive paging"
                assert stats["cache_committed_bytes"] > 0, "missing memory telemetry"
            if policy == "hybrid":
                assert request(base, "/sparkmoe/v1/paging/cache", {"cache_mib": 1})["accepted"]
                assert request(base, "/sparkmoe/v1/paging")["resize_pending_bytes"] == 1048576
                request(base, "/completion", {**body, "prompt": "ABCD"})
                resized = request(base, "/sparkmoe/v1/paging")
                assert resized["cache_limit_bytes"] == 1048576 and resized["resize_pending_bytes"] == 0
                assert resized["cache_committed_bytes"] <= resized["cache_limit_bytes"] and not resized["resize_failures"]
            reset = request(base, "/sparkmoe/v1/paging/reset-stats", {})
            assert reset["paging"]["bytes_read"] == 0, "reset failed"
            print(f"PASS server {mode}/{io_mode}/{policy}: completion, chat, SSE, props, paging, reset, Unicode model/log paths")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def run_manager(binary_dir, root, log_dir):
    base = "http://127.0.0.1:18080"
    try:
        with socket.create_connection(("127.0.0.1", 18080), timeout=.2):
            raise RuntimeError("Manager port 18080 is occupied; refusing to modify an existing instance")
    except (ConnectionRefusedError, TimeoutError, OSError):
        pass
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    suffix = ".exe" if os.name == "nt" else ""
    env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
    env["SPARKMOE_NO_BROWSER"] = "1"
    with (log_dir / "manager.log").open("wb") as log:
        proc = subprocess.Popen([str(binary_dir / ("SparkMoE" + suffix))], cwd=binary_dir, env=env,
                                stdout=log, stderr=log,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 20
            while True:
                try:
                    config = request(base, "/api/config")
                    break
                except (OSError, RuntimeError):
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("manager did not start")
                    time.sleep(.1)
            assert config["io_mode"] == "buffered" and config["cache_policy"] == "lru"
            with urllib.request.urlopen(base, timeout=10) as response:
                assert 'id="resize-cache"' in response.read().decode()
            config.update(model=str(root / "test-sharded-00001-of-00002.gguf"), model_directory=str(root),
                          mode="explicit", cache=1, slots=4, threads=2, inference_threads=2, port=port,
                          ctx_size=256, batch_size=4, ubatch_size=4, parallel=1, cache_policy="hybrid",
                          overlap="on", prefetch="temporal", io_mode="direct", dense_residency="resident")
            request(base, "/api/start", config)
            deadline = time.monotonic() + 45
            while not request(base, "/api/status")["server_ready"]:
                if time.monotonic() > deadline:
                    raise RuntimeError("manager-controlled server did not become ready")
                time.sleep(.2)
            backend = f"http://127.0.0.1:{port}"
            result = request(backend, "/completion", {"prompt": "ABC", "n_predict": 4, "temperature": 0, "ignore_eos": True})
            assert result["tokens_predicted"] == 4 and result["content"]
            stats = request(base, "/api/paging")
            assert stats["enabled"] and stats["process_rss_bytes"] > 0 and stats["generation_tok_s"] > 0
            active = request(base, "/api/server-config")
            assert active["overlap"] and active["cache_policy"] == "hybrid" and active["dense_load_mode"] == "none"
            assert request(base, "/api/paging/cache", {"cache_mib": 2})["accepted"]
            request(backend, "/completion", {"prompt": "ABCD", "n_predict": 2, "ignore_eos": True})
            assert request(base, "/api/paging")["cache_limit_bytes"] == 2 * 1048576
            diagnostic = request(base, "/api/diagnostics")
            assert "model" not in diagnostic["configuration"] and diagnostic["sparkmoe_version"]
            request(base, "/api/restart", {"dense_residency": "mmap", "overlap": "off"})
            deadline = time.monotonic() + 45
            while not request(base, "/api/status")["server_ready"]:
                if time.monotonic() > deadline:
                    raise RuntimeError("manager restart timed out")
                time.sleep(.2)
            assert request(base, "/api/config")["model"] == config["model"]
            assert not request(base, "/api/server-config")["overlap"]
            request(base, "/api/stop", {})
            assert not request(base, "/api/status")["running"]
            print("PASS Desktop manager: default/config/start/generation/telemetry/resize/restart/stop/diagnostics, no browser")
        finally:
            try:
                request(base, "/api/stop", {})
            except (OSError, RuntimeError):
                pass
            proc.terminate()
            proc.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    parser.add_argument("--skip-manager", action="store_true", help="Do not use port 18080 or touch an existing Desktop instance")
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
        run_case(binary_dir / ("llama-server" + suffix), root / "test-sharded-00001-of-00002.gguf", "explicit", args.log_dir, "direct")
        run_case(binary_dir / ("llama-server" + suffix), root / "test-sharded-00001-of-00002.gguf", "explicit", args.log_dir, "direct", "hybrid")
        run_case(binary_dir / ("llama-server" + suffix), root / "test-sharded-00001-of-00002.gguf", "explicit", args.log_dir, "buffered", "hybrid",
                 ["--moe-overlap", "on", "--moe-prefetch", "temporal"])
        run_case(binary_dir / ("llama-server" + suffix), root / "test-moe-regression.gguf", "explicit", args.log_dir, "buffered", "lru",
                 ["--moe-paged-layers", "1", "--moe-dense-residency", "resident"])
        if args.skip_manager:
            print("SKIP Desktop lifecycle (--skip-manager); isolated HTTP cases still ran")
        else:
            run_manager(binary_dir, root, args.log_dir)


if __name__ == "__main__":
    main()
