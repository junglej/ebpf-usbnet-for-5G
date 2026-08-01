// SPDX-License-Identifier: GPL-2.0
/*
 * txdwell.bpf.c - usbnet TX queue dwell-time / queue-length observer.
 *
 * Hook points (Linux 6.8, drivers/net/usb/usbnet.c):
 *   enqueue = usbnet_start_xmit(struct sk_buff *skb, struct net_device *net)
 *             -> ndo_start_xmit of every usbnet minidriver (qmi_wwan sets
 *                .ndo_start_xmit = usbnet_start_xmit and has no tx_fixup
 *                in 6.8, so the skb pointer seen here is exactly the one
 *                handed to the URB).
 *   dequeue = tx_complete(struct urb *urb)
 *             -> URB completion callback; urb->context is the skb
 *                (usb_fill_bulk_urb(..., tx_complete, skb)).
 *
 * dwell time    = tx_complete entry - usbnet_start_xmit entry (per skb ptr)
 * queue length  = in-flight skb count (enqueue increments, completion dec.)
 *
 * Both functions live in the usbnet module. Userspace attaches these
 * programs BY ADDRESS via kprobe_multi, because "tx_complete" is a generic
 * name that exists in other modules too.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include "txdwell.h"

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct txdwell_config);
} config_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, __u64);	/* skb pointer */
	__type(value, struct skb_info);
} skb_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, ST_MAX);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

/* atomically add delta to stats[idx], return value after the add */
static __always_inline __u64 stat_add(__u32 idx, __s64 delta)
{
	__u32 key = idx;
	__u64 *v = bpf_map_lookup_elem(&stats, &key);

	if (!v)
		return 0;
	return __sync_fetch_and_add(v, delta) + delta;
}

SEC("kprobe.multi/usbnet_start_xmit")
int BPF_KPROBE(enq_kprobe, struct sk_buff *skb, struct net_device *net)
{
	struct txdwell_config *cfg;
	struct skb_info info = {};
	struct event *e;
	__u32 ckey = 0, ifindex;
	__u64 key, inflight;

	cfg = bpf_map_lookup_elem(&config_map, &ckey);
	if (!cfg)
		return 0;

	if (!skb || !net)
		return 0;

	ifindex = BPF_CORE_READ(net, ifindex);
	if (cfg->ifindex && ifindex != cfg->ifindex)
		return 0;

	info.ts_ns = bpf_ktime_get_ns();
	info.len = BPF_CORE_READ(skb, len);
	info.ifindex = ifindex;

	key = (__u64)skb;
	if (bpf_map_update_elem(&skb_map, &key, &info, BPF_ANY)) {
		stat_add(ST_INSERT_FAIL, 1);
		return 0;
	}

	stat_add(ST_ENQ_TOTAL, 1);
	inflight = stat_add(ST_INFLIGHT_PKTS, 1);
	stat_add(ST_INFLIGHT_BYTES, (__s64)info.len);

	if (cfg->emit_enq) {
		e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
		if (!e) {
			stat_add(ST_RB_LOST, 1);
			return 0;
		}
		e->type = EV_ENQ;
		e->ts_ns = info.ts_ns;
		e->dwell_ns = 0;
		e->inflight = (__s64)inflight;
		e->len = info.len;
		e->ifindex = ifindex;
		__builtin_memset(e->pad, 0, sizeof(e->pad));
		bpf_ringbuf_submit(e, 0);
	}
	return 0;
}

SEC("kprobe.multi/tx_complete")
int BPF_KPROBE(deq_kprobe, struct urb *urb)
{
	struct sk_buff *skb;
	struct skb_info *info;
	struct event *e;
	__u64 key, ts_enq, now;
	__u32 len, ifindex;
	__s64 inflight;

	if (!urb)
		return 0;

	skb = BPF_CORE_READ(urb, context);
	if (!skb)
		return 0;

	key = (__u64)skb;
	info = bpf_map_lookup_elem(&skb_map, &key);
	if (!info) {
		/* in flight before we attached / filtered out / (on drivers
		 * with tx_fixup) the skb was replaced after our enqueue probe
		 */
		stat_add(ST_MISS, 1);
		return 0;
	}
	ts_enq = info->ts_ns;
	len = info->len;
	ifindex = info->ifindex;
	bpf_map_delete_elem(&skb_map, &key);

	now = bpf_ktime_get_ns();

	inflight = (__s64)stat_add(ST_INFLIGHT_PKTS, -1);
	stat_add(ST_INFLIGHT_BYTES, -(__s64)len);
	stat_add(ST_DEQ_TOTAL, 1);

	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e) {
		stat_add(ST_RB_LOST, 1);
		return 0;
	}
	e->type = EV_DEQ;
	e->ts_ns = now;
	e->dwell_ns = now > ts_enq ? now - ts_enq : 0;
	e->inflight = inflight;
	e->len = len;
	e->ifindex = ifindex;
	__builtin_memset(e->pad, 0, sizeof(e->pad));
	bpf_ringbuf_submit(e, 0);
	return 0;
}
