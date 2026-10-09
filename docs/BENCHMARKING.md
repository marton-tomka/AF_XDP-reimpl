# Benchmarking

Build in Release using [Build & Test](BUILD_AND_TEST.md), verify a UDP round trip, and then measure the configured workload. The reflector accepts the fixed packet format described in the [README](../README.md).

The worker pins itself and calibrates its TSC before activation. After at least one second of warmup, it records the next 4,194,304 callback durations, or fewer if stopped earlier. Once the buffer is full, the callback stops taking timestamps.

`callback_tx_staging` measures layout checks, endpoint swaps, callback counters, and the TX staging decision. It excludes batch publication, kernel wakeups, waiting for completion, and wire time. Unsupported layouts and TX rejections are part of the sampled population. This is not a NIC-to-application or round-trip latency measurement.

Shutdown stops redirection, joins the worker, and attempts a bounded TX drain before releasing I/O resources and reporting. The retained outputs are:

| Output | Contents |
|---|---|
| Console | Cumulative application/kernel counters and callback percentiles |
| `callback_tx_staging_ns.csv` | Recorded durations converted to rounded nanoseconds, sorted ascending; no packet chronology |
| `run_metadata.txt` | Source/build information, actual copy/XDP/UMEM modes, queue/CPU/polling configuration, calibration, sample-window information, final counters, and outcome |

The percentile index is `min(n - 1, floor(p * n))`, without interpolation. Timer-pair overhead is reported separately and is not subtracted from samples. The metadata's sampling start is the control-thread gate time, not a timestamp for each sample.

Counter meanings:

| Field | Meaning |
|---|---|
| `rx_frames_processed` | RX descriptors processed, including unsupported packets and TX rejections |
| `callbacks` / `callback_bytes` | Callback invocations and received frame bytes, including Ethernet headers/padding |
| `unsupported_frames` | Reflector layout checks failed; the frame was recycled without a TX attempt |
| `tx_frames_staged` | TX accepted ownership of a frame into its local descriptor window |
| `tx_frames_submitted` | TX descriptors published to the shared producer index |
| `tx_frames_reclaimed` | Frames returned to the allocator through CQ |
| `tx_frames_rejected` | TX staging rejected the length or found no ring space |
| `fill_refill_batches` | Worker passes that published FILL entries; excludes initial prefill |
| `tx_retryable_wakeup_errors` | TX wakeup calls returning EAGAIN, EINTR, EBUSY, or ENOBUFS |
| `tx_first_wakeup_errno` / `rx_first_wakeup_errno` | First unexpected wakeup errors observed by the worker/TX maintenance |

Reports contain lifetime totals. To calculate a rate, use the difference between successive counts divided by their actual elapsed interval. Concurrent counters are individually atomic, not one coherent snapshot. TX publication and CQ reclamation do not prove peer delivery; use sender/receiver counts or sequence numbers for loss and round-trip measurements.

The control thread queries `XDP_STATISTICS` at report time and after draining. Kernel drop/invalid-descriptor counters provide additional diagnostics; empty-ring counters are not packet-loss totals. Fields absent from an older kernel response are marked `unavailable`. The returned structure size is retained because legacy and extended counters differ in layout and RX-drop accounting.

More than 100,000 TX staging rejections in one reporting interval triggers an error stop. This is a count threshold, not a per-second rate, and it limits sustained overload experiments. A clean signal stop returns zero. Statistics-query, export, and shutdown failures return nonzero. Files use fixed names and can be overwritten, so retain each run's outputs before starting another.

For comparable runs, keep packet size/rate, CPU placement, queue steering, NIC/driver/kernel, and polling settings fixed. Record the generator settings and peer results alongside the outputs; the receiver cannot infer offered load or pre-RX losses. Preserve the matching executable, BPF object, and source revision/patch. Compare actual copy/XDP modes, not requested modes, and repeat at several offered loads to expose tails and backpressure.

A veth or VM check establishes software-path behavior. Physical-NIC measurements are needed for claims about the intended hardware. The architectural reference is the kernel's [AF_XDP guide](https://docs.kernel.org/networking/af_xdp.html).

The earlier lab session on 2026-07-30 recorded the following observations on a different revision and measurement implementation:

| Historical observation | Recorded result |
|---|---|
| Two KVM guests, virtio-net, one worker | Native XDP; zero-copy bind returned EINVAL and fell back to copy mode |
| Rate-limited generator | About 298K packets/s; local TX/RX totals matched with no reported application drops |
| Old callback timer | p50 705 ns, p99 1431 ns, p99.9 12.5 µs |
| Kernel-socket sockperf baseline | p50 about 107 µs, p99 about 248 µs |
| AF_XDP sockperf comparison | Incompatible with the raw reflector; no comparable RTT result |

Those observations are historical, not validation of the current timing implementation, zero-copy operation, or end-to-end loss. The obsolete instrumentation copies and VM/cloud setup walkthroughs have been removed; their original versions remain in Git history.
