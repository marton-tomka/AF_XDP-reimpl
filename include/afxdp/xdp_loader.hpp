// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include "common.hpp"
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
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
        , ip_map_fd_(std::exchange(o.ip_map_fd_, -1)) {}
    XdpLoader& operator=(XdpLoader&& o) noexcept {
        if (this != &o) {
            detach();
            obj_ = std::exchange(o.obj_, nullptr);
            ifindex_ = std::exchange(o.ifindex_, 0);
            flags_ = std::exchange(o.flags_, 0);
            ip_map_fd_ = std::exchange(o.ip_map_fd_, -1);
        }
        return *this;
    }

    ~XdpLoader() { detach(); }

    [[nodiscard]] expect<void> update_filter_ip(std::string_view ip_str) noexcept;

private:
    XdpLoader() = default;

    void detach() noexcept {
        if (obj_ && ifindex_) {
            ::bpf_xdp_detach(static_cast<int>(ifindex_), flags_, nullptr);
            ifindex_ = 0;
        }
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

    if (::bpf_object__load(obj) != 0) {
        ::bpf_object__close(obj);
        return UNexpected(make_errno_error("bpf_object__load failed"));
    }

    bpf_map* xsk_map = ::bpf_object__find_map_by_name(obj, "xsks_map");
    if (!xsk_map) {
        ::bpf_object__close(obj);
        return UNexpected(make_logic_error("xsks_map not found in BPF object"));
    }
    const int xsk_map_fd = ::bpf_map__fd(xsk_map);

    {
        int key = static_cast<int>(queue_id);
        if (::bpf_map_update_elem(xsk_map_fd, &key, &xsk_fd, BPF_ANY) != 0) {
            ::bpf_object__close(obj);
            return UNexpected(make_errno_error("bpf_map_update_elem(xsks_map)"));
        }
    }

    bpf_map* ip_map = ::bpf_object__find_map_by_name(obj, "target_ip_map");
    if (!ip_map) {
        ::bpf_object__close(obj);
        return UNexpected(make_logic_error("target_ip_map not found in BPF object"));
    }
    const int ip_map_fd = ::bpf_map__fd(ip_map);

    {
        auto ip_result = parse_ipv4(filter_ip);
        if (!ip_result) {
            ::bpf_object__close(obj);
            return std::unexpected(ip_result.error());
        }
        std::uint32_t key = 0;
        std::uint32_t addr = *ip_result;
        if (::bpf_map_update_elem(ip_map_fd, &key, &addr, BPF_ANY) != 0) {
            ::bpf_object__close(obj);
            return UNexpected(make_errno_error("bpf_map_update_elem(target_ip_map)"));
        }
        log(LogLevel::Info, std::format("XDP filter: capturing packets from {}", filter_ip));
    }

    bpf_program* prog = ::bpf_object__find_program_by_name(obj, "xdp_filter_ip");
    if (!prog) {
        ::bpf_object__close(obj);
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
            ::bpf_object__close(obj);
            errno = -attach_rc;
            return UNexpected(make_errno_error("bpf_xdp_attach(SKB mode)"));
        }
        log(LogLevel::Info, "XDP attached in SKB (generic) mode");
    } else {
        log(LogLevel::Info, "XDP attached in DRV (native) mode");
    }

    XdpLoader loader;
    loader.obj_ = obj;
    loader.ifindex_ = ifindex;
    loader.flags_ = used_flags;
    loader.ip_map_fd_ = ip_map_fd;
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
