# Warthog optimization progress

Goal: reach or exceed BzMiner v100.60's **478.14 MH/s mean Janus score** in two
five-minute BRminer runs, excluding the first 120 seconds of each run. Use the
same local Stratum endpoint, 14 configured CPU threads, and one NVIDIA GPU as
the recorded BzMiner run. GPU SHA256t and CPU VerusHash are diagnostic rates.

The test machine is a Ryzen 7 5800X and GeForce RTX 4060. Changes target
AMD/Intel CPU features and generic NVIDIA OpenCL/CUDA behavior; no hardware model
IDs are used to select code paths. The AVX SIMD implementation checks CPU AES,
PCLMUL, SSSE3, AVX, and operating system AVX state support at runtime and
retains the portable fallback.

| Run | GPU SHA256t (MH/s) | CPU VerusHash (MH/s) | Janus (MH/s) | Notes |
| --- | ---: | ---: | ---: | --- |
| BzMiner, prior 15 min | 1,016.53 | 24.84 | 478.14 | Target; first 120 s excluded |
| BRminer initial source run | 319.86 | 0.177 | 6.97 | Interrupted prior baseline |
| BRminer goal baseline, 5 min | 356.93 | 0.190 | 7.47 | Build overlapped; diagnostic only |
| SIMD primitives, 5 min | 342.31 | 2.13 | 63.90 | Isolated |
| Cached Verus key, 5 min | 320.44 | 4.68 | 110.54 | Isolated; filter ramp was slow |
| Faster adaptive filter and key restoration, 5 min | 266.19 | 5.42 | 113.93 | Isolated; queue contention visible |
| Batched verifier queue reads, 5 min | 182.18 | 10.94 | 144.34 | Isolated; GPU output cost visible |
| 262,144-nonce OpenCL batches and ordered writes, 5 min | 382.71 | 10.74 | 198.84 | Isolated |
| Compact GPU candidate output, 5 min | 421.52 | 11.08 | 210.60 | Isolated; full CPU share confirmation |
| All CPU workers verify GPU candidates when available, 5 min | 414.17 | 13.35 | 232.83 | Isolated; workers mine directly when queue empties |
| 1,048,576-nonce GPU batches, 5 min | 640.49 | 8.68 | 210.17 | Isolated; filter underfed verifiers |
| Queue-before-batch filter control, 5 min | 482.22 | 13.17 | 246.02 | Isolated; highest Janus rate so far |
| Batched GPU candidate handoff, 5 min | 556.82 | 14.49 | 276.23 | Isolated; one job reference per GPU result batch |
| Reused SHA256 first-block GPU midstate, 5 min | 743.65 | 14.96 | 315.10 | Isolated; GPU candidates checked against CPU |
| Cached Verus header prefix, 5 min | 742.31 | 15.44 | 321.24 | Isolated; reused hasher per CPU thread |
| Eight-byte GPU candidate records, 5 min | 777.90 | 15.78 | 331.33 | Isolated; full CPU confirmation retained |
| Direct SIMD Verus candidate path, 5 min | 785.48 | 15.64 | 330.70 | Isolated; no live gain despite one-core probe improvement |
| Two GPU dispatch workers per device, 5 min | 766.21 | 13.89 | 304.04 | Isolated; reverted after reduced verifier throughput |
| Fixed 256-work-item OpenCL groups, 5 min | 777.48 | 15.21 | 323.68 | Isolated; reverted after lower Janus rate |
| Batched verifier counters and queue wakeups, 5 min | 788.04 | 16.31 | 339.93 | Isolated; 512-candidate CPU batches |
| Direct compact SHA-word conversion, 5 min | 776.87 | 15.58 | 328.48 | Isolated; regressed with per-candidate shift loop |
| Direct GPU candidate readback, 5 min | 857.61 | 15.45 | 338.89 | Isolated; leading-zero SHA conversion; GPU faster, CPU slightly slower |
| Larger 262,144-candidate queue, 5 min | 860.36 | 15.50 | 340.00 | Isolated; negligible Janus gain, reverted |
| AVX code generation for Verus SIMD, 5 min | 850.80 | 16.40 | 351.01 | Isolated; runtime AVX and OS state dispatch |
| Release link-time optimization, 5 min | 853.11 | 15.78 | 342.78 | Isolated; one-core probe improved but live rate fell, reverted |
| CPU direct nonce chunks, 5 min | 846.62 | 16.25 | 348.24 | Isolated; shared counter reduction regressed, reverted |
| CPU affinity, physical cores first, 5 min | 849.70 | 15.96 | 344.70 | Isolated; spread layout regressed |
| Packed-core affinity plus initial VAES keyed Haraka, 5 min | 859.47 | 15.49 | 339.73 | Combined experiment; slower than AVX baseline |
| Imported ccminer CLHash core, 5 min | 850.06 | 13.57 | 310.57 | Key-collision write order fixed; slower than AVX path, reverted |
| Pointer-based Verus key restoration, 5 min | 854.81 | 16.25 | 349.57 | Corrected patch script; similar to AVX baseline |
| Clang AVX2 Verus code generation, 5 min | 849.36 | 16.54 | 352.65 | Isolated; slight gain over MSVC AVX |
| Combined OpenCL candidate readback, 5 min | 842.23 | 16.63 | 352.77 | Two transfers combined by overreading; negligible gain, reverted |
| GPU/network core reserved, 5 min | 835.26 | 16.17 | 345.38 | OS topology affinity regressed, reverted |
| 2048-candidate CPU handoff, 5 min | 838.52 | 16.43 | 349.46 | Larger batches regressed, reverted |
| Workgroup-local GPU candidate counter, 5 min | 837.79 | 16.28 | 347.30 | One global atomic per group regressed, reverted |
| 2,097,152-nonce GPU batches, 5 min | 952.40 | 15.24 | 348.68 | Faster GPU, CPU verifier underfed |
| 2,097,152-nonce batches with 262,144-candidate queue, 5 min | 900.55 | 16.60 | 361.11 | Larger queue feeds verifiers |
| Per-key CLHash mutation log, 5 min | 904.87 | 17.06 | 367.95 | Reversible in-key log improves 14-thread throughput |
| 1,048,576-candidate GPU queue, 5 min | 921.58 | 17.36 | 374.67 | Better GPU and CPU balance |
| 2% minimum GPU candidate fraction, 5 min | 923.38 | 19.21 | 399.37 | Candidate supply keeps CPU Verus workers busy |
| Reused CLHash round key loads, 5 min | 926.15 | 19.21 | 399.87 | Similar live rate; small single-thread probe gain |
| Adaptive GPU candidate floor, 5 min | 784.27 | 19.54 | 378.51 | Overshot to 2.58% average floor; GPU queue waits 19%; retuning |
| Adaptive floor with 1% fallback trigger, 5 min | 900.31 | 19.11 | 394.30 | Settled near 2% floor; queue waits 6.7%; retained for hardware adaptation |
| 4,194,304-nonce GPU batches, 5 min | 914.56 | 19.48 | 401.41 | Previous best before native batching; candidate floor adapts near 2.1% |
| Two-slot OpenCL scan pipeline, 5 min | 917.39 | 19.51 | 402.23 | GPU utilization unchanged near 85%; marginal gain, reverted |
| 16,777,216-nonce GPU batches, 5 min | 833.02 | 19.33 | 385.40 | Candidate queue wait rose to 15.9%; reverted |
| Prior miner's clean OpenCL SHA kernel, 5 min | 916.44 | 19.22 | 398.40 | Passed CPU/GPU checks; no end-to-end gain, reverted |
| Prior paired Verus, MSVC, two candidates per call, 5 min | 943.33 | 9.86 | 258.46 | Hashes matched consensus; repeated per-pair job bookkeeping dominated |
| Prior paired Verus with Clang and mutation log, two candidates per call, 5 min | 932.26 | 10.43 | 267.95 | Still bottlenecked by per-pair native calls; changed to 512 candidates per call |
| Prior paired Verus with Clang, mutation log, 512 candidates per call, 5 min | 923.57 | 21.57 | 428.96 | Best complete run; byte-for-byte Verus and Janus equivalence, seven CTests passed |
| Paired Verus plus measured GPU candidate controller, 5 min | 1,001.37 | 21.12 | 436.89 | Queue wait 0.04%; GPU scan busy 96.52%; retained as current best |
| Paired Verus, measured controller, 1,024-candidate handoff, 5 min | 997.81 | 21.41 | 440.02 | Small CPU throughput gain; retained |
| Paired Verus, measured controller, 2,048-candidate handoff, 5 min | 997.34 | 21.46 | 440.61 | Difference from 1,024 candidates is below run variation; reverted for shorter handoff latency |
| Conservative Verus prefix rejection, 5 min | 995.51 | 23.28 | 462.80 | SHA lower bound proves prefix filter safe; 30 s isolated batch probe rose 23.10 to 25.26 MH/s; retained |
| Compact native Verus prefix output, 5 min | 995.11 | 23.14 | 461.04 | Tiny isolated gain did not survive GPU overlap; reverted |
| Verus prefix rejection repeat with thread profile, 5 min | 997.23 | 23.27 | 463.03 | Fifteen active threads each used about one CPU core: 14 Verus workers plus the OpenCL host worker |
| OpenCL event callback host wait, 5 min | 964.16 | 22.29 | 445.06 | NVIDIA driver added another busy callback thread; reverted |
| OpenCL event wait, 5 min | 994.06 | 22.98 | 458.86 | GPU host thread still used a full CPU core; reverted |
| CUDA blocking context, one 16M stream, 5 min | 931.31 | 23.93 | 458.23 | Correct hash output and lower host CPU use, but GPU throughput was lower than OpenCL; changed to two streams |
| CUDA blocking context, two concurrent 8M streams, 5 min | 932.99 | 23.57 | 454.42 | No throughput gain at the live candidate rate; profiling GPU candidate collection next |
| Warp-aggregated CUDA candidate writes, isolated probe | 961.08 | — | — | Existing atomic writes reached 965.22–965.95 MH/s at a 2.5% filter; aggregation matched CPU hashes but regressed slightly, so reverted before a live run |
| CUDA port of JanusMiner's unrolled SHA kernel, one 16M stream, 5 min | 986.87 | 23.69 | 466.12 | Correct CUDA/CPU hashes and full CTest; improved CUDA GPU rate, still below parity |
| CUDA 16M plus CPU verifier fast path, 5 min | 991.87 | 24.17 | 472.73 | Skipped per-candidate header construction when Verus prefix cannot meet either target; exact Janus batch/scalar equivalence and full CTest passed |
| CUDA 4M plus CPU verifier fast path, 5 min | 920.65 | 23.85 | 455.30 | GPU-only probe favored 4M slightly, but launch/transfer overhead under concurrent CPU mining dominated; keep 16M default |
| CUDA 16M, 256-thread blocks, CPU fast path, 5 min | 997.58 | 24.13 | 473.38 | Isolated launch tuning raised GPU rate slightly; combined result remains below parity |
| CUDA nonce-only candidate output, isolated probe | 931–934 | — | — | Full GPU/CPU candidate and Janus score equivalence passed, but host expansion of 52.9 million nonce records lowered scan rate from about 1,025 MH/s; reverted before a live run |
| CUDA block-local candidate compaction, isolated probe | 1,005–1,014 | — | — | Correct candidate hashes, but three block barriers and staging offset the lower global atomic count; original kernel reached about 1,025 MH/s, so reverted |
| OpenCL plus CPU verifier fast path, 5 min | 996.78 | 23.76 | 468.78 | CPU shortcut helps the fallback path, but OpenCL's busy host thread leaves it below the CUDA result |
| OpenCL event polling, 5 min | 997.06 | 23.90 | 470.48 | Polling did not free the driver CPU thread; reverted |
| CUDA 32M, prior controller, 5 min | 133.66 | 22.69 | 166.39 | Queue backpressure made the wall-time controller raise the filter; diagnostic failure |
| CUDA 32M, queue-depth override, 5 min | 112.45 | 22.63 | 153.97 | Queue snapshot missed stalled intervals; replaced controller |
| CUDA 32M, active-scan controller, 5 min | 999.06 | 24.00 | 472.12 | Uses CPU consumption and GPU active-scan rate; queue stabilized |
| CUDA 32M, three precomputed SHA rounds, 5 min | 1,002.00 | 23.78 | 470.03 | GPU candidate hashes matched CPU; small GPU gain offset by CPU variation |
| CUDA 32M, Clang -O3 CPU, 5 min | 999.99 | 23.72 | 468.98 | Isolated CPU gain did not survive overlap; restored /O2 |
| CUDA 16M, SHA precompute and active controller, 5 min | 999.48 | 23.26 | 463.32 | Smaller batches underfed CPU verifiers in this run |
| CUDA 32M, 2M queue buffer, 5 min | 1,005.57 | 23.73 | 470.10 | Higher GPU duty, combined result still below parity |
| CUDA 32M, 4,096-candidate handoff, 5 min | 1,002.50 | 23.66 | 468.69 | Larger handoff regressed; restored 1,024 |
| CUDA two-stream 32M scan, isolated probe | 1,025.18 | — | — | Correct candidates; slower than 1,031.36 MH/s single stream, reverted |
| BzMiner current-condition reference, 5 min | 1,029.62 | 24.96 | 479.98 | Same node and 14 threads; post-120-second mean confirms prior target remains relevant |
| CUDA 32M, fourth SHA round and two schedule words, run 1 | 1,030.59 | 24.10 | 479.17 | First five-minute run above the prior BzMiner target |
| CUDA 32M, same build, run 2 | 1,025.31 | 23.85 | 475.13 | Did not confirm; retained for another iteration |
| CUDA 32M, same build, run 3 | 1,026.77 | 24.00 | 477.24 | Below target by 0.90 MH/s |
| CUDA 64M, 2M queue, 5 min | 1,033.45 | 23.69 | 474.63 | Queue wait averaged 2.05%; enlarged queue next |
| CUDA 64M, 4M queue, run 1 | 1,053.42 | 23.88 | 480.50 | First qualifying run of final configuration; queue wait 0.09% |
| CUDA 64M, 4M queue, run 2 | 1,050.82 | 23.70 | 477.81 | Below target by 0.33 MH/s |
| CUDA 64M, 4M queue, run 3 | 1,052.00 | 23.72 | 478.28 | Second qualifying run of final configuration; queue wait 0.24% |

Each five-minute run used `out/bench/run_benchmark.py` with its own label. Raw
console logs, GPU samples, and stop metadata are in ignored `out/bench/` on this
machine. The stopped runs were exactly 300 seconds within runner overhead. The
mean is the arithmetic mean of ten-second `BRMINER_METRICS` samples at elapsed
time 120 seconds or later. The goal baseline overlapped a Release build, so it
is excluded from change attribution. The older BzMiner and BRminer figures are
documented in [the performance baseline](warthog-performance-baseline.md).

The Release build and CTest suite pass, including byte-for-byte portable versus
SIMD Verus primitive checks, full Verus hash comparisons across job changes and
both protocol variants, Janus target-result equivalence, and GPU candidate
comparison with CPU SHA256t. A potential GPU share is recomputed with full CPU
SHA256t and Janus before submission. No run so far has observed an accepted or
rejected share at the high assigned difficulty; these short runs cannot prove
pool-side acceptance.

An explicit 256-bit VAES rewrite of the two- and four-state AES rounds passed
hash equivalence, but its single-thread probe fell from 1.97 to 1.78 MH/s, so
it was removed without a full mining run.

The current single-thread profile attributes approximately 94% of a full
VerusHash v2.2 call to CLHash and key restoration; restoration is about 4% of
the CLHash time. Keyed Haraka is a small fraction. The isolated 14-thread
candidate check rose from 20.14 to 21.78 MH/s with the mutation log, whereas
live mining reaches about 19.21 MH/s after raising the GPU candidate floor. A short BzMiner one-thread diagnostic
reached about 2.5–2.6 MH/s versus BRminer's 1.92 MH/s full VerusHash probe.
Current work: optimize the CLHash core and improve the GPU candidate supply to
the CPU verifiers.

A three-lane CLHash variant matched consensus but its isolated 14-worker raw
rate was 24.57 MH/s versus 25.64 MH/s for the retained paired variant. It was
removed before a live run.
The goal's two-run threshold is met by the identical CUDA 64M/4M configuration
in runs 1 and 3: **480.50 and 478.28 MH/s**, versus BzMiner's 478.14 MH/s
prior 15-minute mean. Run 2 at 477.81 MH/s shows the margin is narrow and
affected by normal interval variation. These are miner-reported Janus rates;
the short runs had no accepted pool shares and do not establish accepted-work
or energy-efficiency superiority.

The active-scan controller fixes a positive-feedback failure in the earlier
wall-time candidate controller when large GPU batches fill the queue. It
estimates GPU capacity from completed nonce counts divided by scan time, then
sets the candidate fraction from measured CPU Verus consumption and queue
backlog. This uses measured throughput rather than CPU or GPU model IDs.
The CUDA kernel now precomputes three nonce-independent SHA rounds, the
nonce-independent part of the fourth round, and two SHA schedule words on the
host. The explicit CUDA candidate test and full eight-test CTest suite pass.
The 64M nonce batch remains opt-in; the default is 16M. The current source
retains the 4M candidate queue and 1,024-candidate CPU handoff. The qualifying
runs used `BRMINER_CUDA_BATCH_SIZE=67108864` and
`BRMINER_CUDA_BLOCK_SIZE=256` with the optional NVRTC CUDA backend.

## Earlier custom miner on this host

The previous Warthog miner is under
an earlier local custom-miner checkout.
Its recorded Ryzen 5800X/RTX 4060 Linux Docker comparison used a 16-million
nonce CUDA batch, a 2.8% candidate fraction, two GPU slots, three pending CPU
batches, and a two-lane AVX2 Verus CLHash implementation. Four paired runs
averaged 5.71% more independently accepted work than BzMiner v100.46. That is
evidence for those techniques, but it is a different OS/runtime, BzMiner
version, CPU thread count (16), and benchmark protocol from this goal's
Windows v100.60, 14-thread run. The source and raw evidence are retained there
in `LAB_RESULTS.md`, `native/`, and `docker/results/`.

The 16-million OpenCL batch and MIT licensed clean SHA kernel were each ported
into BRminer and checked with five-minute runs above. Neither improved the
combined score on this Windows configuration, so both trials were reverted.
The prior native paired Verus hasher now runs on 512-candidate CPU handoffs.
It checks AVX2, AES, PCLMUL, SSSE3, and operating-system AVX state at runtime;
the existing single-hash path remains available when the Clang build or CPU
features are absent. Its hashes and Janus results match the consensus path in
the new native batch and score-equivalence tests. The first two integration
trials called the batch API for only two candidates and regressed sharply;
batching the existing 512-candidate handoff raised the completed Janus mean
to 428.96 MH/s.
