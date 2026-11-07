# BzMiner calibration observations

These observations use the saved local `bzminer-15m-internal.log`, which identifies
the tested executable as **v100.60**. A folder/archive name is not its runtime
version. BzMiner's public repository currently exposes documentation and releases,
not the implementation of this calibration code.

## Observed local sequence

| Event | Offset from worker start |
| --- | ---: |
| Worker started | 0.000 s |
| Verus capacity reported: 26.26 MH/s | 15.658 s |
| SHA capacity reported: 1.09 GH/s | 30.879 s |
| Filter tuned: request 26.79 MH/s, delivered 25.96 MH/s | 117.246 s |

The log names these stages `verus (1/3)`, `sha (2/3)`, and `filter (3/3)`.
It also reports 13.04 GB/s device-link bandwidth, 14 workers on 16 logical
processors, an L3-local queue, processor placement, and pinned downloads.
During filter calibration, the requested candidate rate changes; the final
request is near the measured CPU capacity. It continues receiving jobs during
calibration. The displayed `i0` indicates automatic intensity, and does not
reveal which CUDA block size or internal launch dimensions were used.

## Inference and limits

The evidence supports a capacity-and-balance approach: measure CPU Verus
capacity and GPU SHA capacity, account for transfer limits, then adjust candidate
flow while observing the combined pipeline. It does **not** establish the exact
search algorithm, stopping condition, sample windows, kernel launch dimensions,
or caching policy. We are implementing an independent staged planner, not
claiming to reproduce the proprietary implementation.

The public Warthog guide independently describes startup PCIe measurement,
cache-aware worker planning, and a calibration phase of roughly two minutes.
The installed version's logs are the stronger evidence for this particular run.

## BRminer workflow

- Quick calibration measures several CPU thread counts, probes the supported GPU launch and
  transfer capacity, and compares a small number of combined Janus configurations.
- A challenger must beat the portable baseline by at least 1%, then preserve a
  1% average gain in a reverse-order confirmation pair.
- The measured capacities seed the existing adaptive candidate filter; feedback
  continues during mining.
- Quick choices are provisional and cached under the exact hardware, driver,
  compiler, build, and manual-constraint identity. Ordinary CUDA mining loads a
  matching quick plan when no fully validated profile exists; it never reuses
  another device's settings.
- `--calibrate --full` confirms the short-listed configurations against fallbacks
  with repeated five-minute measurements. Only that evidence can promote a saved
  profile. A matching validated profile takes precedence on subsequent launches.
- Manual CPU threads, batch size, and block size remain constraints in both modes.

Target: finish the quick path within about two to three minutes on the available
one-GPU host. Report actual elapsed time; do not promise a fixed duration on all hardware.

References:

- [Local 15-minute BzMiner log](../out/bench/bzminer-15m-internal.log)
- [BzMiner Warthog guide](https://www.bzminer.com/guides/warthog.php)
- [BzMiner public documentation](https://github.com/bzminer/bzminer)
