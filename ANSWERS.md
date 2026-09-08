# DCC831 TP — Hand-in Answers

Experiment: two Cloudlab **m510** nodes (8-core Xeon D-1548 @ 2.0 GHz,
ConnectX-3 10 GbE), directly connected on the private port `eno1d1`, forced
onto the same physical switch (interswitch mapping disabled). DPDK 20.08,
mlx4 PMD (bifurcated driver).

* **server** (node-0): runs `icmp-echo` on DPDK **port 1** (`eno1d1`,
  MAC `14:58:d0:58:fe:23`). Port 0 (`eno1`) stays with Linux for ssh.
* **client** (node-1): plain Linux, `eno1d1` = `192.168.1.2/24`, static ARP
  `192.168.1.3 -> 14:58:d0:58:fe:23`, generates traffic with `ping`.

Raw logs: `results/run1-client-ping.txt`, `results/run1-server-stats.txt`.

---

## Q1 — `ping` output from the client

```
# ping -c 5 192.168.1.3   (cold)
rtt min/avg/max/mdev = 0.040/0.051/0.078/0.014 ms

# sudo ping -f 192.168.1.3   (~15.7 s)
353174 packets transmitted, 353174 received, 0% packet loss, time 15737ms
rtt min/avg/max/mdev = 0.007/0.010/0.424/0.005 ms, ipg/ewma 0.044/0.010 ms
```

The DPDK echo server answered **353179 / 353179** ICMP echo requests
(353174 flood + 5 from the cold run) with **0 % packet loss**. Server-side
statistics (TSC based, 2.0 GHz):

| metric (per echo reply)                    | value          |
|--------------------------------------------|---------------:|
| rx_burst→tx_burst window, avg              | 606 cyc / **303 ns** |
| rx_burst→tx_burst window, min              | 505 cyc / 253 ns |
| rx_burst→tx_burst window, max (outlier)    | 488313 cyc / 244 µs |
| ├─ parse + craft (app logic), avg          | 153 cyc / **77 ns** |
| └─ DPDK rx/tx descriptor+doorbell, avg     | 453 cyc / **227 ns** |

The cold `ping -c 5` averages 51 µs because the `ping` process is scheduled
back in from sleep for each of the first few packets; the **flood average of
10 µs (min 7 µs)** is the representative steady-state round trip and is used
below.

### Correctness validation (`tests/probe.py`, `results/run2-probe.txt`)

A scapy probe from the client crafts raw frames and checks the replies —
**19/19 checks pass**:

* the reply is a real re-craft, not a byte reflection: ICMP type flipped
  8→0, **both checksums recomputed and valid** (verified independently), TTL
  forced to 64, id/seq and the full payload preserved, src/dst IP swapped;
* it still answers a request carrying a deliberately **wrong** ICMP checksum,
  and the reply's checksum is correct — proof it recomputes rather than
  copies;
* reflection is stateless — a request to a fabricated dst IP is answered from
  that IP;
* every non-echo-request frame is **silently dropped without crashing**: an
  ICMP echo *reply*, echo request with code≠0, timestamp request, UDP,
  a truncated IP packet, a non-IPv4 ethertype. The server's
  `other packets dropped` counter accounts for them.

It is definitely the DPDK app answering, not the kernel: with `icmp-echo`
stopped, `ping 192.168.1.3` gets 100 % loss (no IP bound, no ARP handling on
the server); deleting the client's static ARP entry also kills it, because
the server never answers ARP.

---

## Q2 — Time in our software + DPDK vs. the rest of the path. How to measure/infer the raw hardware latency?

### Where the 10 µs round trip goes

```
                                        time    share of RTT
client: sendto() -> kernel v4/ICMP -> mlx4 xmit -> doorbell   ─┐
wire + switch  client -> server            ~0.1 µs            │  ~9.5 µs
server NIC RX: PCIe DMA into mbuf + mlx4 PMD ring handling    │   (~95 %)
>>> our TSC window opens (rx_burst returned)                 ─┘
parse ETH/IP/ICMP, swap MACs+IPs, 2 checksums     0.077 µs    0.8 %
DPDK tx_burst: build WQE, mbuf refcount, TX doorbell 0.227 µs  2.3 %
>>> our TSC window closes (tx_burst returned)
server NIC serialises the frame, wire+switch back  ~0.1 µs    ~1 %
client NIC RX -> IRQ/NAPI -> skb -> socket match, ping reads  ─ (in the 9.5 µs)
```

* **Our software + DPDK RX/TX path (measured, server side):**
  **303 ns avg** (253 ns min). That is **~3 %** of the 10 µs round trip
  (≈ 3.6 % of the 7 µs best case).
* **Everything else** = `RTT − 303 ns` ≈ **9.7 µs (flood avg)** / **6.7 µs
  (flood min)**. This is dominated by the **client's Linux networking stack**
  (send path + receive path, no kernel-bypass on the client), plus both NICs'
  RX/TX engines + PCIe DMA, plus a negligible amount of wire.
* **Wire is not the bottleneck:** a 98-byte ICMP frame (+20 B preamble/IFG)
  at 10 Gbit/s serialises in **~94 ns** one way, ~0.19 µs for the round trip.

So on this link the ICMP round trip is **almost entirely CPU/stack time on
the client**, and our DPDK server is a ~3 % slice of it.

### How to measure or infer the *raw networking hardware* latency

The client RTT still bundles the client kernel, the two NICs and the wire.
To isolate the hardware part:

1. **DPDK-only loopback (no kernel anywhere).** Have the DPDK app transmit a
   probe frame on port 1 and receive it back (loop the link through the
   switch, or a physical loopback), timestamping with `rte_rdtsc_precise()`
   just before `tx_burst` and just after the frame reappears on `rx_burst`.
   The result is `2·(NIC_tx + wire + NIC_rx)` with **zero OS stack**; half of
   it is the one-way hardware latency. (Requires a loop path — see
   "additional experiment" note in the README.)

2. **Instrument the mlx4 PMD (assignment hint).** In
   `drivers/net/mlx4/mlx4_rxtx.c`, read the TSC inside `mlx4_rx_burst()` at
   the point the PMD reads the completion-queue entry, and stash it in an
   mbuf dynfield. Comparing it with the TSC at `rx_burst` return isolates the
   PMD/RX-ring cost from the DMA the CPU can't observe; the same around the
   TX WQE-post / CQE path isolates `tx_burst`'s hardware-facing cost.

3. **ConnectX-3 hardware RX timestamps.** Enable
   `RTE_ETH_RX_OFFLOAD_TIMESTAMP`; the NIC stamps each frame with its own
   clock at wire arrival. `mbuf_timestamp` vs. TSC at `rx_burst` return
   measures RX DMA + ring latency directly, in NIC-clock units.

4. **Same-switch vs. inter-switch delta.** Re-run `ping -f` with "allow
   interswitch mapping" enabled (extra switch hop) and subtract from the
   same-switch RTT — the difference is one switch's store-and-forward plus
   one extra cable, i.e. the per-hop wire+switch latency in isolation.

5. **Kernel-server baseline (cheap, isolates the server's own stack cost).**
   Stop the DPDK app, give the server `192.168.1.3/24` on `eno1d1`, `ping -f`
   again: the RTT increase over the DPDK run is `(kernel echo path) −
   (DPDK echo path)` on the server, which bounds how much of the 9.7 µs
   "rest" is a Linux stack traversal.

---

## Q3 — Overhead introduced by the DPDK software stack

Measured on the server, per echo reply (TSC @ 2.0 GHz):

| component                          | cycles | ns   | share |
|------------------------------------|-------:|-----:|------:|
| parse + craft (`make_echo_reply`)  | 153    | 77   | 25 %  |
| `rx_burst` + `tx_burst` (DPDK)     | 453    | 227  | 75 %  |
| **total (rx_burst→tx_burst)**      | 606    | 303  | 100 % |

* **App logic (77 ns).** Header validation, one `rte_ether_addr` swap, one
  IPv4 address swap, `rte_ipv4_cksum` over 20 B, `rte_raw_cksum` over the
  ~64-byte ICMP message. The two checksum passes are the bulk of it.
* **DPDK RX/TX path (227 ns).** `rte_eth_rx_burst` / `rte_eth_tx_burst` are
  thin inlined wrappers, but each touches the mlx4 completion & work queues
  in host memory, manages mbuf refcounts, and — the expensive part — issues
  an **MMIO doorbell write** to the NIC on TX (~100+ ns by itself). This is
  the irreducible cost of talking to the hardware from user space.
* **Measurement caveat.** The per-packet nested `rte_rdtsc_precise()` used to
  split app vs. DPDK adds ~2 serialising reads (~20–40 cyc) that land in the
  "DPDK path" bucket, so the true split is a few ns more toward the app side.

**Versus the kernel.** The DPDK server contributes 303 ns with ~50 ns of
jitter (min 253, avg 303), excluding rare scheduler outliers. A kernel ICMP
reply for the same packet costs an interrupt + softirq + `sk_buff`
alloc/free + protocol demux — typically **single-digit microseconds** and far
more jittery. DPDK does not *add* overhead here; it *removes* the kernel's,
trading it for a ~230 ns poll-mode descriptor/doorbell path. (Run experiment
5 in Q2 to quantify the kernel baseline on this exact hardware.)

---

## Q4 — Is the implementation optimal? What could be improved?

It is efficient but **not optimal**. Ranked by expected benefit:

1. **Incremental ICMP checksum.** Only the type byte changes (8 → 0), so the
   full `rte_raw_cksum` over 64 B can be replaced by a single 16-bit
   add-with-carry on the checksum field (`csum += ~htons(0x0800); fold`).
   Removes most of the app-logic cost.

2. **NIC checksum offload for IPv4.** Set
   `RTE_ETH_TX_OFFLOAD_IPV4_CKSUM` + `mbuf->ol_flags |= PKT_TX_IP_CKSUM`
   and let the ConnectX-3 fill the IPv4 header checksum, dropping the
   `rte_ipv4_cksum` pass from the CPU. (ICMP has no HW offload on this NIC —
   hence point 1.)

3. **TX doorbell amortisation.** Under the flood the RX burst already carries
   many packets, so one `tx_burst` per burst already amortises the doorbell;
   adding `rte_eth_tx_buffer` with an explicit flush threshold would help at
   lower/bursty rates. Prefetch the next mbuf's header (`rte_prefetch0`)
   while crafting the current reply.

4. **Drop the per-packet nested timing** in production runs (keep it only for
   a calibration pass) — it costs 2 serialised TSC reads per packet.

5. **Core isolation.** Boot the server with `isolcpus`/`nohz_full` on core 0
   (or run under `taskset` + `chrt`) to remove the 244 µs scheduler outliers
   seen in `max`.

6. **NUMA.** Already correct here (single socket); the app warns if the port
   is on a remote node.

7. **Multi-queue / RSS + more lcores** — pure throughput headroom. One m510
   core sustained the flood (22 kpps here, and would handle far more) so
   this is not needed for ICMP echo, but it is the path to line rate.

**Bigger picture.** Our server is already only ~3 % of the round trip. The
dominant cost is the **client's Linux stack**; the highest-impact change for
end-to-end latency would be to bypass the kernel on the *client* too (a DPDK
packet generator), or to reduce hardware/wire latency — neither is a change
to this code. Within this code, points 1 + 2 are the clear wins.
