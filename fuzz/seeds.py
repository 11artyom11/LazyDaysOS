#!/usr/bin/env python3
"""Seed inputs for the fuzz targets (see the input formats at the top of each fuzz_*.c).

net / wan: real traffic, the frames check_net.sh's home scenario sent to the
router (qemu_home_{lan,wan}.pcap in the repository root; run make check first),
merged by time, in chunks. The rest is written by hand: HTTP requests that log in
and use the session, TCP segment sequences, DHCP client and server exchanges.
Inputs named found-* (crashes and leaks the fuzzer found, kept as regression
tests) are left alone.
Run from fuzz/: python3 seeds.py
"""
import os
import random
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "fuzz", "corpus")
ROUTER_MACS = {bytes.fromhex("525400123456"), bytes.fromhex("525400123457")}
random.seed(1)


def write(target, name, data):
    d = os.path.join(CORPUS, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def record(op, payload):
    return bytes([op]) + struct.pack("<H", len(payload)) + payload


def read_pcap(path):
    with open(path, "rb") as f:
        data = f.read()
    off, frames = 24, []
    while off + 16 <= len(data):
        sec, usec, incl, _ = struct.unpack("<IIII", data[off:off + 16])
        frames.append((sec + usec / 1e6, data[off + 16:off + 16 + incl]))
        off += 16 + incl
    return frames


# ---- net / wan: captured traffic -------------------------------------------------

def frame_seeds():
    lan = os.path.join(ROOT, "qemu_home_lan.pcap")
    wan = os.path.join(ROOT, "qemu_home_wan.pcap")
    if not (os.path.exists(lan) and os.path.exists(wan)):
        print("no qemu_home_*.pcap: run make check first; net/wan seeds skipped")
        return
    merged = [(t, 0, f) for t, f in read_pcap(lan)] + [(t, 1, f) for t, f in read_pcap(wan)]
    merged = [m for m in merged if m[2][6:12] not in ROUTER_MACS]     # only what the router received
    merged.sort(key=lambda m: m[0])

    def chunks(frames, per, limit, port_override=None):
        out = []
        for i in range(0, len(frames), per):
            part = frames[i:i + per]
            data, prev = b"", part[0][0]
            for t, port, f in part:
                steps = min(31, int((t - prev) * 1000 / 40))
                prev = t
                op = (steps << 3) | 2 | (port if port_override is None else 0)
                data += record(op, f)
            out.append(data)
        if len(out) > limit:            # keep variety: an even sample over the whole run
            out = [out[round(k * (len(out) - 1) / (limit - 1))] for k in range(limit)]
        return out

    for i, s in enumerate(chunks(merged, 12, 120)):
        write("net", f"capture-{i:03d}", s)
    wan_only = [m for m in merged if m[1] == 1]
    for i, s in enumerate(chunks(wan_only, 12, 60, port_override=True)):
        write("wan", f"capture-{i:03d}", s)


# ---- http -------------------------------------------------------------------------

def http_seeds():
    H = "Host: 10.0.9.2\r\n"
    reqs = {
        "dashboard": ["GET / HTTP/1.1\r\n" + H + "Accept-Encoding: gzip\r\n\r\n"],
        "stats-basic": ["GET /api/stats HTTP/1.1\r\n" + H + "Authorization: {AUTH}\r\n\r\n"],
        "events": ["GET /api/events?since=3 HTTP/1.1\r\n" + H + "Authorization: {AUTH}\r\n\r\n"],
        "unauthenticated": ["GET /api/stats HTTP/1.1\r\n" + H + "\r\n"],
        "login-session": [
            "POST /api/login HTTP/1.1\r\n" + H + "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: 22\r\n\r\npassword=fuzz-password",
            "GET /api/session HTTP/1.1\r\n" + H + "Cookie: lazydays_session={SESSION}\r\n\r\n",
            "POST /api/block?ip=10.0.9.77 HTTP/1.1\r\n" + H + "Cookie: lazydays_session={SESSION}\r\n"
            "X-CSRF-Token: {CSRF}\r\nOrigin: http://10.0.9.2\r\nContent-Length: 0\r\n\r\n",
            "POST /api/unblock?ip=10.0.9.77 HTTP/1.1\r\n" + H + "Cookie: lazydays_session={SESSION}\r\n"
            "X-CSRF-Token: {CSRF}\r\nContent-Length: 0\r\n\r\n",
            "POST /api/logout HTTP/1.1\r\n" + H + "Cookie: theme=dark; lazydays_session={SESSION}\r\n"
            "X-CSRF-Token: {CSRF}\r\nContent-Length: 0\r\n\r\n",
        ],
        "password": [
            "POST /api/password HTTP/1.1\r\n" + H + "Authorization: {AUTH}\r\nContent-Length: 49\r\n\r\n"
            "current=fuzz-password&new=a%20new+password%21%21",
        ],
        "guessing": ["POST /api/login HTTP/1.1\r\n" + H + "Content-Length: 12\r\n\r\npassword=%%%"] * 6,
        "csrf-and-rebinding": [
            "POST /api/block?ip=10.0.9.5 HTTP/1.1\r\n" + H + "Authorization: {AUTH}\r\nOrigin: http://evil.example\r\n\r\n",
            "GET /api/stats HTTP/1.1\r\nHost: evil.example:80\r\nAuthorization: {AUTH}\r\n\r\n",
            "GET /api/stats HTTP/1.1\r\nAuthorization: Basic !!!!\r\n\r\n",
        ],
        "errors": [
            "HELLO\r\n\r\n",
            "GET / HTTP/1.1\r\n" + H + "X-Pad: " + "a" * 3000 + "\r\n\r\n",
            "POST /api/block HTTP/1.1\r\n" + H + "Content-Length: 99999\r\n\r\n",
            "DELETE /api/stats HTTP/1.1\r\n" + H + "\r\n",
            "GET /nope?x=1&y HTTP/1.1\r\n" + H + "\r\n",
            "GET / HTTP/1.1\r\n",
        ],
    }
    for name, requests in reqs.items():
        data = b""
        for k, r in enumerate(requests):
            data += bytes([k * 7 % 32]) + struct.pack("<H", len(r)) + r.encode()
        write("http", name, data)


# ---- tcp --------------------------------------------------------------------------

def tcp_seeds():
    ACK, PSH, FIN, RST, SYN = 0x10, 0x08, 0x01, 0x04, 0x02
    req = b"GET / HTTP/1.1\r\nHost: 10.0.9.2\r\n\r\n"

    def seg(flags, seq=0, ack=0, window=65535, wait=0, payload=b""):
        return struct.pack("<BhhHBB", flags, seq, ack, window, wait, len(payload)) + payload

    write("tcp", "request-in-order", b"\0" + seg(ACK | PSH, payload=req) + seg(ACK, wait=5) + seg(ACK | FIN, wait=10))
    write("tcp", "out-of-order", b"\0" + seg(ACK, seq=10, payload=req[10:]) + seg(ACK, payload=req[:10], wait=3))
    write("tcp", "reset-in-window", b"\0" + seg(ACK | PSH, payload=req[:5]) + seg(RST, seq=0, wait=1))
    write("tcp", "zero-window", b"\0" + seg(ACK | PSH, payload=req, window=0) + seg(ACK, window=0, wait=50)
          + seg(ACK, window=4096, wait=50))
    write("tcp", "syn-unanswered", b"\1" + seg(SYN, seq=-1) + seg(ACK, ack=0, wait=60))
    write("tcp", "duplicate-acks", b"\0" + seg(ACK | PSH, payload=req, window=1460) + seg(ACK, ack=-1000)
          + seg(ACK, ack=-1000) + seg(ACK, ack=-1000, wait=10))


# ---- dhcp -------------------------------------------------------------------------

def dhcp(op, msg_type, xid=0x1234, chaddr=b"\x02\x00\x00\x00\x10\x01", ciaddr=0, yiaddr=0, options=b""):
    fixed = struct.pack("!BBBBIHHIIII", op, 1, 6, 0, xid, 0, 0x8000, ciaddr, yiaddr, 0, 0)
    fixed += chaddr + bytes(10) + bytes(64) + bytes(128) + struct.pack("!I", 0x63825363)
    return fixed + bytes([53, 1, msg_type]) + options + bytes([255])


def ip(s):
    return bytes(int(x) for x in s.split("."))


def dhcp_seeds():
    hostname = bytes([12, 9]) + b"fuzz-host"
    cid = bytes([61, 7, 1]) + b"\x02\x00\x00\x00\x10\x01"
    params = bytes([55, 4, 1, 3, 6, 51])
    discover = dhcp(1, 1, options=hostname + cid + params)
    select = dhcp(1, 3, options=bytes([50, 4]) + ip("10.0.9.100") + bytes([54, 4]) + ip("10.0.9.2") + hostname)
    reboot = dhcp(1, 3, options=bytes([50, 4]) + ip("10.0.9.120"))
    renew = dhcp(1, 3, ciaddr=0x0A000964, options=hostname)
    release = dhcp(1, 7, ciaddr=0x0A000964, options=bytes([54, 4]) + ip("10.0.9.2"))
    decline = dhcp(1, 4, options=bytes([50, 4]) + ip("10.0.9.100") + bytes([54, 4]) + ip("10.0.9.2"))
    inform = dhcp(1, 8, ciaddr=0x0A000932)
    write("dhcp_server", "discover-request", record(0, discover) + record(0, select))
    write("dhcp_server", "init-reboot", record(0, reboot))
    write("dhcp_server", "renew-release", record(0, discover) + record(0, select) + record(1 | (40 << 1), renew)
          + record(1, release))
    write("dhcp_server", "decline-inform", record(0, discover) + record(0, decline) + record(1, inform))
    write("dhcp_server", "lease-expiry", record(0, discover) + record(0, select) + record(127 << 1, discover))

    common = bytes([54, 4]) + ip("10.0.8.1") + bytes([1, 4]) + ip("255.255.255.0") + bytes([3, 4]) + ip("10.0.8.1") \
        + bytes([6, 8]) + ip("10.0.8.53") + ip("8.8.8.8")
    offer = dhcp(2, 2, yiaddr=0x0A000802, options=common + bytes([51, 4]) + struct.pack("!I", 600))
    ack = dhcp(2, 5, yiaddr=0x0A000802, options=common + bytes([51, 4]) + struct.pack("!I", 120)
               + bytes([58, 4]) + struct.pack("!I", 60) + bytes([59, 4]) + struct.pack("!I", 100))
    nak = dhcp(2, 6, options=bytes([54, 4]) + ip("10.0.8.1"))
    write("dhcp_client", "offer-ack", record(3, offer) + record(3 | (1 << 2), ack))
    write("dhcp_client", "offer-ack-renew", record(3, offer) + record(3, ack) + record(3 | (62 << 2), ack)
          + record(3 | (63 << 2), ack))
    write("dhcp_client", "nak", record(3, offer) + record(3, nak) + record(3 | (5 << 2), offer))
    write("dhcp_client", "wrong-xid", record(2, offer) + record(0, ack))


# ---- rip --------------------------------------------------------------------------

def rip_seeds():
    def entry(net, mask, metric, next_hop="0.0.0.0", afi=2):
        return struct.pack("!HH4s4s4sI", afi, 0, ip(net), ip(mask), ip(next_hop), metric)

    def msg(command, *entries):
        return struct.pack("!BBH", command, 2, 0) + b"".join(entries)

    learn = msg(2, entry("192.168.70.0", "255.255.255.0", 1), entry("192.168.71.0", "255.255.255.0", 3),
                entry("0.0.0.0", "0.0.0.0", 2), entry("172.16.0.0", "255.240.0.0", 15))
    worse = msg(2, entry("192.168.70.0", "255.255.255.0", 5))
    withdraw = msg(2, entry("192.168.70.0", "255.255.255.0", 16), entry("192.168.71.0", "255.255.255.0", 16))
    full_table = msg(1, struct.pack("!HH4s4s4sI", 0, 0, bytes(4), bytes(4), bytes(4), 16))
    specific = msg(1, entry("192.168.70.0", "255.255.255.0", 0), entry("10.0.8.0", "255.255.255.0", 0))
    bad = msg(2, entry("127.0.0.0", "255.0.0.0", 1), entry("10.1.2.3", "255.255.255.0", 1),
              entry("10.2.0.0", "255.0.255.0", 1), entry("10.3.0.0", "255.255.0.0", 0),
              entry("10.4.0.0", "255.255.0.0", 1, afi=0xFFFF))
    next_hop = msg(2, entry("192.168.72.0", "255.255.255.0", 1, next_hop="10.0.9.3"))
    write("rip", "learn-then-timeout", record(0, learn) + record(7 << 5, b"") + record(7 << 5, b""))
    write("rip", "better-worse-withdraw", record(0, learn) + record(1 << 2, worse) + record(0, worse) + record(0, withdraw))
    write("rip", "requests", record(0, full_table) + record(1, specific) + record(0, learn) + record(0, full_table))
    write("rip", "invalid-entries", record(0, bad) + record(2, learn) + record(3 << 2, learn))
    write("rip", "link-flap", record(0, learn) + record(0x10, b"") + record(0x10 | (2 << 5), learn) + record(1, next_hop))
    write("rip", "both-ports", record(0, learn) + record(1, learn) + record(1 | (1 << 2), withdraw))


if __name__ == "__main__":
    # Regenerate the seeds; keep inputs the fuzzer found (found-*): they are regression tests
    for target in os.listdir(CORPUS) if os.path.isdir(CORPUS) else []:
        for name in os.listdir(os.path.join(CORPUS, target)):
            if not name.startswith("found-"):
                os.remove(os.path.join(CORPUS, target, name))
    frame_seeds()
    http_seeds()
    tcp_seeds()
    dhcp_seeds()
    rip_seeds()
    for t in sorted(os.listdir(CORPUS)):
        print(f"{t}: {len(os.listdir(os.path.join(CORPUS, t)))} seeds")
