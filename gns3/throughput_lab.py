#!/usr/bin/env python3
"""Build the "LazyDaysOS throughput" GNS3 project (GNS3 2.2 server on 127.0.0.1:3080).

    baseline:  cli-0 ------------------------- srv-0   no router: GNS3's own ceiling
    routed:    cli-1 -- eth0 R-routed eth1 -- srv-1    plain IPv4 forwarding
    nat:       cli-2 -- eth0  R-nat   eth1 -- srv-2    NAPT + firewall (nat=eth1)

Routers run "http" so lab_ui.py can read their counters from the inside client.

Hosts are Docker containers (nicolaka/netshoot: iperf3, jq, tcpdump, ...).
Servers run "iperf3 -s" (their console shows the server log); clients get a
shell with the presets in /lab/tests.sh. Links are direct: GNS3's Ethernet
switch (Dynamips) would be the bottleneck.

Needs the "LazyDaysOS" QEMU template (kernel from sysroot/) and Docker access.
"""
import json, os, sys, urllib.request, urllib.error

BASE = "http://127.0.0.1:3080/v2"
IMAGE = "nicolaka/netshoot:latest"
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "LazyDaysOS throughput"

# name, y, router command line (None = no router), client side, server side, server gateway
ROWS = [
    ("0", -160, None, ("10.0.0.1/24", None), ("10.0.0.2/24", None)),
    ("1", 0, "eth0=10.1.1.1/24 eth1=10.1.2.1/24 http",
     ("10.1.1.2/24", "10.1.1.1"), ("10.1.2.2/24", "10.1.2.1")),
    # the NAT server has no route back to 10.2.1.0/24: replies only work through the NAT
    ("2", 160, "eth0=10.2.1.1/24 eth1=10.2.2.1/24 nat=eth1 http",
     ("10.2.1.2/24", "10.2.1.1"), ("10.2.2.2/24", None)),
]
ROUTER_NAMES = {"1": "R-routed", "2": "R-nat"}
LABELS = {"0": "baseline: no router (GNS3 link ceiling)",
          "1": "routed: plain forwarding 10.1.1.0/24 -> 10.1.2.0/24",
          "2": "NAT: nat=eth1, server has no route back"}


def call(method, path, body=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, method=method, data=data,
                                 headers={} if raw is not None else {"Content-Type": "application/json"})
    try:
        out = urllib.request.urlopen(req).read()
        return json.loads(out) if out else None
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path} -> {e.code} {e.read().decode()[:600]}")


def interfaces(cidr, gateway):
    ip, prefix = cidr.split("/")
    mask = ".".join(str((0xFFFFFFFF << (32 - int(prefix)) >> s) & 0xFF) for s in (24, 16, 8, 0))
    # GNS3 gives container interfaces MTU 65521; ubridge then fails on the jumbo
    # frames ("send: Message too long") and that link stays dead. Real Ethernet.
    text = f"auto eth0\niface eth0 inet static\n\taddress {ip}\n\tnetmask {mask}\n\tmtu 1500\n"
    if gateway:
        text += f"\tgateway {gateway}\n"
    return text


def main():
    templates = {t["name"]: t["template_id"] for t in call("GET", "/templates")}
    if "LazyDaysOS" not in templates:
        sys.exit("create the LazyDaysOS QEMU template first")
    if "netshoot" not in templates:
        templates["netshoot"] = call("POST", "/templates", {
            "name": "netshoot", "template_type": "docker", "compute_id": "local",
            "image": IMAGE, "adapters": 1, "console_type": "telnet", "category": "guest",
            "symbol": ":/symbols/computer.svg", "default_name_format": "host-{0}",
            "extra_volumes": ["/lab"]})["template_id"]

    for p in call("GET", "/projects"):
        if p["name"] == PROJECT:
            sys.exit(f"project '{PROJECT}' exists already ({p['project_id']}); delete it first")
    pid = call("POST", "/projects", {"name": PROJECT})["project_id"]

    def node(template, x, y, name, properties=None):
        n = call("POST", f"/projects/{pid}/templates/{templates[template]}", {"x": x, "y": y, "compute_id": "local"})
        body = {"name": name}
        if properties:
            body["properties"] = properties
        call("PUT", f"/projects/{pid}/nodes/{n['node_id']}", body)
        return n["node_id"]

    def put_file(node_id, path, text):
        call("POST", f"/projects/{pid}/nodes/{node_id}/files/{path}", raw=text.encode())

    def link(a, a_adapter, b, b_adapter):
        call("POST", f"/projects/{pid}/links", {"nodes": [
            {"node_id": a, "adapter_number": a_adapter, "port_number": 0},
            {"node_id": b, "adapter_number": b_adapter, "port_number": 0}]})

    tests = open(os.path.join(HERE, "tests.sh")).read()
    for row, y, cmdline, (cli_ip, cli_gw), (srv_ip, srv_gw) in ROWS:
        cli = node("netshoot", -320, y, f"cli-{row}")
        srv = node("netshoot", 320, y, f"srv-{row}", {"start_command": "iperf3 -s"})
        put_file(cli, "etc/network/interfaces", interfaces(cli_ip, cli_gw))
        put_file(srv, "etc/network/interfaces", interfaces(srv_ip, srv_gw))
        put_file(cli, "lab/tests.sh", tests)
        put_file(cli, "lab/server", srv_ip.split("/")[0] + "\n")
        if cmdline:
            r = node("LazyDaysOS", 0, y, ROUTER_NAMES[row], {"kernel_command_line": cmdline})
            link(cli, 0, r, 0)
            link(r, 1, srv, 0)
        else:
            link(cli, 0, srv, 0)
        svg = ('<svg height="20" width="420"><text font-family="Consolas" font-size="10" '
               f'font-weight="bold" fill="#1f2937" fill-opacity="1.0">{LABELS[row]}</text></svg>')
        call("POST", f"/projects/{pid}/drawings", {"x": -320, "y": y - 40, "svg": svg})

    call("POST", f"/projects/{pid}/nodes/start", {})
    for n in call("GET", f"/projects/{pid}/nodes"):
        print(f"{n['name']:9} {n['status']:8} console {n['console']}")
    print("project", pid)


if __name__ == "__main__":
    main()
