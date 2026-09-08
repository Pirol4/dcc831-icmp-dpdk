# Validation & baseline experiments

All of this runs against a live experiment: `icmp-echo` on the **server**
(node-0, DPDK port 1) and the **client** (node-1) with `eno1d1` =
`192.168.1.2/24` and the static ARP entry for `192.168.1.3`.

---

## A. Quick checks with plain `ping` (client)

These already tell you a lot:

| command | what a PASS proves |
|---|---|
| `ping -c 5 192.168.1.3` | server flips type 8→0 **and** writes a valid ICMP checksum — Linux drops replies whose checksum is wrong |
| `ping -c 20 -p a5a5a5a5 192.168.1.3` | full payload echoed intact (no "wrong data byte" lines) |
| `ping -c 10 -s 1400 192.168.1.3` | large (near-MTU) packets handled |
| `ping -c 5 -s 4000 192.168.1.3` | **expected to FAIL / lose packets** — the server does not reassemble IP fragments (documented limitation) |
| `sudo tcpdump -i eno1d1 -n -e icmp` on the client | on the wire you see the request go to the server MAC and the reply come back *from* it as `echo reply` — MAC swap + type change confirmed |

### Prove it is the DPDK app, not the kernel

1. On the server press **Ctrl+C** to stop `icmp-echo`.
2. On the client: `ping -c 3 192.168.1.3` → **100 % packet loss** (nothing on
   the server owns that IP or answers ARP).
3. Restart `icmp-echo` on the server → pings succeed again.

Also: on the client `sudo arp -d 192.168.1.3` then `ping -c 3 192.168.1.3` →
fails, because the server has **no ARP handling** and the kernel now has no
MAC for `.3`. Re-add with `sudo arp -s 192.168.1.3 14:58:d0:58:fe:23`.

---

## B. Adversarial probe (`probe.py`, client)

Crafts raw frames with scapy and checks the replies. Covers: correct
re-crafting (checksums recomputed not reflected, TTL forced to 64, id/seq and
payload preserved), stateless reflection to any dst IP, and silent drop of
echo *replies*, bad ICMP codes, timestamp requests, UDP, truncated IP, and
non-IPv4 ethertypes.

```bash
# on the client (node-1)
sudo apt-get install -y python3-scapy
cd ~/dcc831-icmp-dpdk/tests
sudo python3 probe.py --iface eno1d1 --server-mac 14:58:d0:58:fe:23
```

Expected: `==== N passed, 0 failed ====`.

After it finishes, **Ctrl+C the server** and confirm the
`other packets dropped` counter grew by ~6 (the negative-test frames) — that
is the server telling you it saw the junk and chose not to answer it.

Save the output to `results/` for the write-up:

```bash
sudo python3 probe.py --iface eno1d1 --server-mac 14:58:d0:58:fe:23 \
    | tee ../results/run2-probe.txt
```

---

## C. Kernel-echo baseline (for Q2 / Q3)

Measures the round trip when the server's **Linux kernel** answers, to
compare against the DPDK server (303 ns of server-side work).

**Server (node-0)** — `icmp-echo` NOT running:

```bash
sudo ip addr add 192.168.1.3/24 dev eno1d1    # kernel now owns .3 and will ping-reply
sudo sysctl -w net.ipv4.icmp_echo_ignore_all=0
```

**Client (node-1):**

```bash
sudo ping -f -w 15 192.168.1.3 | tee ~/dcc831-icmp-dpdk/results/run3-kernel-baseline.txt
```

**Restore (server):**

```bash
sudo ip addr del 192.168.1.3/24 dev eno1d1
cd ~/dcc831-icmp-dpdk && sudo ./build/icmp-echo -l 0 -n 4 -- --port 1
```

Interpretation: `Δ = kernel_avg_RTT − dpdk_avg_RTT (10 µs)` is roughly the
extra latency of a full kernel network-stack traversal on the server for one
ICMP echo. Put the number in `ANSWERS.md` Q3.
