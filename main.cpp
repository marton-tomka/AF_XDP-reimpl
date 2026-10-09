// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#include "bench.hpp"
#include "build_info.hpp"
#include "common.hpp"
#include "frame_alloc.hpp"
#include "receiver.hpp"
#include "transmitter.hpp"
#include "umem.hpp"
#include "xdp_loader.hpp"
#include "xsk.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <optional>
#include <print>
#include <sched.h>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/utsname.h>
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
constexpr std::uint32_t UMEM_HEADROOM = 2;
constexpr int RX_BATCH_SIZE = 64;
constexpr std::uint32_t BUSY_POLL_BUDGET = 64;
constexpr std::uint32_t BUSY_POLL_TIMEOUT_US = 20;
constexpr std::uint64_t TX_REJECTION_LIMIT = 100000; // Count per reporting interval, not packets/s.
constexpr std::size_t SAMPLE_CAPACITY = 1u << 22;
constexpr auto SAMPLE_WARMUP = std::chrono::seconds{1};
constexpr auto TX_DRAIN_TIMEOUT = std::chrono::milliseconds{100};

static_assert(NUM_FRAMES > 0 && NUM_FRAMES <= ALLOC_CAP);
static_assert(std::has_single_bit(RING_SIZE) && RING_SIZE >= 2 && RING_SIZE <= NUM_FRAMES);
static_assert(std::has_single_bit(FRAME_SIZE));
static_assert(RX_BATCH_SIZE > 0 && RX_BATCH_SIZE <= RING_SIZE);
static_assert(XDP_PACKET_HEADROOM + UMEM_HEADROOM + ETH_HLEN + afxdp::bench::REFLECTOR_IPV4_MTU <=
              FRAME_SIZE);

struct RunSummary {
    std::time_t started_unix_seconds = std::time(nullptr);
    int worker_cpu = -1;
    int scheduler_policy = -1;
    int scheduler_priority = -1;
    bool zero_copy = false, native_xdp = false, hugetlb = false;
    bool sampling_started = false, csv_exported = false;
    double sampling_start_after_ms = 0, active_duration_ms = 0;
    std::size_t samples_recorded = 0;
    std::uint64_t rx_processed = 0, callbacks = 0, callback_bytes = 0, unsupported = 0;
    std::uint64_t tx_staged = 0, tx_submitted = 0, tx_reclaimed = 0, tx_rejected = 0;
    std::uint64_t fill_refill_batches = 0, tx_retryable_wakeup_errors = 0;
    std::uint32_t tx_abandoned = 0;
    int rx_first_wakeup_errno = 0, tx_first_wakeup_errno = 0, first_kernel_stats_errno = 0;
    int stop_redirect_errno = 0;
    const char* stop_reason = "signal";
    std::expected<afxdp::Xsk::KernelStats, int> kernel_stats = std::unexpected(ENODATA);
};

// One format for console and sidecar; unsupported legacy fields are explicit, never false zeros.
int print_kernel_stats(std::FILE* output,
                       const std::expected<afxdp::Xsk::KernelStats, int>& result) noexcept {
    if (!result) return std::fprintf(output, "kernel_stats_errno=%d\n", result.error());
    const auto& s = result->counters;
    if (std::fprintf(output,
                     "kernel_stats_returned_bytes=%u\nkernel_rx_dropped=%llu\n"
                     "kernel_rx_invalid_descs=%llu\nkernel_tx_invalid_descs=%llu\n",
                     result->returned_bytes,
                     s.rx_dropped,
                     s.rx_invalid_descs,
                     s.tx_invalid_descs) < 0)
        return -1;
    if (result->has_ring_counters())
        return std::fprintf(output,
                            "kernel_rx_ring_full=%llu\nkernel_rx_fill_ring_empty_descs=%llu\n"
                            "kernel_tx_ring_empty_descs=%llu\n",
                            s.rx_ring_full,
                            s.rx_fill_ring_empty_descs,
                            s.tx_ring_empty_descs);
    return std::fprintf(
        output,
        "kernel_rx_ring_full=unavailable\nkernel_rx_fill_ring_empty_descs=unavailable\n"
        "kernel_tx_ring_empty_descs=unavailable\n");
}

[[nodiscard]] std::expected<void, int> dump_run_metadata(const Args& args,
                                                         const RunSummary& run,
                                                         const afxdp::bench::TscCalibration& tsc,
                                                         int exit_code) noexcept {
    std::FILE* output = std::fopen("run_metadata.txt", "w");
    if (!output) return std::unexpected(errno);
    utsname host{};
    const bool host_known = ::uname(&host) == 0;
    const int written = std::fprintf(
        output,
        "source_revision=%s\nsource_state=%s\nsource_fingerprint_sha256=%s\n"
        "compiler=%s\nbuild_configuration=%s\ncompile_command=%s\nlibbpf_build_version=%s\n"
        "kernel=%s\nmachine=%s\nstarted_unix_seconds=%lld\n"
        "interface=%s\nfilter_source_ipv4=%s\nqueue=%u\nworker_cpu=%d\n"
        "worker_scheduler_policy=%d\nworker_scheduler_priority=%d\n"
        "zerocopy_requested=%d\ncopy_mode=%s\nxdp_mode=%s\nhugepages_requested=%d\numem_mapping=%"
        "s\n"
        "need_wakeup=1\nbusy_poll=1\nbusy_poll_budget=%u\nbusy_poll_timeout_us=%u\n"
        "num_frames=%u\nframe_size=%u\numem_headroom=%u\nring_size=%u\nrx_batch_size=%d\n"
        "workload=unicast_untagged_ipv4_ihl5_unfragmented_udp_reflection\n"
        "ipv4_mtu=%zu\noffered_load=external_generator_not_measured_here\n"
        "metric=callback_tx_staging_ns\ncsv_order=sorted_ascending\n"
        "sample_population=all_callbacks_including_unsupported_and_tx_rejections\n"
        "warmup_min_ms=%lld\nsample_capacity=%zu\nsampling_started=%d\n"
        "sampling_start_after_activation_ms=%.3f\nsample_window=first_capacity_callbacks_or_worker_"
        "stop\n"
        "samples_recorded=%zu\nactive_duration_ms=%.3f\n"
        "tsc_ns_per_tick=%.12g\ntimer_pair_min_ticks=%llu\ntimer_pair_median_ticks=%llu\n"
        "rx_frames_processed=%llu\ncallbacks=%llu\ncallback_bytes=%llu\nunsupported_frames=%llu\n"
        "tx_frames_staged=%llu\ntx_frames_submitted=%llu\ntx_frames_reclaimed=%llu\n"
        "tx_frames_rejected=%llu\nfill_refill_batches=%llu\ntx_retryable_wakeup_errors=%llu\n"
        "tx_first_wakeup_errno=%d\nrx_first_wakeup_errno=%d\nfirst_kernel_stats_errno=%d\n"
        "tx_rejection_limit_per_report=%llu\nstop_reason=%s\nstop_redirect_errno=%d\n"
        "tx_frames_abandoned=%u\ncsv_exported=%d\nexit_code=%d\n",
        afxdp::build::revision,
        afxdp::build::source_state,
        afxdp::build::source_fingerprint,
        afxdp::build::compiler,
        afxdp::build::configuration,
        afxdp::build::compile_command,
        afxdp::build::libbpf_version,
        host_known ? host.release : "unavailable",
        host_known ? host.machine : "unavailable",
        static_cast<long long>(run.started_unix_seconds),
        args.iface.c_str(),
        args.filter_ip.c_str(),
        args.queue,
        run.worker_cpu,
        run.scheduler_policy,
        run.scheduler_priority,
        args.zerocopy,
        run.zero_copy ? "zero_copy" : "copy",
        run.native_xdp ? "native" : "generic",
        args.huge_pages,
        run.hugetlb ? "hugetlb" : "anonymous",
        BUSY_POLL_BUDGET,
        BUSY_POLL_TIMEOUT_US,
        NUM_FRAMES,
        FRAME_SIZE,
        UMEM_HEADROOM,
        RING_SIZE,
        RX_BATCH_SIZE,
        afxdp::bench::REFLECTOR_IPV4_MTU,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(SAMPLE_WARMUP).count()),
        SAMPLE_CAPACITY,
        run.sampling_started,
        run.sampling_start_after_ms,
        run.samples_recorded,
        run.active_duration_ms,
        tsc.ns_per_tick,
        static_cast<unsigned long long>(tsc.pair_min_ticks),
        static_cast<unsigned long long>(tsc.pair_median_ticks),
        static_cast<unsigned long long>(run.rx_processed),
        static_cast<unsigned long long>(run.callbacks),
        static_cast<unsigned long long>(run.callback_bytes),
        static_cast<unsigned long long>(run.unsupported),
        static_cast<unsigned long long>(run.tx_staged),
        static_cast<unsigned long long>(run.tx_submitted),
        static_cast<unsigned long long>(run.tx_reclaimed),
        static_cast<unsigned long long>(run.tx_rejected),
        static_cast<unsigned long long>(run.fill_refill_batches),
        static_cast<unsigned long long>(run.tx_retryable_wakeup_errors),
        run.tx_first_wakeup_errno,
        run.rx_first_wakeup_errno,
        run.first_kernel_stats_errno,
        static_cast<unsigned long long>(TX_REJECTION_LIMIT),
        run.stop_reason,
        run.stop_redirect_errno,
        run.tx_abandoned,
        run.csv_exported,
        exit_code);
    int error = written < 0 ? (errno ? errno : EIO) : 0;
    if (error == 0 && print_kernel_stats(output, run.kernel_stats) < 0) error = errno ? errno : EIO;
    if (std::fclose(output) != 0 && error == 0) error = errno ? errno : EIO;
    if (error != 0) return std::unexpected(error);
    return {};
}

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
    RunSummary run{};
    run.worker_cpu = worker_cpu;
    int exit_code = 0;

    // The entire active I/O lifetime ends before sample sorting/export.
    {
        const afxdp::UmemConfig umem_cfg{.num_frames = NUM_FRAMES,
                                         .frame_size = FRAME_SIZE,
                                         .headroom = UMEM_HEADROOM,
                                         .use_huge_pages = args.huge_pages};

        auto umem_result = afxdp::Umem::create(umem_cfg);
        if (!umem_result) {
            std::println(stderr, "[main] UMEM creation failed: {}", umem_result.error().message());
            return 1;
        }
        afxdp::Umem umem = std::move(*umem_result);
        run.hugetlb = umem.uses_hugetlb();

        std::println("[main] UMEM: {}MB @ {:p}", umem.byte_size() / (1024 * 1024), umem.base_ptr());

        const afxdp::XskConfig xsk_cfg{.iface = args.iface,
                                       .queue_id = args.queue,
                                       .ring_size = RING_SIZE,
                                       .zero_copy = args.zerocopy,
                                       .need_wakeup = true,
                                       .busy_poll = true,
                                       .busy_poll_budget = BUSY_POLL_BUDGET,
                                       .busy_poll_timeout_us = BUSY_POLL_TIMEOUT_US};

        auto xsk_result = afxdp::Xsk::create(xsk_cfg, umem);
        if (!xsk_result) {
            std::println(stderr, "[main] XSK creation failed: {}", xsk_result.error().message());
            return 1;
        }
        afxdp::Xsk xsk = std::move(*xsk_result);
        run.zero_copy = xsk.zero_copy();

        std::println("[main] XSK socket fd={}", xsk.raw_desc());

        afxdp::FrameAllocator<ALLOC_CAP> alloc(NUM_FRAMES, FRAME_SIZE);

        std::println("[main] Frame allocator: {}/{} frames free", alloc.free_count(), NUM_FRAMES);

        xsk.refill_fill(alloc);
        if (const auto result = xsk.request_rx_progress();
            !result && !afxdp::is_expected_wakeup_retry(result.error())) {
            std::println(stderr,
                         "[main] Initial RX wakeup failed: {} (errno={}); worker will retry",
                         std::strerror(result.error()),
                         result.error());
        }

        afxdp::Transmitter<ALLOC_CAP> tx(xsk, alloc, umem);

        struct alignas(afxdp::CACHE_SIZE) BenchStats {
            std::atomic<std::uint64_t> callbacks{0};
            std::atomic<std::uint64_t> bytes{0};
            std::atomic<std::uint64_t> staged{0};
            std::atomic<std::uint64_t> unsupported{0};
        };
        BenchStats bench{};

        auto strategy = [&tx, &bench, &samples](const afxdp::PacketView& pkt) noexcept {
            const bool timed = samples.recording();
            const auto t0 = timed ? afxdp::bench::rdtsc() : 0;
            // Only the poll thread writes these counters; no locked RMW is needed.
            bench.callbacks.store(bench.callbacks.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
            bench.bytes.store(bench.bytes.load(std::memory_order_relaxed) + pkt.data.size(),
                              std::memory_order_relaxed);
            bool transferred = false;
            if (afxdp::bench::reflect_swap(pkt.data)) [[likely]] {
                transferred = tx.send_in_place(pkt.addr,
                                               static_cast<std::uint32_t>(pkt.data.size()));
            } else {
                bench.unsupported.store(bench.unsupported.load(std::memory_order_relaxed) + 1,
                                        std::memory_order_relaxed);
            }
            if (transferred) {
                bench.staged.store(bench.staged.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_relaxed);
            }
            if (timed) samples.record(afxdp::bench::rdtsc() - t0);
            return transferred ? afxdp::FrameDisposition::Transferred
                               : afxdp::FrameDisposition::Recycle;
        };

        auto service_tx = [&tx]() noexcept { tx.publish_and_service_tx(); };

        const afxdp::ReceiverConfig recv_cfg{.cpu_affinity = worker_cpu,
                                             .realtime_sched = args.realtime,
                                             .batch_size = RX_BATCH_SIZE};

        afxdp::Receiver<ALLOC_CAP, decltype(strategy), decltype(service_tx)> receiver(
            xsk, alloc, umem, strategy, service_tx, recv_cfg);

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
            else {
                startup = afxdp::bench::calibrate_tsc();
                run.scheduler_policy = ::sched_getscheduler(0);
                sched_param scheduling{};
                if (::sched_getparam(0, &scheduling) == 0)
                    run.scheduler_priority = scheduling.sched_priority;
            }
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
        run.native_xdp = loader->native_mode();

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
        std::uint64_t last_rejected = 0;

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const auto now = Clock::now();
            if (!run.sampling_started && now - activated_at >= SAMPLE_WARMUP &&
                !g_stop.load(std::memory_order_relaxed)) {
                samples.start();
                run.sampling_started = true;
                run.sampling_start_after_ms =
                    std::chrono::duration<double, std::milli>(now - activated_at).count();
            }
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();

            // Each counter is race-free; the group is not one coherent snapshot.
            const auto rstats = receiver.stats();
            const auto tstats = tx.stats();
            std::println(
                "[stats cumulative] report_interval={}ms | rx_frames_processed={} | "
                "fill_refill_batches={} | "
                "tx_frames_submitted={} | tx_frames_reclaimed={} | tx_frames_rejected={} | "
                "callbacks={} | callback_bytes={} | tx_frames_staged={} | unsupported_frames={} | "
                "tx_retryable_wakeup_errors={} | tx_first_wakeup_errno={} | "
                "rx_first_wakeup_errno={}",
                elapsed_ms,
                rstats.packets_received,
                rstats.fill_refills,
                tstats.frames_submitted,
                tstats.frames_reclaimed,
                tstats.frames_rejected,
                bench.callbacks.load(std::memory_order_relaxed),
                bench.bytes.load(std::memory_order_relaxed),
                bench.staged.load(std::memory_order_relaxed),
                bench.unsupported.load(std::memory_order_relaxed),
                tstats.retryable_wakeup_errors,
                tstats.first_wakeup_error,
                rstats.first_wakeup_error);

            run.kernel_stats = xsk.kernel_stats();
            (void)print_kernel_stats(stdout, run.kernel_stats);
            if (!run.kernel_stats) {
                if (run.first_kernel_stats_errno == 0)
                    run.first_kernel_stats_errno = run.kernel_stats.error();
                exit_code = 1;
            }

            const std::uint64_t rejected_in_window = tstats.frames_rejected - last_rejected;
            if (rejected_in_window > TX_REJECTION_LIMIT) {
                std::println(stderr,
                             "[control] TX staging rejection anomaly ({} in {}ms) — shutting down",
                             rejected_in_window,
                             elapsed_ms);
                run.stop_reason = "tx_rejection_anomaly";
                exit_code = 1;
                g_stop.store(true, std::memory_order_relaxed);
            }
            last_rejected = tstats.frames_rejected;
            last_report = now;
        }

        std::println("[main] Stopping...");
        if (const auto result = loader->stop_redirect(); !result) {
            std::println(stderr,
                         "[main] Stopping redirection failed: {} (errno={})",
                         std::strerror(result.error()),
                         result.error());
            exit_code = 1;
            run.stop_redirect_errno = result.error();
        }
        poll_thread.request_stop();
        poll_thread.join();
        run.active_duration_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - activated_at).count();

        const auto remaining = tx.drain_until(Clock::now() + TX_DRAIN_TIMEOUT);
        run.tx_abandoned = remaining;
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

        const auto rstats = receiver.stats();
        const auto tstats = tx.stats();
        run.rx_processed = rstats.packets_received;
        run.fill_refill_batches = rstats.fill_refills;
        run.rx_first_wakeup_errno = rstats.first_wakeup_error;
        run.tx_submitted = tstats.frames_submitted;
        run.tx_reclaimed = tstats.frames_reclaimed;
        run.tx_rejected = tstats.frames_rejected;
        run.tx_retryable_wakeup_errors = tstats.retryable_wakeup_errors;
        run.tx_first_wakeup_errno = tstats.first_wakeup_error;
        run.tx_staged = bench.staged.load(std::memory_order_relaxed);
        run.callback_bytes = bench.bytes.load(std::memory_order_relaxed);
        run.callbacks = bench.callbacks.load(std::memory_order_relaxed);
        run.unsupported = bench.unsupported.load(std::memory_order_relaxed);
        run.kernel_stats = xsk.kernel_stats();
        if (!run.kernel_stats) {
            if (run.first_kernel_stats_errno == 0)
                run.first_kernel_stats_errno = run.kernel_stats.error();
            exit_code = 1;
        }
    }

    samples.report(calibration.ns_per_tick, "callback_tx_staging");
    run.samples_recorded = samples.count();
    if (const auto result = samples.dump_csv("callback_tx_staging_ns.csv", calibration.ns_per_tick);
        !result) {
        std::println(stderr,
                     "[export] callback_tx_staging_ns.csv failed: {} (errno={})",
                     std::strerror(result.error()),
                     result.error());
        exit_code = 1;
    } else {
        run.csv_exported = true;
    }
    if (const auto result = dump_run_metadata(args, run, calibration, exit_code); !result) {
        std::println(stderr,
                     "[export] run_metadata.txt failed: {} (errno={})",
                     std::strerror(result.error()),
                     result.error());
        exit_code = 1;
    }
    (void)print_kernel_stats(stdout, run.kernel_stats);
    std::println(
        "[main] Done. stop_reason={} exit_code={} rx_frames_processed={} tx_frames_staged={} "
        "tx_frames_submitted={} tx_frames_reclaimed={} tx_frames_rejected={} unsupported_frames={}",
        run.stop_reason,
        exit_code,
        run.rx_processed,
        run.tx_staged,
        run.tx_submitted,
        run.tx_reclaimed,
        run.tx_rejected,
        run.unsupported);
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
