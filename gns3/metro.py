"""The metro network(s): one definition for the builder (metro_lab.py) and the dashboard (net_ui.py).

One metro is a provider-style network of LazyDaysOS routers speaking RIP, built so
that every customer has redundant paths:

    core         C1..C4    full mesh (6 links); server S1 on C1, the NOC host on C2
    aggregation  A1..A8    each connected to two core routers
    access       X1..X16   each connected to two aggregation routers, one customer each

With metros=3 three of them form a mesh of meshes: their core routers are joined in
a triangle by backbone links (Mm-C3 to the next metro, Mm-C4 to the previous one),
so every pair of metros has a direct link and a backup path through the third.
Names get a metro prefix ("M2-C1"). Half the customers use their own metro's server,
half the next metro's, so backbone failures and attacks hit real traffic.

Addresses: metro m owns 10.(16m).0.0/12, so each metro can later be summarized as
one route (two /24 networks per link or LAN, all inside that block):
    10.(16m+0).<ij>.0/24     core Ci - Cj               (.1 Ci, .2 Cj)
    10.(16m+1).<10c+k>.0/24  core Cc - aggregation Ak   (.1 Cc, .2 Ak)
    10.(16m+2).<20a+x>.0/24  aggregation Aa - access Xx (.1 Aa, .2 Xx)
    10.(16m+3).<x>.0/24      customer LAN of Xx         (.1 Xx, .2 Hx)
    10.(16m+4).1.0/24        S1 on C1 (.10);  .2.0 NOC on C2 (.10);  .3.0 S2 on C3 (one metro only)
    10.0.<ab>.0/24           backbone metro a - metro b (.1 a, .2 b)
"""
import math

CORES, AGGS, ACCESS = 4, 8, 16
PASSWORD = "lazydays-lab"
TIMERS = "5,30,20"                      # RIP update/timeout/garbage in seconds: faster than RFC's 30/180/120
CENTERS = {1: [(0, 0)], 3: [(0, -1120), (-970, 560), (970, 560)]}


def prefix(m, metros):
    return f"M{m}-" if metros > 1 else ""


def block(m):
    """The metro's address block (for summarization)"""
    return f"10.{16 * m}.0.0/12"


def spec(metros=1):
    """Who plays which role in the scenarios"""
    p = prefix(1, metros)
    return {"metros": metros,
            "misconfigurable": f"{p}X7", "hijacker": f"{p}H7",
            "victim_server": f"{p}S1", "victim_lan": f"10.{16 + 4}.1.0/24",
            "servers": {f"{prefix(m, metros)}S1": f"10.{16 * m + 4}.1.10" for m in range(1, metros + 1)},
            "nocs": [f"{prefix(m, metros)}NOC" for m in range(1, metros + 1)]}


def topology(metros=1, timers=TIMERS):
    """routers: name -> {role, metro, x, y, ports: [(cidr, peer)], rip_ports: [ethN], cmdline}
    links: [(a, a_port, b, b_port, network)]
    hosts: name -> {router, port, cidr, gateway, role, metro, x, y, probe (customers: server address)}"""
    if metros not in CENTERS:
        raise ValueError("metros: 1 or 3")
    routers, links, hosts = {}, [], {}

    def connect(a, b, network):
        pa, pb = len(routers[a]["ports"]), len(routers[b]["ports"])
        routers[a]["ports"].append((f"{network}.1/24", b))
        routers[b]["ports"].append((f"{network}.2/24", a))
        routers[a]["rip_ports"].append(f"eth{pa}")
        routers[b]["rip_ports"].append(f"eth{pb}")
        links.append((a, pa, b, pb, f"{network}.0/24"))

    def lan(router, network, host, host_addr, role, m, radius_out, center):
        port = len(routers[router]["ports"])
        routers[router]["ports"].append((f"{network}.1/24", host))
        r = routers[router]
        angle = math.atan2(r["y"] - center[1], r["x"] - center[0])
        hosts[host] = {"router": router, "port": port, "cidr": f"{network}.{host_addr}/24",
                       "gateway": f"{network}.1", "role": role, "metro": m,
                       "x": round(r["x"] + radius_out * math.cos(angle)), "y": round(r["y"] + radius_out * math.sin(angle))}

    for m in range(1, metros + 1):
        p, base, (cx, cy) = prefix(m, metros), 16 * m, CENTERS[metros][m - 1]

        def place(name, role, radius, angle):
            routers[p + name] = {"role": role, "metro": m, "x": round(cx + radius * math.cos(angle)),
                                 "y": round(cy + radius * math.sin(angle)), "ports": [], "rip_ports": []}

        for i in range(1, CORES + 1):
            place(f"C{i}", "core", 150, math.radians(-135 + 90 * (i - 1)))
        for k in range(1, AGGS + 1):
            place(f"A{k}", "aggregation", 330, math.radians(-157.5 + 45 * (k - 1)))
        for x in range(1, ACCESS + 1):
            place(f"X{x}", "access", 500, math.radians(-168.75 + 22.5 * (x - 1)))
        for i in range(1, CORES + 1):
            for j in range(i + 1, CORES + 1):
                connect(f"{p}C{i}", f"{p}C{j}", f"10.{base}.{i}{j}")
        for k in range(1, AGGS + 1):
            for c in ((k - 1) % CORES + 1, k % CORES + 1):
                connect(f"{p}C{c}", f"{p}A{k}", f"10.{base + 1}.{10 * c + k}")
        for x in range(1, ACCESS + 1):
            for a in ((x - 1) // 2 + 1, ((x - 1) // 2 + 1) % AGGS + 1):
                connect(f"{p}A{a}", f"{p}X{x}", f"10.{base + 2}.{20 * a + x}")
        for x in range(1, ACCESS + 1):
            lan(f"{p}X{x}", f"10.{base + 3}.{x}", f"{p}H{x}", 2, "customer", m, 85, (cx, cy))
        lan(f"{p}C1", f"10.{base + 4}.1", f"{p}S1", 10, "server", m, -58, (cx, cy))
        lan(f"{p}C2", f"10.{base + 4}.2", f"{p}NOC", 10, "noc", m, -58, (cx, cy))
        if metros == 1:
            lan("C3", f"10.{base + 4}.3", "S2", 10, "server", m, -58, (cx, cy))

    # The mesh of meshes: Mm-C3 to the next metro's C4, so every pair is directly linked
    for m in range(1, metros + 1) if metros > 1 else ():
        n = m % metros + 1
        a, b = sorted((m, n))
        connect(f"M{m}-C3", f"M{n}-C4", f"10.0.{a}{b}")

    # Customers: odd ones use their own metro's server, even ones the next metro's
    servers = spec(metros)["servers"]
    for name, h in hosts.items():
        if h["role"] == "customer":
            number = int(name.split("H")[-1])
            target = h["metro"] if metros == 1 or number % 2 else h["metro"] % metros + 1
            h["probe"] = servers[f"{prefix(target, metros)}S1"]
            h["probe_server"] = f"{prefix(target, metros)}S1"

    for r in routers.values():
        r["cmdline"] = cmdline(r, timers)
    return routers, links, hosts


def cmdline(router, timers=TIMERS, rip_everywhere=False):
    ports = " ".join(f"eth{i}={cidr}" for i, (cidr, _) in enumerate(router["ports"]))
    rip = "rip" if rip_everywhere else "rip=" + ",".join(router["rip_ports"])
    return f"{ports} {rip} rip_timers={timers} http admin_password={PASSWORD}"


def networks(metros=1):
    """How many networks every converged routing table holds"""
    routers, links, hosts = topology(metros)
    return len(links) + len(hosts)


def addresses(router):
    """A router's addresses, LAN first (stays reachable when uplinks fail)"""
    ips = [cidr.split("/")[0] for cidr, _ in router["ports"]]
    return ips[::-1]
