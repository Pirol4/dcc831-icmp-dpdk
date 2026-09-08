#!/usr/bin/env python3
"""
Adversarial / validation probe for the DPDK ICMP echo server (DCC831 TP).

Run this on the CLIENT node (node-1), which must have the private interface
up in 192.168.1.0/24. It crafts raw Ethernet frames straight at the server's
port-1 MAC and checks what comes back (or doesn't), proving that:

  * the DPDK app -- not the Linux kernel -- is what answers;
  * it genuinely parses and re-crafts the packet (recomputes both checksums,
    flips ICMP type 8 -> 0, forces TTL=64) instead of blindly reflecting bytes;
  * it silently ignores every non-"ICMP echo request" frame and does not crash.

Requires scapy:  sudo apt-get install -y python3-scapy   (Ubuntu 18.04)

Usage:
  sudo python3 probe.py --iface eno1d1 --server-mac 14:58:d0:58:fe:23

While this runs, keep `icmp-echo` running on the server. Afterwards, Ctrl+C
the server and check that "other packets dropped" grew by the number of
frames the negative tests sent (7 here).
"""
import argparse
import sys

from scapy.all import Ether, IP, ICMP, UDP, Raw, srp1, get_if_hwaddr, conf

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    ok = bool(cond)
    PASS += ok
    FAIL += not ok
    tag = "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"
    print(f"    [{tag}] {name}" + (f"  ({detail})" if detail else ""))


def cksum_ok(layer_cls, layer):
    """True if `layer`'s on-wire checksum equals a fresh recomputation."""
    on_wire = layer.chksum
    p = layer_cls(bytes(layer))
    p.chksum = None                       # force scapy to recompute on build
    recomputed = layer_cls(bytes(p)).chksum
    return on_wire == recomputed, on_wire, recomputed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", default="eno1d1")
    ap.add_argument("--server-mac", required=True)
    ap.add_argument("--server-ip", default="192.168.1.3")
    ap.add_argument("--client-ip", default="192.168.1.2")
    ap.add_argument("--timeout", type=float, default=1.0)
    args = ap.parse_args()

    conf.verb = 0
    smac = args.server_mac.lower()
    cmac = get_if_hwaddr(args.iface)
    base = Ether(src=cmac, dst=smac)
    print(f"iface {args.iface}  client-mac {cmac}  ->  server-mac {smac}\n")

    def ask(l3):
        return srp1(base / l3, timeout=args.timeout, iface=args.iface, verbose=0)

    # --- 1. sanity: a well-formed echo request must get a well-formed reply --
    print("1. valid echo request  ->  expect a valid echo reply")
    payload = b"DCC831-validation-" + bytes(range(48))
    r = ask(IP(src=args.client_ip, dst=args.server_ip, ttl=9)
            / ICMP(type=8, id=0x4242, seq=7) / Raw(payload))
    check("reply received", r is not None)
    if r is not None and r.haslayer(ICMP):
        check("ICMP type == 0 (echo reply)", r[ICMP].type == 0, f"type={r[ICMP].type}")
        check("IPs swapped", r[IP].src == args.server_ip and r[IP].dst == args.client_ip,
              f"{r[IP].src} -> {r[IP].dst}")
        check("id / seq preserved", r[ICMP].id == 0x4242 and r[ICMP].seq == 7)
        check("payload echoed byte-for-byte", bytes(r[ICMP].payload) == payload)
        ok, ow, rc = cksum_ok(IP, r[IP])
        check("IP header checksum correct", ok, f"0x{ow:04x} vs 0x{rc:04x}")
        ok, ow, rc = cksum_ok(ICMP, r[ICMP])
        check("ICMP checksum correct", ok, f"0x{ow:04x} vs 0x{rc:04x}")
        check("server forced TTL = 64", r[IP].ttl == 64, f"ttl={r[IP].ttl}")
    print()

    # --- 2. bad incoming ICMP checksum: server should still reply, correctly -
    print("2. echo request with a WRONG icmp checksum  ->  reply anyway, with a fixed checksum")
    r = ask(IP(src=args.client_ip, dst=args.server_ip)
            / ICMP(type=8, id=1, seq=1, chksum=0xdead) / Raw(b"x" * 32))
    check("still got a reply (server does not validate input csum)", r is not None)
    if r is not None and r.haslayer(ICMP):
        ok, ow, rc = cksum_ok(ICMP, r[ICMP])
        check("reply ICMP checksum is correct (recomputed, not reflected)", ok,
              f"0x{ow:04x} vs 0x{rc:04x}")
    print()

    # --- 3. reflection is stateless: any dst IP works -----------------------
    print("3. echo request to a made-up dst IP 192.168.1.123  ->  reply from .123")
    r = ask(IP(src=args.client_ip, dst="192.168.1.123")
            / ICMP(type=8, id=2, seq=2) / Raw(b"y" * 16))
    check("reply received", r is not None)
    if r is not None and r.haslayer(IP):
        check("reply src is the made-up IP", r[IP].src == "192.168.1.123", r[IP].src)
    print()

    # --- 4..7 negative tests: these must produce NO reply -----------------
    print("negative tests (expect NO reply, server must not crash):")
    neg = [
        ("4. ICMP echo REPLY (type 0) as input",
         IP(src=args.client_ip, dst=args.server_ip) / ICMP(type=0, id=3, seq=3)),
        ("5. ICMP echo request with code = 5",
         IP(src=args.client_ip, dst=args.server_ip) / ICMP(type=8, code=5, id=4, seq=4)),
        ("6. ICMP timestamp request (type 13)",
         IP(src=args.client_ip, dst=args.server_ip) / ICMP(type=13, id=5, seq=5)),
        ("7. UDP packet to the server",
         IP(src=args.client_ip, dst=args.server_ip) / UDP(sport=1111, dport=9999) / Raw(b"z" * 16)),
        ("8. truncated: IP.len says 200 but frame is tiny",
         IP(src=args.client_ip, dst=args.server_ip, len=200) / ICMP(type=8, id=6, seq=6)),
    ]
    for name, l3 in neg:
        r = srp1(base / l3, timeout=args.timeout, iface=args.iface, verbose=0)
        check(name, r is None, "unexpected reply!" if r is not None else "no reply")

    # non-IPv4 ethertype
    r = srp1(Ether(src=cmac, dst=smac, type=0x88B5) / Raw(b"q" * 40),
             timeout=args.timeout, iface=args.iface, verbose=0)
    check("9. non-IPv4 ethertype (0x88B5)", r is None,
          "unexpected reply!" if r is not None else "no reply")
    print()

    # --- 10. post-crash check: a valid request must still work ------------
    print("10. after all the junk, a valid echo request  ->  server still answers")
    r = ask(IP(src=args.client_ip, dst=args.server_ip)
            / ICMP(type=8, id=0x9999, seq=99) / Raw(b"still-alive"))
    check("server survived and replied", r is not None and r.haslayer(ICMP)
          and r[ICMP].type == 0)
    print()

    print(f"==== {PASS} passed, {FAIL} failed ====")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
