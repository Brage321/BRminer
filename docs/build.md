# Build BRminer from source

The source repository is [Brage321/BRminer](https://github.com/Brage321/BRminer).
Build Windows binaries on Windows and Linux binaries on Linux/WSL2; the
source is shared, but this guide does not claim cross-compilation support.

CMake fetches pinned ConnectCoin Core and JanusMiner sources. The first build
requires internet access and can take several minutes. CUDA headers, nvcc and
NVRTC are not required to compile BRminer: the CUDA driver and runtime compiler
are loaded dynamically when a user selects CUDA at runtime.

## Windows x64

Install Git, CMake 3.25+, Ninja, Python 3, and Visual Studio 2022 or newer with
the **Desktop development with C++** workload and a Windows SDK. Install LLVM
and put `clang-cl.exe` on PATH for the optimized native CPU batch implementation;
otherwise the build uses its other available hashing paths. CPU instructions
are selected at runtime rather than targeting the build computer's CPU.

Use a PowerShell session with the Visual Studio x64 build environment loaded
(for example, Developer PowerShell for VS, configured for x64). These commands
assume vcpkg is installed at `C:\vcpkg`; change that path if needed:

```powershell
git clone https://github.com/Brage321/BRminer.git
cd BRminer
$env:VCPKG_ROOT = 'C:\vcpkg'
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
& "$env:VCPKG_ROOT\vcpkg.exe" install boost-multi-index:x64-windows-static boost-tuple:x64-windows-static
cmake -S . -B out/build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
cmake --build out/build/windows --parallel 4
ctest --test-dir out/build/windows --output-on-failure
.\brminer.exe --pow-self-test
```

If vcpkg is not installed yet:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
& C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Run that bootstrap before the main build commands. The binary is written to
the checkout root as `brminer.exe`. The MSVC runtime and Core dependencies are
linked statically. If Ninja cannot find `cl.exe`, use an x64 Visual Studio
developer environment. Visual Studio generators also work: use
`-G 'Visual Studio 17 2022' -A x64` in a separate build folder, then add
`--config Release` to the build and `-C Release` to CTest.

Create a ZIP and SHA-256 sidecar, with no CUDA libraries:

```powershell
.\scripts\package-release.ps1 -BuildDirectory out/build/windows -Generator Ninja
```

The packager builds and tests before packaging. To repackage the exact build
that already passed, add `-SkipBuildAndTests`; it still runs the embedded-Core
marker and RandomX known-answer checks. Output is under `out/release/`.

## Linux x86_64

On Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install build-essential clang cmake ninja-build git ca-certificates \
  curl python3 libboost-dev libssl-dev pkg-config
git clone https://github.com/Brage321/BRminer.git
cd BRminer
cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/build/linux --parallel 4
ctest --test-dir out/build/linux --output-on-failure
./brminer --pow-self-test
```

The binary is written to the checkout root as `brminer`. Install `qemu-user`
and `python3-pyte` to enable the optional older-CPU and terminal integration
tests. CUDA/OpenCL execution checks need the corresponding installed runtime;
otherwise those device checks explicitly skip. To require CUDA execution:

```sh
BRMINER_NVRTC_DLL=/usr/local/cuda/lib64/libnvrtc.so.13 \
  ctest --test-dir out/build/linux --output-on-failure
```

The matching compiler builtins must remain beside that library. See
[CUDA setup](cuda-setup.md) and [Linux runtime requirements](warthog-portability.md).

Package a tested native build without CUDA libraries:

```sh
python3 scripts/linux/package-release.py --source "$PWD" --binary "$PWD/brminer" \
  --core "$PWD/out/build/linux/_deps/connectcoin_source-src" \
  --janus "$PWD/out/build/linux/_deps/janusminer_source-src" \
  --mbedtls "$PWD/out/build/linux/_deps/connectcoin_mbedtls-src" \
  --output "$PWD/out/release"
```

This packaging helper targets the documented Ubuntu environment and includes
its dependency notices. Pass `--boost-notices PATH` if Boost notices cannot be
identified automatically.

## Optional Docker/WSL2 build

From the checkout root, with Docker configured for Linux containers:

```sh
docker build -t brminer-linux-builder -f scripts/linux/Dockerfile .
docker run --rm --mount "type=bind,source=$PWD,target=/workspace,readonly" \
  --mount type=volume,source=brminer-linux-build,target=/work \
  brminer-linux-builder sh /workspace/scripts/linux/build-in-container.sh
```

The script copies source into the native build volume, uses cached upstream
sources when available, fetches them otherwise, then builds and runs CTest.
Add `--gpus all` when the host supports NVIDIA GPU containers and you want
GPU tests. In PowerShell, use backticks instead of shell backslashes for line
continuations and `${PWD}` for the bind-mount source if needed.

## Check an extracted release

Python is only needed for this developer check, not normal mining:

```sh
python3 scripts/verify-release.py ./brminer --output out/release-check
python3 scripts/verify-release.py ./brminer --backend cuda \
  --nvrtc-library /usr/local/cuda/lib64/libnvrtc.so.13 --output out/release-cuda-check
```

On Windows use `python`, the path to `brminer.exe`, and your installed NVRTC DLL
path. The default check uses CPU; the CUDA check verifies real work with the
user-installed compiler and operation with monitoring disabled. An optional
`--fallback-nvrtc-library PATH` checks a second installed compiler without
modifying either installation. `--profile-dir PATH` also checks a matching
validated profile reload.
