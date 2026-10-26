
# BRminer

BRminer is a standalone Windows CPU miner that embeds the pinned ConnectCoin Core node and miner. It synchronizes and validates the chain, creates Core block templates with the configured payout, searches nonces using ConnectCoin's RandomX v2 proof of work, and submits solutions through Core's P2P node. It does not launch or require `connectcoind.exe` or `connectcoin-cli.exe`.

## Run

Build `brminer.exe`, place it next to `mining-connectcoin.bat`, set `PAYOUT_ADDRESS` and `THREADS` in the batch file, then run it. The launcher explicitly selects ConnectCoin mainnet by default; an explicit network option such as `-regtest` overrides that default. Mainnet chain data is stored in Core's normal data directory unless you pass `-datadir=<path>`.

The payout must be a valid **ConnectCoin type-1 P2PK address** accepted by the pinned Core miner. Make sure you control its private key; BRminer does not contain a wallet. Core RPC, daemon mode, Tor/onion routing/listening, and Core console logs are disabled by default; regular P2P networking remains enabled. Press Ctrl+C to stop. Detailed Core logs are in `debug.log` in the data directory.

Every ten seconds, BRminer prints the CPU name, hashrate, accepted blocks, Core version, solo-share count, and miner state. Sync phase and header/block progress are shown only while the node is loading or syncing; those lines disappear once Core reports synchronization complete. On mainnet and public test networks, header percentage follows Core's time-based estimate using ConnectCoin's target block spacing. Block percentage is the active block height divided by the currently known header height. Neither percentage is a guaranteed remaining-time or full-chain estimate. Regtest displays header percentage as not applicable because its timestamps and block pace are artificial.

This is solo mining, not pool mining. The miner can only earn a reward if it finds and submits a valid mainnet block; CPU hash rates may make that extremely unlikely. It waits instead of mining on a known-behind chain tip.

## ConnectCoin Core and verification

CMake pins ConnectCoin Core commit `ff459a92dab40607cda4d92c6effdb85600ceea7` and links its node, consensus, P2P, block-template, and native CPU-mining implementations directly into BRminer. Core's standalone daemon, CLI, GUI, wallet, and RPC server targets are disabled. `--pow-self-test` remains a RandomX v2 known-answer test; it does not by itself demonstrate block mining.

Local regtest runs have demonstrated Core's template, PoW validation, and block-submission path. Mainnet operation must still be verified by allowing the embedded node to connect and synchronize against live mainnet peers; no accepted mainnet block or reward has been demonstrated. The startup chain label helps distinguish mainnet from a test network.

## Build and test

Use CMake 3.22 or newer, Visual Studio 2026's x64 C++ workload, and Boost 1.74+ headers with their CMake package available. If Boost is installed with vcpkg, set `VCPKG_ROOT` to the vcpkg installation before configuring:

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-msvc --config Release --target brminer config-tests pow-tests
ctest --test-dir build-msvc -C Release --output-on-failure
```

The executable is `build-msvc\Release\brminer.exe`. Core's dependencies are built as static libraries so BRminer does not need a separate ConnectCoin executable or CLI at runtime.
