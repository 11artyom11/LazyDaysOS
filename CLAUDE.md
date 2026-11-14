# CLAUDE.md

Guidance for Claude Code when working in this repository.

New to the project or to this machine? Read [`docs/HANDOVER.md`](docs/HANDOVER.md) first: machine setup with every third-party dependency, the GNS3 installation, the state of the work, gotchas and open items.

## What this is

LazyDaysOS (a.k.a. DudeOS) is a hobby 32-bit x86 kernel in C and GNU `as` (AT&T syntax), booted by GRUB via **Multiboot v1**. It sets up a GDT, remaps the 8259 PICs, installs an IDT, builds a physical frame allocator from the Multiboot memory map, enables paging, creates a kernel heap (`kmalloc`/`kfree`), runs a 100 Hz PIT timer, scans PCI, drives Intel e1000 NICs behind a generic `netdev` layer, runs an IPv4 stack that **forwards between ports** (Ethernet, ARP, IPv4 with a static routing table, ICMP echo and errors, UDP, DHCP client and server, NAT with connection tracking and an inbound firewall, a TCP server endpoint, an HTTP management API with per-device accounting and blocking, and a web dashboard) configured from the kernel command line, runs boot-time self-tests, and echoes keyboard input to the VGA text console. Kernel messages are mirrored to COM1. There is no userspace or scheduler yet.

**Project goal:** a router OS for a degree thesis. The network stack (ARP, IPv4, forwarding, DHCP, NAT, firewall, a small TCP server for a web dashboard) is developed on x86 in QEMU/GNS3 against e1000 NICs, then ported to a MediaTek MT7628 TP-Link router. Keep everything above `netdev` free of x86 and e1000 specifics so it ports unchanged.

## Toolchain

- Requires an **`i686-elf` cross compiler**. The host `gcc` will not work: `libk/include/tty.h` `#error`s if `__linux__` is defined or `__i386__` is not.
- On this machine: binutils 2.47 + GCC 15.3.0 (C only, with libgcc) installed in `~/opt/cross`, with `~/opt/cross/bin` added to `PATH` in `~/.bashrc`. Sources and the rebuild script live in `~/opt/cross-build/build.sh`.
- In a non-interactive shell, prefix commands with `export PATH="$HOME/opt/cross/bin:$PATH";` if `i686-elf-gcc` is not found.
- Other tools: `grub-mkrescue` + `grub-pc-bin` (BIOS `i386-pc` modules are required, since EFI-only GRUB makes an unbootable ISO for this kernel), `xorriso`, `mtools`, `grub-file`, `qemu-system-i386`, `gdb`. No `nasm`; the code uses GNU `as`. The README holds the cross-compiler build recipe.
- `.vscode/c_cpp_properties.json` resolves the toolchain via `${env:HOME}/opt/cross` and hard-codes the GCC version (`15.3.0`) in the include path. Update it if the toolchain is rebuilt with a different version.

## Build / run / debug

```bash
make               # -> ./LazyDaysOS.bin (objects/archives land in the repo root)
make check         # check_qemu.sh (in-kernel self tests, TCG then KVM) then check_net.sh (router tests against Linux), then `make -C fuzz check` (fuzz corpus replay); exit 0 = all passed
make install       # copies libk + headers + kernel into sysroot/, builds ISO, runs multiboot check
./bootit.sh        # qemu -kernel sysroot/boot/kernel/LazyDaysOS.bin, one e1000 on user networking, serial on this terminal
./bootit.sh --iso  # qemu -cdrom sysroot/boot/kernel/LazyDaysOS.iso (GRUB boots immediately)
./bootit.sh -d     # adds qemu -s -S (waits for gdb on :1234); combinable with --iso
./demo.sh          # live home-router demo with 4 simulated devices; dashboard at http://localhost:8080
./debug.sh         # opens two gnome-terminal windows: qemu -d + `gdb --command=gdbconf` (breaks at _start)
make clean
```

- **Run `make check` after every kernel change.** It passes `test` on the kernel command line, and the kernel then reports its self-test result through QEMU's `isa-debug-exit` device (port `0xF4`): QEMU exit status 1 = pass, 3 = a test failed or the kernel panicked. `check_qemu.sh` prints the serial log (also saved to `qemu_check.log`) and also catches hangs (30 s timeout) and triple faults (`-no-reboot`). It boots twice: with QEMU's software CPU (TCG) and, if `/dev/kvm` is usable, with **KVM** (`qemu_check_kvm.log`; `KVM=no` skips it). The second pass exists because TCG doesn't enforce everything real CPUs do: the kernel's GDT once ended at 256 MiB, which TCG ignored but KVM (and GNS3, which always uses KVM) turned into a #GP on the first e1000 register write. It attaches **two e1000 NICs** on separate QEMU user networks (slirp: 10.0.2.0/24 and 10.0.3.0/24), configures them from the command line (eth0 via QEMU's built-in DHCP server, eth1 statically) and pings each network's gateway from the kernel; traffic goes to `qemu_net0.pcap` / `qemu_net1.pcap` (read with `tcpdump -nn -e -r qemu_net0.pcap`).

### Kernel command line

Whitespace-separated words and `key=value` options (`kernel/cmdline.c`). With `-kernel`, pass them via QEMU `-append`; `bootit.sh` uses `eth0=10.0.2.15/24 gw=10.0.2.2` (QEMU user networking defaults).

| Option | Meaning |
|---|---|
| `test` | test mode: run all self tests, exit QEMU via `isa-debug-exit` |
| `ethN=a.b.c.d/len` | IPv4 address of port `ethN` (adds the connected route) |
| `ethN=dhcp` | DHCP client on `ethN`: address, mask, default route and DNS from the lease |
| `gw=a.b.c.d` | default route via that gateway (must be on a connected network) |
| `route=net/len,gateway[,ethN]` | static route (repeatable); without `ethN` the gateway must already be on a connected network |
| `dhcpd=ethN,first,last` | DHCP server for `first`–`last` on `ethN` (repeatable; `ethN` needs a static address in that subnet) |
| `dns=a.b.c.d` | DNS server the DHCP server hands out (default: the one the DHCP client learned) |
| `dhcp_lease=seconds` | DHCP server lease time, 60–604800 (default 3600) |
| `http` | HTTP management API and dashboard on port 80, inside ports only, login required |
| `admin_password=pw` | the API's admin password (default: a random one printed on the console at boot) |
| `tcp_test` | TCP test services: echo on port 7, 1 MiB chargen stream on port 19 |
| `nat=ethN` | `ethN` is the outside (WAN) port: NAPT outbound, only tracked replies inbound (repeatable, max 2) |
| `udp_echo` | enable the RFC 862 UDP echo service on port 7 (testing) |
| `rip` / `rip=ethN,...` | RIPv2 on all non-NAT-outside ports / on these ports |
| `rip_timers=U,T,G` | RIP update, timeout, garbage timers in seconds (default 30,180,120) |
| `ping=a.b.c.d` | test mode: the kernel must get an echo reply from it (repeatable) |
- `check_net.sh` needs no root. For each **scenario** it re-runs itself under `unshare --user --map-root-user --net`; that namespace is the **LAN host** (`tap0` 10.0.9.1/24, default via the router 10.0.9.2). A second namespace (`unshare --net sleep`, entered with the script's `wan` helper = `nsenter --net`) is the **WAN host** (`tap1` 10.0.8.1/24, 192.168.50.1/32 on `lo`, route back to 10.0.9.0/24 via 10.0.8.2). `tap1` is moved into it *after* QEMU has opened it.
  - `static`: `eth1=10.0.8.2/24`, `route=192.168.50.0/24,10.0.8.1`, `udp_echo`. Host behaviour, forwarding both ways (`ttl=63`), static route, forwarded ping/UDP bursts with zero loss, ICMP time exceeded / net / host / port unreachable, UDP echo; with `http` and no `admin_password=`, the random boot password is printed and works.
  - `home`: `eth1=dhcp` against `dnsmasq` on the WAN host (reserves 10.0.8.2 for MAC `52:54:00:12:34:57`, gateway 10.0.8.1, DNS 10.0.8.53), `dhcpd=eth0,10.0.9.100,10.0.9.150`, `nat=eth1`. The WAN host has **no route back** to the LAN (`WAN_ROUTE_BACK=no`), so everything must go through the NAT. Checks the WAN lease and DHCP default route; the LAN server with real `busybox udhcpc` (lease contents incl. DNS and a `-x hostname:lazy-laptop`, stable address, out-of-pool request refused) and with `dhcp_probe`, a Python INIT-REBOOT request (ACK / NAK cases); then NAT against `wan_echo_server` (UDP echo :5002, TCP echo :8080, logs the peer it sees): translated source with preserved port, two LAN hosts on the same port, 1 MiB TCP echo intact, ICMP port unreachable translated back, address-and-port-dependent filtering, and the firewall (a LAN UDP listener must receive nothing; a ping alone can't show this because a leaked reply comes back NATed and ping ignores it); finally the HTTP API (`http`): `/api/stats` cross-checked against what the scenario did, CPU load low when idle and higher under a forwarded `ping -f` (~0.1% vs ~45-50% under TCG), `/api/events` and `since=`, block → ping fails → dashboard still reachable → unblock, 400/404/405/408/413/431, 30 parallel requests. Then attacks, through the `attack` helper (raw sockets, spoofed sources): the WAN can't connect to the dashboard on the WAN address or by addressing the LAN address directly (SYN silently dropped; pings to the LAN address unanswered), the router's TCP ISNs can't be predicted from earlier ones, a WAN SYN flood takes no connection slots, LAN clients still get the dashboard during a spoofed LAN SYN flood (SYN cookies), and the LAN keeps Internet access during a WAN ping flood from ~250 nonexistent neighbours (ARP table can't be flushed of the gateway).

  - `rip`: three routers in a triangle (R1-R2-R3) joined by Linux bridges in the LAN namespace (`br12`, `br23`, `br13`; the namespace has addresses on br12/br13 to listen and inject), LAN host behind R1, far host (WAN namespace) behind R3, `rip_timers=2,6,4`. Checks:
    - convergence, metrics and TTL;
    - poisoned reverse, by listening on the link (`rip_listen`);
    - a cut cable via the QEMU monitor (`set_link nN off` on both ends);
    - a silent failure (bridge port down), detected by timeout;
    - forged and malformed RIP (`rip_inject`, raw Ethernet; MAC via `SIOCGIFHWADDR`, since `/sys` shows the host's interfaces in a user namespace);
    - withdrawal, possibly after counting to infinity.
  - `tcp`: `tcp_test` (echo :7, 1 MiB chargen :19). Echo and chargen integrity, closed port → RST, 200 sequential and 8 concurrent connections, then 10% loss in **both** directions (`netem` on `tap0` egress; `ifb0` + ingress redirect for router → LAN) for echo, chargen and concurrent transfers. The loss checks are random but have been stable (6–10 s worst case vs 30 s socket timeouts).

  Run a subset with `SCENARIOS="tcp" sh check_net.sh LazyDaysOS.bin`. Afterwards `tcpdump -vv` verifies every IP/ICMP/UDP checksum in `qemu_<scenario>_{lan,wan}.pcap`, **outside** the namespaces (tcpdump's privilege drop fails inside them). Kernel output: `qemu_<scenario>.log`. Prints `[SKIP]` and passes if unprivileged namespaces are unavailable.
- To add a check, use `check_ping "description" "0%|100%" <cmd...>` or `check_output "description" "<regex>" <cmd...>` inside a `scenario_*` function; prefix the command with `wan` to run it on the WAN host. A new scenario is a `scenario_<name>` function plus its name in `SCENARIOS`.
- veth pairs are useless for DHCP tests here: Linux leaves UDP checksums to the (nonexistent) NIC, so raw-socket clients like `udhcpc` drop the packets. Always go through QEMU's taps.
- `demo.sh` (no root): the `home` topology plus four `macvlan` devices (own MAC, DHCP lease with hostname via `udhcpc -x hostname:`, own default route by metric) generating traffic through the NAT to a file server / UDP sink on 192.168.50.1, and a `socat` bridge (UNIX socket, shared across namespaces) exposing the dashboard on `127.0.0.1:$DEMO_PORT` (8080), password `$DEMO_PASSWORD` (`lazydays`). `DEMO_SECONDS=N` stops it after N seconds, otherwise it follows the kernel log until Ctrl+C. Kill a background demo by process group: the shell defers TERM while a child runs.
- `bootit.sh` and `debug.sh` read from **`sysroot/`**, so run `make install` (not just `make`) before booting.
- The `.iso` rule deletes `./LazyDaysOS.bin` and `./LazyDaysOS.iso` after copying them into `sysroot/boot/kernel/`. The next `make` relinks.
- Header dependencies are tracked with `-MMD -MP` (`*.d` files next to the objects).
- `make test` checks the image with `grub-file --is-x86-multiboot` and fails the build if it's invalid.
- The build is warning-free; keep it that way.

### Headless verification (for agents)

Prefer `make check`. To look at the screen or type into a normal boot without a display:

```bash
qemu-system-i386 -kernel sysroot/boot/kernel/LazyDaysOS.bin -display none -no-reboot \
  -monitor tcp:127.0.0.1:45454,server,nowait
```

Then over the monitor socket: `sendkey <key>` to type (and `sendkey ret` to get past the GRUB menu for `-cdrom`), and `xp /2000hx 0xb8000` to dump the screen. Use the low byte of each cell as ASCII. Avoid long `-monitor unix:` paths (108-byte limit). `-d int -D log` traces interrupts.

Add `-serial stdio` (or `-serial file:x.log`) to read the console text directly. A healthy boot prints `[K_INFO] Initializing IDT` … `[K_SUCC] self tests passed ... Done` → `[K_SUCC] getting kernel ready... Done`, then echoes typed lowercase letters, digits, space, and Enter (VGA only; keyboard echo is not mirrored to serial).

### Fuzzing (`fuzz/`)

The network stack (`kernel/net/*.c`, `kernel/random.c`, `kernel/cmdline.c`, `libk/util/strbuf.c`) is compiled unchanged for Linux and driven by six fuzz targets: `net` (raw frames on both ports), `wan` (raw frames, WAN only: the Internet attacker's view), `http` (requests over harness-made TCP connections; `{AUTH}`, `{SESSION}`, `{CSRF}` placeholders reach the logged-in paths), `tcp` (segments with sequence numbers relative to the connection), `dhcp_server`, `dhcp_client`. Input formats are at the top of each `fuzz/fuzz_*.c`.

- `fuzz/common.c`: two fake ports (TX captured in `fuzz_tx`), fake clock, frame builders with checksums, a TCP client. **State reset per input:** the Makefile renames the stack objects' `.data`/`.bss` to `fuzz_state_data`/`fuzz_state_bss` (`objcopy`), which are snapshotted after boot and copied back after every input (with an uninstrumented copy: ASan's global red zones are inside); heap blocks from boot are snapshotted too (`fuzz/stubs.c`). Nothing in the kernel had to change for this; new globals are covered automatically.
- **Leak invariant:** after each input the harness idles 20 simulated minutes; any heap block the stack still holds aborts the run. This found two TCP bugs (a peer that never closes kept FIN_WAIT_2 and 64 KiB forever; a zero window had no persist timer). libFuzzer then found a remote crash: free UDP port-table slots have port 0 and a NULL handler, and a datagram to port 0 matched one (the TCP listener table had the same flaw). **Tables that mark free slots with 0 must never match a lookup for 0.** Their inputs are `fuzz/corpus/tcp/found-*`; keep every finding as `found-*` (`seeds.py` leaves those alone) and add a `check_net.sh` test for it.
- Variants (`V=` in `fuzz/Makefile`): `libfuzzer` (`make -C fuzz`, `make -C fuzz run T=http FUZZ_TIME=600`; needs `clang` + `libclang-rt-dev`, not installed on this machine yet), `replay` (gcc ASan+UBSan, `make -C fuzz check`, part of `make check`), `mutate` (gcc ASan+UBSan random mutator without coverage feedback, `make -C fuzz mutate`), `cov` (`make -C fuzz coverage`: gcov line coverage of the stack by the corpus, each input in a forked child).
- `make -C fuzz seeds` rebuilds the corpus: `net`/`wan` from `qemu_home_{lan,wan}.pcap` (run `make check` first), the rest hand-written in `seeds.py`.
- **Determinism:** the replay runs every input twice and compares a hash of all transmitted frames; a difference means some state escaped the reset. clang's ASan puts each global in its own `.bss.<name>` section unless built with `-fno-sanitize-address-globals-dead-stripping` (the Makefile does, and fails the build if a `.data*`/`.bss*` section survives the rename).
- libFuzzer runs write new inputs to `fuzz/work/<target>` (ignored) and crashes to `fuzz/findings/`; `make -C fuzz minimize T=...` merges the inputs that add coverage into `corpus/`.
- Host-build gotchas: `fuzz/include/kio.h` shadows libk's (whose `tty.h` refuses Linux); the kernel's `-m32` assumptions don't hold (64-bit pointers), so don't cast pointers to `uint32_t` in `kernel/net`.

### Attack-surface measurement (`measure/`)

- `sh measure/attack_surface.sh lazydays|openwrt [image]`: the router between a LAN and a WAN namespace (like `check_net.sh`; dnsmasq gives it 10.0.8.2), then nmap TCP/UDP scans of all ports from both sides, IPv6 on the LAN, service versions, the router's own view and logins with no or empty password, into `measure/results/<router>.txt`. `SKIP_SCANS=1` skips nmap.
  - OpenWrt is the official 19.07.10 x86-64 image in `~/opt/openwrt/`, run with `-snapshot`, driven over its serial console by `measure/console.py`.
  - Its ICMP rate limits and firewall `syn_flood` limit are lifted before scanning, or most probes look "filtered".
  - ssh needs `-F /dev/null` inside the user namespace (the host's ssh config looks foreign there) and `HostKeyAlgorithms=+ssh-rsa` for its dropbear.
- `python3 measure/reachable.py`: per file, the stack lines that run only because of outside input (fuzz corpus under gcov, each target minus an empty input); `wan` + `dhcp_client` count as WAN input.
- Write-up and results: `docs/attack-surface.md`.

### GNS3 lab (`gns3/`)

- **Metro lab** (`gns3/metro.py`, `metro_lab.py`, `net_ui.py`): a provider-style network.
  - Topology: 4 core routers in a full mesh, 8 aggregation routers dual-homed to the core, 16 access routers dual-homed to aggregation, one customer each. 28 routers, 54 links, servers S1/S2 on C1/C3, a NOC host on C2.
  - Configuration: RIP timers 5,30,20, 32 MB per router. `metro.py` is the single definition (addresses, ports, layout) that both the builder and the dashboard import.
  - `python3 gns3/metro_lab.py build|start|delete`: the routers boot `LazyDaysOS-metro.bin`, a copy. The project is created with **`auto_close: false`**: otherwise GNS3 closes a project, stopping every node, as soon as the last GUI client disconnects.
  - `python3 gns3/net_ui.py` → http://localhost:8095. The page updates 10×/s. The page (`gns3/net_ui.html`, re-read on every request):
    - a header of vital signs (service, routing, loops, hijack);
    - a zoomable map: wheel or double-click to zoom, drag to pan, `+`/`−`/`F`, per-metro buttons. Labels appear once readable, and nodes keep a minimum size on screen;
    - a "Break" toolbar: pick a link or router on the map, or click one for the inspector with its own actions;
    - a side column: status, the current experiment as a time track, a two-minute timeline (one row per customer, then route changes and looping packets on the same time axis, experiment markers through all three), results and the log.

    Colour carries meaning only: status colours for states, one blue ramp for link load, violet for the hijack. Inputs:
    - a Python poller inside the NOC container reads every router's `/api/stats` in-band, about 16 KB per read. Each router has its own loop, staggered over the interval (selectable on the page: 0.25–2 s, default 0.5), so a dead router only delays itself. A router counts as down after two failed reads. All-at-once reads overflow the NOC router's transmit ring;
    - each customer runs `ping -D -O -i 0.1` to S1 (loss, hops from the TTL, ICMP "time to live exceeded" = looping).
  - **Experiments:** every action is an experiment that lives until its consequences are complete: applied → first effect (what and when) → settled. Settled means:
    - every running router reports;
    - no route has changed for one RIP update cycle (6 s);
    - no customer has changed state for 5 s;
    - no visible pending change, such as forged routes still present while no hijack runs;
    - the action's minimum window has passed (`MIN_WINDOW`, from the RIP timers). For example a silent failure waits T+2U = 40 s, and stopping a hijack T+U = 35 s, because their effects wait for the RIP timeout however quiet the network looks.

    Settled is about stability, not health. The status headline is "settled and healthy", "settled, but degraded: n/16 served" (with root causes from `health()`: S1 or the NOC cut off, which in this topology means C1, C2 or their single link; stopped access routers; hijacked routers; loops), or "settling". Expectations name what the target carries (`impact_of`): crashing C1 takes S1 and so every customer, crashing C2 blinds the dashboard. X7's misconfiguration and a running hijack are read from GNS3 and from H7's processes, so they survive a dashboard restart, and finished experiments are appended to `gns3/results/metro_experiments.jsonl` and reloaded.
    Log entries carry the experiment id. The status panel names the open conditions and what is in effect (with undo). An action started while not settled asks: queue it (`when: settled`; queued actions run one at a time, in order, each after the previous one settled), run now (marked as overlapping), or cancel. The results table has one row per experiment: first effect, peak customers down, customer-seconds of outage, time to settle.
  - **Killing a `docker exec` client does not kill its process in the container.** `stop_agents()` pkills the poller and the pings inside the containers on start, on rate change and on exit. Busybox `pkill -f` only sees the beginning of a long command line, so it matches `python3 -u -c`; the marker is the agent's first argument. Leftovers from earlier runs once made 23 pollers hammer the routers, whose 16 TCP slots filled up, so they stopped answering HTTP.
  - Derived values: converged = every running router reports and no route changed for 3 s; served = a reply in the last second; outage and recovery times per action.
  - Controls:
    - cable cut = QEMU `set_link gns3-N off` on both ends, through the monitor port found in `ps`;
    - silent failure = a GNS3 link filter `packet_loss: [100]` (a GNS3 *suspend* also takes the QEMU link down, so it isn't silent);
    - router crash = node stop;
    - route hijack: X7 rebooted with `rip` on every port, then H7 announces 10.200.1.0/25 and /128/25 with metric 1 from UDP port 520;
    - reset.
- **Mesh of metros:** `python3 gns3/metro_lab.py build --metros 3` builds "LazyDaysOS metro x3" (84 routers, 165 links, 54 hosts, 219 networks in every routing table; about 3.5 GB of RAM). `METROS=3 python3 gns3/net_ui.py` runs the dashboard for it.
  - Three metros in a triangle: Mm-C3 is linked to the next metro's C4, so the backbone uses the core ports freed by dropping S2. Names carry a metro prefix (`M2-X7`).
  - Metro m owns `10.(16m).0.0/12`, so it can later be summarized as one route.
  - Odd customers ping their own metro's S1, even ones the next metro's.
  - One poller per metro runs in that metro's NOC (default 1 s).
  - Results go to `gns3/results/experiments-<project>.jsonl`, one file per project.
  - Flat RIP is at its limits here: 219 of 256 routes, paths up to 6 routers, and a backbone cut makes all 84 routers recompute.
- `gns3/mesh_lab.py build [RxC] [--ram MB] [--timers U,T,G]` builds the project "LazyDaysOS mesh" (`MESH_PROJECT` for another name): an R×C torus of routers speaking RIP (default 10×10: 100 routers, 200 links), 32 MB each like the TL-WR841N, with netshoot hosts on R-0-0 and the farthest router.
  - Addresses are `10.1.(r*C+c).0/24` east, `10.2.(r*C+c).0/24` south.
  - It boots `~/GNS3/images/QEMU/LazyDaysOS-mesh.bin` (copied from `./LazyDaysOS.bin`), so the throughput lab's kernel is untouched.
  - `check` fetches every router's `/api/stats` from host A through the mesh and pings across; `delete` removes it.
  - Measured: 100 routers built and started in ~40 s, about 3 GB of RAM, all 202 networks everywhere within ~35 s of boot, an 11-router path between the hosts.

The thesis test bench: LazyDaysOS in GNS3 between Docker hosts, measured live. Set up on this machine (see the user's memory for details); the pieces in the repo:

- `gns3/throughput_lab.py` builds the GNS3 project "LazyDaysOS throughput" through the GNS3 2.2 API on `127.0.0.1:3080`: three rows (baseline with no router, `R-routed`, `R-nat`) of `nicolaka/netshoot` containers on **direct** links (GNS3's built-in switch is Dynamips and would be the bottleneck). It needs the `LazyDaysOS` QEMU template, which boots `sysroot/boot/kernel/LazyDaysOS.bin`, so `make install` + restarting the node picks up a new kernel. Router command lines carry `admin_password=lazydays-lab`, which `lab_ui.py` reads back from GNS3 to log in.
- `gns3/tests.sh`: iperf3 presets, installed in each client as `/lab/tests.sh`.
- `LAB_PROJECT="<name>"` makes `throughput_lab.py` and `lab_ui.py` build/drive another copy of the lab (e.g. `"LazyDaysOS throughput (dev)"` with `LAB_UI_PORT=8091`), so development doesn't disturb the copy in use; they share the host CPU, though.
- Benchmark (drawer tab, `POST /api/bench/start`): RFC 2544-style throughput per Ethernet frame size (64-1518 B, UDP payload = frame - 46): one trial at `max_pps` (default 50k), binary search to 2% for the highest rate with loss <= the tolerance (default 0.1%: zero loss is too noisy in emulation), then a confirmation trial at that rate with ping alongside for latency under load, stepping down 5% (up to 3 times) if the confirmation fails. Records the router's CPU per trial (`poller.cpu_between`). Results go to `gns3/results/<time>-<path>-<commit>.json` with the git commit, `-O` level, kernel file hash and a warning if the router booted before that kernel was installed. Run benchmarks with nothing else busy on the host; the first runs showed loss starting at ~55-65% CPU, i.e. below CPU saturation (the 32-slot receive ring is the suspect).
- `gns3/lab_ui.py` (stdlib only) serves `gns3/lab_ui.html` on `http://localhost:8090`: live topology from GNS3, iperf3 from both ends at 0.1-1 s, the routers' `/api/stats` once a second (curl -u from the inside client, so routers need `http` and `admin_password=`), link conditions (GNS3 link filters), node start/stop, in-page consoles and link captures. The page is read from disk on every request, the Python only on restart. Only one `lab_ui.py` per lab: its startup kills leftover iperf3 processes in all lab containers.
- Gotchas: the GNS3 client and server versions must match exactly (the Windows client here is 2.2.61; gns3.com still ships 2.2, 3.0 is on GitHub). GNS3 3.0 rejects the kernel as an image because it classifies 32-bit ELF files as Cisco IOU images. Docker nodes need a *statically linked* BusyBox on the server's PATH (or in `~/.local/share/GNS3/docker/bin/`). GNS3 gives container interfaces MTU 65521, which kills ubridge links ("send: Message too long"); the builder sets 1500. GNS3 puts QEMU NICs behind a PCI bridge (the 256-bus scan finds them). VPCS sets `ciaddr` in DHCPREQUEST while sending from 0.0.0.0, which is why the DHCP server only unicasts when `ciaddr` equals the source.
- Measured baseline (KVM, `-O0` build): GNS3 links carry ~3.6 Gbit/s; a LazyDaysOS router tops out at ~12-14k packets/s regardless of packet size (per-packet cost), NAT costs ~3%, and TCP's ACKs count against the same budget.

## Build layout (Makefile)

- `CC=i686-elf-gcc -m32`, `CFLAGS=-nostdlib -ffreestanding -D __LF -Ilibk/include -Ikernel/compat/i386/include -g -O2`. Link: `-T kernel/compat/i386/linker.ld ... -lgcc`.
- Three object groups: `KERN_OBJ` (kernel + compat), `LIBK_OBJS` → `libk.a` (tty, kio, ttyin, serial), `UTIL_OBJS` → `util.a` (memory).
- **Adding a source file:** append its `.o` to the right group and its directory to `VPATH`. Compat files are referenced with a path prefix (`$(COMPAT_DIR)/foo.o`), so their objects stay next to the source.
- Generic network code lives in `kernel/net/` (`NET_DIR`); like compat files, its objects are listed with a path prefix.
- `linker.ld`: load at 1 MiB; `.text` (with `.multiboot` first), `.rodata`, `.data`, `.bss`, all 4K-aligned, wildcards for `.rodata.*` etc. Entry `_start`. `_kernel_start`/`_kernel_end` bound the image; the frame allocator reserves that range.

## Code map and boot flow

```
kernel/boot.s              Multiboot header, 16 KiB stack in .bss, _start
kernel/hwio.s              outb/inb, idt_buf_drain (lidt), i686_GDT_Load
kernel/gdt.c/.h            GDT: null, ring0 code 0x08, ring0 data 0x10, two ring3 entries
kernel/idt.c/.h            PIC remap (IRQ0-15 -> vectors 0x20-0x2F), IDT gates 0-47, init_idt()
kernel/idt_hndlr_setup.s   ISR0-31 / IRQ0-15 stubs -> general_interrupt_handler -> interrupt_handler(struct int_frame*)
kernel/isr.c/.h            EOI, dispatch through isr_map[], panic on unhandled CPU exceptions, isr_self_test
kernel/panic.c/.h          k_panic / k_panic_frame: red register dump (+CR2 on #PF), then halt
kernel/heap.c/.h           kmalloc/kfree: first-fit heap over 16 MiB of contiguous frames
kernel/cpuload.h           generic CPU-load API: cpu_idle_wait, cpu_irq_disable/enable, busy share per second
kernel/net/netdev.c/.h     driver-independent NIC interface: register, transmit, poll, rx handler, stats
kernel/net/net.c/.h        stack entry (net_initialize, net_run_once), byte order, checksum, IP parsing/printing
kernel/net/eth.c/.h        Ethernet rx dispatch by EtherType, eth_send
kernel/net/arp.c/.h        ARP cache (64 entries, at most 16 unresolved), replies, resolution with one held packet per destination
kernel/net/ipv4.c/.h       interface addresses, longest-prefix routing table, header validation, ipv4_send
kernel/net/icmp.c/.h       echo reply, icmp_ping, icmp_send_error (time exceeded, net/host/proto/port unreachable)
kernel/net/udp.c/.h        UDP ports (udp_bind), pseudo-header checksum, udp_send / udp_send_dev, echo service
kernel/net/dhcp.c/.h       DHCP message layout and option helpers, client/server API
kernel/net/dhcp_client.c   RFC 2131 client state machine (one per port with ethN=dhcp)
kernel/net/dhcp_server.c   pools, lease table, OFFER/ACK/NAK, RELEASE/DECLINE
kernel/net/nat.c/.h        NAPT + connection tracking (512 entries, two hash tables), firewall, ICMP error translation
kernel/net/tcp.c/.h        TCP server endpoint: state machine, retransmission, out-of-order queue, SYN cookies, test services
kernel/net/http.c/.h       HTTP/1.1 server (one request per connection, routes, limits, LAN-only)
kernel/net/api.c/.h        /, /api/stats, /api/events, /api/block, /api/unblock
kernel/net/auth.c/.h       admin password, sessions + CSRF tokens, HTTP Basic for scripts, per-address lockout
kernel/net/rip.c/.h        RIPv2: split horizon + poisoned reverse, triggered updates, link-down handling, timers
www/index.html             the dashboard (one self-contained file); gzipped and linked in as www.o
kernel/net/clients.c/.h    inside hosts: per-device byte counters, block switch (enforced in ipv4$$forward)
kernel/net/events.c/.h     64-entry event ring for the dashboard (also echoed to the console as [K_EVNT])
kernel/system.h            system_ready_ms (set in kernel_main)
libk/util/strbuf.c         bounded string/JSON builder (sb_*), used for JSON and event texts
kernel/cmdline.c/.h        kernel command line copy, cmdline_has / cmdline_get
kernel/random.c/.h         ChaCha20 CSPRNG (fast key erasure), entropy pool fed from every interrupt, reseeded once a second
kernel/kernel.c            kernel_main(magic, mbi)
kernel/compat/i386/        arch-specific:
  kern_powerup.c             powerup_kernel_i386 (bring-up order), kern$self_test, test mode + QEMU exit
  pmm.c                      physical frame bitmap allocator from the Multiboot memory map
  vm_watchdog.c              paging setup
  pit.c                      PIT channel 0 at 100 Hz, tick counter, pit_sleep_ms
  cpuload.c                  cpuload.h for x86: TSC around sti; hlt, one-second windows, self test
  entropy.c                  random.h seed hooks for x86: RDRAND if present, TSC jitter around PIT port I/O
  pci.c                      PCI config mechanism #1, bus scan, BAR decode, pci_enable_device
  e1000.c                    Intel 82540EM/82545EM driver: 256-entry RX / 32-entry TX DMA rings, shared IRQ handler, MPC -> rx_missed
  include/multiboot.h        Multiboot v1 info / mmap structures
libk/tty/tty.c             VGA text console 80x25 at 0xB8000 (terminal_*)
libk/tty/serial.c          polled COM1 output, 115200 8N1
libk/kio/kio.c             k_print -> VGA + COM1; supports %% %c %u %d %x %s (no width/padding)
libk/tty/ttyin.c           scancode set 1 -> ASCII, keyboard_read() (gated by input_allowed)
libk/util/memory.c         lazy$block helpers + util$self_test()
```

Boot sequence:

1. GRUB enters `_start` in 32-bit protected mode with EAX = Multiboot magic, EBX = info pointer. `_start` sets up its own stack, pushes both, calls `i686_GDT_Initialize()` (loads the GDT, reloads segments, far-returns to reload CS) and then `kernel_main(magic, mbi)`.
2. `kernel_main`: `terminal_initialize()`, `serial_initialize()`, check the magic, `init_idt()` (PIC remap, gates, `lidt`, `register_routines()`).
3. `powerup_kernel_i386(mbi)`, still with interrupts off: parse the command line for `test`, `mem$pmm_init` (must run **before** paging; reads Multiboot data physically), `mem$setup_paging`, `mem$heap_init`, `pit_initialize`, `pci_initialize`, `e1000_initialize` (one `netdev` per port, named `eth0`, `eth1`, …), `net_initialize` (applies `ethN=`/`gw=`).
4. `sti`, then `kern$self_test()` (util, PMM, heap, ISR register preservation, timer; in test mode also `netdev_self_test`, which ARP-probes QEMU's gateway `10.0.2.2` on every port). In test mode, exit QEMU with the result.
5. Set `input_allowed = true` and loop on `net_run_once()`: every 100 ms run protocol timers (`arp_tick`, `dhcp_client_tick`, `dhcp_server_tick`, `nat_tick`, `tcp_tick`, `http_tick`, `cpu_load_tick`), then `netdev_run_once()` polls NICs with pending frames or sleeps in `cpu_idle_wait()` (`sti; hlt` on x86) until the next interrupt. That sleep is the kernel's only idle point: CPU load is the share of each second not spent there.

Network receive path: the e1000 IRQ handler only reads ICR (which acknowledges the interrupt) and calls `netdev_mark_pending()`. Frames are processed later in `netdev_run_once()` → driver `poll()` → `netdev_receive()` → the single registered `netdev_rx_handler`, i.e. **not in interrupt context**. The registered handler is `eth_receive` → `arp_receive` / `ipv4_receive` → `icmp_receive` / `udp_receive` (→ the handler bound to the port, else ICMP port unreachable). Packets not addressed to us go to `ipv4$$forward`: drop martians (0/8, 127/8, ≥224) and directed broadcasts, TTL ≤ 1 → ICMP time exceeded, no route → net unreachable, else copy, decrement TTL, recompute the header checksum, `arp_output` to the next hop. If ARP gives up on a next hop, the held packet's sender gets ICMP host unreachable.

NAT hooks (`kernel/net/nat.c`): a packet arriving on an outside port and addressed to that port's own address is first offered to `nat_inbound` (on a copy); if it matches a tracked connection it is rewritten toward the inside host and forwarded with `translated = true`, otherwise it is delivered locally (DHCP, pings to the router). Outside ports follow the strong host model: a packet arriving there for one of the router's *other* addresses (the LAN address) is dropped (`ipv4_stats.rx_wrong_port`), so a WAN neighbour can't reach inside-only services by addressing them directly. In `ipv4$$forward`, anything from an outside port that isn't `translated` is dropped (`nat_stats.fw_dropped`), and anything leaving through an outside port goes through `nat_outbound` before the TTL decrement. Mappings are endpoint-independent with port preservation (RFC 4787); inbound filtering is address-and-port-dependent. Checksums are patched incrementally (`net_checksum_replace16/32`, RFC 1624) because the TCP payload isn't re-summed. Changing an outside port's address (`ipv4_clear_address`) flushes its mappings. Sending goes `icmp`/caller → `ipv4_send` (route lookup, header, checksum) → `arp_output` (resolve or hold) → `eth_send` → `netdev_transmit`. Every layer copies into its own stack buffer (up to ~4.5 KiB of stack per send). Transmit copies the frame into a per-descriptor buffer, so callers may reuse their buffer immediately. Transmits are **batched**: the e1000 rings its doorbell (TDT) once per batch, not per frame, and returns received descriptors with one RDT write per poll round, because every device-register write is a VM exit under KVM. `netdev_run_once()` calls `netdev_flush_all()` first, so frames queued by the previous poll round or by protocol timers go out before the loop polls again or sleeps; a driver flushes on its own at 16 queued frames. Code that transmits and then waits must keep running `net_run_once()` (all existing waits do).

Memory layout (identity mapped, virtual = physical): page 0 is **not present** (NULL dereferences panic with `#PF`, `cr2=0x0`); 4 KiB–4 MiB uses 4 KiB pages; 4 MiB–4 GiB uses 4 MiB PSE pages, uncached above the end of RAM. Frames below 1 MiB, the kernel image and the Multiboot structures are reserved. With QEMU's default 128 MiB, the heap lands right after the kernel image.

### Things to know before editing low-level code

- Unhandled CPU exceptions (vectors 0–31 with no handler) panic; returning would re-run the faulting instruction. Register handlers with `isr_register(vector, fn)`; up to `ISR_MAX_SHARED` per vector, all called on every interrupt (PCI NICs share IRQ lines; QEMU puts them on IRQ 11). Handlers are `void (*)(void)` and run **before** the EOI is sent, because PCI interrupts are level-triggered.
- Stack conventions (`kernel/net/net.h`): IPv4 addresses are `uint32_t` in **host** byte order everywhere; header structs are packed with fields in **network** order; convert with `net_htons`/`net_htonl` only at the header boundary. `net_checksum()` returns a value ready to store in a header, and checking a received header yields 0. Nothing in `kernel/net/` may include x86 or e1000 headers or use inline assembly: time comes from `pit.h`, interrupts and idle from `kernel/cpuload.h`.
- `icmp_send_error` enforces RFC 1122's "never send an error about…" rules (ICMP errors, non-first fragments, broadcast/multicast destinations, bad or own sources). Use it for every error so those rules stay in one place.
- Checksums over several pieces (UDP/TCP pseudo-header + segment): `net_checksum_add` each piece, then `net_checksum_finish`; only the last piece may be odd-length.
- DHCP: the client asks for broadcast replies (the stack would otherwise treat a unicast to the not-yet-applied address as someone else's packet). The server broadcasts all replies except unicast ACKs to clients that already have `ciaddr`. `ipv4_send_dev`/`udp_send_dev` send out a given port with an exact source (0.0.0.0) and no route lookup; use them only for that kind of link-local traffic. Random numbers (DHCP xids, NAT ports, secrets) come from `random32()`/`random_bytes()` in `kernel/random.h`, a ChaCha20 CSPRNG; there is no weaker generator to pick by mistake.
- TCP (`kernel/net/tcp.h` has the full list): passive open only; up to 16 connections with a 64 KiB send ring each (kmalloc'd when the handshake completes, so half-open connections cost only their slot). Every ISN is a SYN cookie (keyed ChaCha20 hashes + a 64 s counter, RFC 6528-style): with the table full, SYNs are answered statelessly and the completing ACK recreates the connection, evicting the oldest half-open one if needed. A listener's `admit(dev, remote_ip)` callback runs before anything is allocated or sent; a refused SYN is dropped silently. **`tcp_send`/`tcp_close` only queue**: the stack transmits after the callback returns, so segments fill to MSS; call `tcp_output()` when sending from outside a callback. The advertised window is min(16 KiB, free send space), so a request/response app can always queue its reply, and it stays constant so the peer recognises duplicate ACKs. Out-of-order segments get an immediate pure duplicate ACK and up to 8 are queued. Fast retransmit + NewReno partial-ACK recovery, RTO per RFC 6298 with a 200 ms floor, backoff dropped on new ACKs. No congestion control, no SACK/window scaling/timestamps (Linux falls back), TIME_WAIT is 2 s and the oldest TIME_WAIT slot is reused when full; FIN_WAIT_2 frees the send buffer, lasts at most 60 s and is reused like TIME_WAIT; a zero window starts the persist timer (probes, dropped after 8 unanswered), and with the table full a closed-but-stalled connection is reset to make room. Measured in QEMU: ~120–150 Mbit/s echo without loss; with 10% loss both ways 1 MiB chargen ~2–3 s, 256 KiB echo 5–10 s (the peer's own timeouts dominate). Each of these mechanisms was added after the loss tests exposed its absence.
- Dashboard (`www/index.html`): plain HTML/CSS/JS, no external resources (the router may be offline), light/dark, phone layout. The Makefile gzips it and links it with `i686-elf-ld -r -b binary` (symbols `_binary_dashboard_html_gz_start/_end`); `/` serves it with `Content-Encoding: gzip`. It starts with `GET /api/session` (sign-in screen on 401), sends the CSRF token on every POST, and polls `/api/stats` and `/api/events?since=` once a second and computes all rates in the browser from counter deltas. **Update the DOM in place** (`portEls`/`deviceEls` keyed by name/IP), never rebuild lists: a click spanning a refresh is otherwise lost; keep the device order stable (by IP) for the same reason. Check visual changes with `demo.sh` in a real browser.
- Login (`kernel/net/auth.h`): everything but `/` and `/api/login` needs either a session (browser: `POST /api/login` sets an HttpOnly, SameSite=Strict cookie and returns a CSRF token that every POST must send as `X-CSRF-Token`) or HTTP Basic `admin:<password>` (scripts, tests: `curl -u`). 401s never offer Basic, so browsers don't cache it. 5 wrong passwords lock that source address out (429 + Retry-After) for 60 s, doubling per lockout up to 16 min. `http.c` checks every request's `Host` (must be a router address or localhost, else 421: DNS rebinding) and refuses POSTs with a foreign `Origin` (403), then runs the auth guard for non-`HTTP_PUBLIC` routes. Tests log in with `ADMIN_PW` / `$API_AUTH` (`check_net.sh`).
- Management API (`kernel/net/api.h` lists the routes): JSON is built with `strbuf` into a 16 KiB kmalloc'd buffer handed to the response as `owned` (freed after sending); on overflow the handler answers 500. `/api/stats` field names are what the dashboard (and `gns3/lab_ui.py`) reads; change them only together. `cpu.busy_permille` is the busy share of the last whole second (0 until the first second ends), `cpu.idle_ms` the idle total; each interface's `rx_missed` is the NIC's own count of frames it had no buffer for (e1000 MPC). QEMU's emulated e1000 never counts these: when its ring is full QEMU stops reading the backend, so the excess is lost on the host side and `rx_missed` stays 0 there; on real hardware it is the receive-ring overflow count. The HTTP listener's `admit` drops SYNs arriving on a NAT outside port, so the WAN never reaches the parser (the 403 for outside addresses in `http.c` is a second line of defence). A blocked client loses forwarding only; it can still reach the router (dashboard, DHCP). `system_ready_ms` excludes BIOS/GRUB time.
- The receive path copies packets on the stack at several layers (NAT inbound copy, forward copy, ICMP error, eth frame): worst case ~8 KiB of the 16 KiB kernel stack. Don't add more large stack buffers on that path.
- RIP (`kernel/net/rip.h` has the details): learned routes go into the routing table with origin RIP; connected beats static beats RIP for the same prefix (`ipv4_add_route_from`). The table holds 256 routes, and `/api/stats` (64 KiB buffer) lists each route's `origin` and `metric` plus a `rip` object. RIP is never spoken on NAT outside ports and only accepted from on-link neighbours with source port 520. There is **no authentication**: anyone on a RIP link can inject routes, so enable it only on router-to-router links. Updates are broadcast (no multicast support). A cut link (`link_up` false, e.g. QEMU `set_link`) withdraws its routes at once; a silent failure takes the timeout. In loops of 3+ routers, RIP can count to infinity after a withdrawal (seen in the triangle test).
- Not implemented yet: IP fragment reassembly (fragments are counted and dropped) and fragmentation on send, ICMP redirects, ICMP rate limiting, DHCP relay/INFORM/option overloading and address conflict probing, NAT port forwarding, hairpinning, ALGs and protocols other than TCP/UDP/ICMP echo, TCP active open and congestion control. Every interface uses MTU 1500.
- DMA uses kernel heap addresses directly: memory is identity mapped, so virtual = physical. This breaks if the heap ever moves to a non-identity mapping.
- **QEMU's e1000 model holds received frames for 1 s after every write to RCTL** (verified: the first ARP reply consistently arrives ~920 ms after bring-up, and ~410 ms if the probe starts 500 ms later). Don't mistake that for a driver bug; budget for it in timeouts (self-test pings use 3 s; `check_net.sh` sleeps 1.2 s after boot), and expect the same pause if RCTL is rewritten later (e.g. toggling promiscuous mode). `-trace 'e1000x*'` shows the emulated NIC's receive decisions.
- Stubs push a dummy error code for exceptions without one, so `struct int_frame` is uniform. Error-code vectors: 8, 10–14, 17, 21, 29, 30.
- `pit_sleep_ms` needs interrupts enabled (it panics otherwise). Self tests that need the timer must run after `sti`.
- `k_print` writes to serial only after `serial_initialize()`; anything printed earlier is VGA-only.
- `vgadef.h` defines `static` terminal state (`terminal_row`, `terminal_buffer`, …) in a header, so every TU gets its own copy. Only touch them from `tty.c`.
- The README claims an MIT license, but there is no `LICENSE` file.

## Conventions

- **`facility$name` naming** for functions, types, struct fields and status codes (`mem$setup_paging`, `util$blkcpy_weak`, `struct lazy$block`, `lb$base_addr`, `UTIL$SUCCESS`, `KERN$PWRUP`, `LAZY$BLOCKINV`). `facility$$name` marks internal/static helpers and tests (`util$$blkcpy_weak_test`). GCC accepts `$` in identifiers; keep using it.
- Status codes: success is odd (`UTIL$SUCCESS 0x1`, `UTIL$FAIL 0x0`). Return `size_t`/`uint16_t` statuses rather than `bool`.
- Kernel log lines are prefixed `[K_INFO]`, `[K_SUCC]`, `[K_FAIL]` (`[UTIL_FAIL]` inside util) and printed with `k_print`.
- Header guards are `#ifndef __NAME__` style (some files also use `#pragma once`). Doxygen-style `@brief` comments appear on non-trivial functions.
- Indentation is mixed (tabs in `libk/tty`, `kio`, `kernel.c`; 4 spaces in `memory.c`, `vm_watchdog.c`; 2 spaces in `idt.c` gate setup). Match the file you're editing.
- Arch-specific code goes under `kernel/compat/i386/` with headers in its `include/`. Generic kernel headers sit next to their `.c` and are included with quotes.
- Network behaviour is tested from outside with real Linux tools in `check_net.sh`; extend it for every new protocol feature, including a negative case, and check that it fails when the feature is broken.
- There is no host-side test suite. Tests are boot-time self-tests run by `kern$self_test()` (`kernel/compat/i386/kern_powerup.c`) and checked by `make check`. Each module exports a `*_self_test()` returning `UTIL$SUCCESS`/`UTIL$FAIL` and prints a `[UTIL_FAIL]` line explaining any failure; add new ones to `kern$self_test()`. When fixing a bug, add a test that fails on the old code.
