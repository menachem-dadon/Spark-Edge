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
import urllib.parse
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


def run_qwen(binary, root, log_dir, width, controlled=False):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    args = [str(binary), "-m", str(root / "test-qwen38.gguf"), "--host", "127.0.0.1", "--port", str(port),
            "-c", "256", "-b", "16", "-ub", "2", "-t", "2", "-ngl", "0", "--parallel", "1",
            "--fit", "off", "--no-warmup", "--no-repack", "--chat-template", "chatml",
            "--moe-paging", "explicit", "--moe-slots", "4", "--moe-ple-cache-mib", "1"]
    if width:
        head = "test-qwen38-mtp-accept.gguf" if controlled else "test-qwen38-mtp.gguf"
        args += ["--spec-type", "draft-mtp", "--model-draft", str(root / head),
                 "--spec-draft-n-max", str(width), "--spec-draft-p-min", "0", "--moe-draft-cache-mib", "1"]
    env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
    with (log_dir / f"qwen-mtp-{width}{'-controlled' if controlled else ''}.log").open("wb") as log:
        proc = subprocess.Popen(args, stdout=log, stderr=log, env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 45
            while True:
                if proc.poll() is not None:
                    raise RuntimeError(f"Qwen server exited: draft width {width}")
                try:
                    if request(base, "/health")["status"] == "ok":
                        break
                except (OSError, RuntimeError):
                    pass
                if time.monotonic() > deadline:
                    raise TimeoutError("Qwen startup timed out")
                time.sleep(.1)
            if controlled:
                for text in ("A" * 16, "AAB" * 5 + "A"):
                    result = request(base, "/completion", {"prompt": "ABC", "n_predict": 16,
                                     "temperature": 0, "seed": 42, "cache_prompt": False,
                                     "grammar": f'root ::= "{text}"'})
                    assert result["content"] == text, result
                    timings = result["timings"]
                    assert timings["draft_n_accepted"] > 0, "MTP acceptance path was not used"
                    if "B" in text:
                        assert timings["draft_n_accepted"] < timings["draft_n"], "MTP partial rejection path was not used"
                print(f"PASS Qwen HTTP MTP width {width}: controlled acceptance and partial rejection")
                return
            outputs = []
            for prompt in ("ABC", "ABCDE", "ABC"):
                body = {"prompt": prompt, "n_predict": 16, "temperature": 0, "seed": 42,
                        "ignore_eos": True, "cache_prompt": False}
                result = request(base, "/completion", body)
                assert result["tokens_predicted"] == 16, result
                if width:
                    assert result["timings"]["draft_n"] > 0, "MTP never drafted"
                outputs.append(result["content"])
            stopped = request(base, "/completion", {**body, "ignore_eos": False, "logit_bias": [[2, 1000]]})
            assert stopped["stop_type"] == "eos", stopped
            resumed = request(base, "/completion", body)
            assert resumed["content"] == outputs[-1], "EOG changed the next request"
            stats = request(base, "/sparkmoe/v1/paging")
            assert stats["ple_cache_bytes"] > 0 and stats["ple_pages_read"] > 0 and stats["ple_bytes_read"] > 0
            reset = request(base, "/sparkmoe/v1/paging/reset-stats", {})["paging"]
            assert reset["ple_pages_read"] == 0 and reset["ple_bytes_read"] == 0 and reset["ple_cache_bytes"] > 0
            print(f"PASS Qwen HTTP MTP width {width}: repeated requests, forced EOG, resume")
            return outputs
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def run_manager(binary_dir, root, log_dir):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        manager_port = sock.getsockname()[1]
    if manager_port == port:
        raise RuntimeError("temporary manager and router ports collided")
    base = f"http://127.0.0.1:{manager_port}"
    suffix = ".exe" if os.name == "nt" else ""
    env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
    env["SPARKMOE_NO_BROWSER"] = "1"
    env["SPARKMOE_DATA_DIR"] = str(root / "manager-state")
    env["SPARKMOE_MANAGER_PORT"] = str(manager_port)
    env["SPARKMOE_SERVER_PORT"] = str(port)
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
            assert config["models_max"] == 1 and config["models_autoload"] and config["port"] == port
            config.update(model_directory=str(root), models_max=2)
            request(base, "/api/config", config)
            deadline = time.monotonic() + 45
            while True:
                status = request(base, "/api/status")
                if status["server_ready"]:
                    break
                if time.monotonic() > deadline:
                    logs = request(base, "/api/logs?max_bytes=16384")
                    raise RuntimeError(f"manager-controlled router did not become ready: {status}; logs: {logs}")
                time.sleep(.2)
            backend = f"http://127.0.0.1:{port}"
            models = request(backend, "/models")["data"]
            model_id = next(model["id"] for model in models if "test-sharded-00001" in model["id"])
            profile_response = request(base, "/api/model-profile?" + urllib.parse.urlencode({"model": model_id}))
            assert not profile_response["is_override"]
            profile = profile_response["profile"]
            profile.update(mode="explicit", cache=1, slots=4, io_threads=2, inference_threads=2,
                           ctx_size=256, batch_size=4, ubatch_size=4, parallel=1, cache_policy="hybrid",
                           overlap="on", prefetch="temporal", io_mode="direct", dense_residency="resident")
            request(base, "/api/model-profile", {"model": model_id, "profile": profile})
            try:
                request(backend, "/models/load", {"model": model_id})
            except RuntimeError as cause:
                session = request(base, "/api/status")["log_session"]
                download = base + "/api/logs/download?" + urllib.parse.urlencode({"session": session})
                with urllib.request.urlopen(download, timeout=30) as response:
                    logs = response.read().decode("utf-8", errors="backslashreplace")
                raise RuntimeError(f"{cause}; manager logs: {logs}") from cause
            deadline = time.monotonic() + 45
            while next(model for model in request(backend, "/models")["data"] if model["id"] == model_id)["status"]["value"] != "loaded":
                if time.monotonic() > deadline:
                    session = request(base, "/api/status")["log_session"]
                    download = base + "/api/logs/download?" + urllib.parse.urlencode({"session": session})
                    with urllib.request.urlopen(download, timeout=30) as response:
                        logs = response.read().decode("utf-8", errors="backslashreplace")
                    raise RuntimeError(f"router model did not become ready; manager logs: {logs}")
                time.sleep(.2)
            result = request(backend, "/completion", {
                "model": model_id, "prompt": "ABC", "n_predict": 4, "temperature": 0, "ignore_eos": True,
            })
            assert result["tokens_predicted"] == 4 and result["content"]
            query = "?" + urllib.parse.urlencode({"model": model_id})
            stats = request(backend, "/sparkmoe/v1/paging" + query)
            assert stats["enabled"] and stats["process_rss_bytes"] > 0
            active = request(backend, "/sparkmoe/v1/config" + query)
            assert active["overlap"] and active["cache_policy"] == "hybrid" and active["dense_load_mode"] == "none"
            assert request(backend, "/sparkmoe/v1/paging/cache", {"model": model_id, "cache_mib": 2})["accepted"]
            request(backend, "/completion", {"model": model_id, "prompt": "ABCD", "n_predict": 2, "ignore_eos": True})
            assert request(backend, "/sparkmoe/v1/paging" + query)["cache_limit_bytes"] == 2 * 1048576
            chat = request(backend, "/v1/chat/completions", {
                "model": model_id,
                "messages": [{"role": "user", "content": "ABC"}],
                "max_tokens": 4,
                "reasoning_effort": "none",
                "chat_template_kwargs": {"enable_thinking": False},
            })
            assert chat["choices"], "router did not forward request-level thinking controls"
            diagnostic = request(base, "/api/diagnostics")
            assert "model_directory" not in diagnostic["router"] and diagnostic["profile_count"] == 1
            sessions = request(base, "/api/log-sessions")["sessions"]
            current = next(session for session in sessions if session["current"])
            log_data = request(base, "/api/logs?" + urllib.parse.urlencode({"session": current["name"]}))
            assert log_data["content"] and log_data["total_size"] >= len(log_data["content"])

            subprocess.run([str(binary_dir / ("test-moe-regression" + suffix)), "--qwen-fixtures-only"],
                           cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            request(backend, "/models?" + urllib.parse.urlencode({"reload": 1}))
            models = request(backend, "/models")["data"]
            qwen_id = next(model["id"] for model in models if model["id"] == "test-qwen38")
            qwen_profile = request(base, "/api/model-profile?" + urllib.parse.urlencode({"model": qwen_id}))["profile"]
            qwen_profile.update(mode="explicit", cache=1, slots=4, io_threads=2, inference_threads=2,
                                ctx_size=256, batch_size=16, ubatch_size=2, parallel=1,
                                mtp_enabled=True, mtp_model=str(root / "test-qwen38-mtp.gguf"),
                                mtp_cache_mib=1, ple_cache_mib=1, mtp_draft_tokens=2,
                                mtp_backend_sampling=False, cpu_repack="off", overlap="off", prefetch="off")
            invalid = {**qwen_profile, "mtp_model": str(root / "missing.gguf")}
            try:
                request(base, "/api/model-profile", {"model": qwen_id, "profile": invalid})
                raise AssertionError("manager accepted a missing MTP sidecar")
            except RuntimeError as error:
                assert "MTP sidecar" in str(error), error
            request(base, "/api/model-profile", {"model": qwen_id, "profile": qwen_profile})
            request(backend, "/models/load", {"model": qwen_id})
            deadline = time.monotonic() + 45
            while next(model for model in request(backend, "/models")["data"] if model["id"] == qwen_id)["status"]["value"] != "loaded":
                if time.monotonic() > deadline:
                    raise RuntimeError("router Qwen MTP startup timed out")
                time.sleep(.2)
            result = request(backend, "/completion", {"model": qwen_id, "prompt": "ABC", "n_predict": 16,
                             "temperature": 0, "ignore_eos": True})
            assert result["tokens_predicted"] == 16 and result["timings"]["draft_n"] > 0, result
            active = request(backend, "/sparkmoe/v1/config?" + urllib.parse.urlencode({"model": qwen_id}))
            assert active["mtp_separate_model"] and active["mtp_cache_bytes"] == 1048576
            assert active["ple_cache_bytes"] == 1048576
            request(base, "/api/stop", {})
            assert not request(base, "/api/status")["running"]
            assert (root / "manager-state" / "config.json").is_file()
            assert list((root / "manager-state" / "logs").glob("session-*.log"))
            print("PASS Desktop manager: hidden router, persistent config/profiles, routed generation, telemetry, logs and stop")
            print("PASS Desktop Qwen MTP: routed sidecar validation, launch, budgets and drafting")
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
    parser.add_argument("--qwen", action="store_true", help="Run the Qwen PLE/MTP HTTP regression")
    args = parser.parse_args()
    binary_dir = args.bin_dir.resolve()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="sparkmoe-\u05de\u05d5\u05d3\u05dc-") as folder:
        root = Path(folder)
        subprocess.run([str(binary_dir / ("test-moe-regression" + suffix)), "--fixtures-only"],
                       cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if args.qwen:
            subprocess.run([str(binary_dir / ("test-moe-regression" + suffix)), "--qwen-fixtures-only"],
                           cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            reference = run_qwen(binary_dir / ("llama-server" + suffix), root, args.log_dir, 0)
            for width in (1, 2, 3):
                assert run_qwen(binary_dir / ("llama-server" + suffix), root, args.log_dir, width) == reference, "MTP changed greedy output"
                run_qwen(binary_dir / ("llama-server" + suffix), root, args.log_dir, width, controlled=True)
            return
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
