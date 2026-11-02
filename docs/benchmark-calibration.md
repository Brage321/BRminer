# Warthog benchmarks and calibration

The same C++ engine handles Windows and Linux mining, offline benchmarks, and
calibration. Install [optional CUDA/NVRTC](cuda-setup.md) separately before
selecting the CUDA backend; the packages do not include it. The commands below use `brminer.exe` on Windows; substitute
`./brminer` on Linux. Stop other miners before timing either tool.

## Repeatable offline benchmark

```bat
brminer.exe --benchmark --backend cuda --duration 300 --warmup 30 --json benchmark.json
```

No node or wallet is required. `benchmark-warthog.bat` / `.sh` runs this command.
An optional `--threads 14` fixes the worker count. Batch and block sizes can be
fixed with `--cuda-batch-size 67108864 --cuda-block-size 256`.
`--backend cpu` exercises the portable CPU path without a GPU.

The deterministic `warthog-header-sequence-v1` workload starts with a fixed
80-byte header (byte `i` initially equals `i * 17 + 11`, modulo 256), clears
bytes 32–43 and 68–79, writes the little-endian epoch into bytes 36–43, then sets
byte 71 to 3 and byte 75 to 1. The epoch advances when the nonce range is
exhausted. Startup checks compare known CPU hashes and
GPU candidates with CPU SHA256t before timing. Sampled candidates are also
verified during GPU benchmark execution. All configured CPU verifiers and GPU
workers participate in the same candidate queue and adaptive filter as mining.

JSON schema `brminer.benchmark.v1` records the workload, miner/build identity,
CPU, GPU UUID and architecture, compiler and driver versions, configuration,
warmup and measured duration, one-second interval samples, combined Janus mean,
deviation, descriptive sample error, stage rates, queue pressure, and monitoring.
The mean and deviation use interval durations as weights; a partial final sample
does not receive the weight of a whole second. The reported sample error is
descriptive, not a confidence interval for correlated mining samples.

Compare repeated runs of the same workload after warmup. Keep background load,
driver, power settings, temperatures, and compiler consistent. An offline rate
is not proof of an accepted block, and rates from different workloads or from
WSL and native Windows are not a controlled algorithm comparison.

## Quick calibration

```bat
brminer.exe --calibrate
```

This produces a provisional plan and JSON evidence. It measures CPU Verus
capacity at several thread counts, NVIDIA SHA and pinned-transfer capacity, a
small CUDA launch-size shortlist, then combined Janus throughput. Capacity
measurements seed the adaptive candidate filter. Feedback continues while mining.
Available-host quick runs finished in 68–73 seconds. Duration varies with
hardware and GPU count; this is a short screening procedure, not a sustained
performance guarantee. See [the BzMiner observations](bzminer-calibration-observations.md)
for the independent approach inferred from its logs.

To use a quick plan immediately while mining, add `--autotune` to the normal
mining command. A matching validated profile takes precedence; `--retune`
explicitly requests a new quick plan. A standalone `--calibrate` command prints
its plan and exits. Quick reports are not automatically loaded as trusted
profiles on later ordinary launches.

## Full validation and saved profiles

```bat
brminer.exe --calibrate --full
```

The quick planner shortlists plausible improvements. Full validation compares
up to two finalists with defaults using two independent five-minute measured
runs per configuration, each after 30 seconds of warmup. Baseline/candidate order
is reversed on the second pass. Allow roughly 12–35 minutes, depending on the
shortlist and startup overhead.

A candidate must improve both paired comparisons by at least 1%, with a mean
ratio of at least 1.5%. Incorrect, cancelled, thermally limited, short, or noisy
runs cannot promote a profile. Per-run coefficient of variation must be at most
10%, and baseline repeat drift at most 5%. Without convincing improvement,
stable defaults are saved; inconclusive validation leaves defaults in use and
does not save a new profile.

Normal CUDA mining loads a valid matching profile without benchmarking. Check it:

```bat
brminer.exe --calibration-status
```

The cache key includes CPU/logical-thread identity, OS, GPU UUIDs and compute
capabilities, driver release/API, compiler/builtins file hashes, miner build,
manual constraints, and disabled-SIMD state. Changes invalidate the match.
Malformed, oversized, stale, or out-of-range profiles are ignored. Atomic file
replacement protects an existing profile from a partial write. NVML driver
identity is required for persistent validation; quick tuning and mining remain
usable without monitoring.

Default folders are `%LOCALAPPDATA%\BRminer\profiles` on Windows and
`$XDG_CACHE_HOME/brminer/profiles` or `~/.cache/brminer/profiles` on Linux.
Use `--profile-dir DIRECTORY` or `BRMINER_PROFILE_DIR` to keep profiles elsewhere.
Reports live in its `runs` subfolder.

## Manual settings

```bat
brminer.exe --calibrate --full --threads 14
```

This calibrates only the remaining dimensions. Mining with `--cpu_threads 14`
uses profiles validated with that same constraint; an unconstrained profile is
not silently substituted. CLI settings take precedence over environment
settings. The environment equivalents are `BRMINER_THREADS`,
`BRMINER_CUDA_BATCH_SIZE`, `BRMINER_CUDA_BLOCK_SIZE`, and `BRMINER_NVRTC_DLL`
(`BRMINER_NVRTC_LIBRARY` is also accepted). All three launch dimensions fixed
means there is nothing left to tune.

The supported batch range is powers of two from 1,048,576 to 67,108,864 nonces;
block sizes are 64, 128, 256, or 512. Defaults are all logical CPU threads,
67,108,864 nonces, and block size 256. CPU feature dispatch checks capabilities,
not brand/model names. Calibration changes no clocks, voltage, or power limits.

## Test another Windows PC

Extract the release ZIP, install the NVIDIA driver and optional CUDA Toolkit
as described in [CUDA setup](cuda-setup.md), then run `benchmark-warthog.bat`. Save the
resulting `benchmark.json`. For an apples-to-apples repeat, run it again with the
same options. Then use `calibrate-warthog.bat --full` and repeat the benchmark
with the selected explicit settings. Offline benchmarks deliberately measure
their requested settings and do not silently apply a cached mining profile.
The second PC is not remotely accessible from the development host, so its
performance coverage requires these manually collected results.
