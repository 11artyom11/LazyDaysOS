#!/usr/bin/env python3
"""Build a mesh of LazyDaysOS routers speaking RIP in GNS3 (2.2 server on 127.0.0.1:3080).

    python3 gns3/mesh_lab.py build [ROWSxCOLS] [--ram MB] [--timers U,T,G]   (default 10x10, 32 MB)
    python3 gns3/mesh_lab.py check      convergence: every router's route count, ping across
    python3 gns3/mesh_lab.py delete     stop and delete the project

Topology: a torus. Router R-r-c links east to R-r-(c+1) and south to R-(r+1)-c,
wrapping at the edges, so every router has 4 neighbours, there are 2*rows*cols
links with many redundant paths (and loops), and no two routers are more than
rows/2 + cols/2 hops apart (RIP's limit is 15). Ports: eth0 east, eth1 west,
eth2 south, eth3 north; eth4 on two routers leads to a host.

Addresses: the east link of R-r-c is 10.1.(r*cols+c).0/24 (.1 west end, .2 east
end), its south link 10.2.(r*cols+c).0/24 (.1 north end, .2 south end). Host A
sits on R-0-0 (10.0.1.0/24), host B on the router farthest from it (10.0.2.0/24).

The routers boot a copy of the kernel, ~/GNS3/images/QEMU/LazyDaysOS-mesh.bin
(copied from ./LazyDaysOS.bin by "build"), so the throughput lab's kernel stays
as it is. With 32 MB each, like the TL-WR841N, 100 routers need about 3-4 GB.
"""
import json, os, shutil, subprocess, sys, time, urllib.error, urllib.request

BASE = "http://127.0.0.1:3080/v2"
PROJECT = os.environ.get("MESH_PROJECT", "LazyDaysOS mesh")
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE_NAME = "LazyDaysOS-mesh.bin"
IMAGE_DIR = os.path.expanduser("~/GNS3/images/QEMU")
PASSWORD = "lazydays-lab"
HOST_IMAGE = "nicolaka/netshoot:latest"


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


def layout(rows, cols):
    """Router names, command lines and links of the torus"""
    def link_net(kind, r, c):
        return f"10.{kind}.{r * cols + c}"
    far = (rows // 2, cols // 2)
    routers, links = {}, []
    for r in range(rows):
        for c in range(cols):
            east, west = link_net(1, r, c), link_net(1, r, (c - 1) % cols)
            south, north = link_net(2, r, c), link_net(2, (r - 1) % rows, c)
            ports = [f"eth0={east}.1/24", f"eth1={west}.2/24", f"eth2={south}.1/24", f"eth3={north}.2/24"]
            if (r, c) == (0, 0):
                ports.append("eth4=10.0.1.1/24")
            if (r, c) == far:
                ports.append("eth4=10.0.2.1/24")
            routers[(r, c)] = " ".join(ports + ["rip=eth0,eth1,eth2,eth3", "http", f"admin_password={PASSWORD}"])
            links.append(((r, c), 0, (r, (c + 1) % cols), 1))      # east -- west
            links.append(((r, c), 2, ((r + 1) % rows, c), 3))      # south -- north
    return routers, links, far


def interfaces(cidr, gateway):
    ip, prefix = cidr.split("/")
    mask = ".".join(str((0xFFFFFFFF << (32 - int(prefix)) >> s) & 0xFF) for s in (24, 16, 8, 0))
    # MTU 1500: GNS3's default for containers (65521) breaks ubridge links
    return (f"auto eth0\niface eth0 inet static\n\taddress {ip}\n\tnetmask {mask}\n\tmtu 1500\n"
            f"\tgateway {gateway}\n")


def build(size="10x10", ram=32, timers=None):
    rows, cols = (int(x) for x in size.lower().split("x"))
    if rows // 2 + cols // 2 > 15:
        sys.exit(f"{size}: routers up to {rows // 2 + cols // 2} hops apart, more than RIP's 15")
    if 2 * rows * cols + 2 > 250:
        sys.exit(f"{size}: {2 * rows * cols + 2} networks, more than the 256-route tables hold comfortably")
    if project():
        sys.exit(f"project '{PROJECT}' exists already; delete it first (mesh_lab.py delete)")
    kernel = os.path.join(os.path.dirname(HERE), "LazyDaysOS.bin")
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
    pid = call("POST", "/projects", {"name": PROJECT})["project_id"]
    routers, links, far = layout(rows, cols)
    extra = f" rip_timers={timers}" if timers else ""

    ids = {}
    for (r, c), cmdline in routers.items():
        n = call("POST", f"/projects/{pid}/templates/{templates['LazyDaysOS']}",
                 {"x": c * 130 - cols * 65, "y": r * 110 - rows * 55, "compute_id": "local"})
        call("PUT", f"/projects/{pid}/nodes/{n['node_id']}", {"name": f"R-{r}-{c}", "properties": {
            "kernel_image": IMAGE_NAME, "kernel_command_line": cmdline + extra, "ram": ram,
            "adapters": 5 if (r, c) in ((0, 0), far) else 4}})
        ids[(r, c)] = n["node_id"]
        print(f"\rrouters: {len(ids)}/{rows * cols}", end="", flush=True)
    print()
    for i, (a, a_port, b, b_port) in enumerate(links):
        call("POST", f"/projects/{pid}/links", {"nodes": [
            {"node_id": ids[a], "adapter_number": a_port, "port_number": 0},
            {"node_id": ids[b], "adapter_number": b_port, "port_number": 0}]})
        print(f"\rlinks: {i + 1}/{len(links)}", end="", flush=True)
    print()
    for name, router, net, (x, y) in (("host-A", (0, 0), "10.0.1", (-cols * 65 - 150, -rows * 55 - 80)),
                                      ("host-B", far, "10.0.2", (far[1] * 130 - cols * 65 + 60, far[0] * 110 - rows * 55 + 60))):
        n = call("POST", f"/projects/{pid}/templates/{templates['netshoot']}", {"x": x, "y": y, "compute_id": "local"})
        call("PUT", f"/projects/{pid}/nodes/{n['node_id']}", {"name": name})
        call("POST", f"/projects/{pid}/nodes/{n['node_id']}/files/etc/network/interfaces",
             raw=interfaces(f"{net}.2/24", f"{net}.1").encode())
        call("POST", f"/projects/{pid}/links", {"nodes": [
            {"node_id": n["node_id"], "adapter_number": 0, "port_number": 0},
            {"node_id": ids[router], "adapter_number": 4, "port_number": 0}]})
    print(f"project '{PROJECT}' ({pid}): {rows * cols} routers, {len(links)} links, 2 hosts; starting...")
    start = time.time()
    call("POST", f"/projects/{pid}/nodes/start", {})
    print(f"started in {time.time() - start:.0f} s")


def docker_exec(node, *cmd, timeout=10):
    pid = project()["project_id"]
    return subprocess.run(["docker", "exec", f"GNS3.{node}.{pid}", *cmd], capture_output=True, text=True,
                          timeout=timeout)


def check():
    p = project()
    if not p:
        sys.exit(f"no project '{PROJECT}'")
    nodes = call("GET", f"/projects/{p['project_id']}/nodes")
    names = sorted(n["name"] for n in nodes if n["name"].startswith("R-"))
    rows = 1 + max(int(n.split("-")[1]) for n in names)
    cols = 1 + max(int(n.split("-")[2]) for n in names)
    expected = 2 * rows * cols + 2
    stopped = [n["name"] for n in nodes if n["status"] != "started"]
    print(f"{len(names)} routers ({rows}x{cols}), {len(stopped)} nodes not started {stopped[:5]}")
    # Each router's /api/stats, fetched from host A through the mesh (its east address)
    counts, unreachable = {}, []
    for name in names:
        r, c = int(name.split("-")[1]), int(name.split("-")[2])
        out = docker_exec("host-A", "curl", "-s", "-m", "3", "-u", f"admin:{PASSWORD}",
                          f"http://10.1.{r * cols + c}.1/api/stats")
        try:
            s = json.loads(out.stdout)
            counts[name] = len(s["routes"])
        except (ValueError, KeyError):
            unreachable.append(name)
    full = sum(1 for v in counts.values() if v >= expected)
    print(f"routes: {full}/{len(names)} routers have all {expected} networks; "
          f"min {min(counts.values(), default=0)}, unreachable from host A: {len(unreachable)} {unreachable[:5]}")
    out = docker_exec("host-A", "ping", "-c", "3", "-W", "2", "10.0.2.2")
    print("host A -> host B:", (out.stdout.strip().splitlines() or ["?"])[-2])
    ttl = [l for l in out.stdout.splitlines() if "ttl=" in l]
    if ttl:
        hops = 64 - int(ttl[0].split("ttl=")[1].split()[0])
        print(f"path: {hops} routers (TTL {64 - hops})")


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
        size = next((a for a in args[1:] if "x" in a and not a.startswith("--")), "10x10")
        ram = int(args[args.index("--ram") + 1]) if "--ram" in args else 32
        timers = args[args.index("--timers") + 1] if "--timers" in args else None
        build(size, ram, timers)
    elif args[0] == "check":
        check()
    elif args[0] == "delete":
        delete()
    else:
        sys.exit(__doc__)
