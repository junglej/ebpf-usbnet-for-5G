/* SPDX-License-Identifier: GPL-2.0 */
/*
 * txdwell.h - shared definitions between txdwell.bpf.c (BPF) and txdwell.c
 * (userspace loader).
 */
#ifndef __TXDWELL_H
#define __TXDWELL_H

#define EV_ENQ 1
#define EV_DEQ 2

/* indices into the stats array map */
enum stat_idx {
	ST_ENQ_TOTAL = 0,	/* usbnet_start_xmit hits that passed the filter */
	ST_DEQ_TOTAL,		/* tx_complete hits matched to an enqueue */
	ST_MISS,		/* tx_complete with no matching enqueue record */
	ST_INSERT_FAIL,		/* skb_map insert failures (map full) */
	ST_RB_LOST,		/* ringbuf reserve failures (events dropped) */
	ST_INFLIGHT_PKTS,	/* current driver-level queue depth, packets */
	ST_INFLIGHT_BYTES,	/* current driver-level queue depth, bytes */
	ST_MAX,
};

/* config_map value (single entry, set by userspace before attach) */
struct txdwell_config {
	__u32 ifindex;	/* 0 = observe all usbnet interfaces */
	__u32 emit_enq;	/* 0 = emit DEQ events only (default), 1 = also ENQ */
};

/* skb_map value: per-skb enqueue record, keyed by skb pointer */
struct skb_info {
	__u64 ts_ns;	/* bpf_ktime_get_ns() at usbnet_start_xmit entry */
	__u32 len;	/* skb->len at enqueue */
	__u32 ifindex;
};

/* ringbuf event (CSV row in userspace) */
struct event {
	__u64 ts_ns;	/* ENQ: enqueue time; DEQ: completion time */
	__u64 dwell_ns;	/* DEQ: completion - enqueue; ENQ: 0 */
	__s64 inflight;	/* driver queue depth (pkts) after this event */
	__u32 len;	/* skb->len */
	__u32 ifindex;
	__u8  type;	/* EV_ENQ / EV_DEQ */
	__u8  pad[7];
};

#endif /* __TXDWELL_H */
