// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include "frame_alloc.hpp"
#include "xsk.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <stop_token>
#include <sys/socket.h>
#include <type_traits>

namespace afxdp {

// Borrowed single-buffer RX frame. No access after Transferred; no retention after Recycle.
// The socket uses aligned UMEM chunks, no scatter/gather, and trusted kernel RX descriptors.
struct PacketView {
    std::span<std::byte> data;
    std::uint64_t addr;

    template<typename T>
    [[nodiscard]] T* as() const noexcept {
        if (data.size_bytes() < sizeof(T)) return nullptr;
        assert(reinterpret_cast<std::uintptr_t>(data.data()) % alignof(T) == 0 &&
               "PacketView::as<T>() on misaligned storage");
        return reinterpret_cast<T*>(data.data());
    }
};

enum class FrameDisposition : std::uint8_t {
    Recycle,
    Transferred,
};

struct ReceiverConfig {
    int cpu_affinity = -1;
    bool realtime_sched = false;
    int batch_size = 64;
    std::uint32_t fill_refill_threshold = 0;
};

template<std::size_t CAPACITY, typename Callback, typename CommitTX>
    requires PowerOfTwo<CAPACITY> && std::invocable<Callback&, PacketView&> &&
             std::same_as<std::invoke_result_t<Callback&, PacketView&>, FrameDisposition> &&
             std::invocable<CommitTX&>
class Receiver {
public:
    Receiver(Xsk& xsk,
             FrameAllocator<CAPACITY>& alloc,
             Umem& umem,
             Callback callback,
             CommitTX commit_tx,
             const ReceiverConfig& cfg = {})
        : xsk_(xsk)
        , alloc_(alloc)
        , callback_(std::move(callback))
        , commit_tx_(std::move(commit_tx))
        , cfg_(cfg)
        , umem_base_(umem.base_ptr())
        , fill_threshold_(cfg.fill_refill_threshold > 0 ? cfg.fill_refill_threshold
                                                        : xsk.fill().capacity() / 2)
        , busy_poll_(xsk.busy_poll())
        , frame_mask_(~(static_cast<std::uint64_t>(umem.config().frame_size) - 1)) {}

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    [[nodiscard]] std::expected<void, int> prepare_thread() const noexcept {
        if (cfg_.cpu_affinity >= 0) {
            if (auto result = pin_to_cpu(cfg_.cpu_affinity); !result) return result;
        }

        if (cfg_.realtime_sched) {
            if (auto result = set_realtime(); !result) return result;
        }
        return {};
    }

    // Call prepare_thread() on this thread before activating packet redirection.
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            const std::uint32_t processed = process_rx_and_maintain_io();
            if (processed == 0) {
                if (!busy_poll_) {
                    __builtin_ia32_pause();
                }
            } else {
                packets_received_.fetch_add(processed, std::memory_order_relaxed);
            }
        }
    }

    struct Stats {
        std::uint64_t packets_received = 0;
        std::uint64_t fill_refills = 0;
        int first_wakeup_error = 0;
    };

    [[nodiscard]] Stats stats() const noexcept {
        return {.packets_received = packets_received_.load(std::memory_order_relaxed),
                .fill_refills = fill_refills_.load(std::memory_order_relaxed),
                .first_wakeup_error = first_wakeup_error_.load(std::memory_order_relaxed)};
    }

private:
    [[nodiscard]] std::uint32_t process_rx_and_maintain_io() noexcept {
        const auto [n, start] =
            xsk_.rx().readable_range(static_cast<std::uint32_t>(cfg_.batch_size));

        for (std::uint32_t i{}; i < n; ++i) [[likely]] {
            const xdp_desc& desc = xsk_.rx().desc_at(start + i);

            PacketView pv{.data = std::span<std::byte>(
                              static_cast<std::byte*>(umem_base_) + desc.addr, desc.len),
                          .addr = desc.addr};

            if (callback_(pv) == FrameDisposition::Recycle) {
                alloc_.free(desc.addr & frame_mask_);
            }
        }

        if (n > 0) xsk_.rx().release_consumed_entries(n);

        commit_tx_();

        std::uint32_t pushed = 0;
        if (xsk_.fill().queued_entry_count() < fill_threshold_) {
            pushed = xsk_.refill_fill(alloc_);
            if (pushed > 0) {
                fill_refills_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Wakeups can become necessary after the last refill or RX batch.
        if (n == 0 || pushed > 0 || xsk_.fill().need_wakeup()) {
            if (const auto result = xsk_.request_rx_progress(); !result) {
                const int error = result.error();
                if (!is_expected_wakeup_retry(error) &&
                    first_wakeup_error_.load(std::memory_order_relaxed) == 0) [[unlikely]] {
                    first_wakeup_error_.store(error, std::memory_order_relaxed);
                }
            }
        }

        return n;
    }

    [[nodiscard]] static std::expected<void, int> pin_to_cpu(int cpu) noexcept {
        if (cpu >= CPU_SETSIZE) return std::unexpected(EINVAL);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<std::size_t>(cpu), &cpuset);

        const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
        if (rc != 0) return std::unexpected(rc);
        return {};
    }

    [[nodiscard]] static std::expected<void, int> set_realtime() noexcept {
        struct sched_param param{};
        param.sched_priority = 99;
        if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) return std::unexpected(errno);
        return {};
    }

    Xsk& xsk_;
    FrameAllocator<CAPACITY>& alloc_;
    Callback callback_;
    CommitTX commit_tx_;
    ReceiverConfig cfg_;
    void* const umem_base_;
    std::uint32_t fill_threshold_ = 0;
    bool busy_poll_ = false;
    std::uint64_t frame_mask_;

    alignas(CACHE_SIZE) std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> fill_refills_{0};
    std::atomic<int> first_wakeup_error_{0};
};

} // namespace afxdp
