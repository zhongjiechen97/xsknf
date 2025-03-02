#define _GNU_SOURCE

#include "xsknf.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <errno.h>
#include <getopt.h>
#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xdp/xsk.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

/*
 * The number of frames per socket and the size of the frame shall always be a
 * power of two. This allows to quickly identify the owner (socket) of the frame
 * when frames are put back into the fill ring.
 * Every address in the UMEM area can be structured as follows:
 * | owner-id | frame-id | in-frame-offset |
 */
#define FRAMES_PER_SOCKET_SHIFT 12
#define FRAMES_PER_SOCKET (1 << FRAMES_PER_SOCKET_SHIFT)  // 4096

#define DEFAULT_BIND_FLAGS (XDP_USE_NEED_WAKEUP)

#define POLL_TIMEOUT_MS 1000

static size_t umem_bufsize;
static int stop_workers = 0;
static struct xsknf_config conf;
static struct xsknf_config default_conf = {
	.working_mode = MODE_AF_XDP,
	.xsk_frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE,
	.batch_size = 64,
	.workers = 1,
	.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST
};

struct xsk_socket_info {
	struct worker *worker;
	struct xsk_socket *xsk;
	uint32_t bind_flags;
	void *buffer;
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsknf_socket_stats stats;
	unsigned outstanding_tx;
};

struct worker {
	unsigned id;
	pthread_t thread;
	struct xsk_socket_info *xsks;
	struct xsk_umem *umem;
	void *buffer;
	struct xsk_umem *copy_umem;
	void *copy_buffer;
} __attribute__((aligned(64)));

static unsigned num_sockets;
static int *ifindexes;
static struct worker *workers;
static struct bpf_object *obj;
static int egress_ebpf_program = 0;
static int owner_shift;

static int xsk_get_xdp_stats(int fd, struct xsknf_socket_stats *stats)
{
	struct xdp_statistics xdp_stats;
	socklen_t optlen;
	int err;

	optlen = sizeof(stats);
	err = getsockopt(fd, SOL_XDP, XDP_STATISTICS, &xdp_stats, &optlen);
	if (err)
		return err;

	if (optlen == sizeof(struct xdp_statistics)) {
		stats->rx_dropped_npkts = xdp_stats.rx_dropped;
		stats->rx_invalid_npkts = xdp_stats.rx_invalid_descs;
		stats->tx_invalid_npkts = xdp_stats.tx_invalid_descs;
		stats->rx_full_npkts = xdp_stats.rx_ring_full;
		stats->rx_fill_empty_npkts = xdp_stats.rx_fill_ring_empty_descs;
		stats->tx_empty_npkts = xdp_stats.tx_ring_empty_descs;
		return 0;
	}

	return -EINVAL;
}

static void __exit_with_error(int error, const char *file, const char *func,
		int line)
{
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));

	xsknf_cleanup();

	exit(EXIT_FAILURE);
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

static void xsk_configure_socket(char *iface, unsigned queue,
		struct xsk_socket_info *xsk, unsigned umem_offset)
{
	struct xsk_socket_config cfg;
	int ret, sock_opt;
	uint32_t idx;

	cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	if (conf.working_mode & MODE_XDP) {
		cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
	} else {
		cfg.libbpf_flags = 0;
	}
	cfg.xdp_flags = conf.xdp_flags;
	cfg.bind_flags = xsk->bind_flags;

	struct xsk_umem *umem = xsk->bind_flags & XDP_COPY ?
			xsk->worker->copy_umem : xsk->worker->umem;

	ret = xsk_socket__create_shared(&xsk->xsk, iface, queue, umem, &xsk->rx,
			&xsk->tx, &xsk->fq, &xsk->cq, &cfg);
	if (ret)
		exit_with_error(-ret);

	/* Enable and configure busy poll */
	if (conf.busy_poll && !(xsk->bind_flags & XDP_COPY)) {
		sock_opt = 1;
		if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET,
				SO_PREFER_BUSY_POLL, (void *)&sock_opt, sizeof(sock_opt)) < 0)
			exit_with_error(errno);

		sock_opt = 20;
		if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL,
				(void *)&sock_opt, sizeof(sock_opt)) < 0)
			exit_with_error(errno);

		sock_opt = conf.batch_size;
		if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET,
				SO_BUSY_POLL_BUDGET, (void *)&sock_opt, sizeof(sock_opt)) < 0)
			exit_with_error(errno);
	}

	/* Populate the fill ring */
	ret = xsk_ring_prod__reserve(&xsk->fq, FRAMES_PER_SOCKET, &idx);
	if (ret != FRAMES_PER_SOCKET)
		exit_with_error(-ret);
	for (int i = 0; i < FRAMES_PER_SOCKET; i++) {
		*xsk_ring_prod__fill_addr(&xsk->fq, idx++) =
				(umem_offset + i) * conf.xsk_frame_size;
	}
	xsk_ring_prod__submit(&xsk->fq, FRAMES_PER_SOCKET);
}

static void enter_xsks_into_map(struct bpf_object *obj)
{
	struct bpf_map *map;
	int xsks_map;

	map = bpf_object__find_map_by_name(obj, "xsks");
	xsks_map = bpf_map__fd(map);
	if (xsks_map < 0) {
		fprintf(stderr, "WARNING: no xsks map found: %s\n", strerror(xsks_map));
		return;
	}

	for (int if_idx = 0; if_idx < conf.num_interfaces; if_idx++) {
		for (int wrk_idx = 0; wrk_idx < conf.workers; wrk_idx++) {
			int fd = xsk_socket__fd(workers[wrk_idx].xsks[if_idx].xsk);
			/* TODO: support multiple workers with multiple iterfaces */
			int key = wrk_idx;

			if (bpf_map_update_elem(xsks_map, &key, &fd, 0)) {
				fprintf(stderr, "ERROR: bpf_map_update_elem %d\n", key);
				exit(EXIT_FAILURE);
			}
		}
	}
}

static void load_tc_programs(int fd)
{
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct tcmsg *tcm;
	int ret;
	uint32_t seq, portid;

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL) {
		perror("mnl_socket_open");
		exit(EXIT_FAILURE);
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		exit(EXIT_FAILURE);
	}
	portid = mnl_socket_get_portid(nl);

	for (int i = 0; i < conf.num_interfaces; i++) {
		/* Add (or replace) the clsact qdisc */
		nlh = mnl_nlmsg_put_header(buf);
		nlh->nlmsg_type	= RTM_NEWQDISC;
		nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE
				| NLM_F_ACK;
		nlh->nlmsg_seq = seq = time(NULL);

		tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct tcmsg));
		tcm->tcm_family = AF_UNSPEC;
		tcm->tcm_ifindex = ifindexes[i];
		tcm->tcm_handle = TC_H_MAKE(TC_H_CLSACT, 0);
		tcm->tcm_parent = TC_H_CLSACT;
		mnl_attr_put(nlh, TCA_KIND, sizeof("clsact"), "clsact");

		if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
			perror("mnl_socket_sendto");
			exit(EXIT_FAILURE);
		}

		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret < 0) {
			perror("mnl_socket_recvfrom");
			exit(EXIT_FAILURE);
		}

		ret = mnl_cb_run(buf, ret, seq, portid, NULL, NULL);
		if (ret < 0) {
			fprintf(stderr, "ERROR: failed to add clsact qdisc on %s: ",
					conf.interfaces[i]);
			perror(NULL);
			exit(EXIT_FAILURE);
		}

		/* Add (or replace) the eBPF filter on egress side of clsact qdisc */
		nlh = mnl_nlmsg_put_header(buf);
		nlh->nlmsg_type	= RTM_NEWTFILTER;
		nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE
				| NLM_F_ACK;
		nlh->nlmsg_seq = time(NULL);

		tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct tcmsg));
		tcm->tcm_family = AF_UNSPEC;
		tcm->tcm_ifindex = ifindexes[i];
		tcm->tcm_handle = 1;
		tcm->tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_EGRESS);
		uint32_t protocol = htons(ETH_P_ALL);
		uint32_t prio = 1;
		tcm->tcm_info = TC_H_MAKE(prio << 16, protocol);
		mnl_attr_put(nlh, TCA_KIND, sizeof("bpf"), "bpf");
		struct nlattr *opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
		mnl_attr_put_u32(nlh, TCA_BPF_FD, fd);
		mnl_attr_put_u32(nlh, TCA_BPF_FLAGS, TCA_BPF_FLAG_ACT_DIRECT);
		mnl_attr_nest_end(nlh, opts);

		if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
			perror("mnl_socket_sendto");
			exit(EXIT_FAILURE);
		}

		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret < 0) {
			perror("mnl_socket_recvfrom");
			exit(EXIT_FAILURE);
		}

		ret = mnl_cb_run(buf, ret, seq, portid, NULL, NULL);
		if (ret < 0) {
			fprintf(stderr, "ERROR: failed to add egress eBPF filter on %s: ",
					conf.interfaces[i]);
			perror(NULL);
			exit(EXIT_FAILURE);
		}
	}

	mnl_socket_close(nl);
}

static void del_clsact_qdiscs()
{
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct tcmsg *tcm;
	int ret;
	uint32_t seq, portid;

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL) {
		perror("mnl_socket_open");
		return;
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		mnl_socket_close(nl);
		return;
	}
	portid = mnl_socket_get_portid(nl);

	for (int i = 0; i < conf.num_interfaces; i++) {
		nlh = mnl_nlmsg_put_header(buf);
		nlh->nlmsg_type	= RTM_DELQDISC;
		nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
		nlh->nlmsg_seq = seq = time(NULL);

		tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct tcmsg));
		tcm->tcm_family = AF_UNSPEC;
		tcm->tcm_ifindex = ifindexes[i];
		tcm->tcm_handle = TC_H_MAKE(TC_H_CLSACT, 0);
		tcm->tcm_parent = TC_H_CLSACT;
		mnl_attr_put(nlh, TCA_KIND, sizeof("clsact"), "clsact");

		if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
			perror("mnl_socket_sendto");
			continue;
		}

		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret < 0) {
			perror("mnl_socket_recvfrom");
			continue;
		}

		ret = mnl_cb_run(buf, ret, seq, portid, NULL, NULL);
		if (ret < 0) {
			fprintf(stderr, "ERROR: failed to remove clsact qdisc on %s: ",
					conf.interfaces[i]);
			perror(NULL);
			continue;
		}
	}

	mnl_socket_close(nl);
}

static void load_ebpf_programs(char *path, struct bpf_object **obj,
		char *xdp_progname, char *tc_progname)
{
	struct bpf_program *xdp_prog, *tc_prog;
	int fd;
	int err;

	*obj = bpf_object__open_file(path, NULL);
	if (!*obj) {
		fprintf(stderr, "ERROR: unable to parse eBPF file\n");
		exit(EXIT_FAILURE);
	}
	xdp_prog = bpf_object__next_program(*obj, NULL);
	if (!xdp_prog) {
		fprintf(stderr, "ERROR: no '%s' xdp program found\n", xdp_progname);
		exit(EXIT_FAILURE);
	}
	bpf_program__set_type(xdp_prog, BPF_PROG_TYPE_XDP);
	err = bpf_object__load(*obj);
	if(err){
		fprintf(stderr, "ERROR: unable to load eBPF file\n");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < conf.num_interfaces; i++) {
		if (bpf_xdp_attach(ifindexes[i], bpf_program__fd(xdp_prog),
				conf.xdp_flags, NULL) < 0) {
			fprintf(stderr, "ERROR: failed setting xdp program on %s\n",
					conf.interfaces[i]);
			exit(EXIT_FAILURE);
		}
	}

	if (tc_progname[0]) {
		tc_prog = bpf_object__find_program_by_name(*obj, tc_progname);
		if (!xdp_prog) {
			fprintf(stderr, "ERROR: no '%s' tc program found\n", tc_progname);
			exit(EXIT_FAILURE);
		}
		load_tc_programs(bpf_program__fd(tc_prog));
		egress_ebpf_program = 1;
	}
}

static void kick_tx(struct xsk_socket_info *xsk)
{
	int ret;

	ret = sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN ||
		errno == EBUSY || errno == ENETDOWN)
		return;
	exit_with_error(errno);
}

struct pkt_info {
	uint64_t addr;
	uint32_t len;
};

static inline void complete_tx(struct xsk_socket_info *xsks,
		unsigned ifindex)
{
	struct xsk_socket_info *tx_xsk = &xsks[ifindex];
	uint32_t idx;
	unsigned int sent, ret;
	uint64_t to_fill[conf.num_interfaces][conf.batch_size];
	/* This counters support a max batch size of 511 packets */
	uint8_t nfill[XSKNF_MAX_INTERFACES] = {0};
	size_t ndescs;
	int i, j, owner;
	uint64_t addr;

	if (!tx_xsk->outstanding_tx)
		return;

	/* 
	 * Tx must be manually triggered for COPY mode sockets and when busy polling
	 * is disabled and the NEED_WAKEUP flag of the tx queue is set
	 */
	if (tx_xsk->bind_flags & XDP_COPY || (!conf.poll && !conf.busy_poll
			&& xsk_ring_prod__needs_wakeup(&tx_xsk->tx))) {
		tx_xsk->stats.tx_trigger_sendtos++;
		kick_tx(tx_xsk);
	}

	ndescs = (tx_xsk->outstanding_tx > conf.batch_size) ? conf.batch_size :
			tx_xsk->outstanding_tx;

	/* Recycle completed tx frames */
	sent = xsk_ring_cons__peek(&tx_xsk->cq, ndescs, &idx);
	if (sent > 0) {
		/* Map every frame to its owner */
		for (i = 0; i < sent; i++) {
			addr = *xsk_ring_cons__comp_addr(&tx_xsk->cq, idx++);
			owner = addr >> owner_shift;
			to_fill[owner][nfill[owner]++] = addr;
		}

		xsk_ring_cons__release(&tx_xsk->cq, sent);
		tx_xsk->stats.tx_npkts += sent;

		/* Put frames in their owner's fill queue */
		for (i = 0; i < conf.num_interfaces; i++) {
			if (nfill[i]) {
				ret = xsk_ring_prod__reserve(&xsks[i].fq, nfill[i], &idx);
				if (ret != nfill[i]) {
					/* (0 < ret < nfill[i]) should never happen */
					exit_with_error(-ret);
				}

				for (int j = 0; j < nfill[i]; j++) {
					*xsk_ring_prod__fill_addr(&xsks[i].fq, idx++) =
							to_fill[i][j];
				}

				xsk_ring_prod__submit(&xsks[i].fq, nfill[i]);
			}
		}
		
		tx_xsk->outstanding_tx -= sent;
	}
}

static void process_batch(struct xsk_socket_info *xsks, unsigned ifindex)
{
	struct xsk_socket_info *rx_xsk = &xsks[ifindex];
	struct pkt_info to_drop[conf.batch_size],
			to_tx[conf.num_interfaces][conf.batch_size];
	/* These counters support a max batch size of 511 packets */
	uint8_t ndrop = 0, ntx[XSKNF_MAX_INTERFACES] = {0};
	unsigned int rcvd, i, j;
	uint32_t idx;
	int ret;

	complete_tx(xsks, ifindex);

	/* Check if there are rx packets */
	rcvd = xsk_ring_cons__peek(&rx_xsk->rx, conf.batch_size, &idx);
	if (!rcvd) {
		if (!(rx_xsk->bind_flags & XDP_COPY) && (conf.busy_poll
				|| xsk_ring_prod__needs_wakeup(&rx_xsk->fq))) {
			rx_xsk->stats.rx_empty_polls++;
			recvfrom(xsk_socket__fd(rx_xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL,
					NULL);
		}
		return;
	}

	/* Process packets and store destination queue */
	for (i = 0; i < rcvd; i++) {
		uint64_t addr = xsk_ring_cons__rx_desc(&rx_xsk->rx, idx)->addr;
		uint32_t len = xsk_ring_cons__rx_desc(&rx_xsk->rx, idx++)->len;
		uint64_t orig = addr;

		addr = xsk_umem__add_offset_to_addr(addr);
		void *pkt = xsk_umem__get_data(rx_xsk->buffer, addr);

		ret = xsknf_packet_processor(pkt, len, ifindex);
		if (ret == -1) {
			/* Enqueue to drop queue */
			to_drop[ndrop].addr = orig;
			to_drop[ndrop++].len = len;
		} else {
			/* Enqueue to TX queue of the target dev */
			to_tx[ret][ntx[ret]].addr = orig;
			to_tx[ret][ntx[ret]++].len = len;
		}
	}

	xsk_ring_cons__release(&rx_xsk->rx, rcvd);
	rx_xsk->stats.rx_npkts += rcvd;

	/*
	 * Put frames of dropped packets back in the fill queue of the receiving
	 * interface
	 */
	if (ndrop) {
		ret = xsk_ring_prod__reserve(&rx_xsk->fq, ndrop, &idx);
		if (ret != ndrop) {
			/* (0 < ret < ndrop) should never happen */
			exit_with_error(-ret);
		}

		for (i = 0; i < ndrop; i++) {
			*xsk_ring_prod__fill_addr(&rx_xsk->fq, idx++) = to_drop[i].addr;
		}

		xsk_ring_prod__submit(&rx_xsk->fq, ndrop);
	}

	/*
	 * Put frames of redirected packets in the tx queue of the target interface
	 */
	for (i = 0; i < conf.num_interfaces; i++) {
		if (ntx[i]) {
			ret = xsk_ring_prod__reserve(&xsks[i].tx, ntx[i], &idx);
			while (ret != ntx[i]) {
				if (ret < 0)
					exit_with_error(-ret);
				complete_tx(xsks, ifindex);
				if (conf.busy_poll
						|| xsk_ring_prod__needs_wakeup(&xsks[i].tx)) {
					xsks[i].stats.tx_wakeup_sendtos++;
					kick_tx(&xsks[i]);
				}
				ret = xsk_ring_prod__reserve(&xsks[i].tx, ntx[i], &idx);
			}

			if (rx_xsk->buffer != xsks[i].buffer) {
				for (int j = 0; j < ntx[i]; j++) {
					__builtin_memcpy(xsks[i].buffer + to_tx[i][j].addr,
							rx_xsk->buffer + to_tx[i][j].addr, to_tx[i][j].len);
					xsk_ring_prod__tx_desc(&xsks[i].tx, idx)->addr
							= to_tx[i][j].addr;
					xsk_ring_prod__tx_desc(&xsks[i].tx, idx++)->len
							= to_tx[i][j].len;
				}
			} else {
				for (int j = 0; j < ntx[i]; j++) {
					xsk_ring_prod__tx_desc(&xsks[i].tx, idx)->addr
							= to_tx[i][j].addr;
					xsk_ring_prod__tx_desc(&xsks[i].tx, idx++)->len
							= to_tx[i][j].len;
				}
			}

			xsk_ring_prod__submit(&xsks[i].tx, ntx[i]);
			xsks[i].outstanding_tx += ntx[i];
		}
	}
}

static inline void complete_tx_1if(struct xsk_socket_info *xsk)
{
	uint32_t idx_cq, idx_fq;
	unsigned int sent, ret, i;
	size_t ndescs;

	if (!xsk->outstanding_tx)
		return;

	/* 
	 * Tx must be manually triggered for COPY mode sockets and when busy polling
	 * is disabled and the NEED_WAKEUP flag of the tx queue is set
	 */
	if (xsk->bind_flags & XDP_COPY || (!conf.poll && !conf.busy_poll
			&& xsk_ring_prod__needs_wakeup(&xsk->tx))) {
		xsk->stats.tx_trigger_sendtos++;
		kick_tx(xsk);
	}

	ndescs = (xsk->outstanding_tx > conf.batch_size) ? conf.batch_size :
			xsk->outstanding_tx;

	/* Recycle completed tx frames */
	sent = xsk_ring_cons__peek(&xsk->cq, ndescs, &idx_cq);
	if (sent > 0) {
		xsk->stats.tx_npkts += sent;

		ret = xsk_ring_prod__reserve(&xsk->fq, sent, &idx_fq);
		if (ret != sent) {
			/* (0 < ret < sent) should never happen */
			exit_with_error(-ret);
		}

		for (int i = 0; i < sent; i++)
			*xsk_ring_prod__fill_addr(&xsk->fq, idx_fq++) =
				*xsk_ring_cons__comp_addr(&xsk->cq, idx_cq++);

		xsk_ring_prod__submit(&xsk->fq, sent);
		xsk_ring_cons__release(&xsk->cq, sent);
		xsk->outstanding_tx -= sent;
	}
}

static void process_batch_1if(struct xsk_socket_info *xsk)
{
	struct pkt_info to_drop[conf.batch_size], to_tx[conf.batch_size];
	/* These counters support a max batch size of 511 packets */
	uint8_t ndrop = 0, ntx = 0;
	unsigned int rcvd, i;
	uint32_t idx;
	int ret;

	complete_tx_1if(xsk);

	/* Check if there are rx packets */
	rcvd = xsk_ring_cons__peek(&xsk->rx, conf.batch_size, &idx);
	if (!rcvd) {
		if (!(xsk->bind_flags & XDP_COPY) && (conf.busy_poll
				|| xsk_ring_prod__needs_wakeup(&xsk->fq))) {
			xsk->stats.rx_empty_polls++;
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL,
					NULL);
		}
		return;
	}

	/* Process packets and store destination queue */
	for (i = 0; i < rcvd; i++) {
		uint64_t addr = xsk_ring_cons__rx_desc(&xsk->rx, idx)->addr;
		uint32_t len = xsk_ring_cons__rx_desc(&xsk->rx, idx++)->len;
		uint64_t orig = addr;

		addr = xsk_umem__add_offset_to_addr(addr);
		void *pkt = xsk_umem__get_data(xsk->buffer, addr);

		ret = xsknf_packet_processor(pkt, len, 0);
		if (ret == -1) {
			/* Enqueue to drop queue */
			to_drop[ndrop].addr = orig;
			to_drop[ndrop++].len = len;
		} else {
			/* Enqueue to TX queue of the dev */
			to_tx[ntx].addr = orig;
			to_tx[ntx++].len = len;
		}
	}

	xsk_ring_cons__release(&xsk->rx, rcvd);
	xsk->stats.rx_npkts += rcvd;

	/* Put frames of dropped packets back in the fill queue */
	if (ndrop) {
		ret = xsk_ring_prod__reserve(&xsk->fq, ndrop, &idx);
		if (ret != ndrop) {
			/* (0 < ret < ndrop) should never happen */
			exit_with_error(-ret);
		}

		for (i = 0; i < ndrop; i++) {
			*xsk_ring_prod__fill_addr(&xsk->fq, idx++) = to_drop[i].addr;
		}

		xsk_ring_prod__submit(&xsk->fq, ndrop);
	}

	/* Put frames of redirected packets in the tx queue */
	if (ntx) {
		ret = xsk_ring_prod__reserve(&xsk->tx, ntx, &idx);
		while (ret != ntx) {
			if (ret < 0)
				exit_with_error(-ret);
			complete_tx_1if(xsk);
			if (conf.busy_poll || xsk_ring_prod__needs_wakeup(&xsk->tx)) {
				xsk->stats.tx_wakeup_sendtos++;
				kick_tx(xsk);
			}
			ret = xsk_ring_prod__reserve(&xsk->tx, ntx, &idx);
		}

		for (int i = 0; i < ntx; i++) {
			xsk_ring_prod__tx_desc(&xsk->tx, idx)->addr = to_tx[i].addr;
			xsk_ring_prod__tx_desc(&xsk->tx, idx++)->len = to_tx[i].len;
		}

		xsk_ring_prod__submit(&xsk->tx, ntx);
		xsk->outstanding_tx += ntx;
	}
}

static void *worker_loop(void *arg)
{
	struct worker *worker = (struct worker *)arg;
	struct pollfd fds[XSKNF_MAX_INTERFACES] = {};
	int i, ret;

	while (!stop_workers) {
		if (conf.poll) {
			for (i = 0; i < conf.num_interfaces; i++) {
				fds[i].fd = xsk_socket__fd(worker->xsks[i].xsk);
				fds[i].events = POLLIN;
				worker->xsks[i].stats.opt_polls++;
			}
			ret = poll(fds, conf.num_interfaces, POLL_TIMEOUT_MS);
			if (ret <= 0)
				continue;
		}

		if (conf.num_interfaces > 1) {
			for (i = 0; i < conf.num_interfaces; i++) {
				process_batch(worker->xsks, i);
			}
		} else {
			process_batch_1if(&worker->xsks[0]);
		}
	}
}

static struct option long_options[] = {
	{"iface", required_argument, 0, 'i'},
	{"poll", no_argument, 0, 'p'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"frame-size", required_argument, 0, 'f'},
	{"unaligned", no_argument, 0, 'u'},
	{"batch-size", required_argument, 0, 'b'},
	{"busy-poll", no_argument, 0, 'B'},
	{"mode", required_argument, 0, 'M'},
	{"workers", required_argument, 0, 'w'},
	{0, 0, 0, 0}
};

static void usage()
{
	const char *str =
		"	xsknf options:\n"
		"	-i, --iface=n[:m]	Interface to operate on (a copy mode between copy (c) or zero-copy (z)\n"
		"				can optionally be specified). Can be repeated multiple times\n"
		"	-p, --poll		Use poll syscall\n"
		"	-S, --xdp-skb=n		Use XDP skb-mode\n"
		"	-f, --frame-size=n	Set the frame size (must be a power of two in aligned mode, default is %d)\n"
		"	-u, --unaligned		Enable unaligned chunk placement\n"
		"	-b, --batch-size=n	Batch size for sending or receiving packets. Default is %d\n"
		"	-B, --busy-poll		Busy poll\n"
		"	-M  --mode		Working mode (AF_XDP, XDP, COMBINED)\n"
		"	-w  --workers=n		Number of packet processing workers\n"
		"\n";
	fprintf(stderr, str, XSK_UMEM__DEFAULT_FRAME_SIZE, default_conf.batch_size);

	exit(EXIT_FAILURE);
}

int xsknf_parse_args(int argc, char **argv, struct xsknf_config *config)
{
	int option_index, c;

	memcpy(config, &default_conf, sizeof(struct xsknf_config));
	sprintf(config->ebpf_filename, "%s_kern.o", argv[0]);
	sprintf(config->xdp_progname, "handle_xdp");
	config->tc_progname[0] = 0;

	for (;;) {
		c = getopt_long(argc, argv, "i:pSf:ub:BM:w:", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'i':;
			int i;
			config->bind_flags[config->num_interfaces] = DEFAULT_BIND_FLAGS;
			for (i = 0; optarg[i] != 0 && optarg[i] != ':'; i++);
			if (optarg[i] == ':') {
				switch (optarg[i + 1]) {
				case 'c':
					config->bind_flags[config->num_interfaces] |= XDP_COPY;
					break;
				case 'z':
					config->bind_flags[config->num_interfaces] |= XDP_ZEROCOPY;
					break;
				default:
					fprintf(stderr, "ERROR: unknown copy mode '%c'\n",
							optarg[i + 1]);
					usage();
				}
				optarg[i] = 0;
			}
			config->interfaces[config->num_interfaces++] = optarg;
			break;
		case 'p':
			config->poll = 1;
			break;
		case 'S':
			config->xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'u':
			config->unaligned_chunks = 1;
			break;
		case 'f':
			config->xsk_frame_size = atoi(optarg);
			break;
		case 'b':
			config->batch_size = atoi(optarg);
			break;
		case 'B':
			config->busy_poll = 1;
			break;
		case 'M':
			if (strcmp(optarg, "AF_XDP") == 0) {
				config->working_mode = MODE_AF_XDP;
			} else if (strcmp(optarg, "XDP") == 0) {
				config->working_mode = MODE_XDP;
			} else if (strcmp(optarg, "COMBINED") == 0) {
				config->working_mode = MODE_COMBINED;
			} else {
				fprintf(stderr, "ERROR: unknown working mode %s\n", optarg);
				usage();
			}
			break;
		case 'w':
			config->workers = atoi(optarg);
			if (config->workers < 1) {
				fprintf(stderr, "ERROR: Invalid number of workers %u",
						config->workers);
				usage();
			}
			break;
		default:
			usage();
		}
	}

	if (config->num_interfaces == 0) {
		fprintf(stderr, "ERROR: at least one interface in required\n");
		usage();
	}

	if (!(config->xdp_flags & XDP_FLAGS_SKB_MODE)) {
		config->xdp_flags |= XDP_FLAGS_DRV_MODE;
	}

	if ((config->xsk_frame_size & (config->xsk_frame_size - 1)) &&
		!config->unaligned_chunks) {
		fprintf(stderr, "--frame-size=%d is not a power of two\n",
			config->xsk_frame_size);
		usage();
	}

	return 0;
}

int xsknf_init(struct xsknf_config *config, struct bpf_object **bpf_obj)
{
	int ret;

	memcpy(&conf, config, sizeof(struct xsknf_config));

	ifindexes = malloc(conf.num_interfaces * sizeof(int));
	if (!ifindexes) {
		exit_with_error(errno);
	}

	for (int i = 0; i < conf.num_interfaces; i++) {
		ifindexes[i] = if_nametoindex(conf.interfaces[i]);
		if (!ifindexes[i]) {
			fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
					conf.interfaces[i]);
			exit(EXIT_FAILURE);
		}
	}

	num_sockets = conf.workers * conf.num_interfaces;

	if (conf.working_mode & MODE_AF_XDP) {
		owner_shift = FRAMES_PER_SOCKET_SHIFT
				+ __builtin_ffs(conf.xsk_frame_size) - 1;

		/* Set bind flags for sockets */
		for (int if_idx = 0; if_idx < conf.num_interfaces; if_idx++) {
			if (conf.xdp_flags & XDP_FLAGS_SKB_MODE) {
				/* All sockets need to work in copy mode */
				conf.bind_flags[if_idx] &= ~XDP_ZEROCOPY;
				conf.bind_flags[if_idx] |= XDP_COPY;
			}

			if (!(conf.bind_flags[if_idx] & (XDP_COPY | XDP_ZEROCOPY))) {
				/* 
				 * If no copy mode was explicitly defined try to force
				 * zero-copy. Libxdp allows to avoid specifying a copy mode and
				 * automatically chooses the best available one, but how can I
				 * understand which UMEM to use in this case?
				 */
				conf.bind_flags[if_idx] |= XDP_ZEROCOPY;
			}
		}

		/* Allocate workers */
		workers = calloc(conf.workers, sizeof(struct worker));
		if (!workers) {
			exit_with_error(errno);
		}

		/* Configure the UMEM */
		umem_bufsize = FRAMES_PER_SOCKET * conf.num_interfaces
				* conf.xsk_frame_size;
		int umem_buf_flags = MAP_PRIVATE | MAP_ANONYMOUS
				| (conf.unaligned_chunks ? MAP_HUGETLB : 0);
		struct xsk_umem_config umem_cfg = {
			.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2,
			.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
			.frame_size = conf.xsk_frame_size,
			.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
			.flags = conf.unaligned_chunks ?
					XDP_UMEM_UNALIGNED_CHUNK_FLAG : 0
		};

		for (int wrk_idx = 0; wrk_idx < conf.workers; wrk_idx++) {
			struct worker *worker = &workers[wrk_idx];
			worker->id = wrk_idx;

			worker->xsks = calloc(conf.num_interfaces,
					sizeof(struct xsk_socket_info));
			if (!worker->xsks) {
				exit_with_error(errno);
			}

			/* Create sockets */
			for (int if_idx = 0; if_idx < conf.num_interfaces; if_idx++) {
				struct xsk_socket_info *xsk = &worker->xsks[if_idx];
				xsk->worker = worker;

				if (conf.bind_flags[if_idx] & XDP_COPY) {
					if (worker->copy_buffer == NULL) {
						worker->copy_buffer = mmap(NULL, umem_bufsize,
								PROT_READ | PROT_WRITE, umem_buf_flags, -1, 0);
						if (worker->copy_buffer == MAP_FAILED) {
							exit_with_error(errno);
						}
						ret = xsk_umem__create(&worker->copy_umem,
								worker->copy_buffer, umem_bufsize, &xsk->fq,
								&xsk->cq, &umem_cfg);
						if (ret) {
							exit_with_error(-ret);
						}
					}

					xsk->buffer = worker->copy_buffer;

				} else {
					if (worker->buffer == NULL) {
						worker->buffer = mmap(NULL, umem_bufsize,
								PROT_READ | PROT_WRITE, umem_buf_flags, -1, 0);
						if (worker->buffer == MAP_FAILED) {
							exit_with_error(errno);
						}
						ret = xsk_umem__create(&worker->umem, 
								worker->buffer, umem_bufsize,
								&xsk->fq, &xsk->cq, &umem_cfg);
						if (ret) {
							exit_with_error(-ret);
						}
					}

					xsk->buffer = worker->buffer;
				}

				xsk->bind_flags = conf.bind_flags[if_idx];
				xsk_configure_socket(conf.interfaces[if_idx], wrk_idx,
						xsk, if_idx * FRAMES_PER_SOCKET);
			}
		}
	}
	
	if (conf.working_mode & MODE_XDP) {
		printf("Loading custom eBPF programs...\n");
		load_ebpf_programs(conf.ebpf_filename, &obj, conf.xdp_progname,
				conf.tc_progname);
		*bpf_obj = obj;

		if (conf.working_mode & MODE_AF_XDP) {
			enter_xsks_into_map(obj);
		}

		printf("Programs loaded\n");
	} else {
		*bpf_obj = NULL;
	}

	memcpy(config, &conf, sizeof(struct xsknf_config));

	return 0;
}

int xsknf_cleanup()
{
	xsknf_stop_workers();

	if (conf.working_mode & MODE_AF_XDP) {
		for (int wrk_idx = 0; wrk_idx < conf.workers; wrk_idx++) {
			for (int if_idx = 0; if_idx < conf.num_interfaces; if_idx++) {
				xsk_socket__delete(workers[wrk_idx].xsks[if_idx].xsk);
			}
			xsk_umem__delete(workers[wrk_idx].umem);
			munmap(workers[wrk_idx].buffer, umem_bufsize);
			xsk_umem__delete(workers[wrk_idx].copy_umem);
			munmap(workers[wrk_idx].copy_buffer, umem_bufsize);
			free(workers[wrk_idx].xsks);
		}
		free(workers);
	}

	for (int i = 0; i < conf.num_interfaces; i++) {
		bpf_xdp_attach(ifindexes[i], -1, conf.xdp_flags, NULL);
	}
	if (egress_ebpf_program) {
		del_clsact_qdiscs();
	}

	free(ifindexes);
}

int xsknf_start_workers()
{
	stop_workers = 0;

	if (conf.working_mode & MODE_AF_XDP) {
		/* Get available CPUs */
		pthread_t main_t = pthread_self();
		cpu_set_t cpu_set;
		int ret = pthread_getaffinity_np(main_t, sizeof(cpu_set_t), &cpu_set);
		if (ret) {
			exit_with_error(ret);
		}

		int num_cpus = CPU_COUNT(&cpu_set), curr_cpu = 0;
		int cpus[num_cpus];

		if (num_cpus < conf.workers) {
			fprintf(stderr, "ERROR: not enough CPUs to host all workers\n");
			xsknf_cleanup();
			exit(EXIT_FAILURE);
		}

		for (int i = 0; curr_cpu < num_cpus; i++) {
			if (CPU_ISSET(i, &cpu_set)) {
				cpus[curr_cpu++] = i;
			}
		}

		curr_cpu = 0;
		for (int i = 0; i < conf.workers; i++) {
			ret = pthread_create(&workers[i].thread, NULL, worker_loop,
					&workers[i]);
			if (ret) {
				exit_with_error(ret);
			}

			/*
			 * Set worker affinity to the corresponding CPU (worker N is bound
			 * to the Nth CPU assigned to the application).
			 * It is up to the user to guarantee that NIC interrupts land on the
			 * correct CPU through irq_affinity
			 * (i.e., queue N -> Nth CPU -> worker N).
			 */
			CPU_ZERO(&cpu_set);
			CPU_SET(cpus[curr_cpu++], &cpu_set);
			ret = pthread_setaffinity_np(workers[i].thread, sizeof(cpu_set_t),
					&cpu_set);
			if (ret) {
				exit_with_error(ret);
			}
		}
	}

	return 0;
}

int xsknf_stop_workers()
{
	stop_workers = 1;

	if (conf.working_mode & MODE_AF_XDP) {
		for (int i = 0; i < conf.workers; i++)
			pthread_join(workers[i].thread, NULL);
	}

	return 0;
}

int xsknf_get_socket_stats(unsigned worker_idx, unsigned iface_idx,
		struct xsknf_socket_stats *stats)
{
	xsk_get_xdp_stats(xsk_socket__fd(workers[worker_idx].xsks[iface_idx].xsk),
			&workers[worker_idx].xsks[iface_idx].stats);

	memcpy(stats, &workers[worker_idx].xsks[iface_idx].stats,
			sizeof(struct xsknf_socket_stats));

	return 0;
}