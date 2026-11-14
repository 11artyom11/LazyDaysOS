#!/usr/bin/env python3
"""Live dashboard for the "LazyDaysOS metro" network (gns3/metro_lab.py): routing
resilience and security, measured while you break things.

    python3 gns3/net_ui.py            # then open http://localhost:8095 (NET_UI_PORT to change)

What it measures (stdlib only):
  * every router's /api/stats once a second, fetched in-band by a poller running in
    the NOC container (routes, RIP counters, TTL expiries, port counters, CPU)
  * every customer's service: a ping to server S1 every 100 ms from each customer
    container (loss, latency, hop count from the TTL, ICMP errors such as
    "time to live exceeded", which means the packet was looping)
  * derived: convergence (every router has every network, no route changes), route
    churn, packets dying in loops, link load, and for every action you take the
    time until all customers have service again and until routing has converged

What you can do from the page:
  * cut a cable (both NICs see the link go down: QEMU set_link via the monitor)
  * fail a link silently (frames vanish, links stay up: GNS3 link suspend)
  * crash a router (stop the node), bring it back
  * withdraw the server network (count to infinity: loops become visible)
  * route hijack: misconfigure access router X7 to speak RIP towards its customer,
    then let compromised customer H7 announce a more specific route to the server
  * reset everything
"""
import collections, errno, json, os, queue, re, signal, socket, subprocess, sys, threading, time
import urllib.error, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metro  # noqa: E402

GNS3 = "http://127.0.0.1:3080/v2"
PROJECT = os.environ.get("METRO_PROJECT", "LazyDaysOS metro")
PORT = int(os.environ.get("NET_UI_PORT", "8095"))
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "net_ui.html")
# Every finished experiment, one JSON object per line: survives restarts, and is the raw data for the thesis
RESULTS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results", "metro_experiments.jsonl")
PROBE_INTERVAL = 0.1
POLL_INTERVALS = (0.25, 0.5, 1.0, 2.0)  # router stats, in-band: ~16 KB per router per poll
PUBLISH_INTERVAL = 0.1
HIJACK_MARKER = "lazydays-hijack"
NOC_MARKER = "net-ui-noc-agent"         # in the agent's argv, so pkill -f finds it
HIJACK_PREFIXES = ("10.200.1.0/25", "10.200.1.128/25")


def gns3(method, path, body=None):
    req = urllib.request.Request(GNS3 + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    out = urllib.request.urlopen(req, timeout=30).read()
    return json.loads(out) if out else None


class Hub:
    """Server-sent events to every open page, with a short history for late joiners"""

    def __init__(self):
        self.lock = threading.Lock()
        self.queues = []
        self.seq = 0
        self.log = collections.deque(maxlen=300)     # timeline entries, replayed to new pages

    def subscribe(self):
        q = queue.Queue(maxsize=5000)
        with self.lock:
            for ev in self.log:
                q.put_nowait(ev)
            self.queues.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.queues:
                self.queues.remove(q)

    def publish(self, ev):
        ev.setdefault("ts", time.time())
        with self.lock:
            self.seq += 1
            ev["seq"] = self.seq
            if ev["type"] == "log":
                self.log.append(ev)
            for q in self.queues:
                try:
                    q.put_nowait(ev)
                except queue.Full:
                    pass


hub = Hub()


CURRENT_EXPERIMENT = [None]             # id of the experiment consequences are attributed to


def log(text, kind="info", **extra):
    hub.publish({"type": "log", "text": text, "kind": kind, "exp": CURRENT_EXPERIMENT[0], **extra})
    print(time.strftime("%H:%M:%S"), text, flush=True)


# ---- the lab as GNS3 sees it --------------------------------------------------------

class Lab:
    def __init__(self):
        self.routers, self.links_def, self.hosts = metro.topology()
        self.expected = metro.networks()
        self.pid = None
        self.nodes = {}                 # name -> GNS3 node
        self.links = {}                 # link id -> {a, a_port, b, b_port, network, state}
        self.link_state = {}            # link id -> "up" | "cut" | "silent"
        self.misconfigured = False
        self.hijacking = False

    def discover(self):
        p = next((p for p in gns3("GET", "/projects") if p["name"] == PROJECT), None)
        if not p:
            raise RuntimeError(f"no GNS3 project '{PROJECT}': run python3 gns3/metro_lab.py build")
        if p["status"] != "opened":
            gns3("POST", f"/projects/{p['project_id']}/open")
        self.pid = p["project_id"]
        self.nodes = {n["name"]: n for n in gns3("GET", f"/projects/{self.pid}/nodes")}
        by_id = {n["node_id"]: n["name"] for n in self.nodes.values()}
        links = {}
        for l in gns3("GET", f"/projects/{self.pid}/links"):
            (a, b) = l["nodes"]
            links[l["link_id"]] = {"id": l["link_id"], "a": by_id[a["node_id"]], "a_port": a["adapter_number"],
                                   "b": by_id[b["node_id"]], "b_port": b["adapter_number"]}
            silent = (l.get("filters") or {}).get("packet_loss") == [100]
            self.link_state.setdefault(l["link_id"], "cut" if l.get("suspend") else "silent" if silent else "up")
        self.links = links
        # Scenario state from reality, not memory: survives a restart of this dashboard
        x7 = self.nodes.get(metro.MISCONFIGURABLE)
        if x7:
            cmdline = (x7.get("properties") or {}).get("kernel_command_line", "")
            self.misconfigured = " rip " in f" {cmdline} "
        try:
            out = subprocess.run(["docker", "exec", self.container(metro.HIJACKER), "pgrep", "-f", HIJACK_MARKER],
                                 capture_output=True, text=True, timeout=5)
            self.hijacking = bool(out.stdout.strip())
        except (subprocess.SubprocessError, OSError):
            pass
        return self

    def container(self, host):
        return f"GNS3.{host}.{self.pid}"

    def state(self):
        nodes = []
        for name, n in self.nodes.items():
            meta = self.routers.get(name) or self.hosts.get(name) or {}
            # Positions from metro.py, not GNS3: the drawing doesn't depend on the canvas
            nodes.append({"name": name, "id": n["node_id"], "status": n["status"],
                          "x": meta.get("x", n["x"]), "y": meta.get("y", n["y"]),
                          "role": meta.get("role", "host"), "kind": "router" if name in self.routers else "host",
                          "ports": [c for c, _ in meta.get("ports", [])], "cidr": meta.get("cidr")})
        links = []
        for lid, l in self.links.items():
            ra = self.routers.get(l["a"])
            network = None
            if ra and l["a_port"] < len(ra["ports"]):
                network = ra["ports"][l["a_port"]][0]
            links.append({**l, "state": self.link_state.get(lid, "up"), "network": network})
        return {"project": PROJECT, "nodes": nodes, "links": links, "expected": self.expected,
                "server": metro.SERVER, "misconfigured": self.misconfigured, "hijacking": self.hijacking,
                "x7": metro.MISCONFIGURABLE, "h7": metro.HIJACKER, "timers": metro.TIMERS}


lab = Lab()


def qemu_monitors():
    """node name -> QEMU monitor TCP port, from the running QEMU command lines"""
    out = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
    ports = {}
    for line in out.splitlines():
        if "qemu-system" not in line:
            continue
        name = re.search(r"-name (\S+)", line)
        mon = re.search(r"-monitor tcp:127\.0\.0\.1:(\d+)", line)
        if name and mon:
            ports[name.group(1)] = int(mon.group(1))
    return ports


def monitor(node, command):
    port = qemu_monitors().get(node)
    if not port:
        raise RuntimeError(f"{node}: QEMU monitor not found (is it running?)")
    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
        s.recv(4096)                    # banner
        s.sendall(command.encode() + b"\n")
        time.sleep(0.1)
        return s.recv(4096).decode(errors="replace")


# ---- measurement: routers (in-band from the NOC) and customers (pings) ----------------

NOC_AGENT = r'''
import json, sys, threading, time, urllib.request, base64
# argv: marker (first, so busybox pkill -f sees it), routers, credentials, interval
routers = json.loads(sys.argv[2]); auth = "Basic " + base64.b64encode(sys.argv[3].encode()).decode()
interval = float(sys.argv[4])
out = threading.Lock()
def fetch(ips):
    for ip in ips:
        try:
            req = urllib.request.Request("http://%s/api/stats" % ip, headers={"Authorization": auth})
            return json.load(urllib.request.urlopen(req, timeout=0.8)), ip
        except Exception:
            continue
    return None, None
def poll(name, ips, offset):
    # One loop per router: a dead router only slows itself (retried every 2 s). Down
    # only after two failed polls in a row: one slow answer isn't a dead router.
    # Loops are spread evenly over the interval: 28 responses of 16 KB arriving at
    # once overflow the NOC router's transmit ring and stall the routers' TCP.
    failures = 0
    next_at = time.time() + offset
    while True:
        time.sleep(max(0.0, next_at - time.time()))
        start = time.time()
        stats, ip = fetch(ips)
        failures = 0 if stats is not None else failures + 1
        if stats is None and failures < 2:
            time.sleep(0.1)
            continue
        if stats is None:
            line = json.dumps({"router": name, "t": time.time(), "down": True})
        else:
            if ip != ips[0]:
                ips = [ip] + [x for x in ips if x != ip]   # remember the address that answered
            rip = stats.get("rip", {})
            line = json.dumps({"router": name, "t": time.time(), "uptime_ms": stats["system"]["uptime_ms"],
               "cpu": stats["cpu"]["busy_permille"],
               "ports": [[p["name"], p["link"], p["tx_bytes"], p["rx_bytes"], p["tx_packets"]] for p in stats["interfaces"]],
               "routes": [[r["network"], r["gateway"], r["port"], r.get("metric"), r["origin"]] for r in stats["routes"]],
               "ttl_exceeded": stats["ipv4"]["ttl_exceeded"], "no_route": stats["ipv4"]["no_route"],
               "forwarded": stats["ipv4"]["forwarded"],
               "rip": {k: rip.get(k, 0) for k in ("route_changes", "tx_triggered", "rx_bad", "rx_refused", "rx_responses")}})
        with out:
            print(line, flush=True)
        next_at = max(next_at + (interval if stats is not None else max(interval, 2.0)), time.time() + 0.02)
for i, (name, ips) in enumerate(routers.items()):
    threading.Thread(target=poll, args=(name, ips, i * interval / len(routers)), daemon=True).start()
while True:
    time.sleep(3600)
'''

HIJACK_AGENT = r'''
import socket, struct, sys, time
# %s: announce more specific routes to the server network, metric 1, every 2 s
src = sys.argv[1]
def entry(net, plen):
    mask = (0xFFFFFFFF << (32 - plen)) & 0xFFFFFFFF
    return struct.pack("!HH4sII", 2, 0, socket.inet_aton(net), mask, 0) + struct.pack("!I", 1)
msg = struct.pack("!BBH", 2, 2, 0) + b"".join(entry(n.split("/")[0], int(n.split("/")[1])) for n in sys.argv[2:])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((src, 520))
while True:
    s.sendto(msg, ("255.255.255.255", 520)); time.sleep(2)
''' % HIJACK_MARKER


class Routers(threading.Thread):
    """Reads the NOC agent's stream; keeps the latest and previous sample per router"""

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.latest = {}                # router -> sample
        self.prev = {}
        self.interval = 0.5
        self.proc = None

    def set_interval(self, interval):
        if interval not in POLL_INTERVALS:
            raise ValueError(f"interval: one of {POLL_INTERVALS}")
        self.interval = interval
        stop_agents()                   # run() starts the agent again with the new interval

    def run(self):
        targets = {name: metro.addresses(r) for name, r in lab.routers.items()}
        while True:
            try:
                proc = self.proc = subprocess.Popen(
                    ["docker", "exec", lab.container("NOC"), "python3", "-u", "-c", NOC_AGENT, NOC_MARKER,
                     json.dumps(targets), f"admin:{metro.PASSWORD}", str(self.interval)],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                for line in proc.stdout:
                    try:
                        s = json.loads(line)
                    except ValueError:
                        continue
                    with self.lock:
                        if s["router"] in self.latest:
                            self.prev[s["router"]] = self.latest[s["router"]]
                        self.latest[s["router"]] = s
            except Exception as e:
                print("NOC poller:", e, flush=True)
            time.sleep(0.2)

    def snapshot(self):
        with self.lock:
            return dict(self.latest), dict(self.prev)


PING_OK = re.compile(r"^\[([\d.]+)\] \d+ bytes from [\d.]+: icmp_seq=(\d+) ttl=(\d+) time=([\d.]+)")
PING_LOST = re.compile(r"^\[([\d.]+)\] no answer yet for icmp_seq=(\d+)")
PING_ERROR = re.compile(r"^\[([\d.]+)\] From ([\d.]+) icmp_seq=(\d+) (.+)")


class Probe(threading.Thread):
    """One customer's view of the service: ping S1 every 200 ms"""

    def __init__(self, host):
        super().__init__(daemon=True)
        self.host = host
        self.lock = threading.Lock()
        self.results = collections.deque(maxlen=150)    # (time, ok, rtt, hops, error)
        self.outage_start = None
        self.last_ok_hops = None

    def run(self):
        while True:
            try:
                proc = subprocess.Popen(["docker", "exec", lab.container(self.host), "ping", "-n", "-D", "-O",
                                         "-i", str(PROBE_INTERVAL), "-W", "1", metro.SERVER],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                for line in proc.stdout:
                    self.parse(line.strip())
            except Exception as e:
                print(self.host, "probe:", e, flush=True)
            time.sleep(2)

    def parse(self, line):
        m = PING_OK.match(line)
        if m:
            t, ttl, rtt = float(m.group(1)), int(m.group(3)), float(m.group(4))
            self.record(t, True, rtt, 64 - ttl, None)
            return
        m = PING_LOST.match(line)
        if m:
            self.record(float(m.group(1)), False, None, None, None)
            return
        m = PING_ERROR.match(line)
        if m:
            error = "loop" if "Time to live exceeded" in m.group(4) else "unreachable"
            self.record(float(m.group(1)), False, None, None, f"{error} (from {m.group(2)})")

    def record(self, t, ok, rtt, hops, error):
        with self.lock:
            self.results.append((t, ok, rtt, hops, error))
        if ok:
            if self.outage_start is not None:
                took = t - self.outage_start
                path = f"; path {self.last_ok_hops} → {hops} routers" if self.last_ok_hops not in (None, hops) else ""
                log(f"{self.host} has service again after {took:.1f} s{path}", "recovered", host=self.host, outage=took)
                self.outage_start = None
            elif self.last_ok_hops is not None and hops != self.last_ok_hops:
                log(f"{self.host}: path changed {self.last_ok_hops} → {hops} routers, no loss", "path", host=self.host)
            self.last_ok_hops = hops
        else:
            recent = [r for r in list(self.results)[-3:]]
            if self.outage_start is None and len(recent) == 3 and not any(r[1] for r in recent):
                self.outage_start = recent[0][0]
                log(f"{self.host} lost the service{': ' + error if error else ''}", "outage", host=self.host)

    def summary(self, now):
        with self.lock:
            window = [r for r in self.results if now - r[0] <= 5]
            last = self.results[-1] if self.results else None
        if not window:
            return {"host": self.host, "state": "unknown"}
        lost = sum(1 for r in window if not r[1])
        oks = [r for r in window if r[1]]
        errors = [r[4] for r in window if r[4]]
        recent = window[-int(round(1 / PROBE_INTERVAL)):]   # the last second
        recent_lost = sum(1 for r in recent if not r[1])
        state = "down" if recent_lost == len(recent) else "degraded" if recent_lost else "ok"
        if state == "down" and errors and "loop" in errors[-1]:
            state = "looping"
        return {"host": self.host, "state": state, "loss": round(100 * lost / len(window)),
                "rtt": round(oks[-1][2], 2) if oks else None, "hops": oks[-1][3] if oks else None,
                "error": errors[-1] if errors else None,
                "outage_s": round(now - self.outage_start, 1) if self.outage_start else None,
                "history": [1 if r[1] else (2 if r[4] and "loop" in r[4] else 0) for r in list(self.results)[-50:]]}


def stop_agents():
    """Killing a docker exec client leaves its process running in the container: kill
    our poller and probes inside the containers themselves (on start, on rate change)"""
    # busybox pkill -f only sees the start of a long command line: match its beginning
    procs = [subprocess.Popen(["docker", "exec", lab.container("NOC"), "pkill", "-f", "python3 -u -c"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    for host, meta in lab.hosts.items():
        if meta["role"] == "customer":
            procs.append(subprocess.Popen(["docker", "exec", lab.container(host), "pkill", "-x", "ping"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for p in procs:
        p.wait(timeout=15)


routers = Routers()
probes = {h: Probe(h) for h, meta in metro.topology()[2].items() if meta["role"] == "customer"}


# ---- derived network state, published twice a second ---------------------------------

U, T, G = (int(x) for x in metro.TIMERS.split(","))
EXPECT = {
    "cut": "Both routers see the link go down and withdraw its routes at once: customers routed over it "
           "should be back on another path within ~1-5 s.",
    "silent": f"Nothing notices until the RIP timeout ({T} s): customers routed over this link stay offline "
              f"for about {T} s, then reroute.",
    "restore": "Paths move back over the link within a few seconds, normally without loss.",
    "stop": "Its neighbours see their links go down and reroute within seconds. A crashed access router's own "
            "customer stays offline until it is started again.",
    "start": "The router boots (~2 s) and relearns its routes; some paths move back to it.",
    "withdraw": "C1 announces the server network as unreachable. Other routers may count to infinity: packets "
                "loop (TTL expired) until the routes reach metric 16, then every customer loses the server.",
    "misconfigure": "X7 reboots (~2 s). Nothing else changes: the risk stays latent until someone on its "
                    "customer port speaks RIP.",
    "hijack": "If X7 is misconfigured, the forged /25 routes spread hop by hop (1-5 s per hop): every router "
              "within ~10-20 s, and every customer loses the server. If X7 is fixed, nothing happens.",
    "stop_hijack": f"The forged routes stay until the RIP timeout ({T} s) expires them; count to infinity may "
                   f"loop packets for a few seconds after that.",
    "fix": f"X7 reboots without RIP on its customer port. Forged routes elsewhere age out after {T} s.",
    "reset": f"Everything goes back to normal; routing settles within ~10-{T} s.",
}
def single_homed(router):
    """Hosts that reach the network only through this router"""
    return [(h, m["role"]) for h, m in lab.hosts.items() if m["router"] == router]


def impact_of(kind, target):
    """What a specific router or link carries, so the expectation names what will break"""
    if kind == "stop" and target in lab.routers:
        notes = []
        for host, role in single_homed(target):
            if host == "S1":
                notes.append(f"S1, the server every customer uses, hangs only off {target}: every customer loses "
                             f"the service until {target} is started again (a single point of failure)")
            elif role == "noc":
                notes.append(f"the NOC hangs off {target}: the dashboard loses all router data (monitoring is "
                             f"in-band) until {target} is back")
            elif role == "server":
                notes.append(f"{host} hangs off {target} and goes offline (not probed here)")
            elif role == "customer":
                notes.append(f"its customer {host} is single-homed and stays offline")
        return notes
    if kind in ("cut", "silent") and target in lab.links:
        l = lab.links[target]
        for end in (l["a"], l["b"]):
            if end == "S1":
                return ["this is S1's only link: every customer loses the service"]
            if end == "NOC":
                return ["this is the NOC's only link: the dashboard loses all router data"]
            if end in lab.hosts and lab.hosts[end]["role"] == "customer":
                return [f"this is {end}'s only link: {end} stays offline"]
    return []


SETTLE_ROUTING_S = U + 1                # no route change for a whole update cycle (triggered updates come 1-5 s apart)
SETTLE_SERVICE_S = 5                    # no customer changing state for this long
# The earliest an action's consequences can be complete: effects that wait for a RIP
# timer show up only after it, however quiet the network looks before
MIN_WINDOW = {"silent": (T + 2 * U, f"a silent failure is noticed only at the RIP timeout ({T} s)"),
              "stop_hijack": (T + U, f"forged routes expire only at the RIP timeout ({T} s)"),
              "fix": (T + U, f"forged routes elsewhere expire only at the RIP timeout ({T} s)"),
              "withdraw": (2 * U + 5, "routers may count to infinity before the route is gone"),
              "restore": (U + 3, "routes over the link come back with the next updates"),
              "start": (U + 5, "the router boots and relearns its routes"),
              "hijack": (2 * U + 5, "forged routes spread hop by hop (1-5 s per hop)"),
              "reset": (U + 5, "routes come back with the next updates")}


class Publisher(threading.Thread):
    """Publishes the network state and runs every action as an experiment: applied ->
    first effect -> settled, with its consequences attributed to it"""

    def __init__(self):
        super().__init__(daemon=True)
        self.converged = None
        self.calm_since = None
        self.experiment = None          # the one consequences are attributed to
        self.results = collections.deque(maxlen=20)
        try:
            with open(RESULTS_FILE) as f:
                for line in f.readlines()[-20:]:
                    self.results.appendleft(json.loads(line))
            self.next_id = max((r["id"] for r in self.results), default=0) + 1
        except (OSError, ValueError):
            pass
        self.next_id = getattr(self, "next_id", 1)
        self.queue = []                 # (description, callable) waiting for the network to settle
        self.flow_key = None
        self.flow_changed_at = time.time()
        self.last_discover = 0
        self.last_tick = None
        self.settled = True
        self.waiting = []
        self.dispatching = False        # a queued action is being applied: don't take the next one yet

    def start_action(self, kind, text, target=None):
        now = time.time()
        overlaps = self.experiment["id"] if self.experiment and self.experiment["settled"] is None else None
        if overlaps:
            self.finish(now, interrupted=True)
        net = (self.last_tick or {}).get("network", {})
        flows = {f["host"]: f for f in (self.last_tick or {}).get("flows", [])}
        self.experiment = {
            "id": self.next_id, "kind": kind, "text": text,
            "expected": " ".join([EXPECT.get(kind, "")] + [n[0].upper() + n[1:] + "." for n in impact_of(kind, target)]),
            "t0": now,
            "overlaps": overlaps, "first_effect": None, "first_effect_text": None,
            "peak_down": 0, "peak_hijacked": 0, "peak_loops": 0.0, "outage_s": 0.0,
            "settled": None, "min_s": MIN_WINDOW.get(kind, (3, ""))[0], "min_why": MIN_WINDOW.get(kind, (3, ""))[1],
            "base_served": net.get("served"), "base_hijacked": len(net.get("hijacked", [])),
            "base_hops": {h: f.get("hops") for h, f in flows.items()}, "last_t": now}
        self.next_id += 1
        CURRENT_EXPERIMENT[0] = self.experiment["id"]
        log(text + (f" (while #{overlaps} was still settling)" if overlaps else ""), "action")
        return self.experiment["id"]

    def finish(self, now, interrupted=False):
        e = self.experiment
        e["settled"] = round(now - e["t0"], 1)
        e["interrupted"] = interrupted
        record = {k: v for k, v in e.items() if k not in ("base_hops", "last_t", "min_why")}
        record["outage_s"] = round(record["outage_s"], 1)
        self.results.appendleft(record)
        try:
            os.makedirs(os.path.dirname(RESULTS_FILE), exist_ok=True)
            with open(RESULTS_FILE, "a") as f:
                f.write(json.dumps(record) + "\n")
        except OSError as err:
            print("results file:", err, flush=True)
        if not interrupted:
            effect = f"first effect after {e['first_effect']} s ({e['first_effect_text']})" if e["first_effect"] is not None else "no visible effect"
            log(f"#{e['id']} settled after {e['settled']} s: {effect}; up to {e['peak_down']} customers without "
                f"service, {e['outage_s']:.1f} customer-seconds of outage", "summary")

    def enqueue(self, description, fn, target=None):
        self.queue.append((description, fn, target))
        log(f"queued until the network settles: {description}", "queued")
        return len(self.queue)

    def track(self, now, net, flows, churn):
        """Attribute what changed to the running experiment; decide whether it has settled"""
        key = tuple((f["host"], f["state"]) for f in flows)
        if key != self.flow_key:
            self.flow_key, self.flow_changed_at = key, now
        routing_calm = net["converged"]
        service_stable = now - self.flow_changed_at >= SETTLE_SERVICE_S
        e = self.experiment
        # What still has to happen before the consequences are complete
        waiting = []
        if net["up"] < net["running"]:
            waiting.append(f"{net['running'] - net['up']} running routers not reporting their state")
        elif not routing_calm:
            quiet = now - self.calm_since if self.calm_since else 0
            waiting.append(f"routes changing ({net['churn']}/s)" if net["churn"] > 0
                           else f"no route change for {quiet:.0f} of {SETTLE_ROUTING_S} s")
        if not service_stable:
            waiting.append(f"customer service changed {now - self.flow_changed_at:.0f} s ago (needs {SETTLE_SERVICE_S} s unchanged)")
        if net["hijacked"] and not lab.hijacking:
            waiting.append(f"forged routes still in {len(net['hijacked'])} routers: they expire at the RIP timeout")
        if e and e["settled"] is None and now - e["t0"] < e["min_s"]:
            waiting.append(f"{e['min_why'] or 'minimum observation'}: until +{e['min_s']} s")
        self.waiting = waiting
        if e and e["settled"] is None:
            dt, e["last_t"] = now - e["last_t"], now
            down = net["customers"] - net["served"]
            e["outage_s"] += down * dt
            e["peak_down"] = max(e["peak_down"], down)
            e["peak_hijacked"] = max(e["peak_hijacked"], len(net["hijacked"]))
            e["peak_loops"] = max(e["peak_loops"], net["loops"])
            if e["first_effect"] is None:
                moved = [f["host"] for f in flows if f.get("hops") is not None and e["base_hops"].get(f["host"]) not in (None, f["hops"])]
                what = (f"{down} customers lost service" if e["base_served"] is not None and net["served"] < e["base_served"]
                        else (f"{len(net['hijacked'])} routers hijacked" if len(net["hijacked"]) > e["base_hijacked"]
                              else f"forged routes expiring ({len(net['hijacked'])} routers still hijacked)")
                        if len(net["hijacked"]) != e["base_hijacked"]
                        else f"paths changed for {', '.join(moved[:4])}" if moved
                        else "routes changing" if churn > 0 else None)
                if what:
                    e["first_effect"], e["first_effect_text"] = round(now - e["t0"], 1), what
                    log(f"#{e['id']} first effect after {e['first_effect']} s: {what}", "effect")
            if not waiting:
                self.finish(now)
        self.settled = (e is None or e["settled"] is not None) and not waiting and not self.dispatching
        if self.settled and self.queue:
            # One at a time, in order: the next waits until this one's experiment has settled
            description, fn, target = self.queue.pop(0)
            self.dispatching = True
            threading.Thread(target=run_queued, args=(description, fn, target), daemon=True).start()
            self.settled = False
        return routing_calm, service_stable

    def health(self, net, flows):
        """Settled only means stable. Are customers served, and if not, why?"""
        down = [f["host"] for f in flows if f["state"] not in ("ok", "degraded")]
        causes = []
        status = {n: m["status"] for n, m in lab.nodes.items()}
        for host, role in (("S1", "server"), ("NOC", "noc")):
            router = lab.hosts[host]["router"]
            link_down = any(lab.link_state.get(i) != "up" for i, l in lab.links.items() if host in (l["a"], l["b"]))
            if status.get(router) != "started" or link_down:
                why = f"{router} is stopped" if status.get(router) != "started" else f"its only link is {'cut' if link_down else 'down'}"
                causes.append(f"server S1 unreachable: {why} (single point of failure)" if host == "S1"
                              else f"NOC unreachable: {why}, so the dashboard has no router data")
        for host in down:
            router = lab.hosts[host]["router"]
            if status.get(router) != "started":
                causes.append(f"{host}: its access router {router} is stopped")
        if net["hijacked"]:
            causes.append(f"{len(net['hijacked'])} routers send the server's traffic to {metro.HIJACKER}"
                          + ("" if lab.hijacking else " (forged routes left over; they expire at the RIP timeout)"))
        if net["loops"] > 0:
            causes.append(f"{net['loops']} packets/s dying in routing loops")
        if down and not causes:
            causes.append(f"{len(down)} customers without service: {', '.join(down[:6])}")
        return {"healthy": not down and not net["hijacked"], "down": down, "causes": causes}

    def disturbances(self, net):
        """What is currently not as built, with the action that undoes it"""
        out = []
        for lid, state in lab.link_state.items():
            if state != "up" and lid in lab.links:
                l = lab.links[lid]
                out.append({"text": f"{l['a']}–{l['b']} {'cut' if state == 'cut' else 'silently failing'}",
                            "undo": ["/api/link", {"id": lid, "action": "restore"}]})
        for name, n in lab.nodes.items():
            if name in lab.routers and n["status"] != "started":
                out.append({"text": f"{name} stopped", "undo": ["/api/node", {"name": name, "action": "start"}]})
        if lab.misconfigured:
            out.append({"text": f"{metro.MISCONFIGURABLE} speaks RIP to its customer",
                        "undo": ["/api/scenario", {"action": "fix"}]})
        if lab.hijacking:
            out.append({"text": f"{metro.HIJACKER} announcing forged routes", "undo": ["/api/scenario", {"action": "stop_hijack"}]})
        elif net["hijacked"]:
            out.append({"text": f"forged routes still in {len(net['hijacked'])} routers (they age out after {T} s)", "undo": None})
        return out

    def run(self):
        while True:
            try:
                self.publish()
            except Exception as e:
                print("publisher:", e, flush=True)
            time.sleep(PUBLISH_INTERVAL)

    def publish(self):
        now = time.time()
        latest, prev = routers.snapshot()
        per_router, churn, loops, routed_to_hijacker = {}, 0.0, 0.0, []
        port_rate = {}                  # (router, ethN) -> bits/s out
        for name in lab.routers:
            s, p = latest.get(name), prev.get(name)
            if not s or s.get("down") or now - s["t"] > 5:
                per_router[name] = {"state": "unreachable"}
                continue
            dt = (s["t"] - p["t"]) if p and not p.get("down") and p.get("uptime_ms", 0) <= s["uptime_ms"] else 0
            rate = (lambda a, b: max(0.0, (a - b) / dt)) if dt > 0 else (lambda a, b: 0.0)
            changes = rate(s["rip"]["route_changes"], p["rip"]["route_changes"]) if dt else 0.0
            ttl = rate(s["ttl_exceeded"], p["ttl_exceeded"]) if dt else 0.0
            churn += changes
            loops += ttl
            if dt:
                old = {q[0]: q for q in p["ports"]}
                for q in s["ports"]:
                    if q[0] in old:
                        port_rate[(name, q[0])] = rate(q[2], old[q[0]][2]) * 8
            hijack = [r for r in s["routes"] if r[0] in HIJACK_PREFIXES]
            if hijack:
                routed_to_hijacker.append(name)
            server_route = next((r for r in s["routes"] if r[0] == "10.200.1.0/24"), None)
            per_router[name] = {"state": "up", "routes": len([r for r in s["routes"] if r[0] not in HIJACK_PREFIXES]),
                                "cpu": s["cpu"], "churn": round(changes, 1), "ttl_exceeded": round(ttl, 1),
                                "hijacked": bool(hijack),
                                "server_via": server_route[2] if server_route else None,
                                "links": {q[0]: q[1] for q in s["ports"]}}
        links = {}
        for lid, l in lab.links.items():
            rate = max(port_rate.get((l["a"], f"eth{l['a_port']}"), 0), port_rate.get((l["b"], f"eth{l['b_port']}"), 0))
            links[lid] = {"bps": round(rate), "state": lab.link_state.get(lid, "up")}
        up = [r for r in per_router.values() if r["state"] == "up"]
        complete = sum(1 for r in up if r["routes"] >= lab.expected)
        # Converged: every running router reports and no route has changed for 3 s. (Route
        # counts can't be compared: the two ends of a cut link keep its connected route.)
        calm = churn < 0.05
        self.calm_since = (self.calm_since or now) if calm else None
        running = [n for n in lab.routers if lab.nodes.get(n, {}).get("status") == "started"]
        converged = bool(up) and len(up) == len(running) and self.calm_since is not None and now - self.calm_since >= SETTLE_ROUTING_S
        if converged != self.converged and self.converged is not None:
            log("routing converged" if converged else
                ("routing is changing" if churn > 0 else "routers not reporting their state"), "routing")
        self.converged = converged
        flows = [p.summary(now) for p in probes.values()]
        served = sum(1 for f in flows if f["state"] in ("ok", "degraded"))   # replies in the last second
        if now - self.last_discover > 3:
            self.last_discover = now
            try:
                lab.discover()          # notice routers stopped or started outside the page
            except Exception:
                pass
        net = {"converged": converged, "complete": complete, "up": len(up), "running": len(running),
               "networks": max((r["routes"] for r in up), default=0),
               "routers": len(lab.routers), "churn": round(churn, 1), "loops": round(loops, 1),
               "served": served, "customers": len(flows), "hijacked": routed_to_hijacker, "expected": lab.expected}
        routing_calm, service_stable = self.track(now, net, flows, churn)
        e = self.experiment
        status = {"settled": self.settled, "routing_calm": routing_calm, "service_stable": service_stable,
                  "waiting": self.waiting,
                  "service_stable_for": round(now - self.flow_changed_at, 1),
                  "disturbances": self.disturbances(net), "queue": [q[0] for q in self.queue],
                  "health": self.health(net, flows),
                  "experiment": e and {k: v for k, v in e.items() if k not in ("base_hops", "last_t")} | {"elapsed": round(now - e["t0"], 1)},
                  "results": list(self.results), "settle_rule": [SETTLE_ROUTING_S, SETTLE_SERVICE_S]}
        tick = {"type": "tick", "routers": per_router, "links": links, "flows": flows, "network": net,
                "status": status,
                "scenario": {"misconfigured": lab.misconfigured, "hijacking": lab.hijacking},
                "rate": {"poll": routers.interval, "publish": PUBLISH_INTERVAL, "probe": PROBE_INTERVAL,
                         "choices": POLL_INTERVALS}}
        self.last_tick = tick
        hub.publish(tick)


publisher = Publisher()


# ---- actions ----------------------------------------------------------------------

def link_action(lid, action):
    l = lab.links.get(lid)
    if not l:
        raise ValueError("no such link")
    label = f"{l['a']}–{l['b']}"
    if action == "cut":
        for end, port in ((l["a"], l["a_port"]), (l["b"], l["b_port"])):
            if end in lab.routers:
                monitor(end, f"set_link gns3-{port} off")
        if l["b"] not in lab.routers or l["a"] not in lab.routers:
            gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"suspend": True})
        lab.link_state[lid] = "cut"
        return "cut", f"cable cut: {label} (both ends see the link go down)"
    elif action == "silent":
        # 100% loss in ubridge: frames vanish, both NICs keep link. (A GNS3 suspend would
        # also take the QEMU link down, which routers notice at once.)
        gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"filters": {"packet_loss": [100]}})
        lab.link_state[lid] = "silent"
        return "silent", f"silent failure: {label} (frames vanish, links stay up)"
    elif action == "restore":
        gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"suspend": False, "filters": {}})
        for end, port in ((l["a"], l["a_port"]), (l["b"], l["b_port"])):
            if end in lab.routers:
                monitor(end, f"set_link gns3-{port} on")
        lab.link_state[lid] = "up"
        return "restore", f"link restored: {label}"
    else:
        raise ValueError("action: cut, silent or restore")


def node_action(name, action):
    n = lab.nodes.get(name)
    if not n or action not in ("stop", "start"):
        raise ValueError("unknown node or action")
    gns3("POST", f"/projects/{lab.pid}/nodes/{n['node_id']}/{action}", {})
    lab.discover()
    return action, f"router {name} {'crashed (stopped)' if action == 'stop' else 'started'}"


def set_x7(rip_everywhere):
    name = metro.MISCONFIGURABLE
    n = lab.nodes[name]
    cmdline = metro.cmdline(lab.routers[name], metro.TIMERS, rip_everywhere)
    gns3("PUT", f"/projects/{lab.pid}/nodes/{n['node_id']}", {"properties": {"kernel_command_line": cmdline}})
    gns3("POST", f"/projects/{lab.pid}/nodes/{n['node_id']}/reload", {})
    lab.misconfigured = rip_everywhere


def scenario(action):
    h7 = lab.container(metro.HIJACKER)
    if action == "misconfigure":
        set_x7(True)
        return "misconfigure", f"{metro.MISCONFIGURABLE} misconfigured: RIP on every port, its customer port included"
    elif action == "fix":
        set_x7(False)
        return "fix", f"{metro.MISCONFIGURABLE} fixed: RIP only towards the aggregation routers"
    elif action == "hijack":
        src = lab.hosts[metro.HIJACKER]["cidr"].split("/")[0]
        subprocess.run(["docker", "exec", "-d", h7, "python3", "-c", HIJACK_AGENT, src, *HIJACK_PREFIXES], check=True)
        lab.hijacking = True
        return "hijack", f"{metro.HIJACKER} (compromised) announces {', '.join(HIJACK_PREFIXES)} with metric 1"
    elif action == "stop_hijack":
        subprocess.run(["docker", "exec", h7, "pkill", "-f", HIJACK_MARKER])
        lab.hijacking = False
        return "stop_hijack", f"{metro.HIJACKER} stopped announcing"
    elif action == "withdraw":
        lid = next(i for i, l in lab.links.items() if {l["a"], l["b"]} == {"C1", "S1"})
        link_action(lid, "cut")
        return "withdraw", "server network withdrawn (cable C1–S1 cut)"
    elif action == "reset":
        return reset()
    else:
        raise ValueError("unknown scenario action")


def reset():
    """Undo everything that differs from the built network, in an order that can't trip
    over itself, and say exactly what was undone"""
    lab.discover()                      # fresh node states, not the last poll's
    done, errors = [], []
    publisher.queue.clear()
    # Routers first: restoring a link needs the QEMU monitor of a running router
    started = []
    for name, n in lab.nodes.items():
        if n["status"] != "started":
            try:
                gns3("POST", f"/projects/{lab.pid}/nodes/{n['node_id']}/start", {})
                started.append(name)
            except Exception as e:
                errors.append(f"start {name}: {e}")
    if started:
        done.append("started " + ", ".join(started))
    if lab.hijacking:
        subprocess.run(["docker", "exec", lab.container(metro.HIJACKER), "pkill", "-f", HIJACK_MARKER])
        lab.hijacking = False
        done.append(f"stopped {metro.HIJACKER}'s announcements")
    if lab.misconfigured:
        try:
            set_x7(False)
            done.append(f"fixed {metro.MISCONFIGURABLE}")
        except Exception as e:
            errors.append(f"fix {metro.MISCONFIGURABLE}: {e}")
    restored = []
    for lid, state in list(lab.link_state.items()):
        if state != "up" and lid in lab.links:
            l = lab.links[lid]
            try:
                gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"suspend": False, "filters": {}})
                for end, port in ((l["a"], l["a_port"]), (l["b"], l["b_port"])):
                    if end in lab.routers and end not in started:     # a fresh boot has its links up
                        monitor(end, f"set_link gns3-{port} on")
                lab.link_state[lid] = "up"
                restored.append(f"{l['a']}–{l['b']}")
            except Exception as e:
                errors.append(f"restore {l['a']}–{l['b']}: {e}")
    if restored:
        done.append("restored " + ", ".join(restored))
    lab.discover()
    text = "reset: " + ("; ".join(done) if done else "nothing to undo, the network is as built")
    if errors:
        text += " (failed: " + "; ".join(errors) + ")"
    return "reset", text


def dispatch(path, body):
    if path == "/api/link":
        return link_action(body.get("id"), body.get("action"))
    if path == "/api/node":
        return node_action(body.get("name"), body.get("action"))
    if path == "/api/scenario":
        return scenario(body.get("action"))
    raise ValueError("unknown action")


def describe(path, body):
    if path == "/api/link" and body.get("id") in lab.links:
        l = lab.links[body["id"]]
        return f"{body.get('action')} {l['a']}–{l['b']}"
    return f"{body.get('action')} {body.get('name', '')}".strip()


def target_of(body):
    return body.get("name") or body.get("id")


def run_queued(description, fn, target=None):
    try:
        kind, text = fn()
        publisher.start_action(kind, text, target)
    except Exception as e:
        log(f"queued action failed: {description}: {e}", "error")
    finally:
        publisher.dispatching = False


# ---- HTTP -------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def reply(self, code, obj=None, ctype="application/json", body=None):
        data = body if body is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                return self.reply(200, ctype="text/html; charset=utf-8", body=f.read())
        if url.path == "/api/state":
            try:
                lab.discover()
            except Exception as e:
                return self.reply(503, {"error": str(e)})
            return self.reply(200, lab.state())
        if url.path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            q = hub.subscribe()
            try:
                while True:
                    try:
                        ev = q.get(timeout=10)
                        self.wfile.write(f"data: {json.dumps(ev)}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                hub.unsubscribe(q)
            return None
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
            if url.path in ("/api/link", "/api/node", "/api/scenario"):
                # when=settled: run once the network has settled, so consequences don't mix
                if body.get("when") == "settled" and not publisher.settled:
                    position = publisher.enqueue(describe(url.path, body), lambda: dispatch(url.path, body), target_of(body))
                    return self.reply(200, {"ok": True, "queued": position})
                kind, text = dispatch(url.path, body)
                return self.reply(200, {"ok": True, "experiment": publisher.start_action(kind, text, target_of(body)),
                                        "text": text})
            elif url.path == "/api/queue/clear":
                publisher.queue.clear()
                log("queue cleared", "queued")
            elif url.path == "/api/rate":
                routers.set_interval(float(body.get("poll", 0)))
            else:
                return self.reply(404, {"error": "not found"})
        except (ValueError, RuntimeError, urllib.error.URLError, subprocess.CalledProcessError) as e:
            return self.reply(400, {"error": str(e)})
        return self.reply(200, {"ok": True})


def main():
    try:
        server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    except OSError as e:
        if e.errno == errno.EADDRINUSE:
            sys.exit(f"port {PORT} is taken: net_ui.py is probably running already (http://localhost:{PORT})")
        raise
    server.daemon_threads = True
    try:
        lab.discover()
    except Exception as e:
        sys.exit(str(e))
    stop_agents()                       # leftovers of an earlier run would load the routers twice
    routers.start()
    for p in probes.values():
        p.start()
    publisher.start()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    print(f"LazyDaysOS metro dashboard: http://localhost:{PORT}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_agents()


if __name__ == "__main__":
    main()
