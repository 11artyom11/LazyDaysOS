#!/usr/bin/env python3
"""Live dashboard for the "LazyDaysOS metro" network (gns3/metro_lab.py): routing
resilience and security, measured while you break things.

    python3 gns3/net_ui.py            # then open http://localhost:8095 (NET_UI_PORT to change)

What it measures (stdlib only):
  * every router's /api/stats once a second, fetched in-band by a poller running in
    the NOC container (routes, RIP counters, TTL expiries, port counters, CPU)
  * every customer's service: a ping to server S1 every 200 ms from each customer
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
PROBE_INTERVAL = 0.2
HIJACK_MARKER = "lazydays-hijack"
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


def log(text, kind="info", **extra):
    hub.publish({"type": "log", "text": text, "kind": kind, **extra})
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
import json, sys, time, urllib.request, base64
routers = json.loads(sys.argv[1]); auth = "Basic " + base64.b64encode(sys.argv[2].encode()).decode()
since = {}
while True:
    start = time.time()
    for name, ips in routers.items():
        stats = None
        for ip in ips:
            url = "http://%s/api/stats" % ip
            try:
                req = urllib.request.Request(url, headers={"Authorization": auth})
                stats = json.load(urllib.request.urlopen(req, timeout=0.7)); break
            except Exception:
                continue
        if stats is None:
            print(json.dumps({"router": name, "t": time.time(), "down": True}), flush=True); continue
        rip = stats.get("rip", {})
        out = {"router": name, "t": time.time(), "uptime_ms": stats["system"]["uptime_ms"],
               "cpu": stats["cpu"]["busy_permille"],
               "ports": [[p["name"], p["link"], p["tx_bytes"], p["rx_bytes"], p["tx_packets"]] for p in stats["interfaces"]],
               "routes": [[r["network"], r["gateway"], r["port"], r.get("metric"), r["origin"]] for r in stats["routes"]],
               "ttl_exceeded": stats["ipv4"]["ttl_exceeded"], "no_route": stats["ipv4"]["no_route"],
               "forwarded": stats["ipv4"]["forwarded"],
               "rip": {k: rip.get(k, 0) for k in ("route_changes", "tx_triggered", "rx_bad", "rx_refused", "rx_responses")}}
        print(json.dumps(out), flush=True)
    time.sleep(max(0.05, 1.0 - (time.time() - start)))
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

    def run(self):
        targets = {name: metro.addresses(r) for name, r in lab.routers.items()}
        while True:
            try:
                proc = subprocess.Popen(["docker", "exec", lab.container("NOC"), "python3", "-u", "-c", NOC_AGENT,
                                         json.dumps(targets), f"admin:{metro.PASSWORD}"],
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
            time.sleep(2)

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
        recent = window[-5:]                            # the last second
        recent_lost = sum(1 for r in recent if not r[1])
        state = "down" if recent_lost == len(recent) else "degraded" if recent_lost else "ok"
        if state == "down" and errors and "loop" in errors[-1]:
            state = "looping"
        return {"host": self.host, "state": state, "loss": round(100 * lost / len(window)),
                "rtt": round(oks[-1][2], 2) if oks else None, "hops": oks[-1][3] if oks else None,
                "error": errors[-1] if errors else None,
                "outage_s": round(now - self.outage_start, 1) if self.outage_start else None,
                "history": [1 if r[1] else (2 if r[4] and "loop" in r[4] else 0) for r in list(self.results)[-50:]]}


routers = Routers()
probes = {h: Probe(h) for h, meta in metro.topology()[2].items() if meta["role"] == "customer"}


# ---- derived network state, published twice a second ---------------------------------

class Publisher(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.action = None              # {"text", "t0", "service_at", "converged_at"}
        self.converged = None
        self.calm_since = None

    def start_action(self, text):
        self.action = {"text": text, "t0": time.time(), "service_at": None, "converged_at": None}
        log(text, "action")

    def run(self):
        while True:
            try:
                self.publish()
            except Exception as e:
                print("publisher:", e, flush=True)
            time.sleep(0.5)

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
        converged = bool(up) and len(up) == len(running) and self.calm_since is not None and now - self.calm_since >= 3
        if converged != self.converged and self.converged is not None:
            if converged and self.action and not self.action["converged_at"]:
                self.action["converged_at"] = now
            log("routing converged" if converged else "routing is changing", "routing")
        self.converged = converged
        flows = [p.summary(now) for p in probes.values()]
        served = sum(1 for f in flows if f["state"] in ("ok", "degraded"))   # replies in the last second
        if self.action and not self.action["service_at"] and served == len(flows) and now - self.action["t0"] > 1:
            self.action["service_at"] = now
            log(f"all {len(flows)} customers have service ({now - self.action['t0']:.1f} s after: "
                f"{self.action['text']})", "summary")
        hub.publish({"type": "tick", "routers": per_router, "links": links, "flows": flows,
                     "network": {"converged": converged, "complete": complete, "up": len(up),
                                 "networks": max((r["routes"] for r in up), default=0),
                                 "routers": len(lab.routers), "churn": round(churn, 1), "loops": round(loops, 1),
                                 "served": served, "customers": len(flows), "hijacked": routed_to_hijacker,
                                 "expected": lab.expected},
                     "action": self.action and {**self.action, "elapsed": round(now - self.action["t0"], 1)},
                     "scenario": {"misconfigured": lab.misconfigured, "hijacking": lab.hijacking}})


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
        publisher.start_action(f"cable cut: {label} (both ends see the link go down)")
    elif action == "silent":
        # 100% loss in ubridge: frames vanish, both NICs keep link. (A GNS3 suspend would
        # also take the QEMU link down, which routers notice at once.)
        gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"filters": {"packet_loss": [100]}})
        lab.link_state[lid] = "silent"
        publisher.start_action(f"silent failure: {label} (frames vanish, links stay up)")
    elif action == "restore":
        gns3("PUT", f"/projects/{lab.pid}/links/{lid}", {"suspend": False, "filters": {}})
        for end, port in ((l["a"], l["a_port"]), (l["b"], l["b_port"])):
            if end in lab.routers:
                monitor(end, f"set_link gns3-{port} on")
        lab.link_state[lid] = "up"
        publisher.start_action(f"link restored: {label}")
    else:
        raise ValueError("action: cut, silent or restore")


def node_action(name, action):
    n = lab.nodes.get(name)
    if not n or action not in ("stop", "start"):
        raise ValueError("unknown node or action")
    gns3("POST", f"/projects/{lab.pid}/nodes/{n['node_id']}/{action}", {})
    lab.discover()
    publisher.start_action(f"router {name} {'crashed (stopped)' if action == 'stop' else 'started'}")


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
        publisher.start_action(f"{metro.MISCONFIGURABLE} misconfigured: RIP on every port, its customer port included")
    elif action == "fix":
        set_x7(False)
        publisher.start_action(f"{metro.MISCONFIGURABLE} fixed: RIP only towards the aggregation routers")
    elif action == "hijack":
        src = lab.hosts[metro.HIJACKER]["cidr"].split("/")[0]
        subprocess.run(["docker", "exec", "-d", h7, "python3", "-c", HIJACK_AGENT, src, *HIJACK_PREFIXES], check=True)
        lab.hijacking = True
        publisher.start_action(f"{metro.HIJACKER} (compromised) announces {', '.join(HIJACK_PREFIXES)} with metric 1")
    elif action == "stop_hijack":
        subprocess.run(["docker", "exec", h7, "pkill", "-f", HIJACK_MARKER])
        lab.hijacking = False
        publisher.start_action(f"{metro.HIJACKER} stopped announcing")
    elif action == "withdraw":
        lid = next(i for i, l in lab.links.items() if {l["a"], l["b"]} == {"C1", "S1"})
        link_action(lid, "cut")
    elif action == "reset":
        if lab.hijacking:
            subprocess.run(["docker", "exec", h7, "pkill", "-f", HIJACK_MARKER])
            lab.hijacking = False
        if lab.misconfigured:
            set_x7(False)
        for lid, state in list(lab.link_state.items()):
            if state != "up":
                link_action(lid, "restore")
        for name, n in lab.nodes.items():
            if n["status"] != "started":
                gns3("POST", f"/projects/{lab.pid}/nodes/{n['node_id']}/start", {})
        lab.discover()
        publisher.start_action("reset: every link up, every router running, X7 fixed, no hijack")
    else:
        raise ValueError("unknown scenario action")


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
            if url.path == "/api/link":
                link_action(body.get("id"), body.get("action"))
            elif url.path == "/api/node":
                node_action(body.get("name"), body.get("action"))
            elif url.path == "/api/scenario":
                scenario(body.get("action"))
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


if __name__ == "__main__":
    main()
