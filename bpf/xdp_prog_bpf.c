// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Marton Tomka

// xdp_prog.bpf.c
// Build: clang -O2 -g -target bpf -D__TARGET_ARCH_x86
//              -I/usr/include/x86_64-linux-gnu
//              -c xdp_prog.bpf.c -o xdp_prog.bpf.o

// clang-format off
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
// clang-format on

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(__u32));
} target_ip_map SEC(".maps");

struct vlan_hdr {
    __be16 h_vlan_TCI;
    __be16 h_vlan_encapsulated_proto;
};

#define VLAN_MAX_DEPTH 2

SEC("xdp")
int xdp_filter_ip(struct xdp_md* ctx) {
    void* data_end = (void*)(long)ctx->data_end;
    void* data = (void*)(long)ctx->data;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > data_end) return XDP_PASS;

    __u16 h_proto = eth->h_proto;
    void* cursor = (void*)(eth + 1);

#pragma unroll
    for (int i = 0; i < VLAN_MAX_DEPTH; i++) {
        if (h_proto != bpf_htons(ETH_P_8021Q) && h_proto != bpf_htons(ETH_P_8021AD)) break;

        struct vlan_hdr* vh = cursor;
        if ((void*)(vh + 1) > data_end) return XDP_PASS;

        h_proto = vh->h_vlan_encapsulated_proto;
        cursor = (void*)(vh + 1);
    }

    if (h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    struct iphdr* ip = cursor;
    if ((void*)(ip + 1) > data_end) return XDP_PASS;

    __u32 key = 0;
    __u32* target_ip = bpf_map_lookup_elem(&target_ip_map, &key);
    if (!target_ip) return XDP_PASS;

    if (ip->saddr != *target_ip) return XDP_PASS;

    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
