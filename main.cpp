// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#include "bench.hpp"
#include "common.hpp"
#include "frame_alloc.hpp"
#include "receiver.hpp"
#include "transmitter.hpp"
#include "umem.hpp"
#include "xdp_loader.hpp"
#include "xsk.hpp"
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <print>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

struct Args {
    std::string iface = "eth0";
    std::string filter_ip = "0.0.0.0";
    std::uint32_t queue = 0;
    int cpu_pin = -1;
    bool realtime = false;
    bool huge_pages = true;
    bool zerocopy = true;
};

namespace {

constexpr std::uint32_t NUM_FRAMES = 8192;
constexpr std::uint32_t FRAME_SIZE = 2048;
constexpr std::uint32_t RING_SIZE = 2048;
constexpr std::size_t ALLOC_CAP = 8192;
constexpr std::size_t FRAME_LEN = 64;
constexpr std::uint64_t DROP_ANOMALY = 100000;

// A signal can run on either thread; its atomic store must be signal-safe.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> g_stop{false};
void signal_handler(int /*signum*/) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

void print_usage(const char* prog) {
    std::println(stderr,
                 "Usage: {} -i <iface> -f <filter_ip> [-q <queue>] [-c <cpu>] "
                 "[-r] [--no-hugepages] [--no-zerocopy]",
                 prog);
    std::println(stderr, "  -i  NIC interface (default: eth0)");
    std::println(stderr, "  -f  Source IP to filter (required), e.g. 192.168.1.100");
    std::println(stderr, "  -q  NIC RX queue index (default: 0)");
    std::println(stderr, "  -c  CPU core to pin to (default: no pinning)");
    std::println(stderr, "  -r  Enable SCHED_FIFO realtime scheduling (needs root)");
    std::println(stderr, "  --no-hugepages  Disable 2MB huge page allocation");
    std::println(stderr, "  --no-zerocopy   Force XDP_COPY mode");
}

// clang-format off
std::optional<Args> parse_args(int argc, char* argv[]) {
    Args a{};
    bool got_filter = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-i" && i + 1 < argc) { a.iface = argv[++i]; }
        else if (arg == "-f" && i + 1 < argc) { a.filter_ip = argv[++i]; got_filter = true; }
        else if (arg == "-q" && i + 1 < argc) { a.queue = static_cast<std::uint32_t>(std::stoul(argv[++i])); }
        else if (arg == "-c" && i + 1 < argc) { a.cpu_pin = std::stoi(argv[++i]); }
        else if (arg == "-r") { a.realtime = true; }
        else if (arg == "--no-hugepages") { a.huge_pages = false; }
        else if (arg == "--no-zerocopy") { a.zerocopy = false; }
        else { print_usage(argv[0]); return std::nullopt; }
    }

    if (!got_filter) {
        std::println(stderr, "Error: -f <filter_ip> is required.");
        print_usage(argv[0]);
        return std::nullopt;
    }
    return a;
}
// clang-format on

} // namespace

int main(int argc, char* argv[]) {
    auto maybe_args = parse_args(argc, argv);
    if (!maybe_args) {
        return 1;
    }
    const Args& args = *maybe_args;

    std::println("[main] Starting AF_XDP receiver");
    std::println("[main] Interface : {}", args.iface);
    std::println("[main] Filter IP : {}", args.filter_ip);
    std::println("[main] Queue     : {}", args.queue);

    const afxdp::UmemConfig umem_cfg{.num_frames = NUM_FRAMES,
                                     .frame_size = FRAME_SIZE,
                                     .headroom = 2, // NET_IP_ALIGN: 4-byte-align the L3 header
                                     .use_huge_pages = args.huge_pages};

    auto umem_result = afxdp::Umem::create(umem_cfg);
    if (!umem_result) {
        std::println(stderr, "[main] UMEM creation failed: {}", umem_result.error().message());
        return 1;
    }
    afxdp::Umem umem = std::move(*umem_result);

    std::println("[main] UMEM: {}MB @ {:p}", umem.byte_size() / (1024 * 1024), umem.base_ptr());

    const afxdp::XskConfig xsk_cfg{.iface = args.iface,
                                   .queue_id = args.queue,
                                   .ring_size = RING_SIZE,
                                   .zero_copy = args.zerocopy,
                                   .need_wakeup = true,
                                   .busy_poll = true};

    auto xsk_result = afxdp::Xsk::create(xsk_cfg, umem);
    if (!xsk_result) {
        std::println(stderr, "[main] XSK creation failed: {}", xsk_result.error().message());
        return 1;
    }
    afxdp::Xsk xsk = std::move(*xsk_result);

    std::println("[main] XSK socket fd={}", xsk.raw_desc());

    afxdp::FrameAllocator<ALLOC_CAP> alloc(NUM_FRAMES, FRAME_SIZE);

    std::println("[main] Frame allocator: {}/{} frames free", alloc.free_count(), NUM_FRAMES);

    xsk.refill_fill(alloc);

    auto loader_result = afxdp::XdpLoader::create(
        "xdp_prog.bpf.o", args.iface, args.queue, xsk.raw_desc(), args.filter_ip);
    if (!loader_result) {
        std::println(stderr, "[main] XDP loader failed: {}", loader_result.error().message());
        return 1;
    }
    afxdp::XdpLoader loader = std::move(*loader_result);

    {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        ::sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
    }

    std::println("[main] Running. Press Ctrl+C to stop.");

    afxdp::Transmitter<ALLOC_CAP> tx(xsk, alloc, umem);

    struct alignas(afxdp::CACHE_SIZE) BenchStats {
        std::atomic<std::uint64_t> packets{0};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::uint64_t> echoed{0};
    };
    BenchStats bench{};

    // Latency histogram: one heap allocation here, none on the hot path.
    const double ns_per_tick = afxdp::bench::calibrate_ns_per_tick();
    afxdp::bench::Samples samples(1u << 22); // ~4.2M samples, 16 MB

    auto strategy = [&tx, &bench, &samples](const afxdp::PacketView& pkt) noexcept {
        const auto t0 = afxdp::bench::rdtsc();
        (void)afxdp::bench::reflect_swap(pkt.data);
        // Only the poll thread writes these counters; no locked RMW is needed.
        bench.packets.store(bench.packets.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
        bench.bytes.store(bench.bytes.load(std::memory_order_relaxed) + pkt.data.size(),
                          std::memory_order_relaxed);
        const bool transferred = tx.send_in_place(pkt.addr,
                                                  static_cast<std::uint32_t>(pkt.data.size()));
        if (transferred) {
            bench.echoed.store(bench.echoed.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
        }
        samples.record(afxdp::bench::rdtsc() - t0);
        return transferred ? afxdp::FrameDisposition::Transferred
                           : afxdp::FrameDisposition::Recycle;
    };

    auto flush_tx = [&tx]() noexcept { tx.flush(); };

    const afxdp::ReceiverConfig recv_cfg{.cpu_affinity = args.cpu_pin,
                                         .realtime_sched = args.realtime,
                                         .batch_size = 64};

    afxdp::Receiver<ALLOC_CAP, decltype(strategy), decltype(flush_tx)> receiver(
        xsk, alloc, umem, strategy, flush_tx, recv_cfg);

    {
        std::jthread poll_thread([&receiver](std::stop_token st) { receiver.run(std::move(st)); });

        using Clock = std::chrono::steady_clock;
        auto last_report = Clock::now();
        std::uint64_t last_drops = 0;

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const auto now = Clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();

            // Each counter is race-free; the group is not one coherent snapshot.
            const auto rstats = receiver.stats();
            const auto tstats = tx.stats();
            const std::uint64_t drops = tstats.drops;
            std::println("[stats] Δt={}ms | rx={} | fill_refills={} | tx={} | tx_drops={} | "
                         "bench_pkts={} | bench_bytes={} | bench_echoed={}",
                         elapsed_ms,
                         rstats.packets_received,
                         rstats.fill_refills,
                         tstats.packets_sent,
                         drops,
                         bench.packets.load(std::memory_order_relaxed),
                         bench.bytes.load(std::memory_order_relaxed),
                         bench.echoed.load(std::memory_order_relaxed));

            const std::uint64_t drops_in_window = drops - last_drops;
            if (drops_in_window > DROP_ANOMALY) {
                std::println(stderr,
                             "[control] drop anomaly ({} in last window) — shutting down",
                             drops_in_window);
                g_stop.store(true, std::memory_order_relaxed);
            }
            last_drops = drops;
            last_report = now;
        }

        std::println("[main] Stopping...");
        poll_thread.request_stop();
    }

    // The poll thread has joined, so the histogram's plain reads are safe.
    samples.report(ns_per_tick, "service");
    samples.dump_csv("service_ns.csv", ns_per_tick);

    std::println("[main] Done. rx={} tx={} drops={}",
                 receiver.stats().packets_received,
                 tx.stats().packets_sent,
                 tx.stats().drops);
    return 0;
}
