#!/usr/bin/env python3
"""Live, visual throughput page for the "LazyDaysOS throughput" GNS3 lab.

    python3 gns3/lab_ui.py            # then open http://localhost:8090

Besides the measurements, the page gives access to the lab itself: a shell in
any host container or a router's serial console (in-page terminal), node
start/stop/reload, conditions on any link, and short packet captures decoded
with tcpdump (or downloaded as .pcap).

Data sources, all streamed to the page as server-sent events:
  * iperf3 on the lab's Docker hosts (see throughput_lab.py), sampled every
    0.1-1 s from both ends: the receiver reports what was delivered (and loss
    and jitter for UDP), the sender what was offered (and TCP retransmits);
  * the LazyDaysOS router's own counters (/api/stats, read once a second with
    curl from the inside client): packets per port, forwarded, NAT table;
  * GNS3: the topology (positions, icons, status) and the link conditions
    (delay, loss, corruption filters on the link to the server, applied live).

Standard library only; listens on 127.0.0.1, which WSL forwards to Windows.
"""
import base64, collections, errno, fcntl, json, os, pty, queue, re, secrets, select, signal, socket, struct
import subprocess, sys, termios, threading, time, urllib.error, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from throughput_lab import PROJECT, ROWS, ROUTER_NAMES  # noqa: E402

GNS3 = "http://127.0.0.1:3080/v2"
PORT = int(os.environ.get("LAB_UI_PORT", "8090"))
IPERF_PORT = "5202"                 # one-off servers; the nodes' own "iperf3 -s" keeps 5201
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lab_ui.html")
PATH_NAMES = {"0": "Baseline", "1": "Routed", "2": "NAT"}
INTERVALS = (0.1, 0.2, 0.5, 1.0)
GNS3_ERRORS = (OSError, urllib.error.URLError, RuntimeError, StopIteration, KeyError, ValueError)


def gns3(method, path, body=None, raw=False):
    req = urllib.request.Request(GNS3 + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    out = urllib.request.urlopen(req, timeout=10).read()
    if raw:
        return out
    return json.loads(out) if out else None


def docker_exec(container, *cmd, timeout=None):
    return subprocess.run(["docker", "exec", container, *cmd], capture_output=True, text=True, timeout=timeout)


class Hub:
    """Fan-out of server-sent events to every open page. Keeps the recent
    history so a page opened or reloaded mid-run gets the whole picture;
    events carry a sequence number for de-duplication."""

    def __init__(self):
        self.lock = threading.Lock()
        self.queues = []
        self.seq = 0
        self.history = collections.deque(maxlen=40000)

    def subscribe(self):
        q = queue.Queue(maxsize=60000)
        with self.lock:
            for event in self.history:
                q.put_nowait(event)
            self.queues.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.queues:
                self.queues.remove(q)

    def publish(self, event):
        event.setdefault("ts", time.time())
        with self.lock:
            self.seq += 1
            event["seq"] = self.seq
            self.history.append(event)
            for q in self.queues:
                try:
                    q.put_nowait(event)
                except queue.Full:
                    pass


hub = Hub()


class Lab:
    """The GNS3 project: containers, routers, links and the drawn topology."""

    def __init__(self):
        self.pid = None
        self.rows = {}
        self.topology = None

    def discover(self, start=False):
        projects = [p for p in gns3("GET", "/projects") if p["name"] == PROJECT]
        if not projects:
            raise RuntimeError(f"GNS3 project '{PROJECT}' not found: run gns3/throughput_lab.py")
        pid = projects[0]["project_id"]
        if projects[0]["status"] != "opened":
            gns3("POST", f"/projects/{pid}/open", {})
        nodes = {n["name"]: n for n in gns3("GET", f"/projects/{pid}/nodes")}
        if start and any(n["status"] != "started" for n in nodes.values()):
            gns3("POST", f"/projects/{pid}/nodes/start", {})
            time.sleep(4)
            nodes = {n["name"]: n for n in gns3("GET", f"/projects/{pid}/nodes")}
        links = gns3("GET", f"/projects/{pid}/links")
        drawings = gns3("GET", f"/projects/{pid}/drawings")
        by_id = {n["node_id"]: n for n in nodes.values()}

        def link_between(a, b):
            for l in links:
                ids = {e["node_id"] for e in l["nodes"]}
                if ids == {a, b}:
                    return l
            raise KeyError(f"no link between {by_id[a]['name']} and {by_id[b]['name']}")

        rows = {}
        for row, _y, cmdline, (cli_ip, _), (srv_ip, _gw) in ROWS:
            cli, srv = nodes[f"cli-{row}"], nodes[f"srv-{row}"]
            router = nodes.get(ROUTER_NAMES.get(row, ""))
            if router:
                ingress, egress = link_between(cli["node_id"], router["node_id"]), link_between(router["node_id"], srv["node_id"])
                inside_ip = re.search(r"eth0=([0-9.]+)/", cmdline).group(1)
                cmdline = router["properties"].get("kernel_command_line") or cmdline
            else:
                ingress = egress = link_between(cli["node_id"], srv["node_id"])
                inside_ip = None
            members = [cli, srv] + ([router] if router else [])
            rows[row] = {
                "row": row, "name": PATH_NAMES[row], "router": router["name"] if router else None,
                "cmdline": cmdline if router else None, "client_ip": cli_ip.split("/")[0],
                "server_ip": srv_ip.split("/")[0], "router_ip": inside_ip,
                "client": f"GNS3.cli-{row}.{pid}", "server": f"GNS3.srv-{row}.{pid}",
                "nodes": {"client": cli["node_id"], "server": srv["node_id"],
                          "router": router["node_id"] if router else None},
                "links": {"ingress": ingress["link_id"], "egress": egress["link_id"]},
                "link_id": egress["link_id"], "filters": egress.get("filters") or {},
                "running": all(n["status"] == "started" for n in members),
                "http": bool(router and "http" in cmdline.split()),
            }
        self.topology = {
            "nodes": [{"id": n["node_id"], "name": n["name"], "type": n["node_type"], "symbol": n["symbol"],
                       "x": n["x"], "y": n["y"], "w": n.get("width") or 60, "h": n.get("height") or 60,
                       "status": n["status"], "console": n.get("console"), "console_type": n.get("console_type"),
                       "image": n["properties"].get("image"), "ram": n["properties"].get("ram"),
                       "adapters": n["properties"].get("adapters")} for n in nodes.values()],
            "links": [{"id": l["link_id"], "a": l["nodes"][0]["node_id"], "b": l["nodes"][1]["node_id"],
                       "a_port": l["nodes"][0]["adapter_number"], "b_port": l["nodes"][1]["adapter_number"],
                       "filters": l.get("filters") or {}} for l in links],
            "drawings": [{"x": d["x"], "y": d["y"], "text": re.sub(r"<[^>]+>", "", d["svg"]).strip()}
                         for d in drawings],
        }
        self.pid, self.rows = pid, rows
        return rows

    def set_link_filters(self, link_id, filters):
        """Filters on any link; the row whose server link it is keeps its copy in sync."""
        link = gns3("PUT", f"/projects/{self.pid}/links/{link_id}", {"filters": filters})
        filters = link.get("filters") or {}
        for r in self.rows.values():
            if r["link_id"] == link_id:
                r["filters"] = filters
        for l in (self.topology or {}).get("links", []):
            if l["id"] == link_id:
                l["filters"] = filters
        return filters

    def node(self, node_id):
        for n in (self.topology or {}).get("nodes", []):
            if n["id"] == node_id:
                return n
        raise KeyError("unknown node")

    def link(self, link_id):
        for l in (self.topology or {}).get("links", []):
            if l["id"] == link_id:
                return l
        raise KeyError("unknown link")


lab = Lab()


class Console:
    """One terminal session for the page: a shell in a host container (through a
    pty, so full-screen programs and resizing work) or a router's serial console
    (GNS3's telnet port; the kernel writes its log there and reads the keyboard,
    not the serial line, so it is effectively read-only)."""

    sessions = {}
    lock = threading.Lock()

    def __init__(self, node):
        self.id = secrets.token_hex(8)
        self.node = node
        self.out = queue.Queue(maxsize=4000)
        self.proc = self.sock = self.master = self.container = None
        self.closed = False
        if node["type"] == "docker":
            self.container = container = f"GNS3.{node['name']}.{lab.pid}"
            self.master, slave = pty.openpty()
            self.resize(100, 30)

            def make_controlling_tty():
                os.setsid()
                fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            # LABUI_CONSOLE tags the shell and everything started from it, so close() can end them:
            # killing the docker client alone leaves the shell running inside the container
            self.proc = subprocess.Popen(
                ["docker", "exec", "-it", "-e", "TERM=xterm-256color", "-e", f"LABUI_CONSOLE={self.id}", container, "bash"],
                stdin=slave, stdout=slave, stderr=slave, preexec_fn=make_controlling_tty, close_fds=True)
            os.close(slave)
            threading.Thread(target=self._read_pty, daemon=True).start()
        elif node["type"] == "qemu" and node.get("console"):
            self.sock = socket.create_connection(("127.0.0.1", node["console"]), timeout=5)
            self.sock.settimeout(None)
            self.put(f"\x1b[2m-- {node['name']} serial console (COM1): live kernel log from now on; "
                     f"the kernel reads its keyboard, not this line --\x1b[0m\r\n".encode())
            threading.Thread(target=self._read_telnet, daemon=True).start()
        else:
            raise ValueError(f"{node['name']} has no console")
        with Console.lock:
            Console.sessions[self.id] = self

    def put(self, data):
        try:
            self.out.put_nowait(data)
        except queue.Full:
            pass

    def _read_pty(self):
        while not self.closed:
            r, _, _ = select.select([self.master], [], [], 0.5)
            if not r:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break                       # EIO: the shell exited
            if not data:
                break
            self.put(data)
        self.put(None)

    def _read_telnet(self):
        # Telnet commands are dropped: IAC verb [option], and IAC SB ... IAC SE blocks
        state = "data"
        while not self.closed:
            try:
                data = self.sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            out = bytearray()
            for b in data:
                if state == "data":
                    if b == 255:
                        state = "iac"
                    else:
                        out.append(b)
                elif state == "iac":
                    state = {255: "data", 250: "sb"}.get(b, "option" if 251 <= b <= 254 else "data")
                    if b == 255:
                        out.append(255)     # escaped 0xFF
                elif state == "option":
                    state = "data"
                elif state == "sb":
                    state = "sb_iac" if b == 255 else "sb"
                elif state == "sb_iac":
                    state = "data" if b == 240 else "sb"
            if out:
                self.put(bytes(out).replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.put(None)

    def write(self, data):
        if self.master is not None:
            os.write(self.master, data.encode())
        elif self.sock is not None:
            self.sock.sendall(data.encode())

    def resize(self, cols, rows):
        if self.master is not None:
            fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def close(self):
        if self.closed:
            return
        self.closed = True
        with Console.lock:
            Console.sessions.pop(self.id, None)
        if self.container:
            # tr + grep -x: busybox grep (Alpine images) has no -z
            script = (f"for p in /proc/[0-9]*; do tr '\\0' '\\n' < $p/environ 2>/dev/null | "
                      f"grep -qx 'LABUI_CONSOLE={self.id}' && kill -HUP ${{p#/proc/}}; done; true")
            threading.Thread(target=docker_exec, args=(self.container, "sh", "-c", script), kwargs={"timeout": 10},
                             daemon=True).start()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
        for closer in (lambda: self.sock and self.sock.close(), lambda: self.master is not None and os.close(self.master)):
            try:
                closer()
            except OSError:
                pass

    @classmethod
    def get(cls, session_id):
        with cls.lock:
            s = cls.sessions.get(session_id)
        if not s:
            raise KeyError("no such console session")
        return s


CAPTURE_NAME = re.compile(r"^labui-[0-9a-f]{8}-\d+\.pcap$")


def capture_dir():
    return os.path.expanduser(f"~/GNS3/projects/{lab.pid}/project-files/captures")


def capture(link_id, seconds):
    """Capture a link for a few seconds; returns the packet count and a tcpdump decode of the first ones."""
    lab.link(link_id)
    name = f"labui-{link_id[:8]}-{int(time.time())}.pcap"
    gns3("POST", f"/projects/{lab.pid}/links/{link_id}/start_capture",
         {"capture_file_name": name, "data_link_type": "DLT_EN10MB"})
    try:
        time.sleep(seconds)
    finally:
        gns3("POST", f"/projects/{lab.pid}/links/{link_id}/stop_capture", {})
    path = os.path.join(capture_dir(), name)
    for _ in range(20):                     # ubridge flushes the file asynchronously
        if os.path.exists(path):
            break
        time.sleep(0.1)
    count, size = 0, os.path.getsize(path)
    with open(path, "rb") as f:             # count records: 24-byte file header, 16-byte record headers
        f.seek(24)
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                break
            incl = struct.unpack("<IIII", hdr)[2]
            f.seek(incl, 1)
            count += 1
    decode = subprocess.run(["tcpdump", "-nn", "-e", "-tt", "-c", "80", "-r", path],
                            capture_output=True, text=True)
    lines = decode.stdout.splitlines()
    if lines:
        t0 = float(lines[0].split()[0])
        lines = [f"{float(l.split()[0]) - t0:9.6f} " + l.split(" ", 1)[1] if l[:1].isdigit() else l for l in lines]
    # keep the five newest captures of this tool
    mine = sorted((p for p in os.listdir(capture_dir()) if CAPTURE_NAME.match(p)),
                  key=lambda p: os.path.getmtime(os.path.join(capture_dir(), p)))
    for old in mine[:-5]:
        os.remove(os.path.join(capture_dir(), old))
    return {"name": name, "seconds": seconds, "packets": count, "bytes": size, "lines": lines,
            "error": decode.stderr.strip() if decode.returncode else None}


class Runner:
    """One iperf3 run at a time: a one-off server plus a client running until stopped."""

    def __init__(self):
        self.lock = threading.Lock()
        self.run_id = 0
        self.params = None
        self.procs = []
        self.errored = set()
        self.focus_row = "1"            # whose router counters to poll when idle

    @staticmethod
    def args(p):
        a = ["-P", str(p["parallel"])]
        if p["proto"] == "udp":
            a += ["-u", "-b", f"{p['bitrate']}M", "-l", str(p["length"])]
        elif p["bitrate"]:
            a += ["-b", f"{p['bitrate']}M"]
        if p["reverse"]:
            a.append("-R")
        return a

    @staticmethod
    def cleanup_all():
        """Kill one-off iperf3 processes left in any lab container (e.g. by a killed lab_ui.py)."""
        for r in lab.rows.values():
            for container, pattern in ((r["client"], "iperf3 -c"), (r["server"], f"iperf3 -s -1 -p {IPERF_PORT}")):
                docker_exec(container, "pkill", "-f", pattern)

    def error(self, run_id, message):
        """Report a run's first error only; a replaced or stopped run reports nothing."""
        with self.lock:
            if run_id != self.run_id or run_id in self.errored:
                return False
            self.errored.add(run_id)
        hub.publish({"type": "error", "run": run_id, "message": message})
        return True

    def _kill(self):
        for proc in self.procs:
            if proc.poll() is None:
                proc.terminate()
        if self.params:
            r = lab.rows[self.params["row"]]
            for container, pattern in ((r["client"], "iperf3 -c"), (r["server"], f"iperf3 -s -1 -p {IPERF_PORT}")):
                docker_exec(container, "pkill", "-f", pattern)
        self.procs = []

    def stop(self, reason="stopped"):
        with self.lock:
            was = self.params
            self.run_id += 1                    # first, so the dying run's readers go quiet
            self._kill()
            self.params = None
        if was:
            hub.publish({"type": "stopped", "reason": reason})

    def start(self, p):
        with self.lock:
            self.run_id += 1                    # first, so the dying run's readers go quiet
            self._kill()
            run_id = self.run_id
            self.params = p
            self.focus_row = p["row"]
            r = lab.rows[p["row"]]
            iperf = ["--json-stream", "--forceflush", "-i", str(p["interval"])]
            server = subprocess.Popen(
                ["docker", "exec", r["server"], "iperf3", "-s", "-1", "-p", IPERF_PORT] + iperf,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            time.sleep(0.4)
            client = subprocess.Popen(
                ["docker", "exec", r["client"], "iperf3", "-c", r["server_ip"], "-p", IPERF_PORT,
                 "-t", "0"] + iperf + self.args(p),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            self.procs = [server, client]
        hub.publish({"type": "run", "run": run_id, "params": p, "filters": r["filters"],
                     "command": "iperf3 -c " + r["server_ip"] + " " + " ".join(self.args(p))})
        for proc in (server, client):
            threading.Thread(target=self._read, args=(proc, run_id, p), daemon=True).start()

    def _read(self, proc, run_id, p):
        for line in proc.stdout:
            if run_id != self.run_id:
                return
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("event") == "error":
                if self.error(run_id, "iperf3: " + str(ev.get("data"))):
                    threading.Thread(target=self.stop, args=("error",), daemon=True).start()
                return
            if ev.get("event") != "interval":
                continue
            s = ev["data"]["sum"]
            secs = max(s["end"] - s["start"], 1e-3)
            if secs < p["interval"] * 0.5:      # the partial last interval
                continue
            sample = {"type": "sample", "run": run_id, "side": "sent" if s.get("sender") else "recv",
                      "mbps": s["bits_per_second"] / 1e6}
            if p["proto"] == "udp":
                delivered = s["packets"] - (s.get("lost_packets") or 0)
                sample["pps"] = (delivered if not s.get("sender") else s["packets"]) / secs
                if not s.get("sender"):
                    sample["loss"] = s.get("lost_percent")
                    sample["jitter"] = s.get("jitter_ms")
            else:
                sample["pps"] = s["bytes"] / 1448 / secs      # estimate: full-size segments
                sample["estimated"] = True
                if s.get("sender"):
                    sample["retrans"] = (s.get("retransmits") or 0) / secs
            hub.publish(sample)
        if proc.wait() not in (0, None):
            self.error(run_id, "iperf3 exited unexpectedly")


runner = Runner()


class RouterPoller(threading.Thread):
    """Reads the focused router's /api/stats once a second (curl from the inside
    client) and publishes per-second rates computed from the counter deltas."""

    def __init__(self):
        super().__init__(daemon=True)
        self.prev = {}

    def run(self):
        while True:
            started = time.time()
            try:
                self.poll()
            except Exception as e:  # never let the poller die
                hub.publish({"type": "router", "row": runner.focus_row, "ok": False, "message": str(e)})
            time.sleep(max(0.2, 1.0 - (time.time() - started)))

    def poll(self):
        row = runner.focus_row
        r = lab.rows.get(row)
        if not r or not r["router"]:
            return
        if not r["http"]:
            hub.publish({"type": "router", "row": row, "ok": False,
                         "message": f"{r['router']} has no 'http' on its command line"})
            return
        out = docker_exec(r["client"], "curl", "-s", "-m", "1", f"http://{r['router_ip']}/api/stats", timeout=3)
        now = time.time()
        stats = json.loads(out.stdout)
        prev = self.prev.get(row)
        self.prev[row] = (now, stats)
        if not prev or stats["system"]["uptime_ms"] < prev[1]["system"]["uptime_ms"]:
            return                          # first sample, or the router rebooted
        dt = now - prev[0]
        rate = lambda new, old: max(0.0, (new - old) / dt)
        old_ports = {p["name"]: p for p in prev[1]["interfaces"]}
        ports = []
        for p in stats["interfaces"]:
            o = old_ports.get(p["name"], p)
            ports.append({"name": p["name"], "role": p["role"], "ipv4": p["ipv4"],
                          "rx_pps": rate(p["rx_packets"], o["rx_packets"]),
                          "tx_pps": rate(p["tx_packets"], o["tx_packets"]),
                          "rx_mbps": rate(p["rx_bytes"], o["rx_bytes"]) * 8 / 1e6,
                          "tx_mbps": rate(p["tx_bytes"], o["tx_bytes"]) * 8 / 1e6,
                          "rx_dropped_ps": rate(p["rx_dropped"], o["rx_dropped"]),
                          "tx_dropped_ps": rate(p["tx_dropped"], o["tx_dropped"]),
                          # dropped by the NIC itself (ring full); kernels before the cpu/rx_missed stats lack it
                          "rx_missed_ps": rate(p["rx_missed"], o.get("rx_missed", p["rx_missed"])) if "rx_missed" in p else None})
        n, on = stats["nat"], prev[1]["nat"]
        hub.publish({
            "type": "router", "row": row, "ok": True, "name": r["router"], "ports": ports,
            "forwarded_ps": rate(stats["ipv4"]["forwarded"], prev[1]["ipv4"]["forwarded"]),
            "nat": {"active": n["active"], "created_ps": rate(n["created"], on["created"]),
                    "outbound_ps": rate(n["outbound"], on["outbound"]),
                    "inbound_ps": rate(n["inbound"], on["inbound"]),
                    "firewall_dropped_ps": rate(n["firewall_dropped"], on["firewall_dropped"])},
            # measured by the kernel: share of the last second not spent asleep in hlt
            "cpu_busy_pct": stats["cpu"]["busy_permille"] / 10 if "cpu" in stats else None,
            "heap_used": stats["system"]["heap_used"], "heap_size": stats["system"]["heap_size"],
            "uptime_s": stats["system"]["uptime_ms"] / 1000,
        })


def parse_params(body):
    row = str(body.get("row", "1"))
    if row not in lab.rows:
        raise ValueError("unknown path")
    proto = body.get("proto", "tcp")
    if proto not in ("tcp", "udp"):
        raise ValueError("proto must be tcp or udp")
    parallel = int(body.get("parallel", 1))
    bitrate = int(body.get("bitrate", 0))
    length = int(body.get("length", 1448))
    interval = float(body.get("interval", 0.2))
    if not 1 <= parallel <= 8 or not 0 <= bitrate <= 5000 or not 16 <= length <= 1448:
        raise ValueError("parameter out of range")
    if interval not in INTERVALS:
        raise ValueError(f"interval must be one of {INTERVALS}")
    if proto == "udp" and bitrate == 0:
        raise ValueError("UDP needs a bitrate")
    return {"row": row, "proto": proto, "parallel": parallel, "bitrate": bitrate,
            "length": length, "reverse": bool(body.get("reverse")), "interval": interval}


def parse_filters(body):
    f = {}
    delay, jitter = int(body.get("delay", 0)), int(body.get("jitter", 0))
    loss, corrupt = int(body.get("loss", 0)), int(body.get("corrupt", 0))
    if not (0 <= delay <= 2000 and 0 <= jitter <= 500 and 0 <= loss <= 100 and 0 <= corrupt <= 100):
        raise ValueError("condition out of range")
    if delay or jitter:
        f["delay"] = [delay, jitter]
    if loss:
        f["packet_loss"] = [loss]
    if corrupt:
        f["corrupt"] = [corrupt]
    return f


symbol_cache = {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def reply(self, code, obj=None, ctype="application/json", body=None, cache=False):
        data = body if body is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "max-age=3600" if cache else "no-store")
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def state(self):
        return {"project": PROJECT, "rows": lab.rows, "active": runner.params,
                "topology": lab.topology, "focus": runner.focus_row, "intervals": INTERVALS}

    def events(self):
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

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                return self.reply(200, ctype="text/html; charset=utf-8", body=f.read())
        if url.path == "/api/state":
            try:
                lab.discover()
            except GNS3_ERRORS as e:
                return self.reply(503, {"error": f"GNS3 lab not reachable: {e}"})
            return self.reply(200, self.state())
        if url.path == "/api/symbol":
            symbol = urllib.parse.parse_qs(url.query).get("id", [""])[0]
            if not symbol.startswith(":/symbols/") or ".." in symbol:
                return self.reply(400, {"error": "bad symbol id"})
            if symbol not in symbol_cache:
                try:
                    symbol_cache[symbol] = gns3("GET", f"/symbols/{symbol}/raw", raw=True)
                except GNS3_ERRORS as e:
                    return self.reply(404, {"error": str(e)})
            return self.reply(200, ctype="image/svg+xml", body=symbol_cache[symbol], cache=True)
        if url.path == "/api/events":
            return self.events()
        if url.path == "/api/console/stream":
            return self.console_stream(urllib.parse.parse_qs(url.query).get("id", [""])[0])
        if url.path == "/api/capture/file":
            name = urllib.parse.parse_qs(url.query).get("name", [""])[0]
            path = os.path.join(capture_dir(), name)
            if not CAPTURE_NAME.match(name) or not os.path.exists(path):
                return self.reply(404, {"error": "no such capture"})
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.tcpdump.pcap")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return None
        return self.reply(404, {"error": "not found"})

    def console_stream(self, session_id):
        try:
            session = Console.get(session_id)
        except KeyError as e:
            return self.reply(404, {"error": str(e)})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            while True:
                try:
                    data = session.out.get(timeout=10)
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
                    continue
                if data is None:
                    self.wfile.write(b"event: end\ndata: {}\n\n")
                    self.wfile.flush()
                    break
                chunks = [data]
                while not session.out.empty():      # coalesce bursts into one event
                    more = session.out.get_nowait()
                    if more is None:
                        session.out.put(None)
                        break
                    chunks.append(more)
                self.wfile.write(b"data: " + base64.b64encode(b"".join(chunks)) + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            session.close()                 # the page closed the terminal (or the tab)
        return None

    def do_POST(self):
        try:
            body = self.body()
            if self.path == "/api/lab/start":
                lab.discover(start=True)
                return self.reply(200, self.state())
            if not lab.rows:
                lab.discover()
            if self.path == "/api/run":
                runner.start(parse_params(body))
                return self.reply(200, self.state())
            if self.path == "/api/stop":
                runner.stop()
                return self.reply(200, self.state())
            if self.path == "/api/focus":
                row = str(body.get("row"))
                if row not in lab.rows:
                    raise ValueError("unknown path")
                if not runner.params:
                    runner.focus_row = row
                return self.reply(200, {"focus": runner.focus_row})
            if self.path == "/api/conditions":
                link_id = body.get("link_id")
                if not link_id:                     # by path: the link to that path's server
                    row = str(body.get("row"))
                    if row not in lab.rows:
                        raise ValueError("unknown path")
                    link_id = lab.rows[row]["link_id"]
                lab.link(link_id)
                filters = lab.set_link_filters(link_id, parse_filters(body))
                row = next((r["row"] for r in lab.rows.values() if r["link_id"] == link_id), None)
                hub.publish({"type": "conditions", "row": row, "link_id": link_id, "filters": filters})
                return self.reply(200, self.state())
            if self.path == "/api/node/action":
                node, action = lab.node(body.get("node_id")), body.get("action")
                if action not in ("start", "stop", "reload"):
                    raise ValueError("action must be start, stop or reload")
                if runner.params and action != "start":
                    runner.stop("node " + action)
                gns3("POST", f"/projects/{lab.pid}/nodes/{node['id']}/{action}", {})
                lab.discover()
                hub.publish({"type": "node", "node_id": node["id"], "action": action})
                return self.reply(200, self.state())
            if self.path == "/api/capture":
                seconds = float(body.get("seconds", 1))
                if not 0.5 <= seconds <= 10:
                    raise ValueError("capture 0.5-10 seconds")
                return self.reply(200, capture(body.get("link_id"), seconds))
            if self.path == "/api/console/open":
                session = Console(lab.node(body.get("node_id")))
                return self.reply(200, {"id": session.id, "node": session.node["name"],
                                        "kind": "shell" if session.master is not None else "serial"})
            if self.path == "/api/console/input":
                Console.get(body.get("id")).write(str(body.get("data", "")))
                return self.reply(200, {"ok": True})
            if self.path == "/api/console/resize":
                cols, rows = int(body.get("cols", 80)), int(body.get("rows", 24))
                if not (10 <= cols <= 500 and 3 <= rows <= 200):
                    raise ValueError("terminal size out of range")
                Console.get(body.get("id")).resize(cols, rows)
                return self.reply(200, {"ok": True})
            if self.path == "/api/console/close":
                try:
                    Console.get(body.get("id")).close()
                except KeyError:
                    pass
                return self.reply(200, {"ok": True})
        except ValueError as e:
            return self.reply(400, {"error": str(e)})
        except KeyError as e:
            return self.reply(404, {"error": str(e.args[0]) if e.args else "not found"})
        except GNS3_ERRORS as e:
            return self.reply(503, {"error": f"GNS3 lab not reachable: {e}"})
        return self.reply(404, {"error": "not found"})


def interrupted(*_):
    raise KeyboardInterrupt


def main():
    # Bind first: a second instance must not clean up the iperf3 runs of the first one
    try:
        server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    except OSError as e:
        if e.errno == errno.EADDRINUSE:
            sys.exit(f"Port {PORT} is taken: lab_ui.py is probably running already, so open "
                     f"http://localhost:{PORT}. (Another port: LAB_UI_PORT=8091 python3 {sys.argv[0]})")
        raise
    server.daemon_threads = True
    try:
        lab.discover()
        runner.cleanup_all()
    except Exception as e:  # the page shows the problem and offers to start the lab
        print(f"warning: {e}")
    RouterPoller().start()
    signal.signal(signal.SIGTERM, interrupted)      # kill: stop the run like Ctrl+C does
    print(f"LazyDaysOS throughput lab: http://localhost:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        runner.stop("server shutting down")
        for session in list(Console.sessions.values()):
            session.close()


if __name__ == "__main__":
    main()
