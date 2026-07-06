# Build & Test

How to build `afxdp_receiver`, run it, and verify the full path (XDP filter → redirect → userspace callback) end to end, including on a machine with no spare NIC, using a veth pair.

## Prerequisites

| Requirement | Why |
|---|---|
| Linux kernel ≥ 5.11 | `SO_PREFER_BUSY_POLL` / `SO_BUSY_POLL_BUDGET`; busy-polling is enabled unconditionally in `main.cpp` |
| GCC ≥ 14 | `std::print`, `std::expected` |
| clang | compiles the XDP program to the `bpf` target |
| CMake ≥ 3.20, pkg-config | build system |
| libbpf-dev (+ libelf, zlib) | BPF object loading |
| iproute2 | the veth test setup below |

```bash
sudo apt-get install -y cmake clang gcc-14 g++-14 pkg-config \
    libbpf-dev libelf-dev zlib1g-dev iproute2
```

Tested on Ubuntu 24.04 (gcc-14 14.2, clang 18, libbpf 1.3).

## Build

```bash
cmake -S . -B build
cmake --build build
```

This produces two artifacts in `build/`:

- `afxdp_receiver`: the userspace engine (C++23, compiled by your host compiler)
- `xdp_prog.bpf.o`: the XDP program, built by a separate `clang -target bpf` step. A BPF object is not a host-architecture object; it can't be produced by the normal compiler machinery or linked into the executable, which is why CMake shells out to clang for it.

The binary loads `"xdp_prog.bpf.o"` by a path relative to the **current working directory** — run from `build/`, or keep the two files together wherever you deploy them.

## Runtime requirements

**Privilege.** AF_XDP needs to create the socket, attach a BPF program, and lock UMEM pages. Simplest: run as root. Non-root requires `CAP_BPF`, `CAP_NET_ADMIN`, `CAP_NET_RAW`, `CAP_SYS_RESOURCE` (kernel-version dependent).

**Memlock.** On startup the program raises `RLIMIT_MEMLOCK` itself; the *environment's hard limit* must allow that. If you see `setrlimit(RLIMIT_MEMLOCK): Operation not permitted`, raise the ceiling where the process runs:

| Environment | Fix |
|---|---|
| Docker | `--ulimit memlock=-1` |
| Bare metal / VM | `/etc/security/limits.conf`: `* hard memlock unlimited` |
| systemd service | `LimitMEMLOCK=infinity` |

**Busy-poll sysfs knobs.** `SO_PREFER_BUSY_POLL` only keeps interrupts parked if the interface defers them; without these two settings the feature largely degrades to ordinary IRQ-driven NAPI:

```bash
echo 2      | sudo tee /sys/class/net/<iface>/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/<iface>/gro_flush_timeout
```

## Quick functional test (veth, no hardware needed)

An isolated network namespace on one end of a veth pair, the receiver on the other. XDP attaches to veth in generic (SKB) mode — the loader falls back to it automatically after native mode fails, which is expected on virtual interfaces.

```bash
# topology: veth0 (host) <-> veth1 (namespace "xdptest")
sudo ip netns add xdptest
sudo ip link add veth0 type veth peer name veth1
sudo ip link set veth1 netns xdptest

sudo ip addr add 10.10.0.1/24 dev veth0
sudo ip link set veth0 up
sudo ip netns exec xdptest ip addr add 10.10.0.2/24 dev veth1
sudo ip netns exec xdptest ip link set veth1 up
sudo ip netns exec xdptest ip link set lo up

# receiver on the host end, filtering on the namespace's source IP
cd build
sudo ./afxdp_receiver -i veth0 -f 10.10.0.2

# traffic from the namespace (second shell)
sudo ip netns exec xdptest ping 10.10.0.1
```

**What success looks like:** the once-a-second stats line shows `rx` and `bench_pkts` climbing.

Two expected oddities, neither a failure:

- `ping` itself may report 100% loss. The bundled callback echoes the raw frame back unmodified; that is not a well-formed ICMP reply, so `ping` won't count it. To see the reflected frames: `sudo ip netns exec xdptest tcpdump -i veth1` (tcpdump is promiscuous by default and shows them regardless of destination MAC).
- The loader logs `DRV mode failed … falling back to SKB mode` — normal on veth.

Teardown (deleting one end of a veth pair removes both):

```bash
sudo ip netns del xdptest && sudo ip link del veth0
```

## Real NIC

- **Queue selection.** `-q` must be the queue your target flow actually lands on. Check queue count with `ethtool -l <iface>`; either reduce to one queue (`ethtool -L <iface> combined 1`) or steer the flow explicitly (`ethtool -N` n-tuple rules). Wrong queue = silence, not an error.
- **Zero-copy.** Needs driver support (`i40e`, `ice`, `mlx5`, `igc`, and others on recent kernels — verify for your kernel). The socket tries `XDP_ZEROCOPY` first and falls back to copy mode automatically; the log line (`ZEROCOPY` / `COPY`) tells you which you got, and the two are not comparable in benchmarks.
- **Pinning.** `-c <core>` pins the poll thread; pick an isolated core for measurement. `-r` requests `SCHED_FIFO` — read up on RT throttling (`sched_rt_runtime_us`) before using it on a non-isolated core.

## What the built-in benchmark reports

The bundled callback is a reflector: it counts every matched frame and echoes it back out the TX ring.

| Field | Meaning |
|---|---|
| `rx` | frames delivered to userspace by the RX ring |
| `fill_refills` | Fill-ring top-up passes (buffer supply to the driver) |
| `tx` / `tx_drops` | frames sent / send attempts dropped (ring full, allocator empty, oversized) |
| `bench_pkts` / `bench_bytes` | matched traffic seen by the callback |
| `bench_echoed` | frames successfully reflected |
| `Δt` | actual wall time of the reporting window |

Two validity caveats, stated so the numbers aren't over-read:

1. The stats counters are currently written by the poll thread and read by the reporting thread without synchronization — fine as a liveness/health readout, not yet publication-grade instrumentation.
2. veth numbers exercise the software path only. Latency/throughput claims belong on a physical NIC.

## Running as a service

```ini
# /etc/systemd/system/afxdp-receiver.service
[Unit]
Description=AF_XDP receiver
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/afxdp
ExecStart=/opt/afxdp/afxdp_receiver -i eth0 -f 203.0.113.10 -q 0
AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW CAP_BPF CAP_SYS_RESOURCE CAP_IPC_LOCK
LimitMEMLOCK=infinity
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`WorkingDirectory` matters for the same reason as in Build: the relative `xdp_prog.bpf.o` path resolves against it — ship both files to `/opt/afxdp` together. On kernels where the capability set proves insufficient, fall back to running as root.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `setrlimit(RLIMIT_MEMLOCK): Operation not permitted` | environment caps the hard memlock limit | table in Runtime requirements |
| `setsockopt(SO_PREFER_BUSY_POLL/…)` fails | kernel < 5.11 | upgrade, or remove the hardcoded `busy_poll = true` in `main.cpp` and rebuild |
| `bind(AF_XDP, XDP_COPY)` fails | queue index doesn't exist, or already bound | check `-q` against `ethtool -l`; check for another bound socket |
| `DRV mode failed … falling back to SKB` | no native XDP support on the interface | expected on veth; on real NICs check driver support |
| `xsks_map` / `target_ip_map` not found | stale `xdp_prog.bpf.o` next to the binary | rebuild; keep binary and object in sync |
| runs, but `rx` stays 0 | filter mismatch or wrong queue | `-f` filters on **source** IP; verify the flow's queue (RSS) with `ethtool -l` / n-tuple steering; `ip link show <iface>` should list the attached prog |
