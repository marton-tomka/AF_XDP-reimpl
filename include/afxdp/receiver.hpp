// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include "frame_alloc.hpp"
#include "xsk.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <stop_token>
#include <sys/socket.h>

namespace afxdp {

struct PacketView {
    std::span<const std::byte> data;

    template<typename T>
    [[nodiscard]] const T* as() const noexcept {
        if (data.size_bytes() < sizeof(T)) return nullptr;
        assert(reinterpret_cast<std::uintptr_t>(data.data()) % alignof(T) == 0 &&
               "PacketView::as<T>() on misaligned storage");
        return reinterpret_cast<const T*>(data.data());
    }
};

struct ReceiverConfig {
    int cpu_affinity = -1;
    bool realtime_sched = false;
    int batch_size = 64;
    std::uint32_t fill_refill_threshold = 0;
};

template<std::size_t CAPACITY, typename Callback, typename CommitTX>
    requires PowerOfTwo<CAPACITY> && std::invocable<Callback, const PacketView&> &&
             std::invocable<CommitTX>
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

        stats_ = {};
        while (!st.stop_requested()) {
            const std::uint32_t processed = poll_rx();
            if (processed == 0) {
                if (!busy_poll_) {
                    __builtin_ia32_pause();
                }
            } else {
                stats_.packets_received += processed;
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

            const PacketView pv{
                .data = std::span<const std::byte>(
                    static_cast<const std::byte*>(umem_base_) + desc.addr, desc.len)};

            callback_(pv);

            alloc_.free(desc.addr & frame_mask_);
        }

        xsk_.rx().advance_consumer(n);

        if (xsk_.fill().available() < fill_threshold_) {
            const std::uint32_t pushed = xsk_.refill_fill(alloc_);
            stats_.fill_refills += (pushed > 0 ? 1 : 0);
        }

        // if busy polling, the transmitter side never wakes under heavy load, make sure it does
        commit_tx_();

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
