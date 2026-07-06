// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <linux/if_xdp.h>
#include <span>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

namespace afxdp {

struct UmemConfig {
    std::uint32_t num_frames = 4096;
    std::uint32_t frame_size = 2048;
    std::uint32_t headroom = 0;
    bool use_huge_pages = true;

    [[nodiscard]] constexpr std::size_t total_size() const noexcept {
        return static_cast<std::size_t>(num_frames) * frame_size;
    }
};

class Umem {
public:
    [[nodiscard]] static expect<Umem> create(const UmemConfig& cfg);

    Umem(const Umem&) = delete;
    Umem& operator=(const Umem&) = delete;

    Umem(Umem&& o) noexcept
        : base_(std::exchange(o.base_, nullptr))
        , size_(std::exchange(o.size_, 0))
        , cfg_(o.cfg_) {}
    Umem& operator=(Umem&& o) noexcept {
        if (this != &o) {
            release();
            base_ = std::exchange(o.base_, nullptr);
            size_ = std::exchange(o.size_, 0);
            cfg_ = o.cfg_;
        }
        return *this;
    }

    ~Umem() { release(); }

    [[nodiscard]] void* base_ptr() const noexcept { return base_; }
    [[nodiscard]] std::size_t byte_size() const noexcept { return size_; }
    [[nodiscard]] const UmemConfig& config() const noexcept { return cfg_; }

private:
    Umem() = default;

    void release() noexcept {
        if (base_) {
            ::munmap(base_, size_);
            base_ = nullptr;
            size_ = 0;
        }
    }

    void* base_ = nullptr;
    std::size_t size_ = 0;
    UmemConfig cfg_{};
};

inline expect<Umem> Umem::create(const UmemConfig& cfg) {
    const struct rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
    if (::setrlimit(RLIMIT_MEMLOCK, &unlimited) != 0)
        return UNexpected(make_errno_error("setrlimit(RLIMIT_MEMLOCK)"));

    const std::size_t total = cfg.total_size();

    void* base = MAP_FAILED;
    bool used_huge = false;
    if (cfg.use_huge_pages) {
        base = ::mmap(nullptr,
                      total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                      -1, // no file descriptor
                      0   // no offset
        );
        used_huge = (base != MAP_FAILED);
    }

    if (base == MAP_FAILED) {
        base = ::mmap(nullptr,
                      total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                      -1,
                      0);
        if (base == MAP_FAILED) return UNexpected(make_errno_error("mmap(umem)"));
    }

    if (used_huge) {
        log(LogLevel::Info, "UMEM allocated with 2MB huge pages");
    } else if (cfg.use_huge_pages) {
        log(LogLevel::Warn, "UMEM allocated with standard 4KB pages (huge pages unavailable)");
    } else {
        log(LogLevel::Info, "UMEM allocated with standard 4KB pages (huge pages disabled)");
    }

    // technically redundant(?), the kernel should pin pages anyway
    if (::mlock(base, total) != 0) {
        ::munmap(base, total);
        return UNexpected(make_errno_error("mlock(umem)"));
    }

    Umem u;
    u.base_ = base;
    u.size_ = total;
    u.cfg_ = cfg;
    return u; // std::expected<Umem,AfxdpError> value state, move-constructs
}

} // namespace afxdp
