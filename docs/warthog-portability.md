# Linux x86_64 build and coverage

Linux shares the Warthog CPU hash, CUDA kernel, candidate queue, adaptive
filter, benchmark, calibration, monitoring and console renderer with Windows.
Platform code supplies POSIX sockets, dynamic library loading, CPU identity,
SIGINT/SIGTERM shutdown, terminal detection and size queries.

The paired AVX2/AES/PCLMUL hash compiles separately from baseline feature checks.
Older CPUs use the portable hash. Instructions are selected by capability,
without individual CPU or GPU model special cases. QEMU tests with Intel
Nehalem and AMD Opteron feature sets verify fallback correctness without AVX;
they do not establish physical Intel performance.

## Run a package

The Linux package is built on Ubuntu 24.04 x86_64 with Clang 18. It requires
glibc 2.39 or newer, a compatible C++ runtime and OpenSSL 3 (`libcrypto.so.3`).
Other distributions may require a native build. NVIDIA mining also requires a
working vendor driver and a separately installed NVRTC runtime. CUDA libraries
are not included. A normal Toolkit installation supplies the runtime; see
[CUDA setup](cuda-setup.md).

```sh
tar -xzf BRminer-1.1.0-linux-x64.tar.gz
cd BRminer-1.1.0-linux-x64
./brminer -a warthog -p YOUR-NODE:3456 -w YOUR-WALLET --cpu_threads 14
./benchmark-warthog.sh
./calibrate-warthog.sh --full
```

`mining-warthog.sh` has a public-pool example with a wallet placeholder. Ctrl+C
or SIGTERM stops mining. A terminal pins the table above scrolling events;
redirected output contains plain snapshots. See [calibration controls](benchmark-calibration.md).

## Native build

On Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install build-essential clang cmake ninja-build git ca-certificates \
  curl python3 libboost-dev libssl-dev pkg-config
cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/build/linux --parallel
ctest --test-dir out/build/linux --output-on-failure
```

CMake fetches pinned upstream sources. Set `BRMINER_NVRTC_DLL` to the full
`libnvrtc.so.13` path for CTest to require CUDA execution; keep matching builtins
alongside it. Without that variable the CUDA test explicitly skips. OpenCL
device execution needs an installed OpenCL runtime.

Build the reproducible container environment with
`docker build -t brminer-linux-builder:goal -f scripts/linux/Dockerfile .`.
The helper `build-in-container.sh` uses `/workspace` as a read-only checkout
mount and `/work` as its writable native build/dependency volume. It reuses
pinned source caches when available and fetches them on a fresh checkout.
See [complete build and Docker commands](build.md).

`scripts/linux/fetch-cuda.py 13.4.2 /work` and the same command with `12.9.1`
download NVRTC through NVIDIA's official manifests and verify SHA-256 before
extraction. These optional downloads are for the user's own installation. Run tests, then
use `scripts/linux/package-release.py --help` for packaging paths. The packager
runs in the Ubuntu build environment and excludes CUDA libraries.

## Verified scope

The native Linux binary executes real CUDA work under Docker Desktop/WSL2 on
the host's Ryzen 7 5800X and RTX 4060 using NVRTC 13.4.92. All 19 applicable
checks passed: known-answer/equivalence and fallback hashing, CUDA-versus-CPU
output, calibration cache policy, optional NVML, offline benchmark, terminal
resize/scrolling, and synthetic Stratum acceptance/rejection/reconnection with
graceful shutdown. OpenCL device execution skipped because the container lacks
an OpenCL runtime.

Real solo-node sessions completed over an hour without a mining error or rejected
submission, with controlled reconnection and clean shutdown verified. The final
extracted Linux package then completed five minutes on the user-authorized
HeroMiners pool: 14 accepted submissions, zero rejects and clean shutdown.
Two controlled five-minute default benchmark runs measured 472.80 and 470.25 MH/s
Janus after warmup. No solo block was found in these Linux node sessions.
See [the acceptance report](development-goal.md) for current-build evidence.

WSL establishes Linux functionality on the available hardware, not bare-metal
Linux speed or coverage of every AMD/Intel CPU, NVIDIA generation, or multi-GPU
combination. Those physical configurations remain untested. The second Windows
PC has no remote command access; a manual benchmark launcher is included.

NVIDIA references:

- [NVRTC platforms and compilation output](https://docs.nvidia.com/cuda/nvrtc/)
- [Official CUDA redistributables](https://developer.download.nvidia.com/compute/cuda/redist/)
