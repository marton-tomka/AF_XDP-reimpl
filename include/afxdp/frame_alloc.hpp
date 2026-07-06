// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>

namespace afxdp {

template<std::size_t CAPACITY>
    requires PowerOfTwo<CAPACITY>
class FrameAllocator {
public:
    static constexpr std::size_t capacity = CAPACITY;

    explicit FrameAllocator(std::uint32_t num_frames, std::uint32_t frame_size) noexcept
        : count_(num_frames) {
        assert(num_frames <= capacity && "num_frames exceeds allocator capacity");
        for (std::uint32_t i{}; i < num_frames; ++i)
            frames_[i] = static_cast<std::uint64_t>(i) * frame_size;
    }

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;
    FrameAllocator(FrameAllocator&&) = delete;
    FrameAllocator& operator=(FrameAllocator&&) = delete;

    std::optional<std::uint64_t> alloc() noexcept {
        if (count_ == 0) {
            return std::nullopt;
        }
        return frames_[--count_];
    }

    void free(std::uint64_t offset) noexcept {
        assert(count_ < capacity);
        frames_[count_++] = offset;
    }

    [[nodiscard]] std::uint32_t free_count() const noexcept { return count_; }

private:
    std::array<std::uint64_t, capacity> frames_{};
    std::uint32_t count_ = 0;
};

} // namespace afxdp
