#!/usr/bin/env python3
"""Build the "LazyDaysOS metro" GNS3 project: a provider-style network of 28 LazyDaysOS
routers speaking RIP, with 16 customers, 2 servers and a NOC host (see metro.py).

    python3 gns3/metro_lab.py build [--timers U,T,G] [--ram MB]   (defaults 5,30,20 and 32 MB)
    python3 gns3/metro_lab.py start      (re)open the project and start every node
    python3 gns3/metro_lab.py delete

Routers boot ~/GNS3/images/QEMU/LazyDaysOS-metro.bin, a copy of ./LazyDaysOS.bin made
by "build", so other labs keep their kernel. Hosts are nicolaka/netshoot containers.
The dashboard is gns3/net_ui.py (http://localhost:8095).
"""
import json, os, shutil, sys, time, urllib.error, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metro  # noqa: E402

BASE = "http://127.0.0.1:3080/v2"
PROJECT = os.environ.get("METRO_PROJECT", "LazyDaysOS metro")
IMAGE_NAME = "LazyDaysOS-metro.bin"
IMAGE_DIR = os.path.expanduser("~/GNS3/images/QEMU")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCALE = 1.0                             # metro.py coordinates -> GNS3 canvas


def call(method, path, body=None, raw=None, check=True):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, method=method, data=data,
                                 headers={} if raw is not None else {"Content-Type": "application/json"})
    try:
        out = urllib.request.urlopen(req, timeout=120).read()
        return json.loads(out) if out else None
    except urllib.error.HTTPError as e:
        if not check:
            return None
        sys.exit(f"{method} {path} -> {e.code} {e.read().decode()[:600]}")


def project():
    return next((p for p in call("GET", "/projects") if p["name"] == PROJECT), None)


def interfaces(cidr, gateway):
    ip, prefix = cidr.split("/")
    mask = ".".join(str((0xFFFFFFFF << (32 - int(prefix)) >> s) & 0xFF) for s in (24, 16, 8, 0))
    # MTU 1500: GNS3's default for containers (65521) breaks ubridge links
    return (f"auto eth0\niface eth0 inet static\n\taddress {ip}\n\tnetmask {mask}\n\tmtu 1500\n"
            f"\tgateway {gateway}\n")


def build(timers, ram):
    if project():
        sys.exit(f"project '{PROJECT}' exists already; delete it first (metro_lab.py delete)")
    kernel = os.path.join(REPO, "LazyDaysOS.bin")
    if not os.path.exists(kernel):
        sys.exit("build the kernel first (make)")
    shutil.copy(kernel, os.path.join(IMAGE_DIR, IMAGE_NAME))
    md5 = os.path.join(IMAGE_DIR, IMAGE_NAME + ".md5sum")
    if os.path.exists(md5):
        os.remove(md5)                  # GNS3 recomputes it
    templates = {t["name"]: t["template_id"] for t in call("GET", "/templates")}
    for needed in ("LazyDaysOS", "netshoot"):
        if needed not in templates:
            sys.exit(f"the '{needed}' template is missing (run throughput_lab.py once, or create it)")

    routers, links, hosts = metro.topology(timers)
    # auto_close off: GNS3 otherwise closes the project (stopping every node) when the
    # last GUI client disconnects, even while the dashboard is using it
    pid = call("POST", "/projects", {"name": PROJECT, "auto_close": False})["project_id"]
    ids = {}
    for name, r in routers.items():
        n = call("POST", f"/projects/{pid}/templates/{templates['LazyDaysOS']}",
                 {"x": int(r["x"] * SCALE), "y": int(r["y"] * SCALE), "compute_id": "local"})
        call("PUT", f"/projects/{pid}/nodes/{n['node_id']}", {"name": name, "properties": {
            "kernel_image": IMAGE_NAME, "kernel_command_line": r["cmdline"], "ram": ram,
            "adapters": len(r["ports"])}})
        ids[name] = n["node_id"]
    for name, h in hosts.items():
        n = call("POST", f"/projects/{pid}/templates/{templates['netshoot']}",
                 {"x": int(h["x"] * SCALE), "y": int(h["y"] * SCALE), "compute_id": "local"})
        props = {"start_command": "iperf3 -s"} if h["role"] == "server" else {}
        call("PUT", f"/projects/{pid}/nodes/{n['node_id']}", {"name": name, **({"properties": props} if props else {})})
        call("POST", f"/projects/{pid}/nodes/{n['node_id']}/files/etc/network/interfaces",
             raw=interfaces(h["cidr"], h["gateway"]).encode())
        ids[name] = n["node_id"]
    for a, pa, b, pb, _ in links:
        call("POST", f"/projects/{pid}/links", {"nodes": [
            {"node_id": ids[a], "adapter_number": pa, "port_number": 0},
            {"node_id": ids[b], "adapter_number": pb, "port_number": 0}]})
    for name, h in hosts.items():
        call("POST", f"/projects/{pid}/links", {"nodes": [
            {"node_id": ids[name], "adapter_number": 0, "port_number": 0},
            {"node_id": ids[h["router"]], "adapter_number": h["port"], "port_number": 0}]})
    for text, y in (("core: full mesh", -175), ("aggregation: dual-homed to the core", -345),
                    ("access: dual-homed to aggregation, one customer each", -540)):
        svg = ('<svg height="20" width="400"><text font-family="Segoe UI" font-size="12" fill="#64748b">'
               f'{text}</text></svg>')
        call("POST", f"/projects/{pid}/drawings", {"x": -60, "y": y, "svg": svg})
    print(f"project '{PROJECT}' ({pid}): {len(routers)} routers, {len(links)} router links, "
          f"{len(hosts)} hosts; RIP timers {timers}; starting...")
    t = time.time()
    call("POST", f"/projects/{pid}/nodes/start", {})
    print(f"started in {time.time() - t:.0f} s; dashboard: python3 gns3/net_ui.py")


def start():
    p = project()
    if not p:
        sys.exit(f"no project '{PROJECT}'")
    call("PUT", f"/projects/{p['project_id']}", {"auto_close": False})
    if p["status"] != "opened":
        call("POST", f"/projects/{p['project_id']}/open", {})
    t = time.time()
    call("POST", f"/projects/{p['project_id']}/nodes/start", {})
    print(f"'{PROJECT}' started in {time.time() - t:.0f} s")


def delete():
    p = project()
    if not p:
        sys.exit(f"no project '{PROJECT}'")
    call("POST", f"/projects/{p['project_id']}/nodes/stop", {}, check=False)
    call("DELETE", f"/projects/{p['project_id']}")
    print(f"deleted '{PROJECT}'")


if __name__ == "__main__":
    args = sys.argv[1:] or ["build"]
    if args[0] == "build":
        build(args[args.index("--timers") + 1] if "--timers" in args else metro.TIMERS,
              int(args[args.index("--ram") + 1]) if "--ram" in args else 32)
    elif args[0] == "start":
        start()
    elif args[0] == "delete":
        delete()
    else:
        sys.exit(__doc__)
