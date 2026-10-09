// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include "frame_alloc.hpp"
#include "umem.hpp"
#include "xsk.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <linux/if_xdp.h>
#include <limits>

namespace afxdp {

template<std::size_t CAPACITY>
    requires PowerOfTwo<CAPACITY>
class Transmitter {
public:
    Transmitter(Xsk& xsk, FrameAllocator<CAPACITY>& alloc, Umem& umem)
        : xsk_(xsk)
        , alloc_(alloc)
        , frame_size_(umem.config().frame_size)
        , frame_mask_(~(static_cast<std::uint64_t>(frame_size_) - 1)) {
        assert(frame_size_ != 0 && (frame_size_ & (frame_size_ - 1)) == 0 &&
               "UMEM frame_size must be a power of two (AF_XDP aligned mode)");
    }

    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;

    std::uint32_t reclaim_completed_frames(
        std::uint32_t budget = std::numeric_limits<std::uint32_t>::max()) noexcept {
        const auto [n, start] = xsk_.completion().readable_range(budget);
        if (n == 0) {
            return 0;
        }

        assert(n <= outstanding_);
        for (std::uint32_t i{}; i < n; ++i) [[likely]] {
            alloc_.free(xsk_.completion().desc_at(start + i) & frame_mask_);
        }

        xsk_.completion().release_consumed_entries(n);
        outstanding_ -= n;
        frames_reclaimed_.fetch_add(n, std::memory_order_relaxed);
        return n;
    }

    // Original single-buffer RX address/length from this UMEM; caller owns the frame uniquely.
    // Success transfers ownership to TX staging (not wire delivery); failure retains it.
    [[nodiscard]] bool send_in_place(std::uint64_t addr, std::uint32_t length) noexcept {
        if (length == 0 || length > frame_size_) [[unlikely]] {
            frames_rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!reserved_) {
            const auto [n, start] = xsk_.tx().writable_range(xsk_.tx().capacity());
            tx_avail_ = n;
            tx_start_ = start;
            tx_used_ = 0;
            reserved_ = true;
        }

        if (tx_used_ == tx_avail_) [[unlikely]] {
            tx_avail_ = xsk_.tx().writable_range(xsk_.tx().capacity()).amount;
            if (tx_used_ == tx_avail_) {
                frames_rejected_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        xsk_.tx().desc_at(tx_start_ +
                          tx_used_) = xdp_desc{.addr = addr, .len = length, .options = 0};
        ++tx_used_;
        return true;
    }

    // Bounded publication/CQ/wakeup pass, including idle retries; not a full TX drain.
    void publish_and_service_tx() noexcept {
        if (tx_used_ > 0) {
            outstanding_ += tx_used_;
            xsk_.tx().publish_written_entries(tx_used_);
            frames_submitted_.fetch_add(tx_used_, std::memory_order_relaxed);
        }
        tx_used_ = 0;
        reserved_ = false;

        if (outstanding_ == 0) return;

        // Free CQ slots before retrying TX; both reclamation passes share one ring-sized budget.
        const std::uint32_t budget = xsk_.completion().capacity();
        const std::uint32_t reaped = reclaim_completed_frames(budget);
        if (outstanding_ == 0) return;

        if (const auto result = xsk_.request_tx_progress(); !result) {
            const int error = result.error();
            if (is_expected_wakeup_retry(error) || error == ENOBUFS) {
                retryable_wakeup_errors_.store(
                    retryable_wakeup_errors_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            } else if (first_wakeup_error_.load(std::memory_order_relaxed) == 0) [[unlikely]] {
                first_wakeup_error_.store(error, std::memory_order_relaxed);
            }
        }

        // A failed kick may have made partial progress. Only CQ returns frames.
        if (reaped < budget) reclaim_completed_frames(budget - reaped);
    }

    // Sole-owner operation: the RX worker must be stopped/joined before control calls this.
    [[nodiscard]] std::uint32_t drain_until(std::chrono::steady_clock::time_point deadline) noexcept {
        do {
            publish_and_service_tx();
        } while (outstanding_ != 0 && std::chrono::steady_clock::now() < deadline);
        return outstanding_;
    }

    struct Stats {
        std::uint64_t frames_submitted = 0;
        std::uint64_t frames_reclaimed = 0;
        std::uint64_t frames_rejected = 0;
        std::uint64_t retryable_wakeup_errors = 0;
        int first_wakeup_error = 0;
    };

    [[nodiscard]] Stats stats() const noexcept {
        return {.frames_submitted = frames_submitted_.load(std::memory_order_relaxed),
                .frames_reclaimed = frames_reclaimed_.load(std::memory_order_relaxed),
                .frames_rejected = frames_rejected_.load(std::memory_order_relaxed),
                .retryable_wakeup_errors = retryable_wakeup_errors_.load(std::memory_order_relaxed),
                .first_wakeup_error = first_wakeup_error_.load(std::memory_order_relaxed)};
    }

private:
    Xsk& xsk_;
    FrameAllocator<CAPACITY>& alloc_;
    std::uint32_t frame_size_;
    std::uint64_t frame_mask_;

    std::uint32_t tx_start_ = 0;
    std::uint32_t tx_avail_ = 0;
    std::uint32_t tx_used_ = 0;
    bool reserved_ = false;
    // Published descriptors whose frame ownership has not yet returned through CQ.
    std::uint32_t outstanding_ = 0;

    alignas(CACHE_SIZE) std::atomic<std::uint64_t> frames_submitted_{0};
    std::atomic<std::uint64_t> frames_reclaimed_{0};
    std::atomic<std::uint64_t> frames_rejected_{0};
    std::atomic<std::uint64_t> retryable_wakeup_errors_{0};
    std::atomic<int> first_wakeup_error_{0};
};

} // namespace afxdp
