#!/usr/bin/env python3
"""Run bounded, repeatable BRminer Warthog efficiency experiments.

The launcher keeps an immutable reference binary, clears inherited BRminer
settings, rejects concurrent miners, samples CPU/GPU state, and retains every
raw result.  It intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
from ctypes import wintypes
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CAMPAIGN = ROOT / "out" / "bench" / "bzminer-efficiency-2026-11-07"
KNOWN_MINERS = {
    "brminer.exe", "bzminer.exe", "wartsolver.exe", "wartnode-miner.exe",
    "wartnode desktop.exe", "wartnode.exe",
}
THERMAL_CLOCK_BITS = 32 | 64
SOURCE_PROVENANCE = (
    "CMakeLists.txt",
    "src/bounded_mpmc_ring.hpp",
    "src/warthog_cuda.cpp",
    "src/warthog_cuda.hpp",
    "src/warthog_cuda_kernel.cu",
    "src/warthog_stratum.cpp",
    "src/warthog_benchmark.cpp",
    "src/warthog_benchmark.hpp",
)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def command_output(command: list[str], cwd: Path = ROOT, timeout: float = 30) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=timeout, creationflags=hidden_process_flags())
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def optional_output(command: list[str], cwd: Path = ROOT, timeout: float = 30) -> str:
    try:
        return command_output(command, cwd, timeout)
    except (OSError, RuntimeError, subprocess.TimeoutExpired):
        return ""


def hidden_process_flags() -> int:
    return getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0


def child_process_flags() -> int:
    return getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if os.name == "nt" else 0


def sanitized_label(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}", value):
        raise ValueError(f"Unsafe experiment label: {value!r}")
    return value


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def trimmed_mean(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) >= 5:
        ordered = ordered[1:-1]
    return statistics.fmean(ordered)


def numeric(value: str) -> float | None:
    value = value.strip()
    if value in {"", "N/A", "[N/A]", "[Not Supported]", "Not Supported"}:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_reason(value: str) -> int | None:
    value = value.strip()
    try:
        return int(value, 0)
    except ValueError:
        return None


def gpu_sample() -> list[dict[str, Any]]:
    fields = (
        "name,uuid,driver_version,utilization.gpu,power.draw,temperature.gpu,"
        "clocks.gr,clocks.mem,power.limit,clocks_event_reasons.active"
    )
    output = command_output([
        "nvidia-smi", "--query-gpu=" + fields,
        "--format=csv,noheader,nounits",
    ], timeout=8)
    rows = []
    for values in csv.reader(output.splitlines()):
        if len(values) != 10:
            raise RuntimeError(f"Unexpected nvidia-smi row: {values}")
        rows.append({
            "name": values[0].strip(),
            "uuid": values[1].strip(),
            "driver": values[2].strip(),
            "utilization_pct": numeric(values[3]),
            "power_w": numeric(values[4]),
            "temperature_c": numeric(values[5]),
            "core_mhz": numeric(values[6]),
            "memory_mhz": numeric(values[7]),
            "power_limit_w": numeric(values[8]),
            "clock_reasons": parse_reason(values[9]),
        })
    return rows


class WindowsCpuSampler:
    """Low-overhead total and per-process CPU sampling through Win32."""

    TH32CS_SNAPPROCESS = 0x00000002
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", wintypes.WCHAR * 260),
        ]

    def __init__(self) -> None:
        self.available = os.name == "nt"
        self.previous_system: tuple[int, int, int] | None = None
        self.previous_processes: dict[int, tuple[str, int]] = {}
        if not self.available:
            return
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.GetSystemTimes.argtypes = [
            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
            ctypes.POINTER(wintypes.FILETIME),
        ]
        self.kernel32.GetSystemTimes.restype = wintypes.BOOL
        self.kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self.kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(self.PROCESSENTRY32W)]
        self.kernel32.Process32FirstW.restype = wintypes.BOOL
        self.kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(self.PROCESSENTRY32W)]
        self.kernel32.Process32NextW.restype = wintypes.BOOL
        self.kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel32.OpenProcess.restype = wintypes.HANDLE
        self.kernel32.GetProcessTimes.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        ]
        self.kernel32.GetProcessTimes.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL

    @staticmethod
    def _filetime(value: wintypes.FILETIME) -> int:
        return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)

    def _system(self) -> tuple[int, int, int]:
        idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
        if not self.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        return self._filetime(idle), self._filetime(kernel), self._filetime(user)

    def _process_time(self, pid: int) -> int | None:
        handle = self.kernel32.OpenProcess(self.PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None
        try:
            creation, exit_time, kernel, user = (wintypes.FILETIME() for _ in range(4))
            if not self.kernel32.GetProcessTimes(
                    handle, ctypes.byref(creation), ctypes.byref(exit_time),
                    ctypes.byref(kernel), ctypes.byref(user)):
                return None
            return self._filetime(kernel) + self._filetime(user)
        finally:
            self.kernel32.CloseHandle(handle)

    def _processes(self) -> dict[int, tuple[str, int]]:
        snapshot = self.kernel32.CreateToolhelp32Snapshot(self.TH32CS_SNAPPROCESS, 0)
        invalid = ctypes.c_void_p(-1).value
        if not snapshot or int(snapshot) == invalid:
            return {}
        result: dict[int, tuple[str, int]] = {}
        try:
            entry = self.PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(entry)
            more = bool(self.kernel32.Process32FirstW(snapshot, ctypes.byref(entry)))
            while more:
                pid = int(entry.th32ProcessID)
                ticks = self._process_time(pid)
                if ticks is not None:
                    result[pid] = (entry.szExeFile, ticks)
                more = bool(self.kernel32.Process32NextW(snapshot, ctypes.byref(entry)))
        finally:
            self.kernel32.CloseHandle(snapshot)
        return result

    def snapshot_names(self) -> dict[int, str]:
        if not self.available:
            return {}
        return {pid: item[0] for pid, item in self._processes().items()}

    def sample(self, miner_pid: int | None = None) -> dict[str, Any]:
        if not self.available:
            return {"available": False}
        system = self._system()
        processes = self._processes()
        value: dict[str, Any] = {"available": True}
        if self.previous_system is not None:
            idle_delta = system[0] - self.previous_system[0]
            kernel_delta = system[1] - self.previous_system[1]
            user_delta = system[2] - self.previous_system[2]
            total_delta = kernel_delta + user_delta
            busy_delta = total_delta - idle_delta
            if total_delta > 0:
                value["total_cpu_pct"] = max(0.0, min(100.0, 100.0 * busy_delta / total_delta))
                shares = []
                for pid, (name, ticks) in processes.items():
                    previous = self.previous_processes.get(pid)
                    if previous and ticks >= previous[1]:
                        shares.append({
                            "pid": pid,
                            "name": name,
                            "cpu_pct": 100.0 * (ticks - previous[1]) / total_delta,
                        })
                shares.sort(key=lambda item: item["cpu_pct"], reverse=True)
                value["top_processes"] = shares[:10]
                miner = next((item for item in shares if item["pid"] == miner_pid), None)
                value["miner_cpu_pct"] = miner["cpu_pct"] if miner else 0.0
                value["background_cpu_pct"] = max(
                    0.0, value["total_cpu_pct"] - value["miner_cpu_pct"])
        self.previous_system = system
        self.previous_processes = processes
        return value


def miner_processes(sampler: WindowsCpuSampler, exclude_pid: int | None = None) -> list[dict[str, Any]]:
    return [
        {"pid": pid, "name": name}
        for pid, name in sampler.snapshot_names().items()
        if pid != exclude_pid and name.lower() in KNOWN_MINERS
    ]


def clean_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("BRMINER_")}
    if extra:
        environment.update(extra)
    return environment


def discover_nvrtc() -> Path:
    candidates = [
        Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\bin\x64\nvrtc64_130_0.dll"),
        ROOT / "out" / "toolchain" / "cuda" / "extracted" / "nvidia" / "cuda_nvrtc" / "bin" / "nvrtc64_120_0.dll",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("NVRTC was not found; pass --nvrtc explicitly")


def discover_cuda_test() -> Path:
    candidates = [
        ROOT / "out" / "build" / "verify-msvc" / "warthog-cuda-tests.exe",
        ROOT / "out" / "build" / "windows" / "warthog-cuda-tests.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("warthog-cuda-tests.exe was not found; pass --cuda-test explicitly")


def cpu_description() -> str:
    if os.name == "nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                return str(winreg.QueryValueEx(key, "ProcessorNameString")[0]).strip()
        except OSError:
            pass
    return platform.processor() or "Unknown CPU"


def initialize_campaign(campaign: Path, binary: Path, nvrtc: Path, cuda_test: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    binary = binary.resolve()
    nvrtc = nvrtc.resolve()
    cuda_test = cuda_test.resolve()
    for path in (binary, nvrtc, cuda_test):
        if not path.is_file():
            raise FileNotFoundError(path)
    campaign.mkdir(parents=True, exist_ok=True)
    reference = campaign / "reference" / "brminer.exe"
    reference.parent.mkdir(parents=True, exist_ok=True)
    source_hash = sha256(binary)
    if reference.exists() and sha256(reference) != source_hash:
        raise RuntimeError("Campaign reference binary already exists with a different hash")
    if not reference.exists():
        shutil.copy2(binary, reference)

    status = optional_output(["git", "status", "--porcelain=v1"])
    source_hashes = {
        name: sha256(ROOT / name)
        for name in SOURCE_PROVENANCE if (ROOT / name).is_file()
    }
    manifest = {
        "schema": "brminer.efficiency-campaign.v1",
        "created_utc": utc_now(),
        "root": str(ROOT),
        "campaign": str(campaign),
        "reference_binary": str(reference),
        "reference_binary_sha256": source_hash,
        "original_binary": str(binary),
        "nvrtc_library": str(nvrtc),
        "nvrtc_sha256": sha256(nvrtc),
        "cuda_test": str(cuda_test),
        "cuda_test_sha256": sha256(cuda_test),
        "git_commit": optional_output(["git", "rev-parse", "HEAD"]),
        "git_status": status.splitlines(),
        "source_sha256": source_hashes,
        "host": {
            "hostname": platform.node(),
            "os": platform.platform(),
            "cpu": cpu_description(),
            "logical_processors": os.cpu_count(),
            "gpus": gpu_sample(),
        },
        "fixed_reference": {
            "threads": 14,
            "cuda_batch_size": 67108864,
            "cuda_block_size": 256,
        },
    }
    path = campaign / "campaign-manifest.json"
    if path.exists():
        previous = read_json(path)
        if previous["reference_binary_sha256"] != source_hash:
            raise RuntimeError("Existing campaign manifest names a different reference binary")
        manifest["created_utc"] = previous["created_utc"]
    atomic_json(path, manifest)
    return manifest


def load_campaign(campaign: Path) -> dict[str, Any]:
    path = campaign.resolve() / "campaign-manifest.json"
    if not path.is_file():
        raise FileNotFoundError(f"Initialize the campaign first: {path}")
    manifest = read_json(path)
    reference = Path(manifest["reference_binary"])
    if sha256(reference) != manifest["reference_binary_sha256"]:
        raise RuntimeError("Immutable reference binary hash no longer matches the manifest")
    if sha256(Path(manifest["nvrtc_library"])) != manifest["nvrtc_sha256"]:
        raise RuntimeError("NVRTC library hash no longer matches the manifest")
    return manifest


def stop_owned_process(process: subprocess.Popen[Any]) -> str:
    if process.poll() is not None:
        return "already_exited"
    if os.name == "nt":
        try:
            process.send_signal(signal.CTRL_BREAK_EVENT)
            process.wait(timeout=15)
            return "ctrl_break"
        except (OSError, subprocess.TimeoutExpired):
            pass
    else:
        try:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=15)
            return "sigint"
        except (OSError, subprocess.TimeoutExpired):
            pass
    process.terminate()
    try:
        process.wait(timeout=5)
        return "terminate"
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        return "kill_owned_process"


def longest_background_streak(samples: list[dict[str, Any]], threshold: float = 5.0) -> float:
    longest = current = 0.0
    previous_elapsed: float | None = None
    for sample in samples:
        elapsed = float(sample["elapsed_s"])
        interval = 0.0 if previous_elapsed is None else max(0.0, elapsed - previous_elapsed)
        background = sample.get("cpu", {}).get("background_cpu_pct")
        if background is not None and background > threshold:
            current += interval
            longest = max(longest, current)
        else:
            current = 0.0
        previous_elapsed = elapsed
    return longest


def evaluate_run(benchmark: dict[str, Any] | None, samples: list[dict[str, Any]],
                 requested: float, stop_reason: str) -> dict[str, Any]:
    checks: dict[str, bool] = {}
    if benchmark is None:
        checks["benchmark_json_present"] = False
        return {"valid": False, "checks": checks, "stop_reason": stop_reason}
    checks["benchmark_json_present"] = True
    checks["schema"] = benchmark.get("schema") == "brminer.benchmark.v1"
    checks["correctness"] = bool(benchmark.get("correctness_passed"))
    checks["not_cancelled"] = not bool(benchmark.get("cancelled"))
    checks["full_duration"] = abs(float(benchmark.get("measured_seconds", 0)) - requested) < 0.25
    mean = float(benchmark.get("mean_janus_hps", 0))
    deviation = float(benchmark.get("stddev_janus_hps", math.inf))
    cv = deviation / mean if mean > 0 else math.inf
    checks["positive_rate"] = mean > 0
    checks["cv_at_most_10_pct"] = cv <= 0.10
    checks["normal_exit"] = stop_reason == "completed"
    thermal = False
    maximum_temperature = None
    clocks: list[float] = []
    for sample in samples:
        for gpu in sample.get("gpus", []):
            reason = gpu.get("clock_reasons")
            thermal |= reason is not None and bool(int(reason) & THERMAL_CLOCK_BITS)
            temperature = gpu.get("temperature_c")
            if temperature is not None:
                maximum_temperature = max(maximum_temperature or temperature, temperature)
            clock = gpu.get("core_mhz")
            if clock is not None:
                clocks.append(float(clock))
        thermal |= bool(sample.get("concurrent_miners"))
    background_streak = longest_background_streak(samples)
    checks["no_thermal_slowdown"] = not thermal
    checks["temperature_below_85_c"] = maximum_temperature is None or maximum_temperature < 85
    checks["no_sustained_background_load"] = background_streak < 30.0
    checks["no_concurrent_miner"] = not any(sample.get("concurrent_miners") for sample in samples)
    return {
        "valid": all(checks.values()),
        "checks": checks,
        "stop_reason": stop_reason,
        "janus_cv_pct": cv * 100,
        "maximum_temperature_c": maximum_temperature,
        "median_core_mhz": median(clocks),
        "longest_background_above_5_pct_s": background_streak,
    }


def run_benchmark(campaign: Path, experiment: str, label: str, binary: Path,
                  nvrtc: Path, threads: int, batch: int, block: int,
                  warmup: int, duration: int, variant: str,
                  extra_environment: dict[str, str] | None = None,
                  force: bool = False) -> dict[str, Any]:
    sanitized_label(label)
    run_directory = campaign.resolve() / experiment / label
    metadata_path = run_directory / "metadata.json"
    validity_path = run_directory / "validity.json"
    if metadata_path.is_file() and validity_path.is_file() and not force:
        metadata = read_json(metadata_path)
        metadata["validity"] = read_json(validity_path)
        metadata["resumed"] = True
        return metadata
    run_directory.mkdir(parents=True, exist_ok=True)
    sampler = WindowsCpuSampler()
    competing = miner_processes(sampler)
    if competing:
        raise RuntimeError(f"Other miners are active: {competing}")
    binary = binary.resolve()
    nvrtc = nvrtc.resolve()
    benchmark_path = run_directory / "benchmark.json"
    command = [
        str(binary), "--benchmark", "--backend", "cuda",
        "--threads", str(threads),
        "--cuda-batch-size", str(batch),
        "--cuda-block-size", str(block),
        "--nvrtc-library", str(nvrtc),
        "--warmup", str(warmup), "--duration", str(duration),
        "--json", str(benchmark_path), "--quiet",
    ]
    metadata: dict[str, Any] = {
        "schema": "brminer.efficiency-run.v1",
        "experiment": experiment,
        "label": label,
        "variant": variant,
        "start_utc": utc_now(),
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "nvrtc_library": str(nvrtc),
        "nvrtc_sha256": sha256(nvrtc),
        "configuration": {
            "threads": threads, "cuda_batch_size": batch,
            "cuda_block_size": block,
        },
        "warmup_seconds": warmup,
        "measured_seconds": duration,
        "environment": dict(sorted((extra_environment or {}).items())),
        "command": command,
    }
    atomic_json(metadata_path, metadata)
    samples: list[dict[str, Any]] = []
    sample_path = run_directory / "system-samples.jsonl"
    environment = clean_environment(extra_environment)
    started = time.monotonic()
    stop_reason = "completed"
    stop_method = "natural_exit"
    timeout = warmup + duration + 180
    next_sample = 0.0
    next_progress = 0.0
    with (run_directory / "console.log").open("wb") as console, \
            sample_path.open("w", encoding="utf-8") as sample_stream:
        process = subprocess.Popen(
            command, cwd=ROOT, env=environment, stdout=console,
            stderr=subprocess.STDOUT, creationflags=child_process_flags())
        metadata["pid"] = process.pid
        atomic_json(metadata_path, metadata)
        sampler.sample(process.pid)
        while process.poll() is None:
            elapsed = time.monotonic() - started
            if (campaign / "STOP").exists():
                stop_reason = "stop_requested"
                break
            if elapsed > timeout:
                stop_reason = "timeout"
                break
            if elapsed >= next_sample:
                try:
                    gpus = gpu_sample()
                    cpu = sampler.sample(process.pid)
                    concurrent = miner_processes(sampler, process.pid)
                    row = {
                        "utc": utc_now(), "elapsed_s": elapsed,
                        "phase": "warmup" if elapsed < warmup else "measurement",
                        "gpus": gpus, "cpu": cpu,
                        "concurrent_miners": concurrent,
                    }
                    samples.append(row)
                    sample_stream.write(json.dumps(row, sort_keys=True) + "\n")
                    sample_stream.flush()
                    atomic_json(campaign / "current-state.json", row | {
                        "experiment": experiment, "label": label, "pid": process.pid,
                        "target_seconds": warmup + duration,
                    })
                    temperatures = [gpu["temperature_c"] for gpu in gpus
                                    if gpu.get("temperature_c") is not None]
                    if temperatures and max(temperatures) >= 85:
                        stop_reason = "temperature_limit"
                        break
                    if concurrent:
                        stop_reason = "concurrent_miner_detected"
                        break
                except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                    stop_reason = "monitoring_error"
                    metadata["monitoring_error"] = str(error)
                    break
                next_sample += 5.0
            if elapsed >= next_progress:
                print(json.dumps({
                    "experiment": experiment, "label": label,
                    "elapsed_s": round(elapsed, 1),
                    "phase": "warmup" if elapsed < warmup else "measurement",
                    "target_s": warmup + duration,
                }), flush=True)
                next_progress += 30.0
            time.sleep(0.25)
        if process.poll() is None:
            stop_method = stop_owned_process(process)
        else:
            process.wait()
    benchmark = None
    if benchmark_path.is_file():
        try:
            benchmark = read_json(benchmark_path)
        except (OSError, ValueError) as error:
            metadata["benchmark_json_error"] = str(error)
    if process.returncode and stop_reason == "completed":
        stop_reason = "early_exit"
    validity = evaluate_run(benchmark, samples, duration, stop_reason)
    atomic_json(validity_path, validity)
    metadata.update({
        "end_utc": utc_now(),
        "elapsed_seconds": time.monotonic() - started,
        "exit_code": process.returncode,
        "stop_reason": stop_reason,
        "stop_method": stop_method,
        "benchmark": str(benchmark_path) if benchmark is not None else None,
        "validity": validity,
    })
    atomic_json(metadata_path, metadata)
    print(json.dumps({
        "experiment": experiment, "label": label,
        "valid": validity["valid"],
        "janus_hps": benchmark.get("mean_janus_hps") if benchmark else None,
        "stop_reason": stop_reason,
    }), flush=True)
    return metadata


def benchmark_result(run: dict[str, Any]) -> dict[str, Any]:
    path = run.get("benchmark")
    if not path:
        path = str(Path(run["command"][run["command"].index("--json") + 1]))
    return read_json(Path(path))


def markdown_table(headers: list[str], rows: list[list[Any]]) -> str:
    lines = ["| " + " | ".join(headers) + " |",
             "| " + " | ".join("---" for _ in headers) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def run_e00(campaign: Path, warmup: int, duration: int, force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    binary = Path(manifest["reference_binary"])
    nvrtc = Path(manifest["nvrtc_library"])
    config = manifest["fixed_reference"]
    runs = []
    for index in range(1, 5):
        runs.append(run_benchmark(
            campaign, "E00-baseline", f"A{index}", binary, nvrtc,
            config["threads"], config["cuda_batch_size"], config["cuda_block_size"],
            warmup, duration, "immutable-production", force=force))
        if index != 4:
            time.sleep(15)
    rates = [float(benchmark_result(run)["mean_janus_hps"]) for run in runs]
    cvs = [float(run["validity"].get("janus_cv_pct", math.inf)) for run in runs]
    drift = 100.0 * (max(rates) - min(rates)) / statistics.fmean(rates)
    summary = {
        "schema": "brminer.efficiency-e00.v1",
        "created_utc": utc_now(),
        "runs": [run["label"] for run in runs],
        "all_valid": all(run["validity"]["valid"] for run in runs),
        "mean_janus_hps": statistics.fmean(rates),
        "median_janus_hps": statistics.median(rates),
        "run_rates_hps": rates,
        "repeat_drift_pct": drift,
        "maximum_run_cv_pct": max(cvs),
    }
    summary["repeatable"] = (
        summary["all_valid"] and drift <= 5.0 and max(cvs) <= 10.0)
    directory = campaign / "E00-baseline"
    atomic_json(directory / "summary.json", summary)
    rows = [[f"A{i + 1}", f"{rate / 1e6:.3f}", f"{cvs[i]:.3f}",
             runs[i]["validity"]["valid"]] for i, rate in enumerate(rates)]
    text = "# E00 baseline repeatability\n\n" + markdown_table(
        ["Run", "Janus MH/s", "CV %", "Valid"], rows)
    text += (f"\n\nMean: {summary['mean_janus_hps'] / 1e6:.3f} MH/s. "
             f"Repeat drift: {drift:.3f}%. Repeatable: {summary['repeatable']}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)
    return summary


CUDA_RATE = re.compile(r"CUDA scan MH/s=([0-9]+(?:\.[0-9]+)?)")


def cuda_probe(cuda_test: Path, nvrtc: Path, batch: int, block: int,
               repetitions: int, directory: Path, force: bool) -> dict[str, Any]:
    directory.mkdir(parents=True, exist_ok=True)
    result_path = directory / "result.json"
    if result_path.is_file() and not force:
        return read_json(result_path)
    environment = clean_environment({"BRMINER_NVRTC_DLL": str(nvrtc)})
    rates: list[float] = []
    logs = []
    for index in range(repetitions + 1):
        result = subprocess.run(
            [str(cuda_test), "--bench", str(batch), str(block)],
            cwd=ROOT, env=environment, capture_output=True, text=True,
            timeout=120, creationflags=hidden_process_flags())
        output = result.stdout + result.stderr
        (directory / f"probe-{index:02d}.log").write_text(output, encoding="utf-8")
        match = CUDA_RATE.search(output)
        logs.append({"index": index, "exit_code": result.returncode,
                     "rate_mh_s": float(match.group(1)) if match else None})
        if result.returncode or not match or "passed on" not in output:
            raise RuntimeError(f"CUDA probe failed for batch={batch}, block={block}: {output}")
        if index:
            rates.append(float(match.group(1)))
    value = {
        "batch_size": batch,
        "block_size": block,
        "warmup_repetitions": 1,
        "measured_repetitions": repetitions,
        "rates_mh_s": rates,
        "median_mh_s": statistics.median(rates),
        "trimmed_mean_mh_s": trimmed_mean(rates),
        "runs": logs,
    }
    atomic_json(result_path, value)
    return value


def paired_screen(candidate: dict[str, int], champion: dict[str, int],
                  campaign: Path, directory_name: str, binary: Path, nvrtc: Path,
                  warmup: int, duration: int, force: bool) -> dict[str, Any]:
    order = [("A1", champion, "champion"), ("B1", candidate, "candidate"),
             ("B2", candidate, "candidate"), ("A2", champion, "champion")]
    runs: dict[str, dict[str, Any]] = {}
    for index, (label, config, role) in enumerate(order):
        runs[label] = run_benchmark(
            campaign, f"E01-configuration/{directory_name}", label,
            binary, nvrtc, config["threads"], config["cuda_batch_size"],
            config["cuda_block_size"], warmup, duration, role, force=force)
        if index != len(order) - 1:
            time.sleep(15)
    rates = {label: float(benchmark_result(run)["mean_janus_hps"])
             for label, run in runs.items()}
    ratios = [rates["B1"] / rates["A1"], rates["B2"] / rates["A2"]]
    valid = all(run["validity"]["valid"] for run in runs.values())
    clocks = {label: run["validity"].get("median_core_mhz") for label, run in runs.items()}
    clock_pairs_valid = all(
        clocks[a] is None or clocks[b] is None or abs(clocks[a] - clocks[b]) <= 15.0
        for a, b in (("A1", "B1"), ("A2", "B2")))
    screen = duration < 300
    passed = (valid and clock_pairs_valid and
              (statistics.fmean(ratios) >= (1.005 if screen else 1.015)) and
              all(value >= (0.995 if screen else 1.01) for value in ratios))
    result = {
        "candidate": candidate,
        "champion": champion,
        "warmup_seconds": warmup,
        "measured_seconds": duration,
        "rates_hps": rates,
        "paired_ratios": ratios,
        "mean_ratio": statistics.fmean(ratios),
        "all_runs_valid": valid,
        "clock_pairs_valid": clock_pairs_valid,
        "passed": passed,
        "tier": "screen" if screen else "validation",
    }
    directory = campaign / "E01-configuration" / directory_name
    atomic_json(directory / "decision.json", result)
    return result


def paired_custom(candidate: dict[str, Any], champion: dict[str, Any],
                  campaign: Path, experiment: str, binary_candidate: Path,
                  binary_champion: Path, nvrtc: Path, warmup: int, duration: int,
                  force: bool) -> dict[str, Any]:
    order = [("A1", champion), ("B1", candidate),
             ("B2", candidate), ("A2", champion)]
    runs: dict[str, dict[str, Any]] = {}
    for index, (label, spec) in enumerate(order):
        binary = binary_candidate if label.startswith("B") else binary_champion
        config = spec["configuration"]
        runs[label] = run_benchmark(
            campaign, experiment, label, binary, nvrtc,
            config["threads"], config["cuda_batch_size"], config["cuda_block_size"],
            warmup, duration, spec["variant"], spec.get("environment"), force)
        if index != len(order) - 1:
            time.sleep(15)
    rates = {label: float(benchmark_result(run)["mean_janus_hps"])
             for label, run in runs.items()}
    ratios = [rates["B1"] / rates["A1"], rates["B2"] / rates["A2"]]
    valid = all(run["validity"]["valid"] for run in runs.values())
    clocks = {label: run["validity"].get("median_core_mhz") for label, run in runs.items()}
    clock_pairs_valid = all(
        clocks[a] is None or clocks[b] is None or abs(clocks[a] - clocks[b]) <= 15.0
        for a, b in (("A1", "B1"), ("A2", "B2")))
    screen = duration < 300
    passed = (valid and clock_pairs_valid and
              statistics.fmean(ratios) >= (1.005 if screen else 1.015) and
              all(value >= (0.995 if screen else 1.01) for value in ratios))
    result = {
        "candidate": candidate,
        "champion": champion,
        "candidate_binary": str(binary_candidate),
        "candidate_binary_sha256": sha256(binary_candidate),
        "champion_binary": str(binary_champion),
        "champion_binary_sha256": sha256(binary_champion),
        "warmup_seconds": warmup,
        "measured_seconds": duration,
        "rates_hps": rates,
        "paired_ratios": ratios,
        "mean_ratio": statistics.fmean(ratios),
        "all_runs_valid": valid,
        "clock_pairs_valid": clock_pairs_valid,
        "passed": passed,
        "tier": "screen" if screen else "validation",
    }
    atomic_json(campaign / experiment / "decision.json", result)
    return result


def config_key(config: dict[str, int]) -> tuple[int, int, int]:
    return config["threads"], config["cuda_batch_size"], config["cuda_block_size"]


def run_e01(campaign: Path, repetitions: int, quick_warmup: int, quick_duration: int,
            screen_warmup: int, screen_duration: int, validate: bool,
            validation_warmup: int, validation_duration: int,
            force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    if not (campaign / "E00-baseline" / "summary.json").is_file():
        raise RuntimeError("Run E00 before E01")
    e00 = read_json(campaign / "E00-baseline" / "summary.json")
    if not e00.get("repeatable"):
        raise RuntimeError("E00 did not establish a repeatable baseline")
    binary = Path(manifest["reference_binary"])
    nvrtc = Path(manifest["nvrtc_library"])
    cuda_test = Path(manifest["cuda_test"])
    directory = campaign / "E01-configuration"
    micro = directory / "micro"
    batches = [1 << power for power in range(20, 27)]
    batch_probes = [cuda_probe(cuda_test, nvrtc, batch, 256, repetitions,
                               micro / f"batch-{batch}-block-256", force)
                    for batch in batches]
    ranked_batches = sorted(batch_probes, key=lambda item: item["median_mh_s"], reverse=True)
    top_batches = [item["batch_size"] for item in ranked_batches[:2]]
    block_probes = []
    for batch in top_batches:
        for block in (64, 128, 256, 512):
            block_probes.append(cuda_probe(
                cuda_test, nvrtc, batch, block, repetitions,
                micro / f"batch-{batch}-block-{block}", force))
    ranked_launches = sorted(block_probes, key=lambda item: item["median_mh_s"], reverse=True)
    launch_settings: list[tuple[int, int]] = []
    for item in ranked_launches:
        setting = (item["batch_size"], item["block_size"])
        if setting not in launch_settings:
            launch_settings.append(setting)
        if len(launch_settings) == 2:
            break
    micro_summary = {
        "batch_probes": batch_probes,
        "top_batches": top_batches,
        "block_probes": block_probes,
        "shortlisted_launch_settings": [
            {"cuda_batch_size": batch, "cuda_block_size": block}
            for batch, block in launch_settings
        ],
    }
    atomic_json(micro / "summary.json", micro_summary)

    quick_runs = []
    for threads in (8, 12, 14, 15, 16):
        for batch, block in launch_settings:
            label = f"t{threads}-b{batch}-k{block}"
            run = run_benchmark(
                campaign, "E01-configuration/quick", label, binary, nvrtc,
                threads, batch, block, quick_warmup, quick_duration,
                "configuration-quick", force=force)
            result = benchmark_result(run)
            quick_runs.append({
                "label": label,
                "configuration": {"threads": threads, "cuda_batch_size": batch,
                                  "cuda_block_size": block},
                "janus_hps": float(result["mean_janus_hps"]),
                "valid": run["validity"]["valid"],
            })
            time.sleep(5)
    quick_runs.sort(key=lambda item: item["janus_hps"], reverse=True)
    champion = dict(manifest["fixed_reference"])
    candidates = []
    seen = {config_key(champion)}
    for item in quick_runs:
        key = config_key(item["configuration"])
        if item["valid"] and key not in seen:
            candidates.append(item["configuration"])
            seen.add(key)
        if len(candidates) == 4:
            break

    screens = []
    for index, candidate in enumerate(candidates, 1):
        screens.append(paired_screen(
            candidate, champion, campaign, f"screen-{index}", binary, nvrtc,
            screen_warmup, screen_duration, force))
    advanced = [result for result in screens if result["passed"]]
    advanced.sort(key=lambda item: item["mean_ratio"], reverse=True)
    validation = None
    if validate and advanced:
        validation = paired_screen(
            advanced[0]["candidate"], champion, campaign, "validation",
            binary, nvrtc, validation_warmup, validation_duration, force)

    summary = {
        "schema": "brminer.efficiency-e01.v1",
        "created_utc": utc_now(),
        "micro": micro_summary,
        "quick_runs": quick_runs,
        "screen_candidates": candidates,
        "screens": screens,
        "validation": validation,
        "validated_configuration": (
            validation["candidate"] if validation and validation["passed"] else champion),
        "production_configuration_retained": not bool(validation and validation["passed"]),
    }
    atomic_json(directory / "summary.json", summary)
    rows = []
    for index, result in enumerate(screens, 1):
        config = result["candidate"]
        rows.append([
            index, config["threads"], config["cuda_batch_size"], config["cuda_block_size"],
            f"{(result['mean_ratio'] - 1) * 100:+.3f}%", result["passed"],
        ])
    text = "# E01 configuration experiment\n\n" + markdown_table(
        ["Candidate", "Threads", "Batch", "Block", "Screen delta", "Advanced"], rows)
    if validation:
        text += (f"\n\nValidation delta: {(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    else:
        text += "\n\nNo candidate reached full validation.\n"
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "screens": len(screens),
        "advanced": len(advanced),
        "validation_passed": bool(validation and validation["passed"]),
        "selected": summary["validated_configuration"],
    }, indent=2), flush=True)
    return summary


def run_e02(campaign: Path, candidate_binary: Path, screen_warmup: int,
            screen_duration: int, validate: bool, validation_warmup: int,
            validation_duration: int, force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E02")
    e01 = read_json(e01_path)
    champion_config = dict(e01["validated_configuration"])
    candidate_config = dict(champion_config)
    candidate_config["threads"] = 15
    champion = {
        "variant": "sync-champion",
        "configuration": champion_config,
        "environment": {},
    }
    candidate = {
        "variant": "depth2-t15",
        "configuration": candidate_config,
        "environment": {"BRMINER_EXPERIMENT_PIPELINE": "depth2"},
    }
    candidate_binary = candidate_binary.resolve()
    if not candidate_binary.is_file():
        raise FileNotFoundError(candidate_binary)
    reference = Path(manifest["reference_binary"])
    nvrtc = Path(manifest["nvrtc_library"])
    screen = paired_custom(
        candidate, champion, campaign, "E02-pipeline/screen",
        candidate_binary, reference, nvrtc, screen_warmup, screen_duration, force)
    validation = None
    if validate and screen["passed"]:
        validation = paired_custom(
            candidate, champion, campaign, "E02-pipeline/validation",
            candidate_binary, reference, nvrtc, validation_warmup,
            validation_duration, force)
    summary = {
        "schema": "brminer.efficiency-e02.v1",
        "created_utc": utc_now(),
        "screen": screen,
        "validation": validation,
        "validated": bool(validation and validation["passed"]),
        "selected": candidate if validation and validation["passed"] else champion,
    }
    directory = campaign / "E02-pipeline"
    atomic_json(directory / "summary.json", summary)
    text = "# E02 persistent CUDA pipeline\n\n"
    text += (f"Screen mean delta: {(screen['mean_ratio'] - 1) * 100:+.3f}%; "
             f"passed: {screen['passed']}.\n")
    if validation:
        text += (f"\nValidation mean delta: {(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "screen_delta_pct": (screen["mean_ratio"] - 1) * 100,
        "screen_passed": screen["passed"],
        "validation_delta_pct": ((validation["mean_ratio"] - 1) * 100
                                  if validation else None),
        "validation_passed": bool(validation and validation["passed"]),
    }, indent=2), flush=True)
    return summary


def run_e05(campaign: Path, candidate_binary: Path, quick_warmup: int,
            quick_duration: int, screen_warmup: int, screen_duration: int,
            validate: bool, validation_warmup: int, validation_duration: int,
            force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E05")
    champion_config = dict(read_json(e01_path)["validated_configuration"])
    batch = champion_config["cuda_batch_size"]
    block = champion_config["cuda_block_size"]
    candidate_binary = candidate_binary.resolve()
    if not candidate_binary.is_file():
        raise FileNotFoundError(candidate_binary)
    nvrtc = Path(manifest["nvrtc_library"])
    reference = Path(manifest["reference_binary"])
    variants = [
        {
            "variant": "none-t16",
            "configuration": {"threads": 16, "cuda_batch_size": batch,
                              "cuda_block_size": block},
            "environment": {"BRMINER_EXPERIMENT_AFFINITY": "none"},
        },
        {
            "variant": "physical-first-t16",
            "configuration": {"threads": 16, "cuda_batch_size": batch,
                              "cuda_block_size": block},
            "environment": {"BRMINER_EXPERIMENT_AFFINITY": "physical-first"},
        },
        {
            "variant": "physical-first-t15",
            "configuration": {"threads": 15, "cuda_batch_size": batch,
                              "cuda_block_size": block},
            "environment": {"BRMINER_EXPERIMENT_AFFINITY": "physical-first"},
        },
        {
            "variant": "bz-like-t15",
            "configuration": {"threads": 15, "cuda_batch_size": batch,
                              "cuda_block_size": block},
            "environment": {"BRMINER_EXPERIMENT_AFFINITY": "bz-like"},
        },
        {
            "variant": "reserve-core-t14",
            "configuration": {"threads": 14, "cuda_batch_size": batch,
                              "cuda_block_size": block},
            "environment": {"BRMINER_EXPERIMENT_AFFINITY": "reserve-core"},
        },
    ]
    quick_results = []
    for index, variant in enumerate(variants):
        config = variant["configuration"]
        run = run_benchmark(
            campaign, "E05-affinity/quick", variant["variant"], candidate_binary,
            nvrtc, config["threads"], config["cuda_batch_size"],
            config["cuda_block_size"], quick_warmup, quick_duration,
            variant["variant"], variant["environment"], force)
        quick_results.append({
            "variant": variant,
            "janus_hps": float(benchmark_result(run)["mean_janus_hps"]),
            "valid": run["validity"]["valid"],
            "validity": run["validity"],
        })
        if index != len(variants) - 1:
            time.sleep(5)
    control = variants[0]
    eligible = [result for result in quick_results[1:] if result["valid"]]
    eligible.sort(key=lambda result: result["janus_hps"], reverse=True)
    screen = None
    validation = None
    selected = control
    if eligible:
        candidate = eligible[0]["variant"]
        screen = paired_custom(
            candidate, control, campaign, "E05-affinity/screen",
            candidate_binary, candidate_binary, nvrtc, screen_warmup,
            screen_duration, force)
        if validate and screen["passed"]:
            reference_champion = {
                "variant": "immutable-sync-champion",
                "configuration": champion_config,
                "environment": {},
            }
            validation = paired_custom(
                candidate, reference_champion, campaign, "E05-affinity/validation",
                candidate_binary, reference, nvrtc, validation_warmup,
                validation_duration, force)
            if validation["passed"]:
                selected = candidate
    summary = {
        "schema": "brminer.efficiency-e05.v1",
        "created_utc": utc_now(),
        "candidate_binary": str(candidate_binary),
        "candidate_binary_sha256": sha256(candidate_binary),
        "quick_results": quick_results,
        "screen": screen,
        "validation": validation,
        "validated": bool(validation and validation["passed"]),
        "selected": selected,
    }
    directory = campaign / "E05-affinity"
    atomic_json(directory / "summary.json", summary)
    rows = []
    for result in sorted(quick_results, key=lambda item: item["janus_hps"], reverse=True):
        spec = result["variant"]
        rows.append([
            spec["variant"], spec["configuration"]["threads"],
            f"{result['janus_hps'] / 1e6:.3f}", result["valid"],
        ])
    text = "# E05 CPU affinity and feeder isolation\n\n" + markdown_table(
        ["Variant", "Threads", "Quick Janus MH/s", "Valid"], rows)
    if screen:
        text += (f"\n\nScreen mean delta: {(screen['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {screen['passed']}.\n")
    if validation:
        text += (f"\nValidation mean delta: "
                 f"{(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "quick_winner": eligible[0]["variant"]["variant"] if eligible else None,
        "screen_delta_pct": ((screen["mean_ratio"] - 1) * 100 if screen else None),
        "screen_passed": bool(screen and screen["passed"]),
        "validation_delta_pct": (
            (validation["mean_ratio"] - 1) * 100 if validation else None),
        "validation_passed": bool(validation and validation["passed"]),
    }, indent=2), flush=True)
    return summary


def queue_observation(benchmark: dict[str, Any]) -> dict[str, Any]:
    stats = benchmark.get("queue_stats", {})
    consumer = (int(stats.get("consumer_lock_wait_nanos", 0)) +
                int(stats.get("consumer_lock_hold_nanos", 0)))
    producer = (int(stats.get("producer_lock_wait_nanos", 0)) +
                int(stats.get("producer_lock_hold_nanos", 0)))
    capacity = (float(benchmark.get("elapsed_seconds", 0)) *
                int(benchmark.get("threads", 0)) * 1e9)
    return {
        "queue_policy": benchmark.get("queue_policy"),
        "consumer_lock_nanos": consumer,
        "producer_lock_nanos": producer,
        "aggregate_worker_cpu_pct": (
            100.0 * (consumer + producer) / capacity if capacity else None),
        "consumer_lock_acquisitions": stats.get("consumer_lock_acquisitions"),
        "empty_checks": stats.get("empty_checks"),
        "fast_empty_checks": stats.get("fast_empty_checks"),
        "batches_taken": stats.get("batches_taken"),
        "producer_full_wait_nanos": stats.get("producer_full_wait_nanos"),
        "high_water_candidates": stats.get("high_water_candidates"),
    }


def run_e06(campaign: Path, candidate_binary: Path, observation_warmup: int,
            observation_duration: int, screen_warmup: int, screen_duration: int,
            validate: bool, validation_warmup: int, validation_duration: int,
            force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E06")
    configuration = dict(read_json(e01_path)["validated_configuration"])
    candidate_binary = candidate_binary.resolve()
    if not candidate_binary.is_file():
        raise FileNotFoundError(candidate_binary)
    nvrtc = Path(manifest["nvrtc_library"])
    reference = Path(manifest["reference_binary"])
    observed_run = run_benchmark(
        campaign, "E06-queue/observation", "instrumented-flag-fastpath",
        candidate_binary, nvrtc, configuration["threads"],
        configuration["cuda_batch_size"], configuration["cuda_block_size"],
        observation_warmup, observation_duration, "instrumented-flag-fastpath",
        {"BRMINER_EXPERIMENT_QUEUE": "instrumented-fastpath"}, force)
    observed = queue_observation(benchmark_result(observed_run))
    baseline_path = (campaign / "E06-queue" / "observation" /
                     "instrumented-deque" / "benchmark.json")
    baseline_observed = (queue_observation(read_json(baseline_path))
                         if baseline_path.is_file() else None)
    champion = {
        "variant": "deque",
        "configuration": configuration,
        "environment": {"BRMINER_EXPERIMENT_QUEUE": "deque"},
    }
    candidate = {
        "variant": "deque-flag-fastpath",
        "configuration": configuration,
        "environment": {"BRMINER_EXPERIMENT_QUEUE": "deque-fastpath"},
    }
    screen = paired_custom(
        candidate, champion, campaign, "E06-queue/flag-screen",
        candidate_binary, candidate_binary, nvrtc, screen_warmup,
        screen_duration, force)
    validation = None
    if validate and screen["passed"]:
        immutable = {
            "variant": "immutable-deque-champion",
            "configuration": configuration,
            "environment": {},
        }
        validation = paired_custom(
            candidate, immutable, campaign, "E06-queue/flag-validation",
            candidate_binary, reference, nvrtc, validation_warmup,
            validation_duration, force)
    count_screen_path = campaign / "E06-queue" / "screen" / "decision.json"
    summary = {
        "schema": "brminer.efficiency-e06.v1",
        "created_utc": utc_now(),
        "candidate_binary": str(candidate_binary),
        "candidate_binary_sha256": sha256(candidate_binary),
        "baseline_observation": baseline_observed,
        "count_fastpath_screen": (read_json(count_screen_path)
                                  if count_screen_path.is_file() else None),
        "fastpath_observation": observed,
        "screen": screen,
        "validation": validation,
        "validated": bool(validation and validation["passed"]),
        "selected": candidate if validation and validation["passed"] else champion,
    }
    directory = campaign / "E06-queue"
    atomic_json(directory / "summary.json", summary)
    text = "# E06 candidate queue\n\n"
    if baseline_observed:
        text += (f"Instrumented deque lock cost: "
                 f"{baseline_observed['aggregate_worker_cpu_pct']:.3f}% of aggregate "
                 "worker CPU time.\n\n")
    text += (f"Instrumented fast-path lock cost: "
             f"{observed['aggregate_worker_cpu_pct']:.3f}% of aggregate worker CPU "
             "time.\n\n")
    text += (f"Screen mean delta: {(screen['mean_ratio'] - 1) * 100:+.3f}%; "
             f"passed: {screen['passed']}.\n")
    if validation:
        text += (f"\nValidation mean delta: "
                 f"{(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "baseline_lock_cpu_pct": (
            baseline_observed["aggregate_worker_cpu_pct"]
            if baseline_observed else None),
        "fastpath_lock_cpu_pct": observed["aggregate_worker_cpu_pct"],
        "screen_delta_pct": (screen["mean_ratio"] - 1) * 100,
        "screen_passed": screen["passed"],
        "validation_delta_pct": (
            (validation["mean_ratio"] - 1) * 100 if validation else None),
        "validation_passed": bool(validation and validation["passed"]),
    }, indent=2), flush=True)
    return summary


def filter_diagnostics(benchmark: dict[str, Any]) -> dict[str, Any]:
    samples = benchmark.get("samples", [])
    queue_depths = [float(item.get("queue_depth", 0)) for item in samples]
    queue_wait = [float(item.get("gpu_queue_wait_pct", 0)) for item in samples]
    gpu_rates = [float(item.get("gpu_sha_hps", 0)) for item in samples]
    verus_rates = [float(item.get("verus_hps", 0)) for item in samples]
    thresholds = [int(item.get("gpu_filter_threshold", 0)) for item in samples
                  if item.get("gpu_filter_threshold") is not None]
    return {
        "janus_hps": float(benchmark.get("mean_janus_hps", 0)),
        "gpu_sha_hps": statistics.fmean(gpu_rates) if gpu_rates else None,
        "verus_hps": statistics.fmean(verus_rates) if verus_rates else None,
        "mean_queue_depth": statistics.fmean(queue_depths) if queue_depths else None,
        "maximum_queue_depth": max(queue_depths) if queue_depths else None,
        "queue_depth_stddev": (statistics.pstdev(queue_depths)
                               if len(queue_depths) > 1 else 0.0),
        "mean_gpu_queue_wait_pct": statistics.fmean(queue_wait) if queue_wait else None,
        "minimum_measured_threshold": min(thresholds) if thresholds else None,
        "maximum_measured_threshold": max(thresholds) if thresholds else None,
        "filter_stats": benchmark.get("filter_stats"),
    }


def run_e07(campaign: Path, candidate_binary: Path, quick_warmup: int,
            quick_duration: int, screen_warmup: int, screen_duration: int,
            screen_count: int, validate: bool, validation_warmup: int,
            validation_duration: int, force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E07")
    configuration = dict(read_json(e01_path)["validated_configuration"])
    candidate_binary = candidate_binary.resolve()
    if not candidate_binary.is_file():
        raise FileNotFoundError(candidate_binary)
    nvrtc = Path(manifest["nvrtc_library"])
    reference = Path(manifest["reference_binary"])
    policies = [
        "current",
        "i250-h2-t50", "i1000-h2-t50",
        "i500-h1-t50", "i500-h4-t50",
        "i500-h2-t25", "i500-h2-t75",
    ]
    quick_results = []
    for index, policy in enumerate(policies):
        run = run_benchmark(
            campaign, "E07-filter/quick", policy, candidate_binary, nvrtc,
            configuration["threads"], configuration["cuda_batch_size"],
            configuration["cuda_block_size"], quick_warmup, quick_duration,
            policy, {"BRMINER_EXPERIMENT_FILTER": policy}, force)
        quick_results.append({
            "policy": policy,
            "valid": run["validity"]["valid"],
            "validity": run["validity"],
            "diagnostics": filter_diagnostics(benchmark_result(run)),
        })
        if index != len(policies) - 1:
            time.sleep(5)
    control = {
        "variant": "filter-current",
        "configuration": configuration,
        "environment": {"BRMINER_EXPERIMENT_FILTER": "current"},
    }
    eligible = [item for item in quick_results
                if item["policy"] != "current" and item["valid"]]
    eligible.sort(key=lambda item: item["diagnostics"]["janus_hps"], reverse=True)
    screens = []
    for index, item in enumerate(eligible[:max(0, screen_count)], 1):
        candidate = {
            "variant": "filter-" + item["policy"],
            "configuration": configuration,
            "environment": {"BRMINER_EXPERIMENT_FILTER": item["policy"]},
        }
        screens.append(paired_custom(
            candidate, control, campaign, f"E07-filter/screen-{index}",
            candidate_binary, candidate_binary, nvrtc, screen_warmup,
            screen_duration, force))
    advanced = [screen for screen in screens if screen["passed"]]
    advanced.sort(key=lambda screen: screen["mean_ratio"], reverse=True)
    validation = None
    if validate and advanced:
        immutable = {
            "variant": "immutable-filter-champion",
            "configuration": configuration,
            "environment": {},
        }
        validation = paired_custom(
            advanced[0]["candidate"], immutable, campaign,
            "E07-filter/validation", candidate_binary, reference, nvrtc,
            validation_warmup, validation_duration, force)
    summary = {
        "schema": "brminer.efficiency-e07.v1",
        "created_utc": utc_now(),
        "candidate_binary": str(candidate_binary),
        "candidate_binary_sha256": sha256(candidate_binary),
        "quick_results": quick_results,
        "screens": screens,
        "validation": validation,
        "validated": bool(validation and validation["passed"]),
        "selected": (advanced[0]["candidate"]
                     if validation and validation["passed"] else control),
    }
    directory = campaign / "E07-filter"
    atomic_json(directory / "summary.json", summary)
    rows = []
    for item in sorted(quick_results,
                       key=lambda result: result["diagnostics"]["janus_hps"],
                       reverse=True):
        diagnostics = item["diagnostics"]
        rows.append([
            item["policy"], f"{diagnostics['janus_hps'] / 1e6:.3f}",
            f"{diagnostics['gpu_sha_hps'] / 1e6:.3f}",
            f"{diagnostics['verus_hps'] / 1e6:.3f}",
            f"{diagnostics['mean_queue_depth']:.0f}", item["valid"],
        ])
    text = "# E07 candidate-filter controller\n\n" + markdown_table(
        ["Policy", "Janus MH/s", "GPU MH/s", "Verus MH/s", "Mean queue", "Valid"],
        rows)
    for index, screen in enumerate(screens, 1):
        text += (f"\n\nScreen {index} ({screen['candidate']['variant']}): "
                 f"{(screen['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {screen['passed']}.")
    if validation:
        text += (f"\n\nValidation: {(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    else:
        text += "\n"
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "quick_ranking": [item["policy"] for item in eligible],
        "screen_deltas_pct": [(screen["mean_ratio"] - 1) * 100
                              for screen in screens],
        "screen_passes": [screen["passed"] for screen in screens],
        "validation_delta_pct": (
            (validation["mean_ratio"] - 1) * 100 if validation else None),
        "validation_passed": bool(validation and validation["passed"]),
    }, indent=2), flush=True)
    return summary


def run_e08(campaign: Path, candidate_binary: Path, cubin: Path,
            startup_repetitions: int, screen_warmup: int, screen_duration: int,
            validate: bool, validation_warmup: int, validation_duration: int,
            force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E08")
    configuration = dict(read_json(e01_path)["validated_configuration"])
    candidate_binary = candidate_binary.resolve()
    cubin = cubin.resolve()
    if not candidate_binary.is_file():
        raise FileNotFoundError(candidate_binary)
    if not cubin.is_file():
        raise FileNotFoundError(cubin)
    nvrtc = Path(manifest["nvrtc_library"])
    reference = Path(manifest["reference_binary"])
    startup_runs = []
    for repetition in range(1, startup_repetitions + 1):
        order = ["nvrtc", "cubin"] if repetition % 2 else ["cubin", "nvrtc"]
        for mode in order:
            environment = {"BRMINER_EXPERIMENT_CUDA_MODULE": mode}
            if mode == "cubin":
                environment["BRMINER_CUDA_CUBIN"] = str(cubin)
            label = f"r{repetition}-{mode}"
            run = run_benchmark(
                campaign, "E08-cubin/startup", label, candidate_binary, nvrtc,
                configuration["threads"], configuration["cuda_batch_size"],
                configuration["cuda_block_size"], 0, 1, label, environment, force)
            benchmark = benchmark_result(run)
            devices = benchmark.get("cuda_devices", [])
            startup_runs.append({
                "label": label,
                "mode": mode,
                "valid": run["validity"]["valid"],
                "module_mode": devices[0].get("module_mode") if devices else None,
                "initialization_nanos": (
                    int(devices[0].get("initialization_nanos", 0))
                    if devices else None),
            })
    startup = {}
    for mode in ("nvrtc", "cubin"):
        values = [item["initialization_nanos"] for item in startup_runs
                  if item["mode"] == mode and item["initialization_nanos"] is not None]
        startup[mode] = {
            "samples_nanos": values,
            "median_nanos": statistics.median(values) if values else None,
            "mean_nanos": statistics.fmean(values) if values else None,
        }
    champion = {
        "variant": "nvrtc-cubin",
        "configuration": configuration,
        "environment": {"BRMINER_EXPERIMENT_CUDA_MODULE": "nvrtc"},
    }
    candidate = {
        "variant": "precompiled-cubin",
        "configuration": configuration,
        "environment": {
            "BRMINER_EXPERIMENT_CUDA_MODULE": "cubin",
            "BRMINER_CUDA_CUBIN": str(cubin),
        },
    }
    screen = paired_custom(
        candidate, champion, campaign, "E08-cubin/screen",
        candidate_binary, candidate_binary, nvrtc, screen_warmup,
        screen_duration, force)
    validation = None
    if validate and screen["passed"]:
        immutable = {
            "variant": "immutable-nvrtc-champion",
            "configuration": configuration,
            "environment": {},
        }
        validation = paired_custom(
            candidate, immutable, campaign, "E08-cubin/validation",
            candidate_binary, reference, nvrtc, validation_warmup,
            validation_duration, force)
    module_directory = campaign / "E08-cubin" / "modules"
    nvcc_sass = module_directory / "nvcc.sass"
    nvrtc_sass = module_directory / "nvrtc.sass"
    summary = {
        "schema": "brminer.efficiency-e08.v1",
        "created_utc": utc_now(),
        "candidate_binary": str(candidate_binary),
        "candidate_binary_sha256": sha256(candidate_binary),
        "precompiled_cubin": str(cubin),
        "precompiled_cubin_sha256": sha256(cubin),
        "startup_runs": startup_runs,
        "startup": startup,
        "sass": {
            "nvcc_sha256": sha256(nvcc_sass) if nvcc_sass.is_file() else None,
            "nvrtc_sha256": sha256(nvrtc_sass) if nvrtc_sass.is_file() else None,
            "identical": (nvcc_sass.is_file() and nvrtc_sass.is_file() and
                          sha256(nvcc_sass) == sha256(nvrtc_sass)),
        },
        "screen": screen,
        "validation": validation,
        "validated": bool(validation and validation["passed"]),
        "selected": candidate if validation and validation["passed"] else champion,
    }
    directory = campaign / "E08-cubin"
    atomic_json(directory / "summary.json", summary)
    nvrtc_ms = startup["nvrtc"]["median_nanos"] / 1e6
    cubin_ms = startup["cubin"]["median_nanos"] / 1e6
    text = "# E08 precompiled sm_89 module\n\n"
    text += f"Disassembled instruction streams identical: {summary['sass']['identical']}.\n\n"
    text += (f"Median CUDA initialization: NVRTC {nvrtc_ms:.3f} ms; "
             f"precompiled cubin {cubin_ms:.3f} ms; saving {nvrtc_ms - cubin_ms:.3f} ms.\n\n")
    text += (f"Combined screen mean delta: {(screen['mean_ratio'] - 1) * 100:+.3f}%; "
             f"passed: {screen['passed']}.\n")
    if validation:
        text += (f"\nValidation mean delta: "
                 f"{(validation['mean_ratio'] - 1) * 100:+.3f}%; "
                 f"passed: {validation['passed']}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "sass_identical": summary["sass"]["identical"],
        "nvrtc_median_init_ms": nvrtc_ms,
        "cubin_median_init_ms": cubin_ms,
        "startup_saving_ms": nvrtc_ms - cubin_ms,
        "screen_delta_pct": (screen["mean_ratio"] - 1) * 100,
        "screen_passed": screen["passed"],
        "validation_delta_pct": (
            (validation["mean_ratio"] - 1) * 100 if validation else None),
        "validation_passed": bool(validation and validation["passed"]),
    }, indent=2), flush=True)
    return summary


def run_e10(campaign: Path, validation_warmup: int,
            validation_duration: int, force: bool) -> dict[str, Any]:
    manifest = load_campaign(campaign)
    e01_path = campaign / "E01-configuration" / "summary.json"
    if not e01_path.is_file():
        raise RuntimeError("Run E01 before E10")
    e01 = read_json(e01_path)
    first_block = e01.get("validation")
    if not first_block or not first_block.get("passed"):
        raise RuntimeError("E01 did not provide the first passing full ABBA block")
    candidate_configuration = dict(e01["validated_configuration"])
    champion_configuration = dict(manifest["fixed_reference"])
    candidate = {
        "variant": "validated-configuration",
        "configuration": candidate_configuration,
        "environment": {},
    }
    champion = {
        "variant": "immutable-production-configuration",
        "configuration": champion_configuration,
        "environment": {},
    }
    reference = Path(manifest["reference_binary"])
    nvrtc = Path(manifest["nvrtc_library"])
    second_block = paired_custom(
        candidate, champion, campaign, "E10-integrated/validation-2",
        reference, reference, nvrtc, validation_warmup,
        validation_duration, force)
    passed = bool(first_block["passed"] and second_block["passed"])
    summary = {
        "schema": "brminer.efficiency-e10.v1",
        "created_utc": utc_now(),
        "immutable_reference": str(reference),
        "immutable_reference_sha256": sha256(reference),
        "candidate_configuration": candidate_configuration,
        "production_configuration": champion_configuration,
        "validation_blocks": [first_block, second_block],
        "passed": passed,
        "selected_configuration": (candidate_configuration if passed
                                   else champion_configuration),
    }
    directory = campaign / "E10-integrated"
    atomic_json(directory / "summary.json", summary)
    rows = []
    for index, block in enumerate(summary["validation_blocks"], 1):
        rows.append([
            index,
            f"{(block['mean_ratio'] - 1) * 100:+.3f}%",
            f"{(block['paired_ratios'][0] - 1) * 100:+.3f}%",
            f"{(block['paired_ratios'][1] - 1) * 100:+.3f}%",
            block["passed"],
        ])
    text = "# E10 integrated offline validation\n\n" + markdown_table(
        ["Block", "Mean delta", "Pair 1", "Pair 2", "Passed"], rows)
    text += (f"\n\nSelected configuration: {candidate_configuration if passed else champion_configuration}.\n")
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(json.dumps({
        "block_deltas_pct": [(block["mean_ratio"] - 1) * 100
                             for block in summary["validation_blocks"]],
        "both_blocks_passed": passed,
        "selected_configuration": summary["selected_configuration"],
    }, indent=2), flush=True)
    return summary


def run_e06_ring(campaign: Path, candidate_binary: Path,
                 warmup: int, duration: int, force: bool) -> dict[str, Any]:
    """Run the planned fixed-capacity ring micro screen against the deque."""
    manifest = load_campaign(campaign)
    e10_path = campaign / "E10-integrated" / "summary.json"
    configuration = (read_json(e10_path)["selected_configuration"]
                     if e10_path.is_file() else manifest["fixed_reference"])
    nvrtc = Path(manifest["nvrtc_library"])
    binary = candidate_binary.resolve()
    policies = ("deque", "l3ring-65536", "l3ring-262144", "l3ring-1048576")
    runs = []
    for policy in policies:
        runs.append(run_benchmark(
            campaign, "E06-ring/smoke", policy, binary, nvrtc,
            configuration["threads"], configuration["cuda_batch_size"],
            configuration["cuda_block_size"], warmup, duration, policy,
            {"BRMINER_EXPERIMENT_QUEUE": policy}, force))

    rows = []
    values = []
    for policy, run in zip(policies, runs):
        benchmark = benchmark_result(run)
        samples = benchmark["samples"]
        value = {
            "queue": policy,
            "mean_janus_hps": float(benchmark["mean_janus_hps"]),
            "mean_gpu_sha_hps": statistics.fmean(
                float(sample["gpu_sha_hps"]) for sample in samples),
            "mean_verus_hps": statistics.fmean(
                float(sample["verus_hps"]) for sample in samples),
            "mean_gpu_full_wait_pct": statistics.fmean(
                float(sample["gpu_queue_wait_pct"]) for sample in samples),
            "maximum_filter_fraction": max(
                float(sample["gpu_filter_floor_fraction"]) for sample in samples),
            "correctness_passed": bool(benchmark["correctness_passed"]),
        }
        values.append(value)
    control_rate = values[0]["mean_janus_hps"]
    for value in values:
        value["relative_to_control"] = value["mean_janus_hps"] / control_rate
        rows.append([
            value["queue"], f"{value['mean_janus_hps'] / 1e6:.3f}",
            f"{(value['relative_to_control'] - 1) * 100:+.3f}%",
            f"{value['mean_gpu_sha_hps'] / 1e6:.3f}",
            f"{value['mean_verus_hps'] / 1e6:.3f}",
            f"{value['mean_gpu_full_wait_pct']:.3f}%",
            value["correctness_passed"],
        ])
    selected = max(values, key=lambda value: value["mean_janus_hps"])["queue"]
    summary = {
        "schema": "brminer.efficiency-e06-ring.v1",
        "created_utc": utc_now(),
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "configuration": configuration,
        "tier": "micro",
        "runs": values,
        "selected": selected,
        "ring_advanced": selected != "deque" and
                         max(value["relative_to_control"] for value in values[1:]) >= 1.005,
    }
    directory = campaign / "E06-ring"
    atomic_json(directory / "summary.json", summary)
    report = "# E06 bounded L3-local ring\n\n" + markdown_table(
        ["Queue", "Janus MH/s", "Delta", "GPU MH/s", "Verus MH/s",
         "GPU full-wait", "Correct"], rows)
    report += (f"\n\nSelected: {selected}. Ring advanced: "
               f"{summary['ring_advanced']}.\n")
    (directory / "summary.md").write_text(report, encoding="utf-8")
    print(json.dumps({
        "selected": selected,
        "ring_advanced": summary["ring_advanced"],
        "deltas_pct": {
            value["queue"]: (value["relative_to_control"] - 1) * 100
            for value in values},
    }, indent=2), flush=True)
    return summary


def run_final_equivalence(campaign: Path, candidate_binary: Path,
                          warmup: int, duration: int,
                          force: bool) -> dict[str, Any]:
    """Check that default-off experiment scaffolding did not slow production."""
    manifest = load_campaign(campaign)
    e10_path = campaign / "E10-integrated" / "summary.json"
    if not e10_path.is_file():
        raise RuntimeError("Run E10 before the final binary equivalence check")
    e10 = read_json(e10_path)
    if not e10.get("passed"):
        raise RuntimeError("E10 did not select a validated production configuration")

    candidate_binary = candidate_binary.resolve()
    candidate_hash = sha256(candidate_binary)
    archive_directory = campaign / "E10-integrated" / "final" / candidate_hash[:16]
    archived_binary = archive_directory / "brminer.exe"
    archive_directory.mkdir(parents=True, exist_ok=True)
    if not archived_binary.is_file():
        shutil.copy2(candidate_binary, archived_binary)
    elif sha256(archived_binary) != candidate_hash:
        raise RuntimeError(f"Final binary archive collision: {archived_binary}")

    configuration = dict(e10["selected_configuration"])
    candidate = {
        "variant": "final-production-binary",
        "configuration": configuration,
        "environment": {},
    }
    champion = {
        "variant": "immutable-reference-same-configuration",
        "configuration": configuration,
        "environment": {},
    }
    experiment = f"E10-integrated/final-equivalence/{candidate_hash[:16]}"
    comparison = paired_custom(
        candidate, champion, campaign, experiment,
        archived_binary, Path(manifest["reference_binary"]),
        Path(manifest["nvrtc_library"]), warmup, duration, force)

    # This check compares binaries with identical settings, so it is a
    # non-inferiority gate rather than the improvement gate used by experiments.
    equivalent = bool(
        comparison["all_runs_valid"] and comparison["clock_pairs_valid"] and
        comparison["mean_ratio"] >= 0.995 and
        all(ratio >= 0.99 for ratio in comparison["paired_ratios"]))
    summary = {
        "schema": "brminer.efficiency-final-equivalence.v1",
        "created_utc": utc_now(),
        "configuration": configuration,
        "candidate_binary": str(archived_binary),
        "candidate_binary_sha256": candidate_hash,
        "reference_binary": manifest["reference_binary"],
        "reference_binary_sha256": sha256(Path(manifest["reference_binary"])),
        "warmup_seconds": warmup,
        "measured_seconds": duration,
        "comparison": comparison,
        "gate": {
            "minimum_mean_ratio": 0.995,
            "minimum_each_pair_ratio": 0.99,
        },
        "equivalent": equivalent,
    }
    directory = campaign / "E10-integrated"
    atomic_json(directory / "final-equivalence-summary.json", summary)
    rows = [
        [label, f"{rate / 1e6:.3f}"]
        for label, rate in comparison["rates_hps"].items()
    ]
    report = "# Final binary same-configuration equivalence\n\n"
    report += markdown_table(["Run", "Janus MH/s"], rows)
    report += (
        f"\n\nMean candidate delta: "
        f"{(comparison['mean_ratio'] - 1) * 100:+.3f}%. "
        f"Paired deltas: "
        f"{(comparison['paired_ratios'][0] - 1) * 100:+.3f}% and "
        f"{(comparison['paired_ratios'][1] - 1) * 100:+.3f}%. "
        f"Equivalent: {equivalent}.\n")
    (directory / "final-equivalence-summary.md").write_text(
        report, encoding="utf-8")
    print(json.dumps({
        "candidate_binary_sha256": candidate_hash,
        "mean_delta_pct": (comparison["mean_ratio"] - 1) * 100,
        "paired_deltas_pct": [
            (ratio - 1) * 100 for ratio in comparison["paired_ratios"]],
        "all_runs_valid": comparison["all_runs_valid"],
        "clock_pairs_valid": comparison["clock_pairs_valid"],
        "equivalent": equivalent,
    }, indent=2), flush=True)
    return summary


def parse_config_arguments(args: argparse.Namespace) -> dict[str, int]:
    return {
        "threads": args.threads,
        "cuda_batch_size": args.batch,
        "cuda_block_size": args.block,
    }


def environment_assignments(values: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        key, separator, setting = value.partition("=")
        if not separator or not re.fullmatch(r"BRMINER_EXPERIMENT_[A-Z0-9_]+", key):
            raise ValueError(
                "Experimental settings must use BRMINER_EXPERIMENT_NAME=value")
        result[key] = setting
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=Path, default=DEFAULT_CAMPAIGN)
    subparsers = parser.add_subparsers(dest="command", required=True)

    initialize = subparsers.add_parser("init", help="archive the immutable reference")
    initialize.add_argument("--binary", type=Path, default=ROOT / "brminer.exe")
    initialize.add_argument("--nvrtc", type=Path)
    initialize.add_argument("--cuda-test", type=Path)

    run = subparsers.add_parser("run", help="run one bounded offline benchmark")
    run.add_argument("--experiment", required=True)
    run.add_argument("--label", required=True)
    run.add_argument("--variant", default="manual")
    run.add_argument("--binary", type=Path)
    run.add_argument("--threads", type=int, default=14)
    run.add_argument("--batch", type=int, default=67108864)
    run.add_argument("--block", type=int, default=256)
    run.add_argument("--warmup", type=int, default=60)
    run.add_argument("--duration", type=int, default=120)
    run.add_argument("--set", action="append", default=[], metavar="NAME=VALUE",
                     help="set one BRMINER_EXPERIMENT_* variable")
    run.add_argument("--force", action="store_true")

    e00 = subparsers.add_parser("e00", help="run four immutable-reference baselines")
    e00.add_argument("--warmup", type=int, default=180)
    e00.add_argument("--duration", type=int, default=300)
    e00.add_argument("--force", action="store_true")

    e01 = subparsers.add_parser("e01", help="map and validate fixed launch settings")
    e01.add_argument("--micro-repetitions", type=int, default=5)
    e01.add_argument("--quick-warmup", type=int, default=10)
    e01.add_argument("--quick-duration", type=int, default=20)
    e01.add_argument("--screen-warmup", type=int, default=60)
    e01.add_argument("--screen-duration", type=int, default=120)
    e01.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e01.add_argument("--validation-warmup", type=int, default=180)
    e01.add_argument("--validation-duration", type=int, default=300)
    e01.add_argument("--force", action="store_true")

    e02 = subparsers.add_parser("e02", help="screen and validate depth-2 against E01")
    e02.add_argument("--candidate-binary", type=Path, required=True)
    e02.add_argument("--screen-warmup", type=int, default=60)
    e02.add_argument("--screen-duration", type=int, default=120)
    e02.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e02.add_argument("--validation-warmup", type=int, default=180)
    e02.add_argument("--validation-duration", type=int, default=300)
    e02.add_argument("--force", action="store_true")

    e05 = subparsers.add_parser("e05", help="screen topology-aware CPU affinity policies")
    e05.add_argument("--candidate-binary", type=Path, required=True)
    e05.add_argument("--quick-warmup", type=int, default=30)
    e05.add_argument("--quick-duration", type=int, default=60)
    e05.add_argument("--screen-warmup", type=int, default=60)
    e05.add_argument("--screen-duration", type=int, default=120)
    e05.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e05.add_argument("--validation-warmup", type=int, default=180)
    e05.add_argument("--validation-duration", type=int, default=300)
    e05.add_argument("--force", action="store_true")

    e06 = subparsers.add_parser("e06", help="measure and screen the queue empty fast path")
    e06.add_argument("--candidate-binary", type=Path, required=True)
    e06.add_argument("--observation-warmup", type=int, default=30)
    e06.add_argument("--observation-duration", type=int, default=60)
    e06.add_argument("--screen-warmup", type=int, default=60)
    e06.add_argument("--screen-duration", type=int, default=120)
    e06.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e06.add_argument("--validation-warmup", type=int, default=180)
    e06.add_argument("--validation-duration", type=int, default=300)
    e06.add_argument("--force", action="store_true")

    e06_ring = subparsers.add_parser(
        "e06-ring", help="micro-screen fixed-capacity L3-local rings")
    e06_ring.add_argument("--candidate-binary", type=Path, required=True)
    e06_ring.add_argument("--warmup", type=int, default=5)
    e06_ring.add_argument("--duration", type=int, default=10)
    e06_ring.add_argument("--force", action="store_true")

    e07 = subparsers.add_parser("e07", help="screen candidate-filter controller policies")
    e07.add_argument("--candidate-binary", type=Path, required=True)
    e07.add_argument("--quick-warmup", type=int, default=20)
    e07.add_argument("--quick-duration", type=int, default=40)
    e07.add_argument("--screen-warmup", type=int, default=60)
    e07.add_argument("--screen-duration", type=int, default=120)
    e07.add_argument("--screen-count", type=int, default=2)
    e07.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e07.add_argument("--validation-warmup", type=int, default=180)
    e07.add_argument("--validation-duration", type=int, default=300)
    e07.add_argument("--force", action="store_true")

    e08 = subparsers.add_parser("e08", help="compare NVRTC and precompiled sm_89 cubins")
    e08.add_argument("--candidate-binary", type=Path, required=True)
    e08.add_argument("--cubin", type=Path, required=True)
    e08.add_argument("--startup-repetitions", type=int, default=5)
    e08.add_argument("--screen-warmup", type=int, default=60)
    e08.add_argument("--screen-duration", type=int, default=120)
    e08.add_argument("--validate", action=argparse.BooleanOptionalAction, default=True)
    e08.add_argument("--validation-warmup", type=int, default=180)
    e08.add_argument("--validation-duration", type=int, default=300)
    e08.add_argument("--force", action="store_true")

    e10 = subparsers.add_parser("e10", help="run the second full integrated ABBA block")
    e10.add_argument("--validation-warmup", type=int, default=180)
    e10.add_argument("--validation-duration", type=int, default=300)
    e10.add_argument("--force", action="store_true")

    final_check = subparsers.add_parser(
        "final-check", help="compare the final binary with the immutable reference")
    final_check.add_argument("--candidate-binary", type=Path,
                             default=ROOT / "brminer.exe")
    final_check.add_argument("--warmup", type=int, default=60)
    final_check.add_argument("--duration", type=int, default=120)
    final_check.add_argument("--force", action="store_true")

    args = parser.parse_args()
    campaign = args.campaign.resolve()
    if args.command == "init":
        nvrtc = (args.nvrtc or discover_nvrtc()).resolve()
        cuda_test = (args.cuda_test or discover_cuda_test()).resolve()
        manifest = initialize_campaign(campaign, args.binary, nvrtc, cuda_test)
        print(json.dumps(manifest, indent=2), flush=True)
        return 0
    manifest = load_campaign(campaign)
    if args.command == "run":
        binary = (args.binary or Path(manifest["reference_binary"])).resolve()
        result = run_benchmark(
            campaign, args.experiment, args.label, binary,
            Path(manifest["nvrtc_library"]), args.threads, args.batch, args.block,
            args.warmup, args.duration, args.variant,
            extra_environment=environment_assignments(args.set), force=args.force)
        return 0 if result["validity"]["valid"] else 2
    if args.command == "e00":
        result = run_e00(campaign, args.warmup, args.duration, args.force)
        return 0 if result["repeatable"] else 2
    if args.command == "e01":
        run_e01(campaign, args.micro_repetitions, args.quick_warmup,
                args.quick_duration, args.screen_warmup, args.screen_duration,
                args.validate, args.validation_warmup, args.validation_duration,
                args.force)
        return 0
    if args.command == "e02":
        run_e02(campaign, args.candidate_binary, args.screen_warmup,
                args.screen_duration, args.validate, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "e05":
        run_e05(campaign, args.candidate_binary, args.quick_warmup,
                args.quick_duration, args.screen_warmup, args.screen_duration,
                args.validate, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "e06":
        run_e06(campaign, args.candidate_binary, args.observation_warmup,
                args.observation_duration, args.screen_warmup,
                args.screen_duration, args.validate, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "e06-ring":
        run_e06_ring(campaign, args.candidate_binary, args.warmup,
                     args.duration, args.force)
        return 0
    if args.command == "e07":
        run_e07(campaign, args.candidate_binary, args.quick_warmup,
                args.quick_duration, args.screen_warmup, args.screen_duration,
                args.screen_count, args.validate, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "e08":
        run_e08(campaign, args.candidate_binary, args.cubin,
                args.startup_repetitions, args.screen_warmup,
                args.screen_duration, args.validate, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "e10":
        run_e10(campaign, args.validation_warmup,
                args.validation_duration, args.force)
        return 0
    if args.command == "final-check":
        result = run_final_equivalence(
            campaign, args.candidate_binary, args.warmup,
            args.duration, args.force)
        return 0 if result["equivalent"] else 2
        return 0
    raise AssertionError(args.command)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
