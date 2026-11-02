# Draft GitHub issue for Brage321/BRminer

Status: **draft only; not posted**. Target: [Brage321/BRminer issues](https://github.com/Brage321/BRminer/issues). Source checked: upstream `main` at [`42688586ea70895656439e44dd5fa8da29819300`](https://github.com/Brage321/BRminer/commit/42688586ea70895656439e44dd5fa8da29819300) on 2026-11-01.

## Proposed title

Warthog: investigate ~60× lower Janus rate than BzMiner on RTX 4060 + Ryzen 7 5800X

## Proposed body

### Summary

The published [V.3 release](https://github.com/Brage321/BRminer/releases/tag/V.3) mines Warthog on the same RTX 4060 and Ryzen 7 5800X at roughly **349 MH/s SHA256t and 7.9 MH/s Janus score**, while BzMiner v100.60 reported **1,017 MH/s GPU SHA256t and 478 MH/s Janus score**. Both were configured for 14 CPU threads, ran alone against the same local solo Stratum node, and used the same payout address. The release run was stopped at the user's request after about 10m40s; BzMiner completed 15 minutes. Each mean excludes the first two minutes, so this is a strong performance signal but not the planned alternating four-run benchmark.

| Post-warmup mean | BzMiner v100.60 | BRminer V.3 release |
| --- | ---: | ---: |
| GPU SHA256t / BRminer total SHA256t | 1,016.5 MH/s | 349.5 MH/s |
| CPU VerusHash | 24.84 MH/s | ~0.202 MH/s |
| Combined Janus score | 478.1 MH/s | ~7.93 MH/s |
| RTX 4060 utilization | 95.9% | 51.5% |

The V.3 executable only displays total SHA256t and rounded average share time. I estimated Janus as `pool_difficulty / displayed_share_interval`, then inverted [the repository's Janus formula](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_stratum.cpp#L845-L853) to estimate VerusHash. Against an instrumented source build, this method had under 0.85% maximum sample error for both rates. BRminer's direct CPU SHA contribution was only ~0.058 MH/s, so its displayed total SHA is a close approximation of GPU SHA. The release ZIP matched GitHub's published SHA-256 digest, and the tested executable matched its entry in that ZIP. Neither miner showed rejected shares. Zero BRminer shares over this short window at difficulty ~1.58 trillion does not establish a validity problem.

### Source issues at the latest `main` commit

1. **The active VerusHash path uses portable primitives.** [BRminer's per-candidate call](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_pow.cpp#L70-L85) constructs a new `VerusHasher`. [CMake pins JanusMiner](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/CMakeLists.txt#L14-L18) and builds its portable CLHash source. At that pinned revision, the [optimized header is commented out](https://github.com/CoinFuMasterShifu/janusminer/blob/8d13b2bad00bde6ae10195f30ba3bec8824fd1cb/src/shared/src/crypto/verushash/verushash.cpp#L17-L19), and [`write()` and `finalize()` call portable Haraka/CLHash functions](https://github.com/CoinFuMasterShifu/janusminer/blob/8d13b2bad00bde6ae10195f30ba3bec8824fd1cb/src/shared/src/crypto/verushash/verushash.cpp#L173-L201). Each candidate also creates a fresh key with 276 Haraka256 expansion rounds. This is the first CPU stage to profile and optimize. The local MSVC patch returning false from `can_optimize()` is not by itself the cause: the active functions already use the portable path.
2. **GPU work is serialized into small scans.** [`OpenClSha256tMiner::scan`](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_gpu.cpp#L217-L267) caps a scan at 65,536 nonces, performs blocking header/counter uploads, waits for `clFinish`, then performs blocking reads and per-batch vector allocations. The next scan cannot overlap these operations. This is a plausible cause of the ~3× GPU SHA gap and ~51% utilization; profile kernel time and host/transfer/wait time separately before choosing a fix.
3. **CPU thread allocation is fixed despite a GPU verification bottleneck.** [The worker split](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_stratum.cpp#L790-L801) assigns 4 of 14 threads to direct CPU nonce search and 10 to prioritize GPU candidates. Those 4 cannot drain the GPU candidate queue. Direct CPU SHA contributed only ~0.058 MH/s beside ~349 MH/s total SHA, so this split is unlikely to be optimal for this hardware. Test a mode with all CPU threads available for GPU verification while keeping the same total CPU budget.
4. **Queue handling may add avoidable per-candidate contention.** [`takeGpuCandidate`](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_stratum.cpp#L196-L206) locks a shared mutex, pops one candidate, and calls `notify_all()` for every pop. At roughly 0.2 million verified candidates per second, that is a high-frequency synchronization path. Its actual cost needs profiling. [Queue pressure also pauses GPU work and lowers the candidate threshold](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_stratum.cpp#L575-L628), but the release does not expose queue depth, threshold, or wait time, so its contribution to GPU idle time is not yet measured.
5. **The console hides the effective Janus rate.** [The `HASHRATE` column displays combined SHA256t](https://github.com/Brage321/BRminer/blob/42688586ea70895656439e44dd5fa8da29819300/src/warthog_stratum.cpp#L845-L901), while the share interval uses the unprinted Janus score. It also omits separate GPU SHA and CPU Verus rates. A displayed ~349 MH/s corresponded to only ~7.9 MH/s Janus in this test, making the reported speed easy to misread and bottlenecks hard to diagnose.

### Suggested next steps

- Add optional 10-second counters for CPU SHA, GPU SHA, Verus, Janus, candidate queue depth, filter threshold, GPU scan time, and queue wait time.
- Profile a fixed-header Verus candidate loop in a Release build; evaluate an AVX2/VAES implementation with a portable fallback and byte-for-byte Warthog hash-vector tests.
- Benchmark larger GPU batches and overlapping dispatch/transfers after separating kernel time from host wait time. Keep GPU/CPU hash agreement tests.
- Compare CPU worker allocations and queue synchronization under a fixed 14-thread budget.
- Repeat alternating 15-minute runs on one machine and pool, reporting each of the four runs separately. Prioritize **Janus score** alongside SHA and Verus component rates.

I can provide the local console logs, 10-second GPU samples, and summarizer if useful. This issue tracks the performance gap and investigation; it does not claim a consensus or share-validity bug.
