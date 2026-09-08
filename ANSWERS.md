# DCC831 TP — Hand-in Answers

> Fill in the bracketed numbers after running the experiment on Cloudlab.
> The reasoning/method is written out; only the measured values are missing.

## Q1 — `ping` output from the client

Paste the output of `ping -c 20 192.168.1.3` and `sudo ping -f 192.168.1.3`
(run for ~10 s, then Ctrl+C). Expected: 0% packet loss, RTT on the order of
tens of microseconds over the direct 10 GbE link.

```
<paste ping output here>
```

Also paste the server's statistics block (printed on Ctrl+C).

---

## Q2 — Time in our software + DPDK vs. the rest of the path. How to measure/infer the raw hardware latency?

### Decomposition of one echo round trip

```
client kernel: build request, hand to NIC        ] t_client_tx
  wire + switch  client -> server                 ] t_wire/2
  server NIC RX (DMA into mbuf) + mlx4 PMD        ] t_nic_rx
  >>> our timed window starts (rx_burst returns)
  parse ETH/IP/ICMP, swap fields, 2 checksums     ] t_sw
  DPDK tx_burst -> descriptor write -> NIC        ] t_tx_path
  >>> our timed window ends (tx_burst returns)
  server NIC actually serializes the frame        ] t_nic_tx
  wire + switch  server -> client                 ] t_wire/2
  client NIC RX + kernel matches reply, stops RTT ] t_client_rx
```

`ping` RTT (client) ≈
`t_client_tx + t_client_rx + t_wire + t_nic_rx + t_nic_tx + t_sw + t_tx_path`.

### What we measure directly

* **`t_sw + t_tx_path`** — the "our software + DPDK" number — is measured on the
  server with `rte_rdtsc_precise()` around the loop body
  (`rx_burst` return → `tx_burst` return), converted with
  `microseconds = cycles * 1e6 / rte_get_timer_hz()`.
  Measured: **avg [___] ns, min [___] ns, max [___] ns** per reply.

* **Full RTT** — from the client's `ping` output: **avg [___] µs**.

* **Rest of the path** (wire + both NICs' RX/TX + both kernel stacks on the
  client) = `RTT − (t_sw + t_tx_path)` = **[___] µs**.
  This is the great majority of the RTT; our processing is a small slice.

### Isolating / inferring the raw networking hardware latency

The RTT still lumps together the client's Linux stack, the two NICs, and the
wire. Ways to separate the hardware part:

1. **Loopback subtraction.** Connect the server's port 1 to itself (or to the
   switch and back) and have the DPDK app both send and receive. Timestamp a
   packet with the TSC just before `tx_burst` and just after it comes back on
   `rx_burst`; that value is `2·(t_nic_tx + t_wire/2 + t_nic_rx)` with **no
   Linux stack and no second machine** involved. Half of it is the one-way
   hardware latency.

2. **Instrument the mlx4 PMD (assignment hint).** Add a `rte_rdtsc()` read
   inside `mlx4_rx_burst()` in `drivers/net/mlx4/mlx4_rxtx.c`, right where the
   PMD reads the completion queue entry, and stamp it into the mbuf (e.g.
   `mbuf->udata64` or a dynfield). Comparing that to the TSC value at
   `rx_burst` return in the app gives the PMD/RX-ring contribution
   (`t_nic_rx` minus the DMA time the CPU can't see). Doing the same around the
   TX completion path gives `t_tx_path`.

3. **NIC hardware timestamps.** ConnectX-3 can PTP-timestamp frames on RX
   (`RTE_ETH_RX_OFFLOAD_TIMESTAMP` / `rte_mbuf_dynfield` timestamp).
   `mbuf timestamp` (NIC clock, set at wire arrival) vs. TSC at `rx_burst`
   return measures RX DMA + ring latency in NIC-clock units.

4. **Switch/cable delta.** Measure RTT with the "allow interswitch mapping"
   box unchecked (same switch, ~one hop) vs. checked (extra switch hop); the
   difference is roughly one switch's store-and-forward + one extra cable,
   isolating per-hop wire+switch latency. Also compare against the theoretical
   serialization time: a 98-byte ICMP frame at 10 Gbps ≈ 78 ns on the wire.

### Answer to write up

> Our software + DPDK RX/TX path costs **[___] ns** per echo, which is
> **[___]%** of the **[___] µs** ping RTT. The remaining **[___] µs** is spent
> in the client's Linux networking stack (dominant), the two NICs' RX/TX
> engines, and ~[___] ns of wire/switch serialization. We infer the raw
> hardware latency with a DPDK-only loopback measurement (method 1), which
> yields a one-way NIC+wire latency of **[___] ns**.

---

## Q3 — Overhead introduced by the DPDK software stack

Within the timed window, the cost splits into:

* **Application logic** (`make_echo_reply`): header validation, two
  `rte_ether_addr` copies + one address swap, one IPv4 address swap, one
  `rte_ipv4_cksum` over 20 bytes, one `rte_raw_cksum` over the ICMP message
  (typically 64 bytes for default `ping`). All L1-cache resident, branch-light:
  order of **tens of ns**.

* **DPDK RX/TX path**: `rte_eth_rx_burst`/`rte_eth_tx_burst` are thin inlined
  wrappers, but each call touches the mlx4 completion & work queues in host
  memory, writes a doorbell (MMIO), and manages mbuf refcounts. This
  descriptor/doorbell handling is the bulk of the DPDK-attributable overhead.

Estimate the split by also timing *just* `make_echo_reply` with a nested
`rte_rdtsc_precise()` pair:

| component                     | cycles | ns   |
|-------------------------------|-------:|-----:|
| parse + craft (app)           | [___]  | [___]|
| rx_burst + tx_burst (DPDK)    | [___]  | [___]|
| **total window**              | [___]  | [___]|

Compare with a kernel-based baseline: run the server as a normal Linux host
(`ping` answered by the kernel) and read RTT — the DPDK version should show a
markedly lower and far more stable (low max−min) server contribution because
there is no interrupt, no softirq, no syscall, no sk_buff allocation, and no
context switch. Report the RTT distribution (avg + jitter) for both.

> The DPDK stack adds **[___] ns** per packet, almost entirely descriptor-ring
> and doorbell handling; the kernel path adds **[___] µs** and much higher
> jitter for the same work.

---

## Q4 — Is the implementation optimal? What could be improved?

Not fully optimal. Reasonable, but the following would reduce latency and/or
raise throughput:

1. **Checksum offload.** Let the NIC compute the IPv4 header checksum
   (`RTE_ETH_TX_OFFLOAD_IPV4_CKSUM` + `mbuf->ol_flags`), removing a pass over
   the header on the CPU. ICMP checksum has no HW offload on ConnectX-3, but
   an **incremental update** (only the type byte changed 8→0) replaces the
   full `rte_raw_cksum` with a single 16-bit add + fold.

2. **Avoid the burst-window averaging.** We currently charge one
   `rx→tx` window to a whole burst; per-packet TSC stamping (at a small
   measurement cost) gives a true per-packet distribution.

3. **Batching / prefetch.** Prefetch the next mbuf's header
   (`rte_prefetch0`) while processing the current one; split the loop into
   classify then transform then a single `tx_burst` (already done) to keep the
   TX doorbell amortized over the burst.

4. **Larger RX burst + TX buffering** (`rte_eth_tx_buffer`) under flood load to
   amortize doorbell writes further; tune `nb_rxd`/`nb_txd`.

5. **Multi-queue / RSS + multiple lcores** if a single core saturates — for
   ICMP echo one m510 core is nowhere near the 10 GbE packet rate, so this is
   throughput headroom, not latency.

6. **Drop promiscuous mode** once the MACs are fixed; not required here.

7. **NUMA correctness**: pin the lcore and the mbuf pool to the NIC's socket
   (the app already warns if the port is on a remote node).

> Our per-packet server cost is small relative to the RTT, so the biggest
> real-world win is on the *client* side (also bypass the kernel with DPDK) or
> in the hardware/wire, not in this code. Within this code, checksum offload +
> incremental ICMP checksum is the highest-value change.
