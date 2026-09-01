# Benchmarking

How to produce defensible numbers for `afxdp_receiver` without owning two machines with real NICs.

Three phases, in order. Phase 0 is mandatory — without it the other two produce nothing you can publish. Phase 1 is free and runs on one laptop. Phase 2 costs a few dollars and anchors the result to real hardware.

## What each phase can and cannot prove

| | Zero-copy path exercised | Userspace service time | ZC vs. copy delta | AF_XDP vs. kernel UDP | Absolute NIC→userspace latency | Line-rate pps |
|---|---|---|---|---|---|---|
| **veth** (current) | ✗ never | ✓ | ✗ | partial¹ | ✗ | ✗ |
| **Phase 1** virtio-net in KVM | ✗⁴ | ✓ | ✗⁴ | partial⁴ | ✗ | ✗ |
| **Phase 2** AWS ENA | ✓ | ✓ | ✓ | ✓ | partial² | ✗³ |

¹ Both arms run in copy mode, so the AF_XDP arm carries a kernel-side memcpy that real hardware wouldn't have. The delta is a lower bound, contaminated.
² Virtualised NIC; p50 is meaningful, p99 is a hypervisor jitter floor.
³ ENA's per-queue ceiling is ~1 Mpps and this engine is single-queue. Not fixable in cloud.
⁴ **Measured 2026-07-30 and it did not pan out as hoped** — virtio-net's `XDP_ZEROCOPY` bind returns EINVAL, so Phase 1 runs copy-mode only (no ZC delta here; that moves entirely to Phase 2). AF_XDP-vs-kernel RTT is only *partial*: the reflector is incompatible with `sockperf ping-pong`, and the ~100 µs userspace-virtio backend swamps the µs-scale signal anyway. Full detail in [Resume point](#resume-point--session-log-2026-07-30) at the bottom.

**veth does not support AF_XDP zero-copy.** It has native XDP but no `xsk_pool`, so every veth run has been `XDP_COPY`. Phase 1 was expected to fix that — virtio-net *does* have zero-copy in-tree (RX landed ~6.11, TX followed) — but in practice the bind is rejected with EINVAL (see ⁴), so Phase 1 also stays copy-mode. Its value is that it exercises the full path on a *real native-XDP driver* (not veth's generic mode) and yields honest copy-mode service-time numbers; the zero-copy proof point lives in Phase 2.

---

## Phase 0 — instrumentation

Do all four on your current machine, verify on veth, commit. None of it needs hardware.

### 0.1 Make zero-copy failures visible

[`xsk.hpp`](../include/afxdp/xsk.hpp) currently discards the reason the `XDP_ZEROCOPY` bind failed and silently retries in copy mode. You are about to spend two phases chasing that bind — you need the errno.

```cpp
        } else {
            const int zc_errno = errno;
            log(LogLevel::Warn,
                std::format("XDP_ZEROCOPY bind failed: {} (errno={}) — falling back to copy mode",
                            std::strerror(zc_errno), zc_errno));
            sxdp.sxdp_flags &= ~static_cast<std::uint16_t>(XDP_ZEROCOPY);
        }
```

`EOPNOTSUPP` = driver has no ZC support. `EINVAL` = usually queue index out of range. `EBUSY` = something else owns the queue.

### 0.2 Latency histogram

New file `include/afxdp/bench.hpp`. One heap allocation at construction, none on the hot path.

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <print>
#include <x86intrin.h>

namespace afxdp::bench {

// rdtscp waits for prior instructions to retire; cheaper than lfence+rdtsc+lfence
// and accurate enough at microsecond scale.
[[nodiscard]] inline std::uint64_t rdtsc() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

// TSC ticks -> nanoseconds, calibrated once against CLOCK_MONOTONIC.
[[nodiscard]] inline double calibrate_ns_per_tick(long settle_ms = 200) noexcept {
    timespec t0{}, t1{};
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    const std::uint64_t c0 = rdtsc();
    const timespec req{.tv_sec = settle_ms / 1000, .tv_nsec = (settle_ms % 1000) * 1'000'000};
    ::nanosleep(&req, nullptr);
    const std::uint64_t c1 = rdtsc();
    ::clock_gettime(CLOCK_MONOTONIC, &t1);

    const double ns = static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e9 +
                      static_cast<double>(t1.tv_nsec - t0.tv_nsec);
    return ns / static_cast<double>(c1 - c0);
}

class Samples {
public:
    explicit Samples(std::size_t capacity)
        : buf_(std::make_unique<std::uint32_t[]>(capacity))
        , cap_(capacity) {}

    void record(std::uint64_t ticks) noexcept {
        // uint32_t halves the buffer's memory. A per-packet service time never
        // approaches 2^32 ticks (~1.4 s at 3 GHz); a scheduler stall that long
        // would wrap to a small bogus value, which shows up as a lone low outlier,
        // not a corrupted percentile. Widen to uint64_t if you don't want even that.
        if (n_ < cap_) [[likely]] {
            buf_[n_++] = static_cast<std::uint32_t>(ticks);
        } else {
            ++dropped_;
        }
    }

    // Call at shutdown only — this sorts the buffer in place.
    void report(double ns_per_tick, const char* label) noexcept {
        if (n_ == 0) {
            std::println("[{}] no samples", label);
            return;
        }
        std::sort(buf_.get(), buf_.get() + n_);
        auto pct = [&](double p) noexcept {
            const auto i = std::min(n_ - 1, static_cast<std::size_t>(p * static_cast<double>(n_)));
            return static_cast<double>(buf_[i]) * ns_per_tick;
        };
        std::println("[{}] n={} dropped={} | p50={:.0f}ns p90={:.0f}ns p99={:.0f}ns "
                     "p99.9={:.0f}ns max={:.0f}ns",
                     label, n_, dropped_, pct(0.50), pct(0.90), pct(0.99), pct(0.999),
                     static_cast<double>(buf_[n_ - 1]) * ns_per_tick);
    }

    // Raw dump for offline plotting. Call after report() — buffer is sorted by then.
    void dump_csv(const char* path, double ns_per_tick) const noexcept {
        std::FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "ns\n");
        for (std::size_t i = 0; i < n_; ++i) {
            std::fprintf(f, "%.0f\n", static_cast<double>(buf_[i]) * ns_per_tick);
        }
        std::fclose(f);
    }

    [[nodiscard]] std::size_t count() const noexcept { return n_; }

private:
    std::unique_ptr<std::uint32_t[]> buf_;
    std::size_t cap_;
    std::size_t n_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace afxdp::bench
```

Wire it into the strategy lambda in [`main.cpp`](../main.cpp) — no change to [`receiver.hpp`](../include/afxdp/receiver.hpp) needed. This is the timing skeleton; **the final strategy you'll actually paste is in 0.3 below**, which adds the reflect step inside the same timed region:

```cpp
const double ns_per_tick = afxdp::bench::calibrate_ns_per_tick();
afxdp::bench::Samples samples(1u << 22);   // 16 MB, ~4.2M samples

auto strategy = [&tx, &bench, &samples](const afxdp::PacketView& pkt) noexcept {
    const auto t0 = afxdp::bench::rdtsc();
    bench.packets++;
    bench.bytes += pkt.data.size();
    if (tx.send(pkt.data)) {
        ++bench.echoed;
    }
    samples.record(afxdp::bench::rdtsc() - t0);
};
```

The `samples`/`ns_per_tick` setup and the post-join `report()`/`dump_csv()` calls stay as shown; only the lambda body gains the reflect line in 0.3.

And after the poll thread joins:

```cpp
samples.report(ns_per_tick, "service");
samples.dump_csv("service_ns.csv", ns_per_tick);
```

**What this measures:** per-packet userspace service time — RX descriptor to TX enqueue, including the memcpy in [`transmitter.hpp`](../include/afxdp/transmitter.hpp). Not end-to-end latency. Label it that way.

**Check TSC sanity first**, especially in a VM:

```bash
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u   # want both
```

If either is missing, the numbers drift and you need `clock_gettime(CLOCK_MONOTONIC)` instead (~25 ns overhead, tolerable at this scale).

### 0.3 Make the reflector produce valid replies

This one blocks the whole "vs. kernel UDP socket" comparison. The current callback echoes the frame **unmodified**, so MACs and IPs point the wrong way and no normal socket will accept the reply — that is why `ping` reports 100% loss in [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md). Any timestamping client will do the same, which means you cannot measure round-trip time against either arm.

Fix is cheap. Add to `bench.hpp`:

```cpp
#include <arpa/inet.h>
#include <cstring>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <span>

// Swap L2/L3/L4 endpoints in place so the reflected frame is a valid reply.
// Returns false if this isn't a plain untagged IPv4/UDP frame.
[[nodiscard]] inline bool reflect_swap(std::span<std::byte> f) noexcept {
    if (f.size() < sizeof(ethhdr) + sizeof(iphdr)) return false;

    auto* eth = reinterpret_cast<ethhdr*>(f.data());
    if (eth->h_proto != htons(ETH_P_IP)) return false;
    std::uint8_t mac[ETH_ALEN];
    std::memcpy(mac, eth->h_dest, ETH_ALEN);
    std::memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
    std::memcpy(eth->h_source, mac, ETH_ALEN);

    auto* ip = reinterpret_cast<iphdr*>(f.data() + sizeof(ethhdr));
    if (ip->version != 4) return false;
    std::swap(ip->saddr, ip->daddr);
    // IPv4 header checksum is a sum over 16-bit words — swapping two fields
    // leaves it unchanged, so there is nothing to recompute here.

    const std::size_t ihl = static_cast<std::size_t>(ip->ihl) * 4;
    if (ip->protocol == IPPROTO_UDP &&
        f.size() >= sizeof(ethhdr) + ihl + sizeof(udphdr)) {
        auto* udp = reinterpret_cast<udphdr*>(f.data() + sizeof(ethhdr) + ihl);
        std::swap(udp->source, udp->dest);
        udp->check = 0;   // legal for IPv4: "checksum not computed"
    }
    return true;
}
```

**Wiring it in.** `PacketView::data` is `std::span<const std::byte>` — the receiver deliberately hands out const views ([`receiver.hpp`](../include/afxdp/receiver.hpp) builds them from `static_cast<const std::byte*>(umem_base_)`). But `umem_base_` points into a `PROT_READ | PROT_WRITE` mmap ([`umem.hpp`](../include/afxdp/umem.hpp)), so the bytes are not const objects and a `const_cast` to write them is well-defined. Do it in the strategy lambda, which is the only place that both sees the frame and calls `tx.send()`:

```cpp
auto strategy = [&tx, &bench, &samples](const afxdp::PacketView& pkt) noexcept {
    const auto t0 = afxdp::bench::rdtsc();
    // UMEM is writable; the const on PacketView::data is a courtesy, not a real
    // const object, so casting it away to reflect in place is defined here.
    std::span<std::byte> frame(const_cast<std::byte*>(pkt.data.data()), pkt.data.size());
    afxdp::bench::reflect_swap(frame);           // no-op return ignored: non-IPv4/UDP
    bench.packets++;                             // frames are counted but echoed as-is
    bench.bytes += pkt.data.size();
    if (tx.send(pkt.data)) {
        ++bench.echoed;
    }
    samples.record(afxdp::bench::rdtsc() - t0);
};
```

This is the single strategy for Phase 0.2 **and** 0.3 combined — use this one, not the 0.2 snippet plus a separate edit. If a `const_cast` in the hot path bothers you, the alternative is to add a mutable `PacketView` variant and a second `Receiver` callback concept, but that ripples through `PacketView::as<T>()` and the `std::invocable` constraint for no runtime benefit.

> **VLAN caveat.** The XDP filter ([`xdp_prog_bpf.c`](../bpf/xdp_prog_bpf.c)) redirects IPv4 nested in up to two 802.1Q/802.1ad tags, but `reflect_swap` above only recognises *untagged* IPv4/UDP and returns `false` on tagged frames (they still get echoed unmodified). A direct link, veth, and the virtio/ENA labs below all carry untagged traffic, so this is fine as-is — but if you ever benchmark a tagged flow, the reflected frame won't be a valid reply. Extend `reflect_swap` to skip the VLAN tags the same way the BPF program does.

> **Known caveat to disclose in results.** `Transmitter::send()` memcpy's into a freshly allocated frame. A true zero-copy reflector re-submits the *RX frame's own address* to the TX ring and lets the completion ring recycle it (the `xdpsock` l2fwd pattern). Your reflect path therefore carries one copy plus an alloc/free pair that a ZC forwarder does not. Either fix it or state it — do not report reflect throughput as a zero-copy number without the asterisk.

### 0.4 Fix the stats race — but not the way that breaks the build

[`main.cpp`](../main.cpp) reads counters written by the poll thread with no synchronisation, and the comment admits "technically ub". For a liveness readout it genuinely is fine — aligned 64-bit loads don't tear on x86-64. But two of these counters (`rx` → pps, `tx_drops`) feed numbers you're about to publish, so make the reads well-defined.

**Do not** make the `Stats` members `std::atomic<std::uint64_t>` — that's the obvious move and it breaks the build in two places:

- [`receiver.hpp`](../include/afxdp/receiver.hpp) resets with `stats_ = {};` at the top of `run()`. Atomic members are not copy-assignable, so that line stops compiling.
- [`main.cpp`](../main.cpp) passes `rstats.packets_received` straight to `std::println`. There's no `std::formatter` for `std::atomic`, and format arguments are captured by reference, so the implicit `.load()` never happens — every stats print fails to compile.

The idiomatic fix in this codebase is `std::atomic_ref` over the existing plain `std::uint64_t` members — exactly how [`ring.hpp`](../include/afxdp/ring.hpp) already treats the kernel-shared head/tail words. Members stay plain, `stats_ = {}` keeps working, and the reporter reads them explicitly:

```cpp
// hot path (poll thread), receiver.hpp / transmitter.hpp — relaxed is enough,
// these counters have no ordering relationship with anything else:
std::atomic_ref<std::uint64_t>(stats_.packets_received)
    .store(stats_.packets_received + processed, std::memory_order_relaxed);

// reporter (main.cpp), once a second:
const std::uint64_t rx =
    std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(rstats.packets_received))
        .load(std::memory_order_relaxed);
```

Relaxed atomics are the same `mov` on x86-64, so the cost really is zero — the point is only to make the data race defined. The `const_cast` at the read site is needed because `stats()` returns `const Stats&`; if that reads badly, add a non-const `stats_mut()` accessor for the reporter, or hoist the reads into the receiver/transmitter behind a small `snapshot()` method.

The percentile data in `samples` needs none of this — it has a single writer (the poll thread) and is read only after the thread joins, so there's no race to fix there.

### 0.5 Verify on veth

Run the existing veth setup from [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md). You should now see `COPY` (expected — veth has no ZC), a warning with `EOPNOTSUPP`, and a `[service]` percentile line at shutdown. That confirms the harness works. Then move on.

---

## Phase 1 — virtio-net lab, one laptop, zero cost

Two KVM guests on a bridge. Each gets a management NIC (SSH/apt) and a lab NIC on the bridge. Only the lab NIC is ever under test.

```
   ┌── vm-dut ──┐                    ┌── vm-gen ──┐
   │ lab: eth1  │──tap-dut──┐ ┌──tap-gen──│ eth1: lab │
   │ 10.10.0.1  │           br-afxdp        │ 10.10.0.2  │
   └────────────┘                    └────────────┘
```

### 1.1 Host prerequisites

```bash
sudo apt install -y qemu-system-x86 qemu-utils cloud-image-utils bridge-utils
egrep -c 'vmx|svm' /proc/cpuinfo    # must be > 0
```

### 1.2 Bridge and taps

```bash
sudo ip link add br-afxdp type bridge
sudo ip link set br-afxdp up

for n in dut gen; do
  sudo ip tuntap add dev tap-$n mode tap user "$USER"
  sudo ip link set tap-$n master br-afxdp
  sudo ip link set tap-$n up
done
```

Do **not** add `multi_queue` to the tap: the single-queue QEMU netdev in 1.4 can't attach to a `multi_queue` tap and fails with `could not configure /dev/net/tun (tap-dut): Invalid argument`. Only add it back if you deliberately switch to the `vhost=on,queues=N` device (see 1.4). If `br_netfilter` happens to be loaded, keep iptables out of the path:

```bash
sudo sysctl -w net.bridge.bridge-nf-call-iptables=0 2>/dev/null || true
```

### 1.3 Guest images

Ubuntu 26.04 LTS ships **kernel 7.0**, comfortably past the 6.11 that virtio-net AF_XDP zero-copy needs, and has a new enough GCC for the C++23 in this project.

```bash
mkdir -p ~/afxdp-lab && cd ~/afxdp-lab
wget https://cloud-images.ubuntu.com/releases/26.04/release/ubuntu-26.04-server-cloudimg-amd64.img

cat > user-data <<'EOF'
#cloud-config
password: afxdp
chpasswd: { expire: false }
ssh_pwauth: true
EOF

for n in dut gen; do
  printf 'instance-id: %s\nlocal-hostname: %s\n' "$n" "$n" > meta-data
  cloud-localds seed-$n.iso user-data meta-data
  qemu-img create -f qcow2 -F qcow2 \
    -b "$PWD/ubuntu-26.04-server-cloudimg-amd64.img" $n.qcow2 20G
done
```

### 1.4 Boot the guests

The offload flags are the part that matters. **`ethtool -K` cannot fix this** — the virtio-net driver reads the feature bits from the device, so guest offloads have to be switched off on the QEMU command line or XDP attach fails with `Can't set XDP while host is implementing LRO, disable LRO first`.

Two terminals, one each. Swap `dut`↔`gen`, `2221`↔`2222`, **and the lab NIC's `mac=`** (`…:01` for dut, `…:02` for gen):

```bash
qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=dut.qcow2,if=virtio \
  -drive file=seed-dut.iso,if=virtio,format=raw \
  -netdev user,id=mgmt,hostfwd=tcp::2221-:22 \
  -device virtio-net-pci,netdev=mgmt \
  -netdev tap,id=lab,ifname=tap-dut,script=no,downscript=no \
  -device virtio-net-pci,netdev=lab,mac=52:54:00:00:00:01,gso=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off \
  -nographic
```

Single queue, no vhost — which is what this benchmark wants anyway (the engine is single-RX-queue and §1.5 forces `combined 1`). Keep the whole `-device` line unbroken; a `\` in the middle of the comma-separated options is a common paste failure.

**Set an explicit `mac=` on each lab device, and make them differ.** QEMU's default MAC is deterministic (`52:54:00:12:34:56`, `…:57`, …) and assigned per-instance, so two separately-launched VMs both hand their second NIC `…:34:57`. Identical MACs on one bridge break ARP (the bridge can't place the address) and, because both NICs then derive the same IPv6 link-local, trigger a duplicate-address-detection storm — thousands of packets on an "idle" link, and `ping` reports `Destination Host Unreachable`. `mac=52:54:00:00:00:01` on dut and `…:02` on gen avoids it.

**Why not `vhost=on,queues=4,mq=on`?** `vhost=on` needs to open `/dev/vhost-net`, which is `root:kvm` — if you're not in the `kvm` group it fails with `open vhost char device failed: Permission denied`, and because multiqueue tap *requires* vhost, `queues=4` turns that into a fatal `net_client_init1: Assertion 'nc' failed` abort. Zero-copy is a guest-driver property (the guest's virtio_net against the virtqueues), independent of the host backend, so the userspace backend exercises the same XDP ZC path — it's only slower host-side, which doesn't affect the *relative* numbers Phase 1 collects. If you specifically want the faster vhost backend, `sudo usermod -aG kvm $USER`, re-login, then append `,vhost=on` (single queue) or `,vhost=on,queues=4` plus `mq=on,vectors=10` on the device (`vectors` ≥ `2*queues+2`).

`-cpu host` passes the TSC through, which Phase 0's calibration depends on.

If XDP attach still fails after this with a checksum complaint (some kernels also gate on `VIRTIO_NET_F_GUEST_CSUM`), add `csum=off` to the same device option list. The five flags above are the canonical set and are usually sufficient, but `csum=off` is the documented next step if the attach log still mentions LRO/CSUM.

Log in as `ubuntu` / `afxdp`, or `ssh -p 2221 ubuntu@localhost`.

### 1.5 Guest setup (both)

```bash
sudo apt update && sudo apt install -y cmake clang build-essential pkg-config \
    libbpf-dev libelf-dev zlib1g-dev iproute2 ethtool linux-tools-generic
g++ --version    # must be >= 14; 26.04's default already is, so no gcc-14 pin needed

LAB=$(ip -o link | awk -F': ' '/^3:/{print $2}')   # second NIC = the tap one
sudo ip link set "$LAB" up
sudo ip addr add 10.10.0.1/24 dev "$LAB"           # .2 on vm-gen

sudo ethtool -L "$LAB" combined 1                  # this engine is single-queue
echo 2      | sudo tee /sys/class/net/$LAB/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/$LAB/gro_flush_timeout
echo 64     | sudo tee /proc/sys/vm/nr_hugepages   # UMEM wants 16 MB of 2 MB pages
```

`combined 1` matters — the default `-q 0` will otherwise watch a queue your traffic never lands on, and [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md) already warns that a wrong queue is silence, not an error.

Build as usual, then on **vm-dut**:

```bash
cd build && sudo ./afxdp_receiver -i "$LAB" -f 10.10.0.2 -c 2
```

**What success looks like:** the log line reads `ZEROCOPY`, not `COPY`. This is the first time that code path has ever run. If it says `COPY`, the errno from 0.1 tells you which of the offload flags didn't take.

### 1.6 Generating load

Userspace UDP tops out around 300–600 kpps, so use in-kernel `pktgen` on vm-gen. It needs no dependencies and works over virtio.

```bash
LAB=$(ip -o link | awk -F': ' '/^3:/{print $2}')   # re-detect in this shell (vm-gen)
sudo modprobe pktgen
PG=/proc/net/pktgen

echo "rem_device_all"     | sudo tee $PG/kpktgend_0 >/dev/null
echo "add_device $LAB"    | sudo tee $PG/kpktgend_0 >/dev/null

for k in "count 0" "clone_skb 0" "pkt_size 64" "delay 0" \
         "dst 10.10.0.1" "dst_mac <DUT_LAB_MAC>" "udp_dst_min 9000" "udp_dst_max 9000"; do
  echo "$k" | sudo tee $PG/$LAB >/dev/null
done

echo start | sudo tee $PG/pgctrl      # Ctrl-C or `echo stop` to end
```

`$LAB` must be the lab NIC on **vm-gen**, not the DUT — pktgen binds to the local egress device. Get `<DUT_LAB_MAC>` from `ip link show $LAB` on vm-dut (pktgen won't ARP, so the destination MAC has to be set by hand). `count 0` runs until stopped; `clone_skb 0` forces a fresh skb per packet, which is slower but honest.

### 1.7 The run matrix

Four runs, one variable changed at a time, same core pinning and same generator rate throughout:

`$LAB` below is the DUT's lab interface (`ens4`/`enp0s…`, whatever §1.5 detected) — substitute the real name, there is no NIC literally called `eth1`.

| Run | Command | Gives you |
|---|---|---|
| A | `./afxdp_receiver -i "$LAB" -f 10.10.0.2 -c 2` | ZC service time + pps |
| B | `./afxdp_receiver -i "$LAB" -f 10.10.0.2 -c 2 --no-zerocopy` | copy-mode service time + pps |
| C | UDP echo server, same core | kernel socket baseline |
| D | A, with `--no-hugepages` | hugepage contribution |

**A vs. B is your headline result** — same binary, same box, same core, one flag. That is the zero-copy delta, and it is the most defensible number you can produce without buying hardware.

For run C, a minimal same-core baseline:

```bash
taskset -c 2 python3 -c "
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.bind(('10.10.0.1',9000))
while True:
    d,a=s.recvfrom(2048); s.sendto(d,a)
"
```

Python is a strawman — for a fair fight write the equivalent in C with `recvmmsg`/`sendmmsg` and `SO_BUSY_POLL`, or use `sockperf server` / `sockperf ping-pong -i 10.10.0.1 -p 9000 --full-log`. `sockperf` gives you percentiles for free, and once 0.3 is done it works against the AF_XDP arm too, because the reflected frames are finally valid replies.

**Record for every run:** ZEROCOPY/COPY, kernel version, `-c` core, generator rate, `[service]` percentiles, steady-state `rx`/`tx_drops`, and guest CPU utilisation of the poll core. A percentile with no load level next to it is not a result.

---

## Phase 2 — hardware anchor on AWS

One afternoon, roughly **$1.50–3.00** on spot. Purpose: one real-NIC data point so the Phase 1 relative numbers aren't purely synthetic. ENA has native AF_XDP with zero-copy.

Set expectations: you get real DMA and a real driver, but the ~1 Mpps ENA per-queue ceiling means the throughput row stays out of reach for a single-queue engine, and p99 will be a hypervisor jitter floor. This phase is for the latency comparison and for proving ZC works on physical silicon.

### 2.1 Provision

```bash
sudo apt install -y awscli && aws configure     # needs an AWS account

REGION=us-east-1
aws ec2 create-placement-group --group-name afxdp-lab --strategy cluster --region $REGION
aws ec2 create-key-pair --key-name afxdp --region $REGION \
  --query KeyMaterial --output text > ~/.ssh/afxdp.pem && chmod 600 ~/.ssh/afxdp.pem

SG=$(aws ec2 create-security-group --group-name afxdp-lab \
     --description "afxdp bench" --region $REGION --query GroupId --output text)
MYIP=$(curl -s https://checkip.amazonaws.com)
aws ec2 authorize-security-group-ingress --group-id $SG --protocol tcp --port 22 \
  --cidr $MYIP/32 --region $REGION
aws ec2 authorize-security-group-ingress --group-id $SG --protocol -1 \
  --source-group $SG --region $REGION          # instances talk freely to each other

AMI=$(aws ssm get-parameters --region $REGION \
  --names /aws/service/canonical/ubuntu/server/26.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
  --query 'Parameters[0].Value' --output text)

aws ec2 run-instances --region $REGION --image-id $AMI --count 2 \
  --instance-type c6in.2xlarge --key-name afxdp --security-group-ids $SG \
  --placement GroupName=afxdp-lab \
  --instance-market-options '{"MarketType":"spot"}' \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=afxdp-lab}]'
```

Grab both private and public IPs:

```bash
aws ec2 describe-instances --region $REGION \
  --filters Name=tag:Name,Values=afxdp-lab Name=instance-state-name,Values=running \
  --query 'Reservations[].Instances[].[InstanceId,PublicIpAddress,PrivateIpAddress]' --output table
```

### 2.2 Two gotchas that will otherwise cost you the session

The ENA interface is usually `ens5`, but newer AMIs name it `enp39s0` or similar — confirm with `ip -br link` and substitute below. The examples use `ens5`.

**MTU.** AWS defaults to 9001 inside a VPC. XDP without multi-buffer caps at **3502** on ENA, so attach fails until you lower it. On both instances:

```bash
sudo ip link set dev ens5 mtu 1500
```

**Queues.** ENA gives one queue per vCPU and reserves TX queues when XDP attaches. Reduce before starting:

```bash
sudo ethtool -L ens5 combined 1
sudo ip link set ens5 up
```

Then the same busy-poll knobs and hugepage setting as §1.5, build, and run against the *private* IP of the other instance.

**If it logs `COPY` with `EOPNOTSUPP`:** the ENA driver in that kernel lacks zero-copy. On the 26.04 AMI (kernel 7.0) this shouldn't happen — the in-tree driver has had native AF_XDP zero-copy since ~6.17, per the [driver README](https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst). If you deliberately booted an older AMI and hit it, the fix is the out-of-tree driver via DKMS — clone [amzn-drivers](https://github.com/amzn/amzn-drivers) and follow `kernel/linux/ena/README.rst`, which carries its own `dkms.conf` and exact version string (don't guess the `-v`; read it from the repo). Simpler is to just use the 26.04 AMI and avoid the detour entirely.

Re-run the Phase 1 matrix (§1.7) unchanged. Same runs, real NIC.

### 2.3 Tear down — do this immediately

Spot instances bill until terminated. Run this the moment you're finished:

```bash
aws ec2 describe-instances --region $REGION \
  --filters Name=tag:Name,Values=afxdp-lab Name=instance-state-name,Values=running \
  --query 'Reservations[].Instances[].InstanceId' --output text \
  | xargs -r aws ec2 terminate-instances --region $REGION --instance-ids
```

Then check the console that nothing is left. An 8-vCPU instance forgotten for a month is roughly $330.

### Alternative: hourly bare metal

Vultr (~$0.275/hr) or Cherry Servers (from ~$0.30/hr) rent real bare metal by the hour — better data quality than ENA because you control IRQ affinity, C-states and `isolcpus`, and there is no per-queue governor. Two boxes for four hours is $2–5, comparable to spot.

The catch is that the NIC is a lottery and you cannot always choose. Confirm before paying that you get `mlx5` (ConnectX-4/5/6), `ice` (E810), `i40e` (X710) or `ixgbe` (X520/82599). Avoid Realtek `r8169` (XDP was proposed in 2021 and never merged) and Intel `e1000e` (XDP only submitted upstream in March 2026).

---

## Reporting

Replace the projected table in [`README.md`](../README.md) with measured relative numbers and an explicit scope statement. Suggested shape:

| Metric | Measured | Conditions |
|---|---|---|
| Zero-copy vs. copy mode, service time p50/p99 | *(A vs. B)* | virtio-net, kernel 7.0, single queue, 1 core, N kpps |
| AF_XDP vs. kernel UDP socket, RTT p50/p99 | *(A vs. C)* | as above, `sockperf ping-pong` |
| Zero-copy confirmed on physical NIC | ENA, `XDP_ZEROCOPY` bind | c6in.2xlarge, MTU 1500 |

And state plainly what is **not** claimed: absolute NIC→userspace latency (needs hardware RX timestamping via `bpf_xdp_metadata_rx_timestamp()`, driver-dependent) and sustained line-rate pps (needs a dedicated NIC; ENA caps a single queue near 1 Mpps).

Scoping honestly reads as more rigorous than a table of projections with asterisks. It also tells a reader exactly which experiment would close each gap.

## References

- veth has native XDP but no zero-copy: <https://www.mail-archive.com/ovs-dev@openvswitch.org/msg38048.html>
- virtio-net AF_XDP zero copy: <https://lwn.net/Articles/980882/>
- virtio-net XDP requires guest offloads off at the device level: <https://access.redhat.com/solutions/3939881>
- ENA native AF_XDP with zero-copy, MTU limits: <https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst>
- ENA per-queue pps ceiling (~1 Mpps): <https://toonk.io/aws-network-performance-deep-dive/index.html>
- r8169 XDP never merged: <https://lkml.iu.edu/hypermail/linux/kernel/2109.1/08831.html>
- e1000e XDP submitted March 2026: <https://lkml.iu.edu/hypermail/linux/kernel/2603.1/11671.html>

---

## Resume point / session log (2026-07-30)

Where the benchmarking effort actually stands, so it can be picked up cold.

### Done

- **Phase 0 — implemented and verified.** `include/afxdp/bench.hpp` (rdtscp calibration, `Samples` histogram, `reflect_swap`) plus edits to `main.cpp`, `xsk.hpp` (errno on ZC-bind failure), `receiver.hpp`/`transmitter.hpp` (`std::atomic_ref` stats via `fetch_add`/`load`). Builds clean under `-Wall -Wextra`; `reflect_swap`/histogram unit-tested on the host. **These changes are in the working tree but NOT committed** — commit them before relying on them long-term.
- **Phase 1 — lab built and run.** Two Ubuntu 26.04 KVM guests (`dut`, `gen`) on bridge `br-afxdp`, single-queue userspace-virtio (no vhost — not in `kvm` group), distinct lab MACs `…:01`/`…:02`, lab IPs `10.10.0.1`/`10.10.0.2`. dut has the repo built at `~/af_xdp/build` (from an scp'd tarball snapshot).

### Phase 1 measured results (copy mode, 1 core, in-VM — jittery)

| Metric | Value | Note |
|---|---|---|
| XDP attach | native (DRV) ✓ | virtio supports native XDP |
| `XDP_ZEROCOPY` bind | **EINVAL → COPY fallback** | not EOPNOTSUPP; virtio ZC is RX-first + TX-constrained, reflector needs both. Not chased. |
| Sustained throughput | **~298K pps, tx==rx, 0 drops** | rate-limited pktgen. Full-blast startup burst trips `DROP_ANOMALY=100000` in `main.cpp` → auto-shutdown in ~1 s. |
| Service-time histogram | **p50=705 ns, p90=988 ns, p99=1431 ns, p99.9=12.5 µs, max=362 µs** | the headline Phase-1 number; measured inside the callback, so free of backend noise |
| Kernel-socket RTT baseline (`sockperf`) | p50≈107 µs, p99≈248 µs | dominated by the ~100 µs userspace-virtio tax; coarse |
| AF_XDP RTT arm (`sockperf`) | **incompatible** | reflected reply IS kernel-valid (`UdpInDatagrams` delivered, no error counters), but `sockperf ping-pong` needs a real sockperf server, not a verbatim reflector |

**Takeaways:** the 705 ns/1431 ns service-time histogram is the defensible Phase-1 latency result. Zero-copy is untestable here (EINVAL) → it moves entirely to Phase 2. The RTT comparison is not viable in this no-vhost lab (backend noise + sockperf incompatibility).

### To bring the lab back up

VMs are powered off; the host bridge + taps persist (recreate via §1.2 if gone). Then:

1. **Boot both VMs** with the §1.4 commands (they now bake in `mac=…:01`/`…:02`, so no manual MAC fix needed). SSH: `ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2221 ubuntu@localhost` (dut), `2222` (gen), password `afxdp`.
2. **Re-apply runtime guest config** (does NOT survive reboot; the built binary in the qcow2 does): on each guest re-derive `LAB` and re-add the IP, then on dut re-set the busy-poll knobs + hugepages — the §1.5 block does all of this. Substitute `ens4` if detection differs.
3. **If the host repo changed since 2026-07-30**, re-scp the source into dut and rebuild (dut's copy is a snapshot): `tar czf /tmp/afxdp-src.tgz --exclude=./build --exclude=./.git . && scp -P 2221 … /tmp/afxdp-src.tgz ubuntu@localhost:~/`.
4. **Drive load** with the §1.6 pktgen block (`dst_mac 52:54:00:00:00:01`, `src_min/max 10.10.0.2`), rate-limited (`delay`), not full-blast.

### Open threads / next session

- *(optional, ~2 min)* Confirm the reflector end-to-end with a plain UDP probe that accepts any echo (the Python snippet from the session) — expected 10/10; closes the "is it a bug?" question (it isn't).
- *(optional, richer result)* **Latency-vs-load curve**: run pktgen at several `delay` values (e.g. 10K/50K/100K/200K pps), record `[service]` p50/p99 at each. One point (298K → 705 ns) is thin; a curve is publishable.
- **Phase 2 (AWS ENA)** is now load-bearing, not optional — it's the *only* place zero-copy and a clean RTT comparison can be measured. See §2.
- **Update `README.md`**: replace the projected perf table with the measured copy-mode numbers above + an explicit "zero-copy delta and absolute RTT: pending Phase 2 / hardware" scope line.
- **Commit the Phase 0 code** (still uncommitted).
