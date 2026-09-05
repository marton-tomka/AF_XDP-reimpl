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
#include <format>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <stop_token>
#include <sys/socket.h>
#include <type_traits>

namespace afxdp {

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

    void run(std::stop_token st) {
        if (cfg_.cpu_affinity >= 0) {
            pin_to_cpu(cfg_.cpu_affinity);
        }

        if (cfg_.realtime_sched) {
            set_realtime();
        }

        while (!st.stop_requested()) {
            const std::uint32_t processed = poll_rx();
            if (processed == 0) {
                if (!busy_poll_) {
                    __builtin_ia32_pause();
                }
            } else {
                std::atomic_ref<std::uint64_t>(stats_.packets_received)
                    .fetch_add(processed, std::memory_order_relaxed);
            }
        }
    }

    struct Stats {
        std::uint64_t packets_received = 0;
        std::uint64_t fill_refills = 0;
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] std::uint32_t poll_rx() noexcept {
        const auto [n, start] = xsk_.rx().peek(static_cast<std::uint32_t>(cfg_.batch_size));

        if (n == 0) [[unlikely]] {
            if (busy_poll_) {
                xsk_.busy_poll_rx();
            }
            return 0;
        }

        for (std::uint32_t i{}; i < n; ++i) [[likely]] {
            const xdp_desc& desc = xsk_.rx().desc_at(start + i);

            PacketView pv{.data = std::span<std::byte>(
                              static_cast<std::byte*>(umem_base_) + desc.addr, desc.len),
                          .addr = desc.addr};

            if (callback_(pv) == FrameDisposition::Recycle) {
                alloc_.free(desc.addr & frame_mask_);
            }
        }

        xsk_.rx().advance_consumer(n);

        commit_tx_();

        if (xsk_.fill().available() < fill_threshold_) {
            const std::uint32_t pushed = xsk_.refill_fill(alloc_);
            if (pushed > 0) {
                std::atomic_ref<std::uint64_t>(stats_.fill_refills)
                    .fetch_add(1, std::memory_order_relaxed);
            }
        }

        return n;
    }

    static void pin_to_cpu(int cpu) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<std::size_t>(cpu), &cpuset);

        const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
        if (rc != 0) {
            log(LogLevel::Warn,
                std::format("pthread_setaffinity_np failed: {}", std::strerror(rc)));
        }
    }

    static void set_realtime() noexcept {
        struct sched_param param{};
        param.sched_priority = 99;
        if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
            log(LogLevel::Warn,
                std::format("sched_setscheduler(SCHED_FIFO) failed: {}", std::strerror(errno)));
        }
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

    alignas(CACHE_SIZE) Stats stats_{};
};

} // namespace afxdp
