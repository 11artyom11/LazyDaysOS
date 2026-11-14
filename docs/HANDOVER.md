# LazyDaysOS handover

Written 2026-11-14 at the end of a long working session, for whoever (person or AI assistant) continues the project. It describes:

- what exists;
- how to set up a machine to build, test and run all of it;
- every third-party piece it depends on;
- the traps that already cost time;
- what is left to do.

**Read these too:**
- [`CLAUDE.md`](../CLAUDE.md): the authoritative technical reference. It covers the code map, boot flow, kernel command line, test scenarios, fuzzing, the GNS3 labs and all low-level gotchas, and it is kept current with every change.
- [`docs/thesis-plan.md`](thesis-plan.md): the thesis framing, threat model, audit findings, work packages and results so far.
- [`docs/attack-surface.md`](attack-surface.md): the WP0 write-up comparing the attack surface with OpenWrt.

This file doesn't repeat those; it fills in everything around them.

---

## 1. What the project is

LazyDaysOS (a.k.a. DudeOS) is a 32-bit x86 router kernel in C and GNU `as`, booted by GRUB (Multiboot v1). It has no userspace and no scheduler. It provides:

- Ethernet, ARP and IPv4 forwarding with a routing table;
- ICMP and UDP;
- a DHCP client and server;
- NAT with connection tracking and a firewall;
- a TCP server with SYN cookies;
- an HTTP management API and dashboard with login, CSRF and DNS-rebinding protection;
- RIPv2;
- a ChaCha20 CSPRNG.

All of this runs on Intel e1000 NICs in QEMU, behind a driver-independent `netdev` layer.

**Thesis (degree):** *"A minimal, hardened router OS for abandoned 4/32 hardware"*. The final target is the TP-Link TL-WR841N v14 (MediaTek MT7628, MIPS 24KEc, 4 MB flash, 32 MB RAM), which OpenWrt no longer supports. Development happens on x86 in QEMU and GNS3. Everything above `netdev` must stay free of x86 and e1000 specifics so it ports unchanged.

### Where things stand (all merged into `master`)

| Area | State | Evidence |
|---|---|---|
| Core stack (ARP/IPv4/ICMP/UDP/DHCP/NAT/TCP/HTTP) | done | `make check`: self-tests on TCG and KVM, ~140 network checks against real Linux tools |
| Audit fixes F1, F2, F2b, F2c (CSPRNG, SYN cookies, WAN can't reach management, ARP exhaustion) | done | thesis-plan.md §3 status tables |
| F3, management login (sessions, CSRF, Host/Origin checks, lockout) | done | same |
| WP2, fuzzing (6+1 targets, libFuzzer, ~29M inputs; found 4 real bugs incl. a remote crash) | done | `fuzz/`, thesis-plan.md "Status of WP2" |
| WP0, attack surface vs OpenWrt 19.07.10 | done | `docs/attack-surface.md`, `measure/results/` |
| Throughput benchmark (RFC 2544-style) | done; numbers in `gns3/results/*.json` | `gns3/lab_ui.py` |
| Performance work: -O2, 256-entry RX ring (heap 16 MiB), batched e1000 doorbells | merged from branch `perf` | `gns3/results/20261110-*.json` (before/after per commit) |
| RIPv2 + metro lab (28 routers) + mesh of 3 metros (84 routers) + live experiment dashboard | done | thesis-plan.md "metro lab" and "mesh of metros" sections, `gns3/results/experiments-*.jsonl` |
| F4 / WP3 in-kernel hardening (W^X, guard page, stack protector) | **not started** | thesis-plan.md WP3 |
| WP5, MIPS port to the real router | **not started**: the biggest remaining risk | thesis-plan.md WP5 |

### Git state at handover

- **`master` contains everything.** Every feature branch has been merged:
  - `security-f1-f2`, `mgmt-auth`, `fuzzing`, `attack-surface`;
  - `gns3-fixes`, `benchmark`, `router-load`, `perf`, `rip`.
  
  The branches still exist locally and can be deleted with `git branch -d <name>`.
- **Nothing has been pushed.** `origin/master` (github.com/11artyom11/LazyDaysOS) is far behind. The owner pushes themselves.
  - `gh` is **not installed** in WSL, and git has no credentials there.
  - Either `sudo apt install gh && gh auth login`, or use a personal access token with git's credential helper.
- Old remote branches (`FSsetup`, `MMOS`, `main`, `memtest`) predate this work and are unrelated.
- Build outputs (`*.o`, `*.d`, `*.a`, `*.bin`, `sysroot/`, `qemu_*.log`, `qemu_*.pcap`) are ignored; they land in the repo root.

---

## 2. The machine

- **Host:** Windows, running the GNS3 GUI client and Microsoft Edge (used headless for screenshots).
- **Dev environment:** WSL2, **Ubuntu 26.04.1 LTS**, Linux 6.18 (microsoft-standard). systemd is enabled, WSLg is available, `/dev/kvm` is present. 16 cores, ~15 GB RAM visible to WSL. Networking mode NAT: Windows reaches WSL services on `localhost` through wslrelay.
- **User:** `tyom`, in groups `sudo docker libvirt kvm`. **No passwordless sudo**: anything that needs root (apt, modprobe, setcap, virsh network changes) must be handed to the owner as a command to run.
- **Repo:** `~/LazyDaysOS`.

---

## 3. Setting up from scratch

Every step below was done on the current machine. Versions are the ones in use; newer ones are probably fine unless stated otherwise.

### 3.1 apt packages

```bash
sudo apt install build-essential bison flex bzip2 make \
  grub-pc-bin grub-common xorriso mtools \
  qemu-system-x86 gdb gnome-terminal \
  tcpdump dnsmasq-base busybox socat iputils-ping iproute2 curl kmod \
  clang libclang-rt-dev \
  nmap \
  docker.io python3-venv libpcap-dev \
  libvirt-daemon-system vpcs dynamips
sudo usermod -aG docker,libvirt,kvm $USER     # then log out of WSL and back in (wsl --shutdown)
```

What each group is for:

- **Building the kernel and ISO:** `build-essential bison flex bzip2 make grub-pc-bin grub-common xorriso mtools`.
  - **`grub-pc-bin` is required.** With EFI-only GRUB, `grub-mkrescue` produces an ISO this kernel can't boot.
- **Running and debugging:** `qemu-system-x86` (QEMU 10.2.1) and `gdb`. `gnome-terminal` is needed only by `debug.sh`.
- **`check_net.sh`, `demo.sh` and `measure/`:**
  - `tcpdump dnsmasq-base` (the WAN-side DHCP server);
  - `busybox` (`udhcpc` as a real DHCP client);
  - `socat` (the demo's dashboard bridge);
  - `iproute2` (`ip`, and `tc` with netem);
  - `curl`, `iputils-ping`.
  
  Everything else these scripts need is in util-linux (`unshare`, `nsenter`) or is Python 3 stdlib.
- **Fuzzing:**
  - The `libfuzzer` variant needs `clang` (21.1) and `libclang-rt-dev`.
  - The `replay`, `mutate` and `cov` variants only need gcc with ASan/UBSan, which comes with build-essential.
- **WP0, `measure/attack_surface.sh`:** `nmap` (7.98).
- **The GNS3 server and labs:** `docker.io` (29.1), `libvirt-daemon-system` (virbr0 backs GNS3's NAT node), `vpcs` (0.8.3), `dynamips` (0.2.14), and `libpcap-dev` for building ubridge.
- `iperf3` is **not** needed on the host; it runs inside the netshoot containers.

### 3.2 Kernel modules (WSL builds these as modules)

`check_net.sh`'s loss tests need `ifb` and `sch_netem`; `demo.sh` needs `macvlan`.

```bash
printf 'ifb\nsch_netem\nmacvlan\n' | sudo tee /etc/modules-load.d/lazydaysos.conf
sudo modprobe -a ifb sch_netem macvlan
```

If the `tcp` scenario's loss tests or `demo.sh` suddenly fail after a WSL update, check `lsmod` for these modules first.

### 3.3 Cross compiler: `i686-elf`, binutils 2.47 + GCC 15.3.0

The host gcc can't build the kernel: `libk/include/tty.h` refuses Linux-targeting compilers. The full recipe is in [`README.md`](../README.md#building-the-cross-compiler), and the script used here is `~/opt/cross-build/build.sh` (~4.5 min on 16 cores). It installs into `~/opt/cross`, then:

```bash
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
```

Non-interactive shells (including an AI assistant's tool shell) may not read `.bashrc`, so prefix commands with `export PATH="$HOME/opt/cross/bin:$PATH";`.

`.vscode/c_cpp_properties.json` hard-codes `15.3.0` in an include path; update it if you rebuild with a different GCC.

### 3.4 GNS3: server 2.2.61 in WSL, GUI client on Windows

- **The client and server versions must match exactly.** The Windows client is **2.2.61**; gns3.com distributes 2.2, while 3.0 is only on GitHub. Don't use 3.0: it rejects 32-bit ELF kernels as "IOU images".
- **Server install** (a venv; works with Python 3.14):

  ```bash
  python3 -m venv ~/opt/gns3-2.2
  ~/opt/gns3-2.2/bin/pip install gns3-server==2.2.61
  ```

- **Start it** (it isn't a service). Start it from `~`: Dynamips drops log files into its working directory.

  ```bash
  cd ~ && ~/opt/gns3-2.2/bin/gns3server --host 127.0.0.1 --port 3080 --log ~/GNS3-server.log
  ```

  The API is v2 on `http://127.0.0.1:3080/v2/`, with no authentication. Windows reaches it, and the telnet consoles on ports 5000+, through WSL's localhost forwarding.
- **Client config:** `%APPDATA%\GNS3\2.2\gns3_server.ini`. Main server `localhost:3080`, `auto_start = False`.
- **ubridge 1.3.2**, built from source (`~/opt/ubridge`):

  ```bash
  cd ~/opt/ubridge && make && sudo make install
  sudo setcap cap_net_admin,cap_net_raw,cap_bpf+ep /usr/local/bin/ubridge
  ```

- **Static BusyBox for Docker nodes.** GNS3 injects a busybox into every Docker node, and it must be statically linked. Ubuntu's `busybox-static` .deb was unpacked without root:

  ```bash
  apt download busybox-static && dpkg -x busybox-static_*.deb ~/opt/busybox-static/pkg
  ```

  Put it on the server's PATH as `busybox` (or in `~/.local/share/GNS3/docker/bin/busybox`, which is where GNS3 copied it).
- **Docker image** used for every lab host:

  ```bash
  docker pull nicolaka/netshoot:latest
  ```

  netshoot is Alpine-based: busybox `grep` has no `-z`, and `pkill -f` only sees the start of a long command line.
- **Templates** (create them in the GUI or through the API):
  - **`LazyDaysOS`** (QEMU):
    - platform i386, `qemu_path` `/bin/qemu-system-amd64`, 128 MB RAM;
    - 2 × `e1000` adapters, port names `eth{0}`, telnet console;
    - `kernel_image` `LazyDaysOS.bin`, which is a **symlink**: `~/GNS3/images/QEMU/LazyDaysOS.bin -> ~/LazyDaysOS/sysroot/boot/kernel/LazyDaysOS.bin`. So `make install` plus a node restart picks up a new kernel;
    - default command line `eth0=192.168.1.1/24 dhcpd=eth0,192.168.1.100,192.168.1.150 eth1=dhcp nat=eth1 http`. The lab builders override it per node.
  - **`netshoot`** (Docker): `nicolaka/netshoot:latest`, 1 adapter, telnet console.
- **Images the lab builders copy** into `~/GNS3/images/QEMU/`, so that rebuilding the kernel doesn't change a running lab:
  - `LazyDaysOS-metro.bin` (metro labs);
  - `LazyDaysOS-mesh.bin` (torus);
  - `LazyDaysOS-<commit>.bin` (benchmark kernels).

### 3.5 OpenWrt image (WP0 comparison only)

The official **19.07.10 x86-64** `openwrt-19.07.10-x86-64-combined-ext4.img(.gz)` and its `sha256sums` are in `~/opt/openwrt/`, downloaded from downloads.openwrt.org/releases/19.07.10/targets/x86/64/. `measure/attack_surface.sh openwrt` runs it with `-snapshot`, so the image is never modified.

### 3.6 Windows side

- **GNS3 GUI 2.2.61** (optional: every lab can be driven from the scripts and dashboards).
- **Microsoft Edge** at the default path. [`tools/labshot.ps1`](../tools/labshot.ps1) uses it for headless screenshots through the DevTools protocol, which is how dashboard changes were checked visually. Copy the script to `%TEMP%` and call it from WSL:

  ```bash
  WTW=$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r'); WT=$(wslpath "$WTW")
  cp ~/LazyDaysOS/tools/labshot.ps1 "$WT/"
  cd /mnt/c && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WTW\\labshot.ps1" \
      -Url "http://localhost:8095/" -Out "$WTW\\shot.png" -Wait 10 -Size "1700,1050" \
      -Js "metroFocus(1); 1" -After 2
  cp "$WT/shot.png" /some/where/
  ```

  How it works:
  - `-Js` runs in the page before the screenshot.
  - The value of `window.__errors` is printed as "page errors", so you can return data by assigning it there.
  - Plain `--screenshot` doesn't work: it fires before server-sent-event data arrives, and `--virtual-time-budget` hangs on the open event stream.

### 3.7 Verify the setup

```bash
cd ~/LazyDaysOS && export PATH="$HOME/opt/cross/bin:$PATH"
make && make check          # self tests (TCG, then KVM), check_net.sh (4 scenarios), fuzz corpus replay; exit 0 = all passed
make install                # sysroot/ + ISO; needed before bootit.sh / GNS3
```

`make check` takes a few minutes.
- It prints `[SKIP]` and passes if unprivileged user namespaces are unavailable.
- Run a single scenario with `SCENARIOS="tcp" sh check_net.sh LazyDaysOS.bin`.

---

## 4. Everyday workflows

| Task | Command | Notes |
|---|---|---|
| Build / test | `make`, `make check`, `make install` | Run `make check` after **every** kernel change; the build must stay warning-free |
| Boot interactively | `./bootit.sh` (`--iso`, `-d` for gdb) | reads `sysroot/`, so `make install` first |
| Home-router demo | `./demo.sh` | dashboard on http://localhost:8080, password `lazydays` |
| Fuzz | `make -C fuzz check` (replay), `make -C fuzz run T=http FUZZ_TIME=600` (libFuzzer), `make -C fuzz coverage`, `make -C fuzz seeds` | keep every crash as `fuzz/corpus/<t>/found-*` and add a `check_net.sh` test for it |
| Attack surface | `sh measure/attack_surface.sh lazydays\|openwrt`, `python3 measure/reachable.py` | results in `measure/results/` |
| GNS3 server | see 3.4 | must be running for everything below |
| Throughput lab | `python3 gns3/throughput_lab.py build`, `python3 gns3/lab_ui.py` → http://localhost:8090 | benchmark in the drawer; results in `gns3/results/<time>-<path>-<commit>.json` |
| Metro lab (28 routers) | `python3 gns3/metro_lab.py build\|start\|delete`, `python3 gns3/net_ui.py` → http://localhost:8095 | |
| Mesh of metros (84 routers, ~3.5 GB RAM) | `python3 gns3/metro_lab.py build --metros 3`, `METROS=3 python3 gns3/net_ui.py` | results in `gns3/results/experiments-<project>.jsonl` |
| Torus mesh | `python3 gns3/mesh_lab.py build 10x10\|check\|delete` | the 10×10 project was deleted after measuring |

GNS3 projects that existed at handover:
- "LazyDaysOS throughput", the owner's live lab;
- "LazyDaysOS throughput (dev)", for development;
- "LazyDaysOS home router", NAT node + VPCS PCs;
- "LazyDaysOS metro";
- "LazyDaysOS metro x3";
- "LazyDaysOS test lab", "untitled": old.

**Updating lab routers to a new kernel:**
- The throughput lab follows the `LazyDaysOS.bin` symlink.
- The metro and mesh labs boot their own copy, so re-run the builder or copy the new `LazyDaysOS.bin` over `~/GNS3/images/QEMU/LazyDaysOS-metro.bin`.
- The metro routers have **32 MB RAM** like the real device. Since the `perf` merge the kernel's heap is 16 MiB; test a new kernel with `qemu-system-i386 -m 32` before rolling it out (see §6).

---

## 5. Gotchas that already cost time

These are consolidated from the whole session; the kernel-level ones are in more detail in `CLAUDE.md`.

**Environment**
- No passwordless sudo: hand root commands to the owner.
- Some actions are refused by the AI assistant's auto-mode safety classifier: building VPCS from a GitHub clone, creating libvirt networks with `virsh`, and `rm -rf` inside Makefiles. Use apt packages, ask the owner, or find a non-destructive alternative (e.g. libFuzzer `-merge`).
- Missing kernel modules (`ifb`, `sch_netem`, `macvlan`) look like test failures.

**QEMU / KVM**
- TCG doesn't enforce everything real CPUs do. A GDT that ended at 256 MiB passed under TCG but caused a #GP on KVM, and GNS3 always uses KVM. That is why `make check` boots twice.
- QEMU's e1000 holds received frames for ~1 s after every RCTL write. This is not a driver bug.
- QEMU's e1000 never counts `rx_missed`: when its ring is full it stops reading the backend.

**GNS3**
- **Create projects with `auto_close: false`.** Otherwise GNS3 stops every node as soon as the last GUI client disconnects.
- GNS3 puts QEMU NICs behind a PCI bridge (the kernel scans 256 buses).
- Container interfaces get MTU 65521, which breaks ubridge ("send: Message too long"); the builders set 1500.
- A link *suspend* also takes the QEMU link down. For a *silent* failure use the link filter `{"packet_loss": [100]}`; for a clean cable cut use the QEMU monitor (`set_link gns3-N off` on both ends, monitor port found in `ps`).
- VPCS sends DHCPREQUEST with `ciaddr` set from source 0.0.0.0, so the DHCP server unicasts only when `ciaddr` equals the source.
- **Killing a `docker exec` client does not kill its process in the container.** Dashboards `pkill` their agents (marked by their first argument) on start, on rate change and on exit. Leftovers once filled every router's 16 TCP slots.
- In-band monitoring is heavy: each `/api/stats` read is ~16–30 KB. All-at-once polling overflows the NOC router's transmit ring, so the pollers stagger per router. At 0.25 s polling customers see real packet loss.

**Testing**
- veth pairs are useless for DHCP tests: Linux leaves UDP checksums to the NIC. Always go through QEMU's taps.
- Inside a user namespace `/sys` shows the host's interfaces; get MACs with `SIOCGIFHWADDR`.
- `tcpdump`'s privilege drop fails inside the namespaces, so checksum verification runs outside them.

**Fuzzing**
- Tables that mark free slots with 0 must never match a lookup for 0. That was a remote crash (UDP to port 0 matched a free slot with a NULL handler).
- clang's ASan puts each global in its own section unless built with `-fno-sanitize-address-globals-dead-stripping`. The Makefile does that and checks for it.

**Dashboards**
- `gns3/*_ui.html` pages are re-read from disk on every request; the Python backends need a restart (wait for the port to free).
- Run only **one** `lab_ui.py` per lab: its startup kills iperf3 in all lab containers.
- **Don't run tests on the owner's live lab** ("LazyDaysOS throughput" on :8090) while they may be using it. Use the "(dev)" copy (`LAB_PROJECT="LazyDaysOS throughput (dev)" LAB_UI_PORT=8091`) and stop its nodes afterwards: it shares host CPU and skews measurements.

---

## 6. Open items and next steps

In rough priority order. The owner hadn't yet decided between the first few.

1. **Push** `master` to GitHub (owner, after setting up credentials).
2. **Roll the merged `perf` kernel out to the metro labs.**
   - They still run the pre-`perf` copy (`LazyDaysOS-metro.bin`).
   - The merged kernel passes `make check`, and it boots with `-m 32` and 6 ports, which is the largest metro router: 16 MiB heap at 0x15f000, all ports up, RIP running.
   - What hasn't been tried is a full metro experiment on it.
   - WP5 needs the heap sized from the memory map instead of a fixed 16 MiB anyway.
3. **Route summarization at metro borders.** A `rip_summary=` kernel option; each metro announces its `10.(16m).0.0/12`. Re-run the "backbone cut" experiment before and after. Expected: ~75 instead of 219 routes per router, and failures contained in their metro.
4. **RIP authentication** (RFC 4822, HMAC-SHA256). Re-run the route-hijack experiment as the before/after test.
5. **Dual-homing S1.** Today C1, or the C1–S1 link, is a single point of failure that takes out every customer.
6. **WP3 / F4 in-kernel hardening:**
   - read-only `.text`/`.rodata` with CR0.WP;
   - a guard page under the stack;
   - `-fstack-protector-strong` with a CSPRNG canary;
   - a UBSan runtime for test builds.
   
   Measure each one's cost with the benchmark.
7. **WP5, the MIPS port** (5–6 weeks, highest risk):
   - UART on the TL-WR841N v14, TFTP boot into RAM only;
   - `kernel/compat/mips/`;
   - the MT7628 frame engine and switch driver.
   
   Buy two units. The fallback plan is in thesis-plan.md.
8. Smaller items:
   - From the WAN, UDP 67/68 and TCP 80 look "open|filtered" instead of closed, which makes the router easy to fingerprint.
   - The stock TP-Link firmware is not measured yet (needs the device).
   - The README claims MIT, but there is no `LICENSE` file.
   - The README is outdated compared to CLAUDE.md.

Not implemented in the stack (by design, so far):
- IP fragment reassembly and fragmentation;
- ICMP redirects and ICMP rate limiting;
- DHCP relay / INFORM;
- NAT port forwarding, hairpinning and ALGs;
- TCP active open and congestion control;
- MTUs other than 1500.

---

## 7. How the owner likes to work

Notes for an AI assistant taking over.

- **Conventions:** follow `CLAUDE.md` exactly.
  - `facility$name` identifiers, odd success status codes, `[K_INFO]`-style log lines.
  - Match the indentation of the file you edit.
  - A self-test or `check_net.sh` test for every fix, including a negative case, and one that fails on the old code.
- **Git:**
  - Work on a feature branch and merge into `master` when asked.
  - Commit with clear messages.
  - **Never push**; the owner does that.
- **Results are thesis material.** Keep measurements reproducible: commit hash, `-O` level and kernel hash are recorded with every benchmark, and experiments go to JSONL files. Write findings into `docs/thesis-plan.md`.
- **Dashboards:** the owner reacts to what they see.
  - They want action → consequence to be unambiguous: an experiment isn't over until its effects have settled.
  - They want "settled" kept separate from "healthy".
  - Every click needs visible feedback.
  - Designs should be clean and informative, not "AI-looking": no emoji, colour only for meaning.
  - Check visual changes with screenshots (§3.6) before reporting them.
- **Admit mistakes plainly.** Earlier "routers unreachable" problems turned out to be caused by leftover test processes, and the owner appreciated a straight explanation.
