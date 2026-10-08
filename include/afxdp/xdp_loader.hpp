// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <cerrno>
#include <cstdint>
#include <linux/if_link.h>
#include <net/if.h>
#include <string>
#include <string_view>

namespace afxdp {

class XdpLoader {
public:
    [[nodiscard]] static expect<XdpLoader> create(std::string_view bpf_obj_path,
                                                  std::string_view iface,
                                                  std::uint32_t queue_id,
                                                  int xsk_fd,
                                                  std::string_view filter_ip);

    XdpLoader(const XdpLoader&) = delete;
    XdpLoader& operator=(const XdpLoader&) = delete;

    XdpLoader(XdpLoader&& o) noexcept
        : obj_(std::exchange(o.obj_, nullptr))
        , ifindex_(std::exchange(o.ifindex_, 0))
        , flags_(std::exchange(o.flags_, 0))
        , ip_map_fd_(std::exchange(o.ip_map_fd_, -1))
        , xsk_map_fd_(std::exchange(o.xsk_map_fd_, -1))
        , queue_id_(o.queue_id_)
        , redirecting_(std::exchange(o.redirecting_, false)) {}
    XdpLoader& operator=(XdpLoader&& o) noexcept {
        if (this != &o) {
            release();
            obj_ = std::exchange(o.obj_, nullptr);
            ifindex_ = std::exchange(o.ifindex_, 0);
            flags_ = std::exchange(o.flags_, 0);
            ip_map_fd_ = std::exchange(o.ip_map_fd_, -1);
            xsk_map_fd_ = std::exchange(o.xsk_map_fd_, -1);
            queue_id_ = o.queue_id_;
            redirecting_ = std::exchange(o.redirecting_, false);
        }
        return *this;
    }

    ~XdpLoader() { release(); }

    [[nodiscard]] expect<void> update_filter_ip(std::string_view ip_str) noexcept;

    [[nodiscard]] std::expected<void, int> stop_redirect() noexcept {
        if (!redirecting_) return {};
        if (::bpf_map_delete_elem(xsk_map_fd_, &queue_id_) != 0) {
            const int error = errno;
            if (error != ENOENT) return std::unexpected(error);
        }
        redirecting_ = false;
        return {};
    }

    [[nodiscard]] std::expected<void, int> detach() noexcept {
        if (ifindex_ == 0) return {};
        const int result = ::bpf_xdp_detach(static_cast<int>(ifindex_), flags_, nullptr);
        if (result != 0) return std::unexpected(-result);
        ifindex_ = 0;
        return {};
    }

private:
    XdpLoader() = default;

    void release() noexcept {
        static_cast<void>(stop_redirect());
        static_cast<void>(detach());
        if (obj_) {
            ::bpf_object__close(obj_);
            obj_ = nullptr;
        }
    }

    [[nodiscard]] static expect<std::uint32_t> parse_ipv4(std::string_view ip_str) noexcept {
        std::uint32_t addr = 0;
        const int rc = ::inet_pton(AF_INET, std::string(ip_str).c_str(), &addr);
        if (rc != 1) {
            return UNexpected(make_logic_error(std::format("invalid IPv4 address: '{}'", ip_str)));
        }
        return addr;
    }

    bpf_object* obj_ = nullptr;
    std::uint32_t ifindex_ = 0;
    std::uint32_t flags_ = 0;
    int ip_map_fd_ = -1;
    int xsk_map_fd_ = -1;
    std::uint32_t queue_id_ = 0;
    bool redirecting_ = false;
};

inline expect<XdpLoader> XdpLoader::create(std::string_view bpf_obj_path,
                                           std::string_view iface,
                                           std::uint32_t queue_id,
                                           int xsk_fd,
                                           std::string_view filter_ip) {
    const std::uint32_t ifindex = ::if_nametoindex(std::string(iface).c_str());
    if (ifindex == 0) {
        return UNexpected(make_errno_error(std::format("if_nametoindex('{}') failed", iface)));
    }

    bpf_object* obj = ::bpf_object__open(std::string(bpf_obj_path).c_str());
    if (!obj) {
        return UNexpected(
            make_errno_error(std::format("bpf_object__open('{}') failed", bpf_obj_path)));
    }

    XdpLoader loader;
    loader.obj_ = obj;
    loader.queue_id_ = queue_id;

    if (::bpf_object__load(obj) != 0) {
        return UNexpected(make_errno_error("bpf_object__load failed"));
    }

    bpf_map* xsk_map = ::bpf_object__find_map_by_name(obj, "xsks_map");
    if (!xsk_map) {
        return UNexpected(make_logic_error("xsks_map not found in BPF object"));
    }
    const int xsk_map_fd = ::bpf_map__fd(xsk_map);
    loader.xsk_map_fd_ = xsk_map_fd;

    {
        int key = static_cast<int>(queue_id);
        if (::bpf_map_update_elem(xsk_map_fd, &key, &xsk_fd, BPF_ANY) != 0) {
            return UNexpected(make_errno_error("bpf_map_update_elem(xsks_map)"));
        }
        loader.redirecting_ = true;
    }

    bpf_map* ip_map = ::bpf_object__find_map_by_name(obj, "target_ip_map");
    if (!ip_map) {
        return UNexpected(make_logic_error("target_ip_map not found in BPF object"));
    }
    const int ip_map_fd = ::bpf_map__fd(ip_map);
    loader.ip_map_fd_ = ip_map_fd;

    {
        auto ip_result = parse_ipv4(filter_ip);
        if (!ip_result) {
            return std::unexpected(ip_result.error());
        }
        std::uint32_t key = 0;
        std::uint32_t addr = *ip_result;
        if (::bpf_map_update_elem(ip_map_fd, &key, &addr, BPF_ANY) != 0) {
            return UNexpected(make_errno_error("bpf_map_update_elem(target_ip_map)"));
        }
        log(LogLevel::Info, std::format("XDP filter: capturing packets from {}", filter_ip));
    }

    bpf_program* prog = ::bpf_object__find_program_by_name(obj, "xdp_filter_ip");
    if (!prog) {
        return UNexpected(make_logic_error("xdp_filter_ip not found in BPF object"));
    }

    const int prog_fd = ::bpf_program__fd(prog);

    DECLARE_LIBBPF_OPTS(bpf_xdp_attach_opts, attach_opts);

    std::uint32_t used_flags = XDP_FLAGS_DRV_MODE;
    int attach_rc = ::bpf_xdp_attach(static_cast<int>(ifindex), prog_fd, used_flags, &attach_opts);

    if (attach_rc != 0) {
        // libbpf returns negative errno values; std::strerror expects positive.
        log(LogLevel::Warn,
            std::format("DRV mode failed ({}), falling back to SKB mode",
                        std::strerror(-attach_rc)));

        used_flags = XDP_FLAGS_SKB_MODE;
        attach_rc = ::bpf_xdp_attach(static_cast<int>(ifindex), prog_fd, used_flags, &attach_opts);
        if (attach_rc != 0) {
            errno = -attach_rc;
            return UNexpected(make_errno_error("bpf_xdp_attach(SKB mode)"));
        }
    }

    loader.ifindex_ = ifindex;
    loader.flags_ = used_flags;
    log(LogLevel::Info,
        used_flags == XDP_FLAGS_DRV_MODE ? "XDP attached in DRV (native) mode"
                                         : "XDP attached in SKB (generic) mode");
    return loader;
}

inline expect<void> XdpLoader::update_filter_ip(std::string_view ip_str) noexcept {
    auto ip_result = parse_ipv4(ip_str);
    if (!ip_result) {
        return std::unexpected(ip_result.error());
    }

    std::uint32_t key = 0;
    std::uint32_t addr = *ip_result;

    if (::bpf_map_update_elem(ip_map_fd_, &key, &addr, BPF_ANY) != 0) {
        return UNexpected(make_errno_error("update_filter_ip: bpf_map_update_elem"));
    }

    log(LogLevel::Info, std::format("XDP filter updated to {}", ip_str));
    return {};
}

} // namespace afxdp
