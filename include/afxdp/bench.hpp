// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <memory>
#include <print>
#include <span>
#include <utility>
#include <x86intrin.h>

namespace afxdp::bench {

// rdtscp waits for prior instructions to retire; cheaper than lfence+rdtsc+lfence
// and accurate enough at microsecond scale.
[[nodiscard]] inline std::uint64_t rdtsc() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

// TSC ticks -> nanoseconds, calibrated once against CLOCK_MONOTONIC.
// Only valid if the CPU has constant_tsc + nonstop_tsc; check /proc/cpuinfo.
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

// Fixed-capacity latency sample buffer. One heap allocation at construction,
// none on the hot path. Single-writer: the poll thread records, the main thread
// reports only after that thread has joined, so no synchronization is needed.
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

// Swap L2/L3/L4 endpoints in place so a reflected frame is a valid reply.
// Returns false (frame left untouched past the point of failure) if this isn't a
// plain untagged IPv4/UDP frame — the XDP filter also accepts VLAN-tagged IPv4,
// which this does not yet rewrite; extend it to skip tags if you benchmark those.
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
    // The IPv4 header checksum is a one's-complement sum over 16-bit words;
    // swapping two of those words leaves the sum unchanged, so it stays valid.

    const std::size_t ihl = static_cast<std::size_t>(ip->ihl) * 4;
    if (ip->protocol == IPPROTO_UDP && f.size() >= sizeof(ethhdr) + ihl + sizeof(udphdr)) {
        auto* udp = reinterpret_cast<udphdr*>(f.data() + sizeof(ethhdr) + ihl);
        std::swap(udp->source, udp->dest);
        udp->check = 0; // legal for IPv4: "checksum not computed"
    }
    return true;
}

} // namespace afxdp::bench
