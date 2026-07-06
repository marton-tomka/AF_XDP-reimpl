// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <linux/if_xdp.h>
#include <span>
#include <sys/mman.h>
#include <type_traits>

namespace afxdp {

enum class RingKind : std::uint8_t {
    Rx = 0,
    Tx = 1,
    Fill = 2,
    Completion = 3,
};

struct Range {
    std::uint32_t amount;
    std::uint32_t start;
};

template<typename DescT>
    requires std::is_trivially_copyable_v<DescT>
class MmapRing {
public:
    friend class Xsk;

    [[nodiscard]] static expect<MmapRing<DescT>> create(int fd,
                                                        std::uint64_t pgoff,
                                                        const xdp_ring_offset& ro,
                                                        std::uint32_t num_descs);

    MmapRing(const MmapRing&) = delete;
    MmapRing& operator=(const MmapRing&) = delete;

    MmapRing(MmapRing&& o) noexcept
        : map_base_(std::exchange(o.map_base_, nullptr))
        , map_size_(std::exchange(o.map_size_, 0))
        , producer_(std::exchange(o.producer_, nullptr))
        , consumer_(std::exchange(o.consumer_, nullptr))
        , descs_(std::exchange(o.descs_, nullptr))
        , flags_(std::exchange(o.flags_, nullptr))
        , num_descs_(std::exchange(o.num_descs_, 0))
        , mask_(std::exchange(o.mask_, 0))
        , cached_prod_(std::exchange(o.cached_prod_, 0))
        , cached_cons_(std::exchange(o.cached_cons_, 0)) {}
    MmapRing& operator=(MmapRing&& o) noexcept {
        if (this != &o) {
            unmap();
            map_base_ = std::exchange(o.map_base_, nullptr);
            map_size_ = std::exchange(o.map_size_, 0);
            producer_ = std::exchange(o.producer_, nullptr);
            consumer_ = std::exchange(o.consumer_, nullptr);
            descs_ = std::exchange(o.descs_, nullptr);
            flags_ = std::exchange(o.flags_, nullptr);
            num_descs_ = std::exchange(o.num_descs_, 0);
            mask_ = std::exchange(o.mask_, 0);
            cached_prod_ = std::exchange(o.cached_prod_, 0);
            cached_cons_ = std::exchange(o.cached_cons_, 0);
        }
        return *this;
    }

    ~MmapRing() { unmap(); }

    [[nodiscard]] DescT& desc_at(std::uint32_t i) noexcept { return descs_[i & mask_]; }
    [[nodiscard]] const DescT& desc_at(std::uint32_t i) const noexcept { return descs_[i & mask_]; }

    [[nodiscard]] bool need_wakeup() const noexcept {
        return (std::atomic_ref<std::uint32_t>(*flags_).load(std::memory_order_relaxed) &
                XDP_RING_NEED_WAKEUP) != 0;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return num_descs_; }

    // receiver side
    [[nodiscard]] Range peek(std::uint32_t batch_size) noexcept {
        std::uint32_t entries = cached_prod_ - cached_cons_;
        if (entries == 0) {
            cached_prod_ =
                std::atomic_ref<std::uint32_t>(*producer_).load(std::memory_order_acquire);
            entries = cached_prod_ - cached_cons_;
        }
        if (entries > batch_size) entries = batch_size;

        return Range{.amount = entries, .start = cached_cons_};
    }

    void advance_consumer(std::uint32_t amount) noexcept {
        cached_cons_ += amount;
        std::atomic_ref<std::uint32_t>(*consumer_).store(cached_cons_, std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t available() const noexcept {
        std::uint32_t prod =
            std::atomic_ref<std::uint32_t>(*producer_).load(std::memory_order_acquire);
        std::uint32_t cons =
            std::atomic_ref<std::uint32_t>(*consumer_).load(std::memory_order_acquire);
        return prod - cons;
    }

    // producer side
    [[nodiscard]] Range reserve(std::uint32_t n) noexcept {
        std::uint32_t free_n = num_descs_ - (cached_prod_ - cached_cons_);
        if (free_n < n) {
            cached_cons_ =
                std::atomic_ref<std::uint32_t>(*consumer_).load(std::memory_order_acquire);
            free_n = num_descs_ - (cached_prod_ - cached_cons_);
        }
        if (free_n < n) n = free_n;

        return Range{.amount = n, .start = cached_prod_};
    }

    void advance_producer(std::uint32_t amount) noexcept {
        cached_prod_ += amount;
        std::atomic_ref<std::uint32_t>(*producer_).store(cached_prod_, std::memory_order_release);
    }

private:
    MmapRing() = default;

    void unmap() noexcept {
        if (map_base_) {
            ::munmap(map_base_, map_size_);
            map_base_ = nullptr;
        }
    }

    void* map_base_ = nullptr;
    std::size_t map_size_ = 0;
    std::uint32_t* producer_ = nullptr;
    std::uint32_t* consumer_ = nullptr;
    DescT* descs_ = nullptr;
    std::uint32_t* flags_ = nullptr;
    std::uint32_t num_descs_ = 0;
    std::uint32_t mask_ = 0;
    std::uint32_t cached_prod_ = 0;
    std::uint32_t cached_cons_ = 0;
};

using RxRing = MmapRing<xdp_desc>;
using TxRing = MmapRing<xdp_desc>;
using FillRing = MmapRing<std::uint64_t>;
using CompletionRing = MmapRing<std::uint64_t>;

template<typename DescT>
    requires std::is_trivially_copyable_v<DescT>
inline expect<MmapRing<DescT>> MmapRing<DescT>::create(int fd,
                                                       std::uint64_t pgoff,
                                                       const xdp_ring_offset& ro,
                                                       std::uint32_t num_descs) {
    const std::size_t map_size = ro.desc + static_cast<std::uint64_t>(num_descs) * sizeof(DescT);

    void* base = ::mmap(nullptr,
                        map_size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        fd,
                        static_cast<off_t>(pgoff));
    if (base == MAP_FAILED) {
        return UNexpected(make_errno_error("mmap ring"));
    }

    // could've used std::byte* too but uint_8t is simpler
    auto* b = static_cast<std::uint8_t*>(base);

    MmapRing<DescT> ring;
    ring.map_base_ = base;
    ring.map_size_ = map_size;
    ring.producer_ = reinterpret_cast<std::uint32_t*>(b + ro.producer);
    ring.consumer_ = reinterpret_cast<std::uint32_t*>(b + ro.consumer);
    ring.descs_ = reinterpret_cast<DescT*>(b + ro.desc);
    ring.flags_ = reinterpret_cast<std::uint32_t*>(b + ro.flags);
    ring.num_descs_ = num_descs;
    ring.mask_ = num_descs - 1;
    return ring;
}

} // namespace afxdp
