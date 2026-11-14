"""The metro network: one definition for the builder (metro_lab.py) and the dashboard (net_ui.py).

A provider-style network of LazyDaysOS routers speaking RIP, built so that every
customer has redundant paths:

    core         C1..C4    full mesh (6 links); servers on C1 and C3, the NOC host on C2
    aggregation  A1..A8    each connected to two core routers
    access       X1..X16   each connected to two aggregation routers, one customer behind each

28 routers, 54 router-to-router links, 16 customers (H1..H16), 2 servers (S1, S2)
and a NOC host that reads every router's /api/stats in-band.

Addresses (all /24):
    10.10.<i><j>.0       core Ci - Cj           (.1 Ci, .2 Cj)
    10.20.<10c+k>.0      core Cc - aggregation Ak (.1 Cc, .2 Ak)
    10.30.<20a+m>.0      aggregation Aa - access Xm (.1 Aa, .2 Xm)
    10.100.<m>.0         customer LAN of Xm     (.1 Xm, .2 Hm)
    10.200.1.0 / .2.0 / .3.0   C1: S1 (.10), C2: NOC (.10), C3: S2 (.10)
"""
import math

CORES, AGGS, ACCESS = 4, 8, 16
PASSWORD = "lazydays-lab"
SERVER = "10.200.1.10"
SERVER2 = "10.200.3.10"
NOC = "10.200.2.10"
TIMERS = "5,30,20"                      # RIP update/timeout/garbage in seconds: faster than RFC's 30/180/120
MISCONFIGURABLE = "X7"                  # the access router the hijack scenario misconfigures
HIJACKER = "H7"


def topology(timers=TIMERS):
    """routers: name -> {role, x, y, ports: [(cidr, peer)], rip_ports: [ethN]}
    links: [(a, a_port, b, b_port, network)]; hosts: name -> {router, port, cidr, gateway, role, x, y}"""
    routers, links, hosts = {}, [], {}

    def place(name, role, radius, angle):
        routers[name] = {"role": role, "x": round(radius * math.cos(angle)), "y": round(radius * math.sin(angle)),
                         "ports": [], "rip_ports": []}

    for i in range(1, CORES + 1):
        place(f"C{i}", "core", 150, math.radians(-135 + 90 * (i - 1)))
    for k in range(1, AGGS + 1):
        place(f"A{k}", "aggregation", 330, math.radians(-157.5 + 45 * (k - 1)))
    for m in range(1, ACCESS + 1):
        place(f"X{m}", "access", 500, math.radians(-168.75 + 22.5 * (m - 1)))

    def connect(a, b, network):
        pa, pb = len(routers[a]["ports"]), len(routers[b]["ports"])
        routers[a]["ports"].append((f"{network}.1/24", b))
        routers[b]["ports"].append((f"{network}.2/24", a))
        routers[a]["rip_ports"].append(f"eth{pa}")
        routers[b]["rip_ports"].append(f"eth{pb}")
        links.append((a, pa, b, pb, f"{network}.0/24"))

    for i in range(1, CORES + 1):
        for j in range(i + 1, CORES + 1):
            connect(f"C{i}", f"C{j}", f"10.10.{i}{j}")
    for k in range(1, AGGS + 1):
        for c in ((k - 1) % CORES + 1, k % CORES + 1):
            connect(f"C{c}", f"A{k}", f"10.20.{10 * c + k}")
    for m in range(1, ACCESS + 1):
        for a in ((m - 1) // 2 + 1, ((m - 1) // 2 + 1) % AGGS + 1):
            connect(f"A{a}", f"X{m}", f"10.30.{20 * a + m}")

    def lan(router, network, host, host_addr, role, radius_out):
        port = len(routers[router]["ports"])
        routers[router]["ports"].append((f"{network}.1/24", host))
        r = routers[router]
        angle = math.atan2(r["y"], r["x"])
        hosts[host] = {"router": router, "port": port, "cidr": f"{network}.{host_addr}/24",
                       "gateway": f"{network}.1", "role": role,
                       "x": round(r["x"] + radius_out * math.cos(angle)), "y": round(r["y"] + radius_out * math.sin(angle))}

    for m in range(1, ACCESS + 1):
        lan(f"X{m}", f"10.100.{m}", f"H{m}", 2, "customer", 85)
    lan("C1", "10.200.1", "S1", 10, "server", -58)
    lan("C2", "10.200.2", "NOC", 10, "noc", -58)
    lan("C3", "10.200.3", "S2", 10, "server", -58)

    for name, r in routers.items():
        r["cmdline"] = cmdline(r, timers)
    return routers, links, hosts


def cmdline(router, timers=TIMERS, rip_everywhere=False):
    ports = " ".join(f"eth{i}={cidr}" for i, (cidr, _) in enumerate(router["ports"]))
    rip = "rip" if rip_everywhere else "rip=" + ",".join(router["rip_ports"])
    return f"{ports} {rip} rip_timers={timers} http admin_password={PASSWORD}"


def networks():
    """How many networks every converged routing table holds"""
    routers, links, hosts = topology()
    return len(links) + len(hosts)


def addresses(router):
    """A router's addresses, LAN first (stays reachable when uplinks fail)"""
    ips = [cidr.split("/")[0] for cidr, _ in router["ports"]]
    return ips[::-1]
