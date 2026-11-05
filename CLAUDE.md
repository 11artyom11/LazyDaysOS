# CLAUDE.md

Guidance for Claude Code when working in this repository.

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
make check         # check_qemu.sh (in-kernel self tests) then check_net.sh (router tests against Linux); exit 0 = all passed
make install       # copies libk + headers + kernel into sysroot/, builds ISO, runs multiboot check
./bootit.sh        # qemu -kernel sysroot/boot/kernel/LazyDaysOS.bin, one e1000 on user networking, serial on this terminal
./bootit.sh --iso  # qemu -cdrom sysroot/boot/kernel/LazyDaysOS.iso (GRUB boots immediately)
./bootit.sh -d     # adds qemu -s -S (waits for gdb on :1234); combinable with --iso
./demo.sh          # live home-router demo with 4 simulated devices; dashboard at http://localhost:8080
./debug.sh         # opens two gnome-terminal windows: qemu -d + `gdb --command=gdbconf` (breaks at _start)
make clean
```

- **Run `make check` after every kernel change.** It passes `test` on the kernel command line, and the kernel then reports its self-test result through QEMU's `isa-debug-exit` device (port `0xF4`): QEMU exit status 1 = pass, 3 = a test failed or the kernel panicked. `check_qemu.sh` prints the serial log (also saved to `qemu_check.log`) and also catches hangs (30 s timeout) and triple faults (`-no-reboot`). It attaches **two e1000 NICs** on separate QEMU user networks (slirp: 10.0.2.0/24 and 10.0.3.0/24), configures them from the command line (eth0 via QEMU's built-in DHCP server, eth1 statically) and pings each network's gateway from the kernel; traffic goes to `qemu_net0.pcap` / `qemu_net1.pcap` (read with `tcpdump -nn -e -r qemu_net0.pcap`).

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
| `http` | HTTP management API and dashboard on port 80, inside ports only |
| `tcp_test` | TCP test services: echo on port 7, 1 MiB chargen stream on port 19 |
| `nat=ethN` | `ethN` is the outside (WAN) port: NAPT outbound, only tracked replies inbound (repeatable, max 2) |
| `udp_echo` | enable the RFC 862 UDP echo service on port 7 (testing) |
| `ping=a.b.c.d` | test mode: the kernel must get an echo reply from it (repeatable) |
- `check_net.sh` needs no root. For each **scenario** it re-runs itself under `unshare --user --map-root-user --net`; that namespace is the **LAN host** (`tap0` 10.0.9.1/24, default via the router 10.0.9.2). A second namespace (`unshare --net sleep`, entered with the script's `wan` helper = `nsenter --net`) is the **WAN host** (`tap1` 10.0.8.1/24, 192.168.50.1/32 on `lo`, route back to 10.0.9.0/24 via 10.0.8.2). `tap1` is moved into it *after* QEMU has opened it.
  - `static`: `eth1=10.0.8.2/24`, `route=192.168.50.0/24,10.0.8.1`, `udp_echo`. Host behaviour, forwarding both ways (`ttl=63`), static route, forwarded ping/UDP bursts with zero loss, ICMP time exceeded / net / host / port unreachable, UDP echo.
  - `home`: `eth1=dhcp` against `dnsmasq` on the WAN host (reserves 10.0.8.2 for MAC `52:54:00:12:34:57`, gateway 10.0.8.1, DNS 10.0.8.53), `dhcpd=eth0,10.0.9.100,10.0.9.150`, `nat=eth1`. The WAN host has **no route back** to the LAN (`WAN_ROUTE_BACK=no`), so everything must go through the NAT. Checks the WAN lease and DHCP default route; the LAN server with real `busybox udhcpc` (lease contents incl. DNS and a `-x hostname:lazy-laptop`, stable address, out-of-pool request refused) and with `dhcp_probe`, a Python INIT-REBOOT request (ACK / NAK cases); then NAT against `wan_echo_server` (UDP echo :5002, TCP echo :8080, logs the peer it sees): translated source with preserved port, two LAN hosts on the same port, 1 MiB TCP echo intact, ICMP port unreachable translated back, address-and-port-dependent filtering, and the firewall (a LAN UDP listener must receive nothing; a ping alone can't show this because a leaked reply comes back NATed and ping ignores it); finally the HTTP API (`http`): `/api/stats` cross-checked against what the scenario did, `/api/events` and `since=`, block → ping fails → dashboard still reachable → unblock, 403 from the WAN, 400/404/405/408/413/431, 30 parallel requests.

  - `tcp`: `tcp_test` (echo :7, 1 MiB chargen :19). Echo and chargen integrity, closed port → RST, 200 sequential and 8 concurrent connections, then 10% loss in **both** directions (`netem` on `tap0` egress; `ifb0` + ingress redirect for router → LAN) for echo, chargen and concurrent transfers. The loss checks are random but have been stable (6–10 s worst case vs 30 s socket timeouts).

  Run a subset with `SCENARIOS="tcp" sh check_net.sh LazyDaysOS.bin`. Afterwards `tcpdump -vv` verifies every IP/ICMP/UDP checksum in `qemu_<scenario>_{lan,wan}.pcap`, **outside** the namespaces (tcpdump's privilege drop fails inside them). Kernel output: `qemu_<scenario>.log`. Prints `[SKIP]` and passes if unprivileged namespaces are unavailable.
- To add a check, use `check_ping "description" "0%|100%" <cmd...>` or `check_output "description" "<regex>" <cmd...>` inside a `scenario_*` function; prefix the command with `wan` to run it on the WAN host. A new scenario is a `scenario_<name>` function plus its name in `SCENARIOS`.
- veth pairs are useless for DHCP tests here: Linux leaves UDP checksums to the (nonexistent) NIC, so raw-socket clients like `udhcpc` drop the packets. Always go through QEMU's taps.
- `demo.sh` (no root): the `home` topology plus four `macvlan` devices (own MAC, DHCP lease with hostname via `udhcpc -x hostname:`, own default route by metric) generating traffic through the NAT to a file server / UDP sink on 192.168.50.1, and a `socat` bridge (UNIX socket, shared across namespaces) exposing the dashboard on `127.0.0.1:$DEMO_PORT` (8080). `DEMO_SECONDS=N` stops it after N seconds, otherwise it follows the kernel log until Ctrl+C. Kill a background demo by process group: the shell defers TERM while a child runs.
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

## Build layout (Makefile)

- `CC=i686-elf-gcc -m32`, `CFLAGS=-nostdlib -ffreestanding -D __LF -Ilibk/include -Ikernel/compat/i386/include -g -O0`. Link: `-T kernel/compat/i386/linker.ld ... -lgcc`.
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
kernel/heap.c/.h           kmalloc/kfree: first-fit heap over 4 MiB of contiguous frames
kernel/net/netdev.c/.h     driver-independent NIC interface: register, transmit, poll, rx handler, stats
kernel/net/net.c/.h        stack entry (net_initialize, net_run_once), byte order, checksum, IP parsing/printing
kernel/net/eth.c/.h        Ethernet rx dispatch by EtherType, eth_send
kernel/net/arp.c/.h        ARP cache (64 entries), replies, resolution with one held packet per destination
kernel/net/ipv4.c/.h       interface addresses, longest-prefix routing table, header validation, ipv4_send
kernel/net/icmp.c/.h       echo reply, icmp_ping, icmp_send_error (time exceeded, net/host/proto/port unreachable)
kernel/net/udp.c/.h        UDP ports (udp_bind), pseudo-header checksum, udp_send / udp_send_dev, echo service
kernel/net/dhcp.c/.h       DHCP message layout and option helpers, client/server API
kernel/net/dhcp_client.c   RFC 2131 client state machine (one per port with ethN=dhcp)
kernel/net/dhcp_server.c   pools, lease table, OFFER/ACK/NAK, RELEASE/DECLINE
kernel/net/nat.c/.h        NAPT + connection tracking (512 entries, two hash tables), firewall, ICMP error translation
kernel/net/tcp.c/.h        TCP server endpoint: state machine, retransmission, out-of-order queue, test services
kernel/net/http.c/.h       HTTP/1.1 server (one request per connection, routes, limits, LAN-only)
kernel/net/api.c/.h        /, /api/stats, /api/events, /api/block, /api/unblock
www/index.html             the dashboard (one self-contained file); gzipped and linked in as www.o
kernel/net/clients.c/.h    inside hosts: per-device byte counters, block switch (enforced in ipv4$$forward)
kernel/net/events.c/.h     64-entry event ring for the dashboard (also echoed to the console as [K_EVNT])
kernel/system.h            system_ready_ms (set in kernel_main)
libk/util/strbuf.c         bounded string/JSON builder (sb_*), used for JSON and event texts
kernel/cmdline.c/.h        kernel command line copy, cmdline_has / cmdline_get
kernel/kernel.c            kernel_main(magic, mbi)
kernel/compat/i386/        arch-specific:
  kern_powerup.c             powerup_kernel_i386 (bring-up order), kern$self_test, test mode + QEMU exit
  pmm.c                      physical frame bitmap allocator from the Multiboot memory map
  vm_watchdog.c              paging setup
  pit.c                      PIT channel 0 at 100 Hz, tick counter, pit_sleep_ms
  pci.c                      PCI config mechanism #1, bus scan, BAR decode, pci_enable_device
  e1000.c                    Intel 82540EM/82545EM driver: 32-entry RX/TX DMA rings, shared IRQ handler
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
5. Set `input_allowed = true` and loop on `net_run_once()`: every 100 ms run protocol timers (`arp_tick`, `dhcp_client_tick`, `dhcp_server_tick`, `nat_tick`, `tcp_tick`, `http_tick`), then `netdev_run_once()` polls NICs with pending frames or does `sti; hlt` until the next interrupt.

Network receive path: the e1000 IRQ handler only reads ICR (which acknowledges the interrupt) and calls `netdev_mark_pending()`. Frames are processed later in `netdev_run_once()` → driver `poll()` → `netdev_receive()` → the single registered `netdev_rx_handler`, i.e. **not in interrupt context**. The registered handler is `eth_receive` → `arp_receive` / `ipv4_receive` → `icmp_receive` / `udp_receive` (→ the handler bound to the port, else ICMP port unreachable). Packets not addressed to us go to `ipv4$$forward`: drop martians (0/8, 127/8, ≥224) and directed broadcasts, TTL ≤ 1 → ICMP time exceeded, no route → net unreachable, else copy, decrement TTL, recompute the header checksum, `arp_output` to the next hop. If ARP gives up on a next hop, the held packet's sender gets ICMP host unreachable.

NAT hooks (`kernel/net/nat.c`): a packet arriving on an outside port and addressed to that port's own address is first offered to `nat_inbound` (on a copy); if it matches a tracked connection it is rewritten toward the inside host and forwarded with `translated = true`, otherwise it is delivered locally (DHCP, pings to the router). In `ipv4$$forward`, anything from an outside port that isn't `translated` is dropped (`nat_stats.fw_dropped`), and anything leaving through an outside port goes through `nat_outbound` before the TTL decrement. Mappings are endpoint-independent with port preservation (RFC 4787); inbound filtering is address-and-port-dependent. Checksums are patched incrementally (`net_checksum_replace16/32`, RFC 1624) because the TCP payload isn't re-summed. Changing an outside port's address (`ipv4_clear_address`) flushes its mappings. Sending goes `icmp`/caller → `ipv4_send` (route lookup, header, checksum) → `arp_output` (resolve or hold) → `eth_send` → `netdev_transmit`. Every layer copies into its own stack buffer (up to ~4.5 KiB of stack per send). Transmit copies the frame into a per-descriptor buffer, so callers may reuse their buffer immediately.

Memory layout (identity mapped, virtual = physical): page 0 is **not present** (NULL dereferences panic with `#PF`, `cr2=0x0`); 4 KiB–4 MiB uses 4 KiB pages; 4 MiB–4 GiB uses 4 MiB PSE pages, uncached above the end of RAM. Frames below 1 MiB, the kernel image and the Multiboot structures are reserved. With QEMU's default 128 MiB, the heap lands right after the kernel image.

### Things to know before editing low-level code

- Unhandled CPU exceptions (vectors 0–31 with no handler) panic; returning would re-run the faulting instruction. Register handlers with `isr_register(vector, fn)`; up to `ISR_MAX_SHARED` per vector, all called on every interrupt (PCI NICs share IRQ lines; QEMU puts them on IRQ 11). Handlers are `void (*)(void)` and run **before** the EOI is sent, because PCI interrupts are level-triggered.
- Stack conventions (`kernel/net/net.h`): IPv4 addresses are `uint32_t` in **host** byte order everywhere; header structs are packed with fields in **network** order; convert with `net_htons`/`net_htonl` only at the header boundary. `net_checksum()` returns a value ready to store in a header, and checking a received header yields 0. Nothing in `kernel/net/` may include x86 or e1000 headers (`pit.h` for time is the only platform API used).
- `icmp_send_error` enforces RFC 1122's "never send an error about…" rules (ICMP errors, non-first fragments, broadcast/multicast destinations, bad or own sources). Use it for every error so those rules stay in one place.
- Checksums over several pieces (UDP/TCP pseudo-header + segment): `net_checksum_add` each piece, then `net_checksum_finish`; only the last piece may be odd-length.
- DHCP: the client asks for broadcast replies (the stack would otherwise treat a unicast to the not-yet-applied address as someone else's packet). The server broadcasts all replies except unicast ACKs to clients that already have `ciaddr`. `ipv4_send_dev`/`udp_send_dev` send out a given port with an exact source (0.0.0.0) and no route lookup; use them only for that kind of link-local traffic. `net_random32()` is xorshift seeded from uptime and MAC: transaction ids only, never security.
- TCP (`kernel/net/tcp.h` has the full list): passive open only; up to 16 connections with a 64 KiB send ring each (kmalloc'd at SYN time). **`tcp_send`/`tcp_close` only queue**: the stack transmits after the callback returns, so segments fill to MSS; call `tcp_output()` when sending from outside a callback. The advertised window is min(16 KiB, free send space), so a request/response app can always queue its reply, and it stays constant so the peer recognises duplicate ACKs. Out-of-order segments get an immediate pure duplicate ACK and up to 8 are queued. Fast retransmit + NewReno partial-ACK recovery, RTO per RFC 6298 with a 200 ms floor, backoff dropped on new ACKs. No congestion control, no SACK/window scaling/timestamps (Linux falls back), TIME_WAIT is 2 s and the oldest TIME_WAIT slot is reused when full. Measured in QEMU: ~120–150 Mbit/s echo without loss; with 10% loss both ways 1 MiB chargen ~2–3 s, 256 KiB echo 5–10 s (the peer's own timeouts dominate). Each of these mechanisms was added after the loss tests exposed its absence.
- Dashboard (`www/index.html`): plain HTML/CSS/JS, no external resources (the router may be offline), light/dark, phone layout. The Makefile gzips it and links it with `i686-elf-ld -r -b binary` (symbols `_binary_dashboard_html_gz_start/_end`); `/` serves it with `Content-Encoding: gzip`. It polls `/api/stats` and `/api/events?since=` once a second and computes all rates in the browser from counter deltas. **Update the DOM in place** (`portEls`/`deviceEls` keyed by name/IP), never rebuild lists: a click spanning a refresh is otherwise lost; keep the device order stable (by IP) for the same reason. Check visual changes with `demo.sh` in a real browser.
- Management API (`kernel/net/api.h` lists the routes): JSON is built with `strbuf` into a 16 KiB kmalloc'd buffer handed to the response as `owned` (freed after sending); on overflow the handler answers 500. `/api/stats` field names are what the dashboard reads; change them only together. HTTP requests from a NAT outside port get 403. A blocked client loses forwarding only; it can still reach the router (dashboard, DHCP). `system_ready_ms` excludes BIOS/GRUB time.
- The receive path copies packets on the stack at several layers (NAT inbound copy, forward copy, ICMP error, eth frame): worst case ~8 KiB of the 16 KiB kernel stack. Don't add more large stack buffers on that path.
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
