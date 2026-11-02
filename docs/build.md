# Build BRminer from source

The source repository is [Brage321/BRminer](https://github.com/Brage321/BRminer).
Build Windows binaries on Windows and Linux binaries on Linux/WSL2; the
source is shared, but this guide does not claim cross-compilation support.

CMake fetches pinned ConnectCoin Core and JanusMiner sources. The first build
requires internet access and can take several minutes. CUDA headers, nvcc and
NVRTC are not required to compile BRminer: the CUDA driver and runtime compiler
are loaded dynamically when a user selects CUDA at runtime.

## Windows x64

### 1. Install the required tools

- Install [Git for Windows](https://git-scm.com/install/windows) and
  [Python 3 for Windows](https://www.python.org/downloads/windows/). Make sure
  `git` and `python` are available in a new PowerShell window.
- Install [Visual Studio Build Tools 2022 or newer](https://visualstudio.microsoft.com/downloads/)
  (or the full Visual Studio) using the **Desktop development with C++** workload.
  In the installer's **Installation details**, include **MSVC x64/x86 build tools**,
  a **Windows SDK**, and **C++ CMake tools for Windows**. The CMake component
  provides CMake and Ninja, so separate installs are unnecessary when it is selected.
  BRminer requires CMake 3.25 or newer.

LLVM is optional for the first build. Without it, the build uses its other
available hashing paths. To enable the optimized native CPU batch implementation,
install the Windows x64 installer from the [LLVM releases](https://github.com/llvm/llvm-project/releases)
and, before configuring CMake, run the following (adjust the path if needed):

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
clang-cl --version
```

CPU instructions are selected at runtime rather than targeting the build
computer's CPU.

### 2. Set up PowerShell

Open **Developer PowerShell for VS** configured for x64. If you are already in a
regular PowerShell window, you can load that environment there. For a default
Visual Studio 2022 Build Tools installation, run:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

Adjust the path if Visual Studio is installed elsewhere. Skip this command if
you opened Developer PowerShell directly. Add Visual Studio's bundled CMake and
Ninja to this session's PATH, then check the tools:

```powershell
$vsCmake = Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$vsCmake\CMake\bin;$vsCmake\Ninja;$env:PATH"
git --version
python --version
cmake --version
ninja --version
where.exe cl
```

Each command should find a tool; `cmake --version` must report 3.25 or newer.
If `VSINSTALLDIR` is empty, the Visual Studio developer environment was not
loaded. If CMake or Ninja is missing, check **C++ CMake tools for Windows** in
the Visual Studio Installer and rerun this setup in a new PowerShell window.

### 3. Set up vcpkg and build

If `C:\vcpkg` does not exist yet, install vcpkg first:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
& C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

The commands below assume vcpkg is at `C:\vcpkg`; change `VCPKG_ROOT` if you
installed it elsewhere. If you already cloned BRminer, `cd` to that checkout
instead of cloning it again:

```powershell
git clone https://github.com/Brage321/BRminer.git
cd BRminer
$env:VCPKG_ROOT = 'C:\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install boost-multi-index:x64-windows-static boost-tuple:x64-windows-static
cmake -S . -B out/build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
cmake --build out/build/windows --parallel 4
ctest --test-dir out/build/windows --output-on-failure
.\brminer.exe --pow-self-test
```

The binary is written to the checkout root as `brminer.exe`. The MSVC runtime
and Core dependencies are linked statically. If Ninja cannot find `cl.exe`,
repeat step 2. Visual Studio generators also work: use
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
