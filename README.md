# afxdp_receiver

Kernel-bypass packet I/O on Linux: an AF_XDP receive/transmit engine in C++23, with an eBPF/XDP program steering selected traffic from the NIC directly into userspace memory, possibly removing context switches, a copy, and a ton of latency.

The point of the project is the latency toolbox: kernel bypass, zero-copy DMA buffers, lock-free SPSC rings, busy-polling instead of interrupts, huge pages, CPU pinning, and no heap allocation on the hot path.

## Packet path

![Packet path: kernel UDP socket path versus this project's AF_XDP path](docs/packet-path.svg)

Non-matching traffic never leaves the normal kernel path, so the machine stays reachable (SSH, etc.) while the engine owns its target flow.

## What's inside

| Piece | File | The interesting bit |
|---|---|---|
| XDP filter | `xdp_prog_bpf.c` | In-kernel classification: Ethernet → up to two stacked VLAN tags (802.1Q/802.1ad) → IPv4 source match. Hits are redirected to the AF_XDP socket; everything else `XDP_PASS`es. The filter IP lives in a BPF array map, so it's swappable at runtime without reattaching the program. |
| UMEM | `umem.hpp` | One mmap'd 16 MB region (8192 × 2048 B frames), 2 MB huge pages with automatic 4 KB fallback, mlocked. |
| Rings | `ring.hpp` | One template over all four AF_XDP rings (RX/TX/Fill/Completion). `std::atomic_ref` with acquire/release ordering on the kernel-shared head/tail words; producer/consumer indices are cached locally and re-read from shared memory only when the cached view runs dry. |
| Frame allocator | `frame_alloc.hpp` | Fixed-capacity LIFO stack of UMEM frame offsets, O(1) alloc/free, zero heap after startup. |
| Socket setup | `xsk.hpp` | `XDP_ZEROCOPY` bind with automatic copy-mode fallback; NAPI busy-polling via `SO_PREFER_BUSY_POLL` (budget 64, 20µs timeout (hardcoded)). |
| BPF loader | `xdp_loader.hpp` | libbpf attach - native (driver) mode first, generic (SKB) fallback. |
| Receiver | `receiver.hpp` | Pinned, optionally `SCHED_FIFO` polling thread; 64-frame batches; hands each frame to a user callback as a raw byte span; refills the Fill ring past a threshold. |
| Transmitter | `transmitter.hpp` | TX ring producer plus completion reaping; frames return to the allocator. |

Error handling is `std::expected` end to end; there are no exceptions.

## Current scope

Single RX queue, single socket, IPv4-only filtering. The receive callback gets raw Ethernet frames. The bundled reflector expects known unicast peers, untagged IPv4/UDP, a 20-byte IPv4 header, no fragmentation, and an IPv4 MTU of 1500. Unsupported layouts are counted and recycled. The XDP source-IP filter is broader than this callback's packet contract.

Reports distinguish RX processing, TX staging, TX publication, CQ reclamation, and rejected frames. Kernel socket statistics are queried on the control thread. After shutdown, `callback_tx_staging_ns.csv` contains the sorted callback/staging sample distribution and `run_metadata.txt` retains build settings, actual operating modes, counters, and outcome. These measurements do not establish wire delivery or end-to-end latency. The [implementation notes](docs/REPORTING_AND_PACKET_CONTRACT.md) explain the contract, counter meanings, renamed methods, assembly, and regressions.

## Performance

Not yet measured in a proper test environment; functionality confirmed via veth (COPY mode only).

## Build & run

```bash
cmake -S . -B build && cmake --build build
sudo ./build/afxdp_receiver -i <iface> -f <source_ip_to_capture>
```

Needs Linux ≥ 5.11 (≥ 5.14 for zero-copy on Intel igc / I225/I226), GCC ≥ 14, clang, CMake ≥ 3.20, libbpf-dev. Full walkthrough (including the veth-based test setup that needs no physical NIC) in [`BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md).

## License

MIT, except `xdp_prog_bpf.c`, which is GPL-2.0-only (required by the BPF helpers it uses). Full text in [`LICENSE`](LICENSE).

## Note on AI usage

Everything that has to do with benchmarking (that is: bench.hpp, and parts of main with extensive commenting [strategy and load lambdas]) were entirely written by fable 5, as I do not yet have the hardware to test and benchmark this extensively down to the nanos, when the time comes I'll work through it myself though. :D

Also the .md-s and most of the readme were of course written by AI, along with the C code (xdp_prog_bfc.c).

Nontheless the entirity of the infrastructure and all logical elements - that is: 8 implementation headers and main.cpp - are custom designed and built by me.
