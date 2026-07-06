// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include "frame_alloc.hpp"
#include "ring.hpp"
#include "umem.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace afxdp {

struct XskConfig {
    std::string iface;
    std::uint32_t queue_id = 0;
    std::uint32_t ring_size = 2048;
    bool zero_copy = true;
    bool need_wakeup = true;
    bool busy_poll = false;
    std::uint32_t busy_poll_budget = 64;
    std::uint32_t busy_poll_timeout_us = 20;
};

class Xsk {
public:
    [[nodiscard]] static expect<Xsk> create(const XskConfig& cfg, Umem& umem);

    Xsk(const Xsk&) = delete;
    Xsk& operator=(const Xsk&) = delete;
    Xsk(Xsk&&) noexcept = default;
    Xsk& operator=(Xsk&&) noexcept = default;

    ~Xsk() = default;

    [[nodiscard]] RxRing& rx() noexcept { return rx_; }
    [[nodiscard]] FillRing& fill() noexcept { return fill_; }
    [[nodiscard]] TxRing& tx() noexcept { return tx_; }
    [[nodiscard]] CompletionRing& completion() noexcept { return completion_; }

    [[nodiscard]] int raw_desc() const noexcept { return fd_.get(); }
    [[nodiscard]] bool busy_poll() const noexcept { return busy_poll_; }

    void busy_poll_rx() const noexcept {
        ::recvfrom(fd_.get(), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    }

    void kick_fill() const noexcept {
        if (busy_poll_ || fill_.need_wakeup()) {
            ::recvfrom(fd_.get(), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        }
    }

    void kick_tx() const noexcept {
        if (busy_poll_ || tx_.need_wakeup()) {
            ::sendto(fd_.get(), nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        }
    }

    template<std::size_t CAPACITY>
    std::uint32_t refill_fill(FrameAllocator<CAPACITY>& alloc) noexcept {
        const auto [n, start] = fill_.reserve(fill_.capacity());

        std::uint32_t pushed = 0;
        while (pushed < n) {
            std::optional<uint64_t> frame = alloc.alloc();
            if (!frame) {
                break;
            }
            fill_.desc_at(start + pushed++) = *frame;
        }

        if (pushed > 0) {
            fill_.advance_producer(pushed);
            kick_fill();
        }

        return pushed;
    }

private:
    Xsk() = default;

    FileDesc fd_{};
    RxRing rx_{};
    TxRing tx_{};
    FillRing fill_{};
    CompletionRing completion_{};
    bool busy_poll_ = false;
};

inline expect<Xsk> Xsk::create(const XskConfig& cfg, Umem& umem) {
    int raw_fd = ::socket(AF_XDP, SOCK_RAW, 0);
    if (raw_fd < 0) {
        return UNexpected(make_errno_error("socket(AF_XDP)"));
    }
    FileDesc fd{raw_fd};

    const std::uint32_t ring_sz = cfg.ring_size;
    auto set_ring_size = [&](int opt, std::string_view name) -> expect<void> {
        if (::setsockopt(fd.get(), SOL_XDP, opt, &ring_sz, sizeof(ring_sz)) != 0) {
            return UNexpected(make_errno_error(std::string(name)));
        }
        return {};
    };

    if (auto r = set_ring_size(XDP_RX_RING, "XDP_RX_RING"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = set_ring_size(XDP_TX_RING, "XDP_TX_RING"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = set_ring_size(XDP_UMEM_FILL_RING, "XDP_UMEM_FILL_RING"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = set_ring_size(XDP_UMEM_COMPLETION_RING, "XDP_UMEM_COMPLETION_RING"); !r) {
        return std::unexpected(r.error());
    }

    {
        const UmemConfig& ucfg = umem.config();
        xdp_umem_reg reg{};
        reg.addr = reinterpret_cast<std::uint64_t>(umem.base_ptr());
        reg.len = umem.byte_size();
        reg.chunk_size = ucfg.frame_size;
        reg.headroom = ucfg.headroom;
        reg.flags = 0;

        if (::setsockopt(fd.get(), SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)) != 0) {
            return UNexpected(make_errno_error("setsockopt(XDP_UMEM_REG)"));
        }
    }

    xdp_mmap_offsets offsets{};
    socklen_t offlen = sizeof(offsets);
    if (::getsockopt(fd.get(), SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &offlen) != 0) {
        return UNexpected(make_errno_error("getsockopt(XDP_MMAP_OFFSETS)"));
    }

    auto rx = MmapRing<xdp_desc>::create(fd.get(), XDP_PGOFF_RX_RING, offsets.rx, ring_sz);
    if (!rx) {
        return std::unexpected(rx.error());
    }

    auto tx = MmapRing<xdp_desc>::create(fd.get(), XDP_PGOFF_TX_RING, offsets.tx, ring_sz);
    if (!tx) {
        return std::unexpected(tx.error());
    }

    auto fill =
        MmapRing<std::uint64_t>::create(fd.get(), XDP_UMEM_PGOFF_FILL_RING, offsets.fr, ring_sz);
    if (!fill) {
        return std::unexpected(fill.error());
    }

    auto comp = MmapRing<std::uint64_t>::create(
        fd.get(), XDP_UMEM_PGOFF_COMPLETION_RING, offsets.cr, ring_sz);
    if (!comp) {
        return std::unexpected(comp.error());
    }

    const std::uint32_t ifindex = ::if_nametoindex(cfg.iface.c_str());
    if (ifindex == 0) {
        return UNexpected(make_errno_error("if_nametoindex: unknown interface"));
    }

    sockaddr_xdp sxdp{};
    sxdp.sxdp_family = AF_XDP;
    sxdp.sxdp_ifindex = ifindex;
    sxdp.sxdp_queue_id = cfg.queue_id;
    sxdp.sxdp_flags = 0;
    if (cfg.need_wakeup) sxdp.sxdp_flags |= XDP_USE_NEED_WAKEUP;

    bool bound = false;
    if (cfg.zero_copy) {
        sxdp.sxdp_flags |= XDP_ZEROCOPY;
        if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sxdp), sizeof(sxdp)) == 0) {
            log(LogLevel::Info, "ZEROCOPY");
            bound = true;
        } else {
            sxdp.sxdp_flags &= ~static_cast<std::uint16_t>(XDP_ZEROCOPY);
        }
    }
    if (!bound) {
        sxdp.sxdp_flags |= XDP_COPY;
        if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sxdp), sizeof(sxdp)) != 0) {
            return UNexpected(make_errno_error("bind(AF_XDP, XDP_COPY)"));
        }
        log(LogLevel::Info, "COPY");
    }

    if (cfg.busy_poll) {
        auto set_opt = [&](int opt, int val, std::string_view name) -> expect<void> {
            if (::setsockopt(fd.get(), SOL_SOCKET, opt, &val, sizeof(val)) != 0) {
                return UNexpected(make_errno_error(std::string(name)));
            }
            return {};
        };

        if (auto r = set_opt(SO_PREFER_BUSY_POLL, 1, "SO_PREFER_BUSY_POLL"); !r) {
            return std::unexpected(r.error());
        }
        if (auto r =
                set_opt(SO_BUSY_POLL, static_cast<int>(cfg.busy_poll_timeout_us), "SO_BUSY_POLL");
            !r) {
            return std::unexpected(r.error());
        }
        if (auto r = set_opt(
                SO_BUSY_POLL_BUDGET, static_cast<int>(cfg.busy_poll_budget), "SO_BUSY_POLL_BUDGET");
            !r) {
            return std::unexpected(r.error());
        }

        log(LogLevel::Info,
            std::format("busy-poll NAPI enabled (budget={}, timeout={}us)",
                        cfg.busy_poll_budget,
                        cfg.busy_poll_timeout_us));
    }

    Xsk xsk;
    xsk.fd_ = std::move(fd);
    xsk.rx_ = std::move(*rx);
    xsk.tx_ = std::move(*tx);
    xsk.fill_ = std::move(*fill);
    xsk.completion_ = std::move(*comp);
    xsk.busy_poll_ = cfg.busy_poll;

    return xsk;
}

} // namespace afxdp
