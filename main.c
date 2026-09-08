/* SPDX-License-Identifier: BSD-3-Clause
 *
 * DCC831 - Advanced Operating Systems - TP
 * DPDK ICMP Ping Echo Server
 *
 * Receives raw Ethernet frames on port 1 (the m510 private NIC port),
 * parses IPv4 + ICMP headers, turns every ICMP echo request into an echo
 * reply *in place*, fixes the checksums, and transmits it back.
 *
 * Based on DPDK's examples/skeleton (basicfwd.c).
 */

#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_version.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

/* The m510 NIC exposes 2 DPDK ports. Port 0 = public (Linux/ssh), port 1 =
 * private link between the two experiment machines. We only touch port 1. */
static uint16_t g_port_id = 1;

/* ---- DPDK version compatibility ------------------------------------------ *
 * The Ethernet header field names were renamed in DPDK 21.11
 * (s_addr/d_addr -> src_addr/dst_addr). The assignment targets DPDK 20.08. */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#define ETH_SRC(h) ((h)->src_addr)
#define ETH_DST(h) ((h)->dst_addr)
#else
#define ETH_SRC(h) ((h)->s_addr)
#define ETH_DST(h) ((h)->d_addr)
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#define RXMODE_MTU_FIELD mtu
#endif

/* ---- performance measurement ------------------------------------------- */
struct proc_stats {
	uint64_t replies;        /* echo replies produced                    */
	uint64_t other_pkts;     /* non echo-request packets seen and dropped */
	uint64_t proc_cycles;    /* TSC cycles: rx_burst return -> tx_burst   */
	uint64_t app_cycles;     /* TSC cycles: parse+craft only (no rx/tx)   */
	uint64_t min_cycles;
	uint64_t max_cycles;
};
static struct proc_stats g_stats = { .min_cycles = UINT64_MAX };

static volatile bool g_force_quit;

static void
handle_signal(int sig)
{
	if (sig == SIGINT || sig == SIGTERM) {
		printf("\n\nSignal %d received, shutting down...\n", sig);
		g_force_quit = true;
	}
}

static void
dump_stats(void)
{
	uint64_t hz = rte_get_timer_hz();
	uint64_t n = g_stats.replies;

	printf("\n===== ICMP echo server statistics =====\n");
	printf("echo replies sent      : %" PRIu64 "\n", g_stats.replies);
	printf("other packets dropped  : %" PRIu64 "\n", g_stats.other_pkts);
	if (n == 0 || hz == 0) {
		printf("(no replies, nothing to time)\n");
		return;
	}
	double avg_c = (double)g_stats.proc_cycles / (double)n;
	double avg_app = (double)g_stats.app_cycles / (double)n;
	printf("TSC frequency          : %" PRIu64 " Hz\n", hz);
	printf("--- window: rx_burst return -> tx_burst return ---\n");
	printf("cycles / reply  (avg)  : %.1f\n", avg_c);
	printf("cycles / reply  (min)  : %" PRIu64 "\n", g_stats.min_cycles);
	printf("cycles / reply  (max)  : %" PRIu64 "\n", g_stats.max_cycles);
	printf("nanoseconds/reply(avg) : %.1f\n", avg_c * 1e9 / (double)hz);
	printf("nanoseconds/reply(min) : %.1f\n",
	       (double)g_stats.min_cycles * 1e9 / (double)hz);
	printf("nanoseconds/reply(max) : %.1f\n",
	       (double)g_stats.max_cycles * 1e9 / (double)hz);
	printf("--- of which: parse + craft only (app logic) ---\n");
	printf("cycles / reply  (avg)  : %.1f\n", avg_app);
	printf("nanoseconds/reply(avg) : %.1f\n", avg_app * 1e9 / (double)hz);
	printf("--- remainder attributed to DPDK rx/tx path ---\n");
	printf("nanoseconds/reply(avg) : %.1f\n",
	       (avg_c - avg_app) * 1e9 / (double)hz);
	printf("=======================================\n");
}

/*
 * Initialize port `port` with 1 RX and 1 TX queue.
 */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(port_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error getting device (port %u) info: %s\n",
		       port, strerror(-retval));
		return retval;
	}

#ifdef DEV_TX_OFFLOAD_MBUF_FAST_FREE
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
#elif defined(RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
#endif

	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       port,
	       addr.addr_bytes[0], addr.addr_bytes[1], addr.addr_bytes[2],
	       addr.addr_bytes[3], addr.addr_bytes[4], addr.addr_bytes[5]);

	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

/*
 * Recompute the ICMP checksum over `len` bytes starting at `icmp`.
 * The icmp_cksum field must already be zeroed by the caller.
 */
static inline uint16_t
icmp_checksum(const void *icmp, uint16_t len)
{
	uint16_t sum = rte_raw_cksum(icmp, len);   /* folded, not inverted */
	uint16_t cksum = (uint16_t)~sum;
	return cksum ? cksum : (uint16_t)0xffff;
}

/*
 * Try to turn one received mbuf into an ICMP echo reply, in place.
 * Returns true if `m` now holds a reply ready to transmit,
 * false if the packet is not an ICMP echo request (caller should free it).
 */
static inline bool
make_echo_reply(struct rte_mbuf *m)
{
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	struct rte_icmp_hdr *icmp;
	uint16_t ip_hlen, ip_total, icmp_len;

	if (unlikely(m->data_len < sizeof(*eth) + sizeof(*ip) + sizeof(*icmp)))
		return false;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		return false;

	ip = (struct rte_ipv4_hdr *)(eth + 1);
	if (ip->next_proto_id != IPPROTO_ICMP)
		return false;

	ip_hlen = (uint16_t)((ip->version_ihl & RTE_IPV4_HDR_IHL_MASK) *
			     RTE_IPV4_IHL_MULTIPLIER);
	if (unlikely(ip_hlen < sizeof(*ip)))
		return false;

	ip_total = rte_be_to_cpu_16(ip->total_length);
	if (unlikely(ip_total < ip_hlen + sizeof(*icmp) ||
		     ip_total > m->data_len - sizeof(*eth)))
		return false;

	icmp = (struct rte_icmp_hdr *)((char *)ip + ip_hlen);
	if (icmp->icmp_type != RTE_IP_ICMP_ECHO_REQUEST || icmp->icmp_code != 0)
		return false;

	icmp_len = (uint16_t)(ip_total - ip_hlen);

	/* --- Ethernet: swap src/dst MAC (reply goes back to the sender) --- */
	struct rte_ether_addr tmp_mac;
	rte_ether_addr_copy(&ETH_SRC(eth), &tmp_mac);
	rte_ether_addr_copy(&ETH_DST(eth), &ETH_SRC(eth));
	rte_ether_addr_copy(&tmp_mac, &ETH_DST(eth));

	/* --- IPv4: swap src/dst addr, reset TTL, recompute header checksum - */
	uint32_t tmp_ip = ip->src_addr;
	ip->src_addr = ip->dst_addr;
	ip->dst_addr = tmp_ip;
	ip->time_to_live = 64;
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);

	/* --- ICMP: echo request (8) -> echo reply (0), recompute checksum -- */
	icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
	icmp->icmp_cksum = 0;
	icmp->icmp_cksum = icmp_checksum(icmp, icmp_len);

	return true;
}

/*
 * Main polling loop: run-to-completion on a single lcore.
 *
 * Timing: we read the TSC right after rte_eth_rx_burst() returns (packet is
 * already in host memory at that point) and again right after
 * rte_eth_tx_burst() hands the reply back to the NIC. The difference is the
 * time our software + the DPDK RX/TX path spend on one echo round trip on the
 * server side. Everything else in the ping RTT (wire, switch, the Linux
 * client's kernel stack) is outside this window -- see ANSWERS.md.
 */
static void
lcore_main(void)
{
	const uint16_t port = g_port_id;
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *out[BURST_SIZE];

	if (rte_eth_dev_socket_id(port) >= 0 &&
	    rte_eth_dev_socket_id(port) != (int)rte_socket_id())
		printf("WARNING: port %u is on a remote NUMA node to the "
		       "polling thread; performance will not be optimal.\n",
		       port);

	printf("\nCore %u answering ICMP echo on port %u. [Ctrl+C to quit]\n",
	       rte_lcore_id(), port);

	while (!g_force_quit) {
		const uint16_t nb_rx =
			rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		if (nb_rx == 0)
			continue;

		uint64_t t0 = rte_rdtsc_precise();

		uint16_t nb_out = 0;
		for (uint16_t i = 0; i < nb_rx; i++) {
			uint64_t a0 = rte_rdtsc_precise();
			bool ok = make_echo_reply(bufs[i]);
			g_stats.app_cycles += rte_rdtsc_precise() - a0;
			if (ok)
				out[nb_out++] = bufs[i];
			else {
				g_stats.other_pkts++;
				rte_pktmbuf_free(bufs[i]);
			}
		}

		uint16_t nb_tx = 0;
		if (nb_out > 0)
			nb_tx = rte_eth_tx_burst(port, 0, out, nb_out);

		uint64_t elapsed = rte_rdtsc_precise() - t0;

		if (nb_tx > 0) {
			/* charge the whole burst window to the replies in it */
			uint64_t per = elapsed / nb_tx;
			g_stats.proc_cycles += elapsed;
			g_stats.replies += nb_tx;
			if (per < g_stats.min_cycles)
				g_stats.min_cycles = per;
			if (per > g_stats.max_cycles)
				g_stats.max_cycles = per;
		}

		/* free replies the NIC could not accept */
		for (uint16_t i = nb_tx; i < nb_out; i++)
			rte_pktmbuf_free(out[i]);
	}
}

int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;

	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	argc -= ret;
	argv += ret;

	/* optional: "--port N" after the EAL "--" separator */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
			g_port_id = (uint16_t)atoi(argv[++i]);
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	unsigned nb_ports = rte_eth_dev_count_avail();
	printf("DPDK sees %u ethernet port(s); using port %u.\n",
	       nb_ports, g_port_id);
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No available ethernet ports\n");
	if (!rte_eth_dev_is_valid_port(g_port_id))
		rte_exit(EXIT_FAILURE, "Port %u is not valid\n", g_port_id);

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	if (port_init(g_port_id, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %u\n", g_port_id);

	if (rte_lcore_count() > 1)
		printf("\nWARNING: too many lcores enabled, only 1 is used.\n");

	lcore_main();

	dump_stats();

	rte_eth_dev_stop(g_port_id);
	rte_eth_dev_close(g_port_id);
	rte_eal_cleanup();
	return 0;
}
