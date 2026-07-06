// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include "frame_alloc.hpp"
#include "umem.hpp"
#include "xsk.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <linux/if_xdp.h>
#include <optional>
#include <span>

namespace afxdp {

template<std::size_t CAPACITY>
    requires PowerOfTwo<CAPACITY>
class Transmitter {
public:
    Transmitter(Xsk& xsk, FrameAllocator<CAPACITY>& alloc, Umem& umem)
        : xsk_(xsk)
        , alloc_(alloc)
        , umem_base_(umem.base_ptr())
        , frame_size_(umem.config().frame_size) {
        assert(frame_size_ != 0 && (frame_size_ & (frame_size_ - 1)) == 0 &&
               "UMEM frame_size must be a power of two (AF_XDP aligned mode)");
    }

    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;

    std::uint32_t reap_completions() noexcept {
        const auto [n, start] = xsk_.completion().peek(xsk_.completion().capacity());
        if (n == 0) {
            return 0;
        }

        for (std::uint32_t i{}; i < n; ++i) [[likely]] {
            alloc_.free(xsk_.completion().desc_at(start + i));
        }

        xsk_.completion().advance_consumer(n);
        stats_.completions += n;
        return n;
    }

    [[nodiscard]] bool send(std::span<const std::byte> payload) noexcept {
        if (payload.size() > frame_size_) [[unlikely]] {
            ++stats_.drops;
            return false;
        }

        if (!reserved_) {
            const auto [n, start] = xsk_.tx().reserve(xsk_.tx().capacity());
            tx_avail_ = n;
            tx_start_ = start;
            tx_used_ = 0;
            reserved_ = true;
        }
        if (tx_used_ == tx_avail_) [[unlikely]] {
            tx_avail_ = xsk_.tx().reserve(xsk_.tx().capacity()).amount;
            if (tx_used_ == tx_avail_) {
                ++stats_.drops;
                return false;
            }
        }

        std::optional<std::uint64_t> frame = alloc_.alloc();
        if (!frame) [[unlikely]] {
            reap_completions();
            frame = alloc_.alloc();
            if (!frame) {
                ++stats_.drops;
                return false;
            }
        }

        const std::uint64_t addr = *frame;
        std::memcpy(static_cast<std::byte*>(umem_base_) + addr, payload.data(), payload.size());
        xsk_.tx().desc_at(tx_start_ + tx_used_) =
            xdp_desc{.addr = addr, .len = static_cast<std::uint32_t>(payload.size()), .options = 0};
        ++tx_used_;
        return true;
    }

    void flush() noexcept {
        if (tx_used_ > 0) {
            xsk_.tx().advance_producer(tx_used_);
            xsk_.kick_tx();
            stats_.packets_sent += tx_used_;
        }
        tx_used_ = 0;
        reserved_ = false;
        reap_completions();
    }

    struct Stats {
        std::uint64_t packets_sent = 0;
        std::uint64_t completions = 0;
        std::uint64_t drops = 0;
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    Xsk& xsk_;
    FrameAllocator<CAPACITY>& alloc_;
    void* const umem_base_;
    std::uint32_t frame_size_;

    std::uint32_t tx_start_ = 0;
    std::uint32_t tx_avail_ = 0;
    std::uint32_t tx_used_ = 0;
    bool reserved_ = false;

    alignas(CACHE_SIZE) Stats stats_{};
};

} // namespace afxdp
