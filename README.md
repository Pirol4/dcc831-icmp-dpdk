# DCC831 TP — DPDK ICMP Ping Echo Server

A DPDK application that answers ICMP echo requests (`ping`) directly from
Ethernet frames, without using the Linux network stack. Built on top of
`examples/skeleton` from DPDK.

## Files

| file        | purpose                                                        |
|-------------|---------------------------------------------------------------|
| `main.c`    | the echo server + TSC-based latency instrumentation           |
| `Makefile`  | pkg-config build (with a DPDK 20.08 legacy-build fallback)    |
| `ANSWERS.md`| answers to hand-in questions Q2–Q4                            |

## What the server does

Run-to-completion loop on one lcore, polling **port 1** (the m510 private NIC
port — port 0 stays with Linux for ssh):

1. `rte_eth_rx_burst()` a batch of frames.
2. For each frame: check `ethertype == IPv4`, `ip.proto == ICMP`,
   `icmp.type == echo request (8)`.
3. Rewrite the packet **in place**:
   - swap Ethernet src/dst MAC,
   - swap IPv4 src/dst address, reset TTL, recompute the IPv4 header checksum
     (`rte_ipv4_cksum`),
   - set `icmp.type = echo reply (0)`, recompute the ICMP checksum
     (`rte_raw_cksum`).
4. `rte_eth_tx_burst()` the modified buffers back out port 1.
5. Non-echo packets are freed.

Checksum note: only the ICMP type byte changes (8→0), so an incremental
checksum update would also work; we recompute for clarity.

## Build

On the Cloudlab m510 node, after building DPDK as in the assignment:

```bash
# meson/pkg-config DPDK (recent releases)
export PKG_CONFIG_PATH=$HOME/dpdk/build/meson-uninstalled
make

# --- OR, classic DPDK 20.08 make-based build ---
export RTE_SDK=$HOME/dpdk
export RTE_TARGET=x86_64-native-linuxapp-gcc
make
```

Produces `./icmp-echo` (or `build/icmp-echo` with the legacy build).

## Run — server machine

```bash
# hugepages (already done during setup, repeat if the experiment restarted)
echo 1024 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

sudo ./icmp-echo -l 0 -n 4 -- --port 1
```

`-l 0` pins the poll loop to core 0, `-n 4` sets memory channels, everything
after `--` is app args. With the mlx4 bifurcated driver you do **not** unbind
the kernel driver; DPDK and Linux share the NIC.

Print the port MAC address it reports at startup — you need it for the ARP
entry below.

## Run — client machine (plain Linux, no DPDK)

```bash
# bring up the private interface (name is usually eno1d1 on m510)
sudo ifconfig eno1d1 192.168.1.2 netmask 255.255.255.0

# static ARP so the kernel doesn't need ARP resolution for the server
sudo arp -s 192.168.1.3 <SERVER_PORT1_MAC>

# correctness check
ping -c 5 192.168.1.3

# throughput / loss check for the hand-in
sudo ping -f 192.168.1.3
```

Give the server an address in the same subnet by putting `192.168.1.3` into
`ip->dst_addr` expectations — the server does not actually bind an IP, it
just reflects whatever destination the request carried, so any
`192.168.1.3` request works as long as the ARP entry points at port 1's MAC.

Debug with `sudo tcpdump -i eno1d1 -n icmp` on either side.

## Measurement output

On `Ctrl+C` the server prints, using the CPU time-stamp counter:

```
echo replies sent      : N
cycles / reply  (avg/min/max)
nanoseconds/reply (avg/min/max)   = cycles * 1e9 / rte_get_timer_hz()
```

The timed window is **from `rte_eth_rx_burst()` returning to
`rte_eth_tx_burst()` returning** — i.e. our parsing/crafting plus the DPDK
RX/TX descriptor handling. See `ANSWERS.md` for how this relates to the full
ping RTT and to the raw hardware latency.

## Hand-in checklist

- [ ] `ping` / `ping -f` output from the client (Q1)
- [ ] server-side software+DPDK time vs. rest-of-path time (Q2)
- [ ] how to measure/infer the raw NIC+wire latency (Q2)
- [ ] DPDK stack overhead + is the implementation optimal (Q3, Q4)
- [ ] source code: `main.c`, `Makefile`, `README.md`, `ANSWERS.md` (no DPDK tree)
- [ ] submit to Moodle
