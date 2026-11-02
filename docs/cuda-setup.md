# Optional NVIDIA CUDA setup

BRminer source and release packages do not include NVIDIA CUDA/NVRTC libraries.
Install these separately if you want the CUDA backend. CPU mining does not need
CUDA; OpenCL mining needs a working vendor OpenCL runtime.

## Windows: normal Toolkit installation

1. Install a current NVIDIA graphics driver compatible with your GPU and chosen Toolkit.
2. Download the Windows x86_64 CUDA Toolkit from
   [NVIDIA](https://developer.nvidia.com/cuda-downloads).
3. Use the default/Express installation, keeping the NVRTC runtime component.
4. Close any old terminal, open the miner folder and run the command below.

```bat
brminer.exe --benchmark --backend cuda --duration 10 --warmup 3 --json cuda-check.json
```

A successful run reports the CUDA backend, GPU name and a passed correctness
check. No wallet or node is needed. Check `compiler_library` in the JSON to see
the actual installed library used.

BRminer searches `CUDA_PATH` and the standard
`C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v*` installations,
including both `bin\x64` and `bin`. You normally do not need to copy DLLs,
change PATH, install Python, or install Visual Studio to run the miner.
Build tools are only needed when compiling BRminer from source.

CUDA Toolkit 13.4/NVRTC 13.4.92 and CUDA 12.9/NVRTC 12.9.86 were tested.
Choose a version that supports your GPU and driver; a newer Toolkit can drop
older GPU architectures. The NVIDIA driver alone does not normally supply
NVRTC. The Toolkit's NVRTC runtime includes a matching compiler and builtins
pair; keep both from the same installation.

For a nonstandard installation, set an explicit compiler path before launching:

```bat
set "BRMINER_NVRTC_DLL=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\bin\x64\nvrtc64_130_0.dll"
mining-warthog.bat
```

For this 13.4 example the matching file is `nvrtc-builtins64_134.dll` in the
same folder. The tested 12.9 pair is `nvrtc64_120_0.dll` and
`nvrtc-builtins64_129.dll`. Use the actual path on your computer. Remove an old
`BRMINER_NVRTC_DLL` or `BRMINER_NVRTC_LIBRARY` override if it points to a missing
file; an explicit override takes precedence over automatic discovery.

## Linux

Follow NVIDIA's [Linux installation guide](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html)
for your distribution, installing a compatible driver and the Toolkit/NVRTC
runtime. BRminer searches `CUDA_HOME/lib64`, `/usr/local/cuda/lib64` and
`/usr/lib/x86_64-linux-gnu`. For another location:

```sh
export BRMINER_NVRTC_LIBRARY=/usr/local/cuda-13.4/lib64/libnvrtc.so.13
./brminer --benchmark --backend cuda --duration 10 --warmup 3 --json cuda-check.json
```

Keep `libnvrtc-builtins.so.13.4` beside that compiler, including its installed
symlinks. Use the actual installation path. On WSL2, use the Windows host's
NVIDIA driver and NVIDIA's WSL-specific Toolkit instructions; do not replace
the host driver with a Linux display driver inside WSL.

For an optional NVRTC-only Linux download, the source checkout includes a
helper that verifies NVIDIA's official manifest and archive SHA-256:

```sh
python3 scripts/linux/fetch-cuda.py 13.4.2 "$HOME/.local/share/brminer-cuda"
export BRMINER_NVRTC_LIBRARY="$HOME/.local/share/brminer-cuda/cuda_nvrtc-linux-x86_64-13.4.92-archive/lib/libnvrtc.so.13"
```

This downloads CUDA libraries from NVIDIA to your own machine; it does not add
them to BRminer packages. The helper requires Python 3.12+ and curl.

## Verify or troubleshoot

- Force `--backend cuda` during the check so a fallback does not hide a missing installation.
- A missing compiler/builtins error usually means an incomplete install, moved files or an old override.
- An unsupported architecture or driver error needs a compatible Toolkit/driver combination.
- Normal automatic mining tries CUDA, then OpenCL, then CPU. Confirm the backend printed at startup.
- Optional NVML monitoring comes from the NVIDIA driver; unavailable readings show `N/A`.

See NVIDIA's [Windows installation guide](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html)
and [NVRTC documentation](https://docs.nvidia.com/cuda/nvrtc/) for component and
platform details.
