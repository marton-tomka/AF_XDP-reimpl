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
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <optional>
#include <print>
#include <sched.h>
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
constexpr std::uint64_t DROP_ANOMALY = 100000;
constexpr std::size_t SAMPLE_CAPACITY = 1u << 22;
constexpr auto SAMPLE_WARMUP = std::chrono::seconds{1};
constexpr auto TX_DRAIN_TIMEOUT = std::chrono::milliseconds{100};

// A signal can run on either thread; its atomic store must be signal-safe.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> g_stop{false};
void signal_handler(int /*signum*/) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

[[nodiscard]] std::expected<void, int> install_signal_handlers() noexcept {
    struct sigaction action{};
    action.sa_handler = signal_handler;
    action.sa_flags = SA_RESTART;
    if (::sigemptyset(&action.sa_mask) != 0) return std::unexpected(errno);
    struct sigaction previous_int{};
    if (::sigaction(SIGINT, &action, &previous_int) != 0) return std::unexpected(errno);
    if (::sigaction(SIGTERM, &action, nullptr) != 0) {
        const int error = errno;
        (void)::sigaction(SIGINT, &previous_int, nullptr);
        return std::unexpected(error);
    }
    return {};
}

void print_usage(const char* prog) {
    std::println(stderr,
                 "Usage: {} -i <iface> -f <filter_ip> [-q <queue>] [-c <cpu>] "
                 "[-r] [--no-hugepages] [--no-zerocopy]",
                 prog);
    std::println(stderr, "  -i  NIC interface (default: eth0)");
    std::println(stderr, "  -f  Source IP to filter (required), e.g. 192.168.1.100");
    std::println(stderr, "  -q  NIC RX queue index (default: 0)");
    std::println(stderr, "  -c  Worker CPU (default: CPU selected during startup)");
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

int run_receiver(const Args& args) {
    std::println("[main] Starting AF_XDP receiver");
    std::println("[main] Interface : {}", args.iface);
    std::println("[main] Filter IP : {}", args.filter_ip);
    std::println("[main] Queue     : {}", args.queue);

    const int worker_cpu = args.cpu_pin >= 0 ? args.cpu_pin : ::sched_getcpu();
    if (worker_cpu < 0) {
        std::println(stderr, "[main] Cannot select worker CPU: {}", std::strerror(errno));
        return 1;
    }

    afxdp::bench::Samples samples(SAMPLE_CAPACITY); // 32 MiB, initialized before attachment.
    afxdp::bench::TscCalibration calibration{};
    std::uint64_t final_rx = 0, final_tx = 0, final_drops = 0;
    int exit_code = 0;

    // The entire active I/O lifetime ends before sample sorting/export.
    {
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
        if (const auto result = xsk.kick_fill();
            !result && !afxdp::is_expected_wakeup_retry(result.error())) {
            std::println(stderr,
                         "[main] Initial RX wakeup failed: {} (errno={}); worker will retry",
                         std::strerror(result.error()),
                         result.error());
        }

        afxdp::Transmitter<ALLOC_CAP> tx(xsk, alloc, umem);

        struct alignas(afxdp::CACHE_SIZE) BenchStats {
            std::atomic<std::uint64_t> packets{0};
            std::atomic<std::uint64_t> bytes{0};
            std::atomic<std::uint64_t> echoed{0};
        };
        BenchStats bench{};

        auto strategy = [&tx, &bench, &samples](const afxdp::PacketView& pkt) noexcept {
            const bool timed = samples.recording();
            const auto t0 = timed ? afxdp::bench::rdtsc() : 0;
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
            if (timed) samples.record(afxdp::bench::rdtsc() - t0);
            return transferred ? afxdp::FrameDisposition::Transferred
                               : afxdp::FrameDisposition::Recycle;
        };

        auto flush_tx = [&tx]() noexcept { tx.flush(); };

        const afxdp::ReceiverConfig recv_cfg{.cpu_affinity = worker_cpu,
                                             .realtime_sched = args.realtime,
                                             .batch_size = 64};

        afxdp::Receiver<ALLOC_CAP, decltype(strategy), decltype(flush_tx)> receiver(
            xsk, alloc, umem, strategy, flush_tx, recv_cfg);

        // Declared before the worker: exceptional exits join it before loader cleanup.
        std::optional<afxdp::XdpLoader> loader;
        std::atomic<bool> prepared{false};
        std::atomic<bool> activated{false};
        std::expected<afxdp::bench::TscCalibration, int> startup = std::unexpected(ECANCELED);
        std::jthread poll_thread([&](std::stop_token st) noexcept {
            std::stop_callback unblock(st, [&]() noexcept {
                activated.store(true, std::memory_order_release);
                activated.notify_one();
            });
            if (const auto result = receiver.prepare_thread(); !result)
                startup = std::unexpected(result.error());
            else
                startup = afxdp::bench::calibrate_tsc();
            prepared.store(true, std::memory_order_release);
            prepared.notify_one();
            if (!startup) return;
            activated.wait(false, std::memory_order_acquire);
            receiver.run(std::move(st));
        });

        prepared.wait(false, std::memory_order_acquire);
        if (g_stop.load(std::memory_order_relaxed)) return 0;
        if (!startup) {
            std::println(stderr,
                         "[main] Worker preparation/calibration failed: {} (errno={})",
                         std::strerror(startup.error()),
                         startup.error());
            return 1;
        }
        calibration = *startup;

        auto loader_result = afxdp::XdpLoader::create(
            "xdp_prog.bpf.o", args.iface, args.queue, xsk.raw_desc(), args.filter_ip);
        if (!loader_result) {
            std::println(stderr, "[main] XDP loader failed: {}", loader_result.error().message());
            return 1;
        }
        loader.emplace(std::move(*loader_result));

        using Clock = std::chrono::steady_clock;
        const auto activated_at = Clock::now();
        if (!g_stop.load(std::memory_order_relaxed)) {
            activated.store(true, std::memory_order_release);
            activated.notify_one();
            std::println("[main] Worker CPU={} | TSC={:.6f} ns/tick | empty timer pair min={} "
                         "median={} ticks",
                         worker_cpu,
                         calibration.ns_per_tick,
                         calibration.pair_min_ticks,
                         calibration.pair_median_ticks);
            std::println(
                "[measure] callback_tx_staging: warmup={}ms, next {} callbacks, then no timestamps",
                std::chrono::duration_cast<std::chrono::milliseconds>(SAMPLE_WARMUP).count(),
                SAMPLE_CAPACITY);
            std::println("[main] Running. Press Ctrl+C to stop.");
        }

        auto last_report = activated_at;
        std::uint64_t last_drops = 0;
        bool sampling_started = false;

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const auto now = Clock::now();
            if (!sampling_started && now - activated_at >= SAMPLE_WARMUP &&
                !g_stop.load(std::memory_order_relaxed)) {
                samples.start();
                sampling_started = true;
            }
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();

            // Each counter is race-free; the group is not one coherent snapshot.
            const auto rstats = receiver.stats();
            const auto tstats = tx.stats();
            const std::uint64_t drops = tstats.drops;
            std::println("[stats] Δt={}ms | rx={} | fill_refills={} | tx={} | tx_drops={} | "
                         "bench_pkts={} | bench_bytes={} | bench_echoed={} | "
                         "tx_retry_kicks={} | tx_first_wakeup_errno={} | rx_first_wakeup_errno={}",
                         elapsed_ms,
                         rstats.packets_received,
                         rstats.fill_refills,
                         tstats.packets_sent,
                         drops,
                         bench.packets.load(std::memory_order_relaxed),
                         bench.bytes.load(std::memory_order_relaxed),
                         bench.echoed.load(std::memory_order_relaxed),
                         tstats.wakeup_retries,
                         tstats.first_wakeup_error,
                         rstats.first_wakeup_error);

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
        if (const auto result = loader->stop_redirect(); !result) {
            std::println(stderr,
                         "[main] Stopping redirection failed: {} (errno={})",
                         std::strerror(result.error()),
                         result.error());
            exit_code = 1;
        }
        poll_thread.request_stop();
        poll_thread.join();

        const auto remaining = tx.drain_until(Clock::now() + TX_DRAIN_TIMEOUT);
        if (remaining != 0) {
            std::println(stderr,
                         "[main] TX drain timed out: abandoning {} outstanding frames at teardown",
                         remaining);
            exit_code = 1;
        }
        if (const auto result = loader->detach(); !result) {
            std::println(stderr,
                         "[main] XDP detach failed: {} (errno={})",
                         std::strerror(result.error()),
                         result.error());
            return 1;
        }

        final_rx = receiver.stats().packets_received;
        final_tx = tx.stats().packets_sent;
        final_drops = tx.stats().drops;
    }

    samples.report(calibration.ns_per_tick, "callback_tx_staging");
    samples.dump_csv("callback_tx_staging_ns.csv", calibration.ns_per_tick);
    std::println("[main] Done. rx={} tx={} drops={}", final_rx, final_tx, final_drops);
    return exit_code;
}

int main(int argc, char* argv[]) {
    auto maybe_args = parse_args(argc, argv);
    if (!maybe_args) return 1;
    if (const auto result = install_signal_handlers(); !result) {
        std::println(stderr,
                     "[main] Signal setup failed: {} (errno={})",
                     std::strerror(result.error()),
                     result.error());
        return 1;
    }
    try {
        return run_receiver(*maybe_args);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr,
                     "[main] Aborted: %s; cleanup attempted, queued work abandoned\n",
                     error.what());
        return 1;
    }
}
