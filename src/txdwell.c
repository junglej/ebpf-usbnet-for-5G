// SPDX-License-Identifier: GPL-2.0
/*
 * txdwell.c - userspace loader/reader for the txdwell observer.
 *
 * Resolves usbnet_start_xmit / tx_complete in /proc/kallsyms (restricted to
 * the [usbnet] module, because "tx_complete" exists in other modules too),
 * then attaches the BPF kprobes BY ADDRESS via kprobe_multi.
 *
 * Output CSV columns:
 *   ENQ,ts_ns,ifindex,len,0,inflight
 *   DEQ,ts_ns,ifindex,len,dwell_ns,inflight
 *   STATS,ts_ns,inflight_pkts,inflight_bytes,enq_total,deq_total,miss,insert_fail,rb_lost
 *
 * Usage: sudo ./txdwell [-i ifname|ifindex] [-o out.csv] [-v] [-d seconds] [-b bpf_obj]
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "txdwell.h"

#define USBNET_MODULE "usbnet"
#define STATS_INTERVAL_NS 1000000000ull

static volatile sig_atomic_t exiting;
static FILE *out = NULL;

static void sig_handler(int sig)
{
	exiting = 1;
}

static __u64 mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ull + (__u64)ts.tv_nsec;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, format, args);
}

/* Resolve "sym [module]" (or bare "sym" when module == NULL) in kallsyms. */
static int kallsyms_lookup(const char *sym, const char *module, __u64 *addr)
{
	char line[512], name[256], mod[80], want[84], type;
	unsigned long long a;
	FILE *f;
	int n;

	f = fopen("/proc/kallsyms", "r");
	if (!f) {
		fprintf(stderr, "open /proc/kallsyms: %s\n", strerror(errno));
		return -1;
	}
	if (module)
		snprintf(want, sizeof(want), "[%s]", module);

	while (fgets(line, sizeof(line), f)) {
		mod[0] = '\0';
		n = sscanf(line, "%llx %c %255s %79s", &a, &type, name, mod);
		if (n < 3 || strcmp(name, sym) != 0)
			continue;
		if (module && (n < 4 || strcmp(mod, want) != 0))
			continue;
		fclose(f);
		if (a == 0) {
			fprintf(stderr,
				"kallsyms addresses are zeroed (kptr_restrict); run as root\n");
			return -1;
		}
		*addr = a;
		return 0;
	}
	fclose(f);
	if (module)
		fprintf(stderr,
			"symbol '%s [%s]' not found in kallsyms (module loaded?)\n",
			sym, module);
	else
		fprintf(stderr, "symbol '%s' not found in kallsyms\n", sym);
	return -1;
}

static struct bpf_link *attach_by_addr(struct bpf_program *prog, __u64 addr,
				       const char *what)
{
	unsigned long kaddr = addr;	/* .addrs wants unsigned long * */
	LIBBPF_OPTS(bpf_kprobe_multi_opts, opts, .addrs = &kaddr, .cnt = 1);
	struct bpf_link *link;

	link = bpf_program__attach_kprobe_multi_opts(prog, NULL, &opts);
	if (!link) {
		fprintf(stderr, "FAIL: kprobe_multi attach %s @ 0x%llx: %s\n",
			what, (unsigned long long)addr, strerror(errno));
		return NULL;
	}
	fprintf(stderr, "attached %-24s @ 0x%llx\n", what,
		(unsigned long long)addr);
	return link;
}

static __u64 stat_read(int stats_fd, __u32 idx)
{
	__u64 v = 0;

	bpf_map_lookup_elem(stats_fd, &idx, &v);
	return v;
}

static void print_stats(int stats_fd)
{
	fprintf(out, "STATS,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
		(unsigned long long)mono_ns(),
		(unsigned long long)stat_read(stats_fd, ST_INFLIGHT_PKTS),
		(unsigned long long)stat_read(stats_fd, ST_INFLIGHT_BYTES),
		(unsigned long long)stat_read(stats_fd, ST_ENQ_TOTAL),
		(unsigned long long)stat_read(stats_fd, ST_DEQ_TOTAL),
		(unsigned long long)stat_read(stats_fd, ST_MISS),
		(unsigned long long)stat_read(stats_fd, ST_INSERT_FAIL),
		(unsigned long long)stat_read(stats_fd, ST_RB_LOST));
	fflush(out);
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = data;

	if (data_sz < sizeof(*e))
		return 0;
	fprintf(out, "%s,%llu,%u,%u,%llu,%lld\n",
		e->type == EV_ENQ ? "ENQ" : "DEQ",
		(unsigned long long)e->ts_ns, e->ifindex, e->len,
		(unsigned long long)e->dwell_ns, (long long)e->inflight);
	return 0;
}

static unsigned int parse_ifindex(const char *s)
{
	char *end;
	unsigned long v;

	v = strtoul(s, &end, 10);
	if (end != s && *end == '\0')
		return (unsigned int)v;
	return if_nametoindex(s);	/* 0 on failure */
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-i ifname|ifindex] [-o out.csv] [-v] [-d seconds] [-b bpf_obj]\n"
		"  -i  only observe this interface (default: all usbnet interfaces)\n"
		"  -o  CSV output file (default: stdout)\n"
		"  -v  also emit ENQ events (default: DEQ events + STATS only)\n"
		"  -d  stop after N seconds (default: until Ctrl-C)\n"
		"  -b  txdwell.bpf.o path (default: ./txdwell.bpf.o or $TXDWELL_BPF_OBJ)\n",
		prog);
}

int main(int argc, char **argv)
{
	struct txdwell_config cfg = {};
	struct bpf_object *obj = NULL;
	struct bpf_program *enq_prog, *deq_prog;
	struct bpf_link *enq_link = NULL, *deq_link = NULL;
	struct ring_buffer *rb = NULL;
	struct bpf_map *config_map, *events_map, *stats_map;
	__u64 enq_addr = 0, deq_addr = 0, deadline = 0, last_stats;
	const char *obj_path = NULL, *out_path = NULL;
	int opt, err = 1, stats_fd, duration = 0;
	__u32 zero = 0;

	while ((opt = getopt(argc, argv, "i:o:vd:b:h")) != -1) {
		switch (opt) {
		case 'i':
			cfg.ifindex = parse_ifindex(optarg);
			if (!cfg.ifindex) {
				fprintf(stderr, "bad interface '%s'\n", optarg);
				return 1;
			}
			break;
		case 'o':
			out_path = optarg;
			break;
		case 'v':
			cfg.emit_enq = 1;
			break;
		case 'd':
			duration = atoi(optarg);
			if (duration <= 0) {
				fprintf(stderr, "bad duration '%s'\n", optarg);
				return 1;
			}
			break;
		case 'b':
			obj_path = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (!obj_path)
		obj_path = getenv("TXDWELL_BPF_OBJ");
	if (!obj_path)
		obj_path = "./txdwell.bpf.o";

	if (geteuid() != 0) {
		fprintf(stderr, "must run as root (kallsyms + bpf)\n");
		return 1;
	}

	if (kallsyms_lookup("usbnet_start_xmit", USBNET_MODULE, &enq_addr) ||
	    kallsyms_lookup("tx_complete", USBNET_MODULE, &deq_addr))
		return 1;
	fprintf(stderr, "usbnet_start_xmit [usbnet] @ 0x%llx\n",
		(unsigned long long)enq_addr);
	fprintf(stderr, "tx_complete        [usbnet] @ 0x%llx\n",
		(unsigned long long)deq_addr);

	out = stdout;
	if (out_path) {
		out = fopen(out_path, "w");
		if (!out) {
			fprintf(stderr, "open %s: %s\n", out_path,
				strerror(errno));
			return 1;
		}
	}
	setvbuf(out, NULL, _IOLBF, 0);

	libbpf_set_print(libbpf_print_fn);

	obj = bpf_object__open_file(obj_path, NULL);
	if (libbpf_get_error(obj)) {
		fprintf(stderr, "open %s failed\n", obj_path);
		obj = NULL;
		goto cleanup;
	}

	if (bpf_object__load(obj)) {
		fprintf(stderr, "BPF load failed (verifier?)\n");
		goto cleanup;
	}

	config_map = bpf_object__find_map_by_name(obj, "config_map");
	events_map = bpf_object__find_map_by_name(obj, "events");
	stats_map = bpf_object__find_map_by_name(obj, "stats");
	if (!config_map || !events_map || !stats_map) {
		fprintf(stderr, "maps not found in BPF object\n");
		goto cleanup;
	}
	if (bpf_map_update_elem(bpf_map__fd(config_map), &zero, &cfg, 0)) {
		fprintf(stderr, "set config: %s\n", strerror(errno));
		goto cleanup;
	}
	stats_fd = bpf_map__fd(stats_map);

	enq_prog = bpf_object__find_program_by_name(obj, "enq_kprobe");
	deq_prog = bpf_object__find_program_by_name(obj, "deq_kprobe");
	if (!enq_prog || !deq_prog) {
		fprintf(stderr, "programs not found in BPF object\n");
		goto cleanup;
	}

	enq_link = attach_by_addr(enq_prog, enq_addr, "usbnet_start_xmit");
	deq_link = attach_by_addr(deq_prog, deq_addr, "tx_complete");
	if (!enq_link || !deq_link)
		goto cleanup;

	fprintf(stderr,
		"observing %s, ENQ events %s, output %s%s\n",
		cfg.ifindex ? "filtered interface" : "all usbnet interfaces",
		cfg.emit_enq ? "on" : "off",
		out_path ? out_path : "<stdout>",
		duration ? "" : " (Ctrl-C to stop)");

	fprintf(out, "# type,ts_ns,ifindex,len,dwell_ns,inflight\n");
	fprintf(out, "# STATS,ts_ns,inflight_pkts,inflight_bytes,enq_total,deq_total,miss,insert_fail,rb_lost\n");

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	rb = ring_buffer__new(bpf_map__fd(events_map), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "ring_buffer__new: %s\n", strerror(errno));
		goto cleanup;
	}

	if (duration)
		deadline = mono_ns() + (__u64)duration * 1000000000ull;
	last_stats = mono_ns();

	while (!exiting) {
		__u64 now;

		if (deadline && mono_ns() >= deadline)
			break;
		err = ring_buffer__poll(rb, 100 /* ms */);
		if (err == -EINTR) {
			err = 0;
			break;
		}
		if (err < 0) {
			fprintf(stderr, "ring buffer poll: %s\n", strerror(-err));
			break;
		}
		now = mono_ns();
		if (now - last_stats >= STATS_INTERVAL_NS) {
			print_stats(stats_fd);
			last_stats = now;
		}
	}

	/* drain anything left, final stats + summary */
	ring_buffer__poll(rb, 0);
	print_stats(stats_fd);
	fprintf(stderr,
		"summary: enq=%llu deq=%llu miss=%llu insert_fail=%llu rb_lost=%llu inflight=%llu pkts/%llu bytes\n",
		(unsigned long long)stat_read(stats_fd, ST_ENQ_TOTAL),
		(unsigned long long)stat_read(stats_fd, ST_DEQ_TOTAL),
		(unsigned long long)stat_read(stats_fd, ST_MISS),
		(unsigned long long)stat_read(stats_fd, ST_INSERT_FAIL),
		(unsigned long long)stat_read(stats_fd, ST_RB_LOST),
		(unsigned long long)stat_read(stats_fd, ST_INFLIGHT_PKTS),
		(unsigned long long)stat_read(stats_fd, ST_INFLIGHT_BYTES));
	err = 0;

cleanup:
	ring_buffer__free(rb);
	bpf_link__destroy(enq_link);
	bpf_link__destroy(deq_link);
	bpf_object__close(obj);
	if (out_path && out)
		fclose(out);
	return err;
}
