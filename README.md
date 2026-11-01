# BRminer

BRminer is a standalone Windows miner. ConnectCoin pool mode uses the pinned ConnectCoin Core hashing and consensus code and connects directly to MeshPool over Stratum; it does not launch or require `connectcoind.exe` or `connectcoin-cli.exe`. Warthog mode can use CPU workers and detected OpenCL GPUs.

## Run

Build `brminer.exe`, place it next to `mining-connectcoin.bat`, replace the `PAYOUT_ADDRESS` placeholder in the batch file with your own address, then run it. The launcher connects to `stratum+tcp://pool.meshpool.net:3357` using plain TCP and authorizes the payout address as the worker identity. The payout must be a valid **ConnectCoin type-1 P2PK address**; make sure you control its private key because BRminer does not contain a wallet. No personal payout address is included in the release launcher. The miner does not start a local node, synchronize the chain, or use an explorer as a source of work.

BRminer does not take a developer fee. MeshPool's own solo-pool terms and any pool-side fees still apply. Press Ctrl+C to stop.

Warthog console status is printed every ten seconds in aligned columns for CPU name, GPU name(s), combined SHA256t hashrate (H/s, kH/s, MH/s, GH/s, or TH/s), pool-assigned difficulty, estimated average share interval, accepted/rejected pool shares, and blocks found. The interval is an estimate, not a deadline. If no GPU is detected, the GPU field shows `CPU only`. The selected pool controls the accepted share target and any vardiff. Connection, GPU, and share errors are reported separately when they occur.

ConnectCoin pool mode currently uses MeshPool's observed 10-field `mining.notify` format and checks hashes with the pinned Core RandomX v2 implementation. A positive Stratum response only acknowledges a submission; verify that MeshPool records the share as valid in its dashboard.

## Warthog

Warthog mining defaults to BobFarm at `mine.bobfarm.icu:3033`. In `mining-warthog.bat`, edit `NODE` (under `THREADS`) to a compatible Warthog Stratum `hostname:port`; the payout address is used as the Stratum username. The selected pool controls the accepted target and any vardiff. BRminer displays the assigned difficulty and estimated average interval; this estimate is statistical, not a deadline.

With a GPU detected, CPU threads are split between direct CPU mining and parallel verification of GPU candidates; GPU candidate filtering adapts to verifier queue pressure. When a job's 32-bit nonce range is exhausted, BRminer advances extranonce2 and rebuilds the same pool job so CPU and GPU workers keep searching without waiting for another pool notification. Without a detected GPU, all configured CPU threads mine directly.

## ConnectCoin Core and verification

CMake pins ConnectCoin Core commit `ff459a92dab40607cda4d92c6effdb85600ceea7`

Local regtest runs have demonstrated Core's template, PoW validation, and block-submission path. Mainnet operation must still be verified by allowing the embedded node to connect and synchronize against live mainnet peers; no accepted mainnet block or reward has been demonstrated. The startup chain label helps distinguish mainnet from a test network.

## Release status and packaging

`brminer.exe` is a single Windows x64 executable; it has no ConnectCoin executable/CLI or VC++ runtime DLL dependency. Warthog GPU mode additionally needs a working vendor OpenCL driver. The release launcher files contain wallet placeholders only. The packaging script builds and tests Release, checks the embedded-Core marker and RandomX self-test, and creates a ZIP plus SHA-256 checksum under `out\release`:

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
.\scripts\package-release.ps1
```

A successful build, local GPU-vs-CPU hash comparison, or authorized pool job does not prove pool acceptance. Live tests have confirmed valid ConnectCoin shares on MeshPool and an accepted Warthog share; these results do not guarantee future shares or block rewards. Do not describe the miner as having found a block unless the pool confirms it.

## Build and test

Use CMake 3.22 or newer, Visual Studio 2026's x64 C++ workload, and Boost 1.74+ headers with their CMake package available. If Boost is installed with vcpkg, set `VCPKG_ROOT` to the vcpkg installation before configuring:

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-msvc --config Release --target brminer config-tests pow-tests warthog-pow-tests warthog-gpu-tests
ctest --test-dir build-msvc -C Release --output-on-failure
```

CMake writes the single `brminer.exe` into the repository root, next to the launchers. Core's dependencies are built as static libraries so BRminer does not need a separate ConnectCoin executable or CLI at runtime.
