#!/usr/bin/env python3
"""Check an extracted release using CPU or separately installed NVIDIA NVRTC."""
import argparse
import json
import os
from pathlib import Path
import subprocess

if not __debug__:
    raise RuntimeError("Run release verification without Python -O/PYTHONOPTIMIZE")

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
parser.add_argument("--nvrtc-library", type=Path)
parser.add_argument("--fallback-nvrtc-library", type=Path)
parser.add_argument("--profile-dir", type=Path)
args = parser.parse_args()
if args.backend != "cuda" and (args.nvrtc_library or args.fallback_nvrtc_library):
    parser.error("NVRTC library options require --backend cuda")
binary = args.binary.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
environment = {k: v for k, v in os.environ.items() if not k.upper().startswith("BRMINER_")}
if args.nvrtc_library:
    environment["BRMINER_NVRTC_LIBRARY"] = str(args.nvrtc_library.resolve())

def command(options, label, env=None):
    result = subprocess.run([str(binary)] + options, cwd=binary.parent,
                            env=environment if env is None else env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    (output / (label + ".log")).write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{label} exited {result.returncode}: {result.stdout}")
    return result.stdout

def benchmark(label, duration, warmup, env=None, expected_library=None):
    path = output / (label + ".json")
    command(["--benchmark", "--backend", args.backend, "--duration", str(duration),
             "--warmup", str(warmup), "--quiet", "--json", str(path)], label, env)
    data = json.loads(path.read_text())
    assert data["schema"] == "brminer.benchmark.v1"
    assert data["backend"] == args.backend.upper()
    assert data["correctness_passed"] and not data["cancelled"]
    assert abs(data["measured_seconds"] - duration) < .1
    assert data["mean_janus_hps"] > 0
    assert abs(sum(s["interval_s"] for s in data["samples"]) - duration) < .1
    if args.backend == "cuda":
        assert data["cuda_devices"]
        assert Path(data["compiler_library"]).is_file()
        if expected_library:
            assert Path(data["compiler_library"]).samefile(expected_library)
    return data

assert command(["--embedded-core-check"], "embedded-core").strip() == "BRMINER_EMBEDDED_CORE=7"
command(["--pow-self-test"], "randomx-known-answer")
primary = benchmark("installed-" + args.backend, 5, 2, expected_library=args.nvrtc_library)
without_monitor = benchmark("without-nvml", 2, 1, dict(environment, BRMINER_DISABLE_NVML="1"),
                            args.nvrtc_library)
assert not without_monitor["gpu_monitoring"]
assert all(not s["gpu_monitoring"] for s in without_monitor["samples"])
fallback = None
if args.fallback_nvrtc_library:
    fallback = benchmark("alternate-installed-nvrtc", 3, 1,
                         dict(environment, BRMINER_NVRTC_LIBRARY=str(args.fallback_nvrtc_library.resolve())),
                         args.fallback_nvrtc_library)
cached = None
if args.profile_dir:
    text = command(["--calibration-status", "--profile-dir", str(args.profile_dir.resolve())], "profile-reload")
    assert "Loaded calibrated hardware profile:" in text
    cached = True
report = {"schema": "brminer.release-smoke.v2", "binary": str(binary),
          "embedded_core": True, "randomx_known_answer": True,
          "backend": args.backend, "correctness_passed": True,
          "compiler_library": primary.get("compiler_library"),
          "alternate_installed_compiler": fallback is not None,
          "mining_engine_without_nvml": True, "relocated_profile_reload": cached}
(output / "release-smoke.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report), flush=True)
