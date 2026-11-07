# BRminer

BRminer is a Windows and Linux x86_64 miner for **Warthog (WART)** and
ConnectCoin. Warthog combines CPU VerusHash with GPU SHA256t work. CPU hashing
uses runtime feature checks for AMD and Intel processors; NVIDIA CUDA and
OpenCL are optional GPU backends. BRminer takes no developer fee.

- [Build from source](docs/build.md): Windows, Linux and Docker/WSL2.
- [Optional NVIDIA CUDA setup](docs/cuda-setup.md): install the Toolkit yourself.
- [Benchmark and calibration](docs/benchmark-calibration.md).
- [Verified results and hardware coverage](docs/development-goal.md).

## Get started with Warthog

Build from source or extract a release package. **CUDA libraries are not
included.** For NVIDIA CUDA mining, install a compatible driver and the CUDA
Toolkit using its default/Express installation. BRminer detects standard
Toolkit installations automatically. See [CUDA setup](docs/cuda-setup.md) for
supported tested versions, a ten-second check and nonstandard paths.
Python and Visual Studio are not needed to run a release binary.

On Windows, edit the pool and wallet in `mining-warthog.bat`, then run it.
For example, this leaves the CPU and CUDA launch settings hardware-adaptive:

```bat
@echo off
cd /d "%~dp0"
brminer.exe -a warthog -p us2.warthog.herominers.com:1143 -w YOUR-WARTHOG-WALLET
pause
```

On Linux, edit `mining-warthog.sh`, then run `./mining-warthog.sh`, or:

```sh
./brminer -a warthog -p us2.warthog.herominers.com:1143 -w YOUR-WARTHOG-WALLET
```

For solo mining, replace the endpoint with your Warthog Stratum node,
for example `YOUR-NODE:3456`. Use a wallet you control. Endpoint formats accept
`hostname:port` or `stratum+tcp://hostname:port`. Check the pool's own setup
instructions and terms for your region. Press Ctrl+C to stop; Linux SIGTERM
also shuts down cleanly.

On the first CUDA launch without a matching profile, BRminer measures CPU
worker count, CUDA batch/block sizes, and combined balance, then caches the
hardware-specific quick plan. Later launches reuse it. `--backend cuda`,
`--backend opencl` and `--backend cpu` explicitly select a backend. Automatic
mode tries CUDA, OpenCL, then CPU. Confirm the backend printed at startup.
A missing CUDA installation can therefore still allow mining with a fallback.

## Benchmark and tune

No wallet or server is needed for an offline benchmark:

```bat
brminer.exe --benchmark --backend cuda --duration 300 --warmup 30 --json benchmark.json
brminer.exe --calibrate
brminer.exe --calibrate --full
brminer.exe --calibration-status
```

Use `./brminer` on Linux. The benchmark launcher runs five measured minutes
after warmup. Use `--backend cpu` when CUDA is not installed.

Quick calibration screens CPU worker count, the full supported CUDA batch
ladder, block sizes, and combined Janus performance. A combined candidate must
show at least a 1% gain and repeat that gain in a reverse-order confirmation.
It takes roughly two to three minutes on the tested one-GPU host; duration
varies with hardware. The cached quick plan is provisional; a matching fully
validated profile takes precedence. Use `--calibrate --full` for repeated
five-minute validation, `--retune` after a system change, or `--no-autotune` to
skip a missing-profile measurement. Manual overrides remain fixed.

If calibration is disabled or unavailable, the portable CUDA fallbacks are
all logical CPU threads, 4,194,304 nonces per batch, and 256 threads per block.
They are safety fallbacks, not a claim that one configuration is optimal on
every device. Override them with `--cpu_threads`, `--cuda-batch-size`, and
`--cuda-block-size`; `BRMINER_AUTOTUNE=0` is the environment equivalent of
`--no-autotune`.
Environment equivalents include `BRMINER_THREADS`,
`BRMINER_CUDA_BATCH_SIZE`, `BRMINER_CUDA_BLOCK_SIZE`, and
`BRMINER_NVRTC_LIBRARY` (`BRMINER_NVRTC_DLL` also works).
CLI settings take precedence. See [all calibration controls](docs/benchmark-calibration.md)
for cache identity, retuning, promotion thresholds and repeatable comparisons.

## Mining display

An interactive console keeps one table pinned above timestamped events for
jobs, connection changes, submissions and acceptance/rejection. The table
shows interval and average **combined Janus** rates, supporting GPU SHA256t and
CPU Verus rates, share and block counters, difficulty and estimated share
interval. The estimate is statistical, not a countdown. Resizing preserves
recent events; redirected output uses plain text snapshots.

Optional NVIDIA monitoring shows temperature, utilization, power, clocks and
clock-limit reasons where supported. Unavailable values show `N/A` and JSON
`null`. Missing NVML does not prevent mining; `BRMINER_DISABLE_NVML=1`
disables monitoring. `BRMINER_BENCHMARK_TELEMETRY=1` adds machine-readable
`BRMINER_METRICS` lines during mining.

## Build, test and package

Follow [the complete build guide](docs/build.md). CMake fetches pinned
ConnectCoin Core and JanusMiner sources and writes the executable into the
checkout root. The CUDA Toolkit is optional at build time: NVRTC compiles the
embedded GPU source at runtime when CUDA is used.

Windows packaging builds and tests Release, checks the embedded-Core marker
and RandomX known answer, then writes a ZIP and SHA-256 sidecar:

```powershell
.\scripts\package-release.ps1 -BuildDirectory out/build/windows -Generator Ninja
```

The Linux packaging command is in [the build guide](docs/build.md).
Both packages contain launchers, instructions and required source dependency
notices. Neither packages nor Git include NVIDIA CUDA binaries, personal wallets,
solo-node settings, calibration profiles, logs or build products.
The CUDA kernel source and its attribution remain part of BRminer.

## ConnectCoin pool mode

Edit `mining-connectcoin.bat` with your ConnectCoin type-1 P2PK payout address.
It connects to `stratum+tcp://pool.meshpool.net:3357` using plain TCP and the
pool's observed ten-field notify format. Pool-side terms and fees apply.
Core hashing and consensus code are embedded; no separate
`connectcoind.exe` or `connectcoin-cli.exe` is needed for pool mining.
BRminer does not contain a wallet.

## Validation and source map

The tested host is a Ryzen 7 5800X / RTX 4060. Windows passed 16 tests and native
Ubuntu 24.04 under Docker/WSL2 passed 19, including CUDA correctness and older
AMD/Intel feature emulation. Final five-minute HeroMiners sessions accepted
10 Windows and 14 Linux shares with zero rejects. Controlled benchmarks,
profile invalidation, node reconnection and clean shutdown were also verified.

These results do not establish performance on physical Intel systems,
bare-metal Linux, other NVIDIA architectures or multi-GPU systems.
Accepted shares do not establish a confirmed block reward.
See [the acceptance report](docs/development-goal.md),
[Linux runtime requirements](docs/warthog-portability.md),
[the architecture map](docs/knowledge-graph.md), and
[historical performance work](docs/warthog-optimization-progress.md).
