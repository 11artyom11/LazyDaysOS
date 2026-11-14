# Thesis plan: a minimal, secure router OS for abandoned hardware

Draft, 2026-11-14. Working title:
**"LazyDaysOS: a minimal, hardened router kernel for home routers that mainstream firmware no longer supports."**

## 1. The problem

- **Abandoned routers stay online.** Cheap home routers keep running for years after their vendor stops shipping updates, and they are a favourite botnet target:
  - Mirai (2016) and its descendants.
  - VPNFilter (2018), at least 500,000 routers.
  - KV-botnet: Volt Typhoon's proxy network of end-of-life Cisco and Netgear SOHO routers, taken down by the FBI in January 2024.
  - Mirai exploiting CVE-2023-1389, a command injection in a TP-Link web interface.
- **The open-source alternative has dropped the smallest devices.** OpenWrt's last release for 4 MB flash / 32 MB RAM ("4/32") devices was 19.07.10, and support ended in 2022.
- **The target board is in that class.** The TL-WR841N v14 (MediaTek MT7628, MIPS 24KEc at 580 MHz, 4 MB flash, 32 MB RAM) was never officially supported even in 19.07. Image builds for it were disabled in OpenWrt master because the image no longer fits the flash.
- **LazyDaysOS fits with room to spare.** The stripped kernel is **88 KB**, gzipped dashboard included, and the network stack is about **5,800 lines** of C. There is no shell, no userspace, no busybox, telnet or CGI scripts: the components behind most router compromises don't exist here.

**Thesis claim:** a purpose-built router kernel can give 4/32 hardware a second life with the core home-router functions and a much smaller, measurably harder attack surface than the alternatives, at an acceptable performance cost.

### Research questions
- **RQ1 (feasibility):** Can a purpose-built kernel provide routing, NAT, firewall, DHCP and management on 4/32 hardware that mainstream firmware has abandoned?
- **RQ2 (attack surface):** How much smaller is its attack surface than OpenWrt and stock firmware? Measured by WAN- and LAN-reachable code, open services and parsers exposed to untrusted input.
- **RQ3 (robustness):** How many defects do systematic fuzzing and hardening find in a small stack written from scratch, and do the fixes hold under replayed attacks?
- **RQ4 (cost):** What does each hardening measure cost in throughput, latency, image size and RAM?

## 2. Threat model

| Attacker | Capabilities | Must not achieve |
|---|---|---|
| **Internet (WAN)** | Sends arbitrary packets to the WAN address, including spoofed sources; off-path, so it can't see LAN traffic | Code execution, management access, injecting into or resetting connections, denial of service beyond what the link itself allows |
| **Compromised LAN device** (an infected camera or plug) | Arbitrary packets on the LAN, including ARP and DHCP tricks | Taking over the router or changing its configuration without credentials |
| **Malicious web page** in a LAN user's browser | Makes the browser send requests to the router (CSRF), DNS rebinding | Reading or changing router state |
| Out of scope | Physical access (serial, flash chip), malicious bootloader, compromised build machine, Wi-Fi (not implemented) | |

**Assets to protect:**
- The router's integrity: it must not become a bot.
- Availability of forwarding.
- Isolation of the LAN from the WAN.
- Control over the management API.

## 3. Baseline audit: issues already visible in the code

Each issue gets a test that fails on today's code before it is fixed, per the project's conventions.

| # | Issue | Where | Impact | Fix |
|---|---|---|---|---|
| F1 | **Predictable randomness.** `net_random32()` is a xorshift seeded from PIT ticks and the MAC. It is used for TCP initial sequence numbers and NAT port selection. | `kernel/net/net.c:146`, `tcp.c:426`, `nat.c:189` | Off-path attackers can guess sequence numbers to inject into or reset TCP connections (RFC 6528 asks for unpredictable ISNs) | A CSPRNG (ChaCha20) seeded from real entropy (x86: RDRAND if present plus TSC jitter; MT7628: to be found); ISNs per RFC 6528 |
| F2 | **WAN can open TCP connections to the management port.** The 403 for WAN clients is sent only after the handshake and request parsing. Every SYN takes one of 16 slots and allocates 64 KiB. | `http.c:226`, `tcp.c:407` | A spoofed SYN flood from the Internet fills all slots: the LAN dashboard goes down and the heap is under pressure. The WAN can also reach the HTTP parser. | Per-listener interface policy enforced *before* the TCP layer (WAN SYNs dropped silently); allocate the send buffer only once the handshake completes, or use SYN cookies |
| F3 | **Management API without authentication**, with no Host or Origin checks | `http.c`, `api.c` | Any LAN device can block others; a web page in a LAN browser can `POST /api/block` (CSRF) or read `/api/stats` via DNS rebinding | Password, session token in an `HttpOnly; SameSite=Strict` cookie, CSRF token on POST, `Host` must be a router address, rate-limited login |
| F4 | **No memory protection inside the kernel.** All pages are writable (`vm_watchdog.c:36-46`), including `.text`; the 16 KiB stack sits in `.bss` with no guard page (`boot.s:20`); no stack protector | paging, boot, Makefile | A memory bug becomes code modification or silent data corruption instead of a clean panic | Read-only `.text`/`.rodata` with CR0.WP, an unmapped guard page below the stack, `-fstack-protector-strong`, UBSan in test builds |
| F2b | **The WAN could use the management API by addressing the LAN IP** (found while fixing F2). The router accepted packets on the WAN port for *any* of its addresses (weak host model), and the 403 check looked at the local address, not the arrival port. A WAN neighbour routing to 10.0.9.2 got `HTTP/1.1 200 OK` from `/api/stats`, and could also `POST /api/block`. | `ipv4.c` local delivery, `http.c:226` | Anyone on the ISP side controls the router's management API | Strong host model on NAT outside ports (packets for other own addresses dropped); admission by arrival port |
| F2c | **ARP table exhaustion** (found while fixing F2). Replies to spoofed sources created up to 64 unresolved ARP entries, which pushed out resolved ones and then blocked new resolutions. | `arp.c` allocation | A spoofed ping flood on the WAN segment evicts the default gateway: the whole LAN loses the Internet (100% loss in the test) | At most 16 unresolved entries, replacing each other; confirmed neighbours keep the rest |
| F5 | **Parsers never fuzzed:** Ethernet, ARP, IPv4, ICMP, UDP, DHCP client and server, TCP, HTTP, NAT | `kernel/net/*` | Unknown | Section 4, WP2 |

### Status of the audit fixes (2026-11-14, branch `security-f1-f2`)

| Check in `check_net.sh` (home scenario) | Old kernel (`master`) | New kernel |
|---|---|---|
| WAN connects to the dashboard on the WAN address | connected, `403 Forbidden` | silently dropped |
| WAN reaches the dashboard via the LAN address | connected, **`200 OK`** | silently dropped |
| WAN pings the LAN address | answered | no answer |
| Next ISN predictable from the previous one | **19 of 19** | 0 of 19 |
| LAN opens the dashboard right after a WAN SYN flood | failed (`000`) | `200` |
| LAN opens the dashboard during a spoofed LAN SYN flood | 0 of 3 | 3 of 3 |
| LAN pings the Internet during a spoofed WAN ping flood | 100% loss | 0% loss |

- **F1:** fixed with a ChaCha20 CSPRNG (`kernel/random.c`, RFC 8439 test vectors in the self-test). It is seeded from RDRAND, TSC jitter and the PIT count, mixed with the timing of every interrupt, and reseeded every second. TCP ISNs are SYN cookies built on keyed ChaCha20 hashes, in the RFC 6528 form.
- **F2:** fixed. The HTTP listener's `admit()` drops WAN SYNs before any state exists. The send buffer is allocated only after the handshake. SYN cookies take over when the connection table is full.
- **F2b and F2c:** fixed as described in the table above.
- `make check` passes: self-tests on TCG and KVM, and 91 network checks.

### Status of F3 (2026-11-14, branch `mgmt-auth`)

| Check in `check_net.sh` | `master` (F1/F2 only) | New kernel |
|---|---|---|
| `/api/stats` without credentials | **200** (device list, MACs, hostnames) | 401 |
| Request with a foreign `Host` (DNS rebinding) | **200** | 421 |
| POST with a foreign `Origin` (CSRF) | **200** (blocks a device) | 403 |
| POST in a session without or with a wrong CSRF token | n/a (no sessions) | 403 |
| 6 password guesses from one address | n/a (no password) | 401 ×5, then 429 with `Retry-After` |
| No password configured | open to every LAN device | random 12-character password printed on the console |
| Framing, MIME sniffing | allowed | `X-Frame-Options: DENY`, `nosniff`, CSP `frame-ancestors 'none'` |

- **Browsers use sessions.** An HttpOnly, SameSite=Strict cookie, plus a CSRF token on every POST.
- **Scripts use HTTP Basic** (`curl -u admin:...`). It is never offered to browsers.
- **Guessing is throttled per address:** 5 failures lock that address for 60 s, doubling with each lockout up to 16 min.
- **The password** comes from `admin_password=` or is generated at boot; nothing in the image is a shared default (ETSI EN 303 645 provision 5.1, UK PSTI). Changing it from the dashboard lasts until reboot, because there is no flash storage yet.
- **Dashboard:** sign-in screen, Password and Sign out buttons. `demo.sh` uses `lazydays`; the GNS3 lab uses `admin_password=lazydays-lab`, which `lab_ui.py` reads back from GNS3.
- **Tests:** `make check` passes (TCG + KVM self-tests, 113 network checks). All 19 new login checks fail on the `master` kernel.
- **Open:** passwords are compared in plain text in RAM. A proper password hash (needs SHA-256) comes with flash storage in WP5/WP6.

### Routing resilience and security: the metro lab (2026-11-14, branch `rip`)

**Setup:** RIPv2 in the kernel (`kernel/net/rip.c`), and a provider-style network of 28 LazyDaysOS routers in GNS3 (core full mesh, dual-homed aggregation and access) with 16 customers pinging a server every 200 ms. Measured on the live dashboard (`gns3/net_ui.py`) with RIP timers 5/30/20 s:

| Event | Customers affected | Outage | Notes |
|---|---|---|---|
| Cable cut C1–A1 (NICs see link down) | H1, H2 | 3.3–3.5 s | detour 3 → 5 routers, then 4 once RIP settles |
| Cable cut C1–C2 | H3, H4, H11, H12 | 2.2–2.4 s | |
| Silent failure C1–A1 (frames vanish, link up) | H1, H2 | **27.7 s** | only the RIP timeout (30 s) notices: detection dominates outage time |
| Restoring a link | none | 0 | paths shift back without loss |
| **Route hijack**: X7 misconfigured to speak RIP to its customer, compromised H7 announces two /25s of the server network | **all 16** | until removed | 28/28 routers hijacked within ~3 s; traffic loops between X7 and H7 until its TTL runs out |
| Stopping the hijack and fixing X7 | all 16 | ~47 s, then a second ~2 s outage | the forged routes live until the RIP timeout; then count to infinity loops packets through the core (~80 TTL expiries/s) |

**Takeaways for the thesis:**
- One misconfigured port plus one compromised device takes down the whole network for as long as the attacker likes, and the cleanup itself causes routing loops.
- **Next:** RIPv2 authentication (RFC 4822, HMAC-SHA256) to reject the forged routes, with this experiment as the before/after test. Faster failure detection for silent failures (a BFD-style hello) would cut the 28 s outage.

### Status of WP0 (2026-11-14, branch `attack-surface`)

- `measure/attack_surface.sh` puts LazyDaysOS and OpenWrt 19.07.10 x86-64 (factory defaults) into the `check_net.sh` topology and records:
  - nmap scans of all ports from the LAN and the WAN, plus IPv6 on the LAN
  - the router's own view (OpenWrt over its serial console)
  - login with no or empty password
- `measure/reachable.py` counts the stack lines that run only because of outside input (fuzz corpus under gcov).
- **Write-up:** [attack-surface.md](attack-surface.md). Headline results:
  - Factory OpenWrt: **root over SSH and an admin web session with an empty password from any LAN device**; SSH, DNS and HTTP on the LAN over IPv4 and IPv6; dnsmasq bound to the WAN address behind the firewall; 117 packages, 23 MB RAM in use.
  - LazyDaysOS: TCP 80 only, behind a login with no default password, nothing listening on the WAN except the DHCP client; a 111 KB image, ~0.7 MB RAM.
  - **692 of 3,454 network-stack lines are reachable from the WAN**, none of them in the HTTP parser, login or API.
- Found on the LazyDaysOS side: from the WAN, UDP 67/68 and TCP 80 look "open|filtered"/"filtered" instead of closed (easy to fingerprint).
- Not done: the stock TP-Link firmware (needs the device), and line-level exposure for OpenWrt.

### Status of WP2 (2026-11-14, branch `fuzzing`)

- **Harness** (`fuzz/`): the network stack compiled unchanged for Linux with AddressSanitizer and UndefinedBehaviorSanitizer.
  - Six targets: `net` (raw frames, both ports), `wan` (raw frames, WAN port only), `http`, `tcp`, `dhcp_server`, `dhcp_client`.
  - Every input starts from the same post-boot state: the stack's globals are moved into their own sections, snapshotted, and restored after each input.
  - **Leak invariant:** after 20 simulated idle minutes, no heap block older than 10 minutes may remain.
  - The corpus replay is part of `make check`. The seeds cover **80.5%** of the stack's 3,413 lines (HTTP 94%, DHCP client 95%, IPv4 89%, TCP 82%, NAT 76%).
- **First campaign:** gcc random mutation without coverage feedback, ~1.9 million inputs (6 targets × 6 seeds × 50,000, plus reruns). No memory errors and no undefined behaviour. Two real bugs, both found by the leak invariant:

| # | Finding | Impact | Fix | Old kernel |
|---|---|---|---|---|
| Z1 | **FIN_WAIT_2 never ends.** After the server closes, a peer that acknowledges the FIN but never sends its own keeps the connection forever | 16 such clients hold all TCP slots and 1 MiB of heap: the dashboard is unreachable | FIN_WAIT_2 frees the send buffer, times out after 60 s, and its slot is reused when the table is full | `TimeoutError` after 16 lingering clients |
| Z2 | **No persist timer.** With data queued and a zero window, no timer runs | A peer that stops reading (or one lost window update) keeps the connection and 64 KiB forever; 16 such clients lock out the dashboard | Persist timer with probes, dropped after 8 unanswered; closed-but-stalled connections are reset when the table is full | `TimeoutError` after 16 stalled clients |

- **libFuzzer campaign** (clang 21, coverage-guided, all six targets in parallel for 2 hours). The `wan` target crashed after 5.1 million inputs (~58 min):

| # | Finding | Impact | Fix | Old kernel (QEMU) |
|---|---|---|---|---|
| Z3 | **UDP to port 0 called a NULL function.** Free slots in the UDP port table have port 0 and no handler, and the lookup didn't skip them | **One datagram from the internet crashes the router**: jump to address 0, page fault, kernel panic | The lookup skips free slots | `[K_PANIC] #PF page fault ... eip=0x0` |
| Z4 | **TCP SYN to port 0 matched a free listener slot** (same pattern). It was accepted with no application and no `admit()` check, so from the WAN too, and never closed | Internet hosts could hold all 16 connection slots and 1 MiB forever (dashboard down) | Port 0 never matches; `tcp_listen(0)` refused; segments from 0.0.0.0, broadcast, multicast, loopback or our own addresses dropped (RFC 1122) | the router answers `SYN-ACK` from the WAN |

- One false positive along the way (a DHCP client renewing a 25-minute lease during the drain) led to the age-based form of the leak invariant.
- **libFuzzer campaign totals:** 2 hours per target, 6 targets in parallel on one laptop.

| Target | Inputs run | Inputs/s | Result |
|---|---|---|---|
| `net` | 7.05 M | 1,501 | crash Z3 (found again from the LAN side) |
| `wan` | 5.12 M | 1,482 | crash Z3 after ~58 min |
| `http` | 4.01 M | 435 | clean |
| `tcp` | 3.27 M | 355 | clean |
| `dhcp_server` | 5.48 M | 594 | clean |
| `dhcp_client` | 2.02 M | 218 | clean |

- About **27 million inputs** in total. Merging the inputs that added coverage grew the regression corpus from 209 to 3,546 inputs (5.7 MB; `make check` replays them twice each in seconds).
- **Line coverage of the stack rose from 80.5% to 88.4%** (3,418 lines), for example TCP 82→93%, IPv4 89→98%, NAT 76→88%, auth 69→84%. What's left is mostly boot-time self-tests and the NIC polling path the harness bypasses.
- Apart from Z3/Z4: no memory errors, no undefined behaviour and no leaks in any target.
- **Next:** longer campaigns (overnight) on `wan` and `tcp`; a fuzz target for the management API's JSON output and the events log.

## 4. Work packages

### WP0: attack-surface inventory and comparison baseline (1 week)
- **Inventory:** list every entry point for untrusted input, separately for the WAN and the LAN. Measure the code reachable from each, using a call graph (`-fcallgraph-info` or `cflow`) from `eth_receive` with the WAN gating taken into account.
- **Comparison systems:**
  - **OpenWrt 19.07** x86 image in the same GNS3 lab (same topology, same tests).
  - Stock TP-Link firmware on the real TL-WR841N v14, if one is available.
  - A plain Linux router (`ip_forward` + nftables) as a reference.
- **Metrics, for every system:**
  - TCP/UDP ports open from the WAN and the LAN (nmap)
  - number of parsers and lines of code exposed to untrusted input
  - image size and RAM in use
  - number of third-party components
  - the CVE history of those components
- **Deliverable:** the threat-model and attack-surface chapter, plus a script that reproduces the measurements.

### WP1: fix the audit findings (2 weeks)
Fix F1–F4. Each fix comes with tests:
- **F1:** the self-test checks the CSPRNG against known ChaCha20 test vectors. `check_net.sh` collects ISNs over many connections and several reboots and fails if consecutive ISNs are correlated. The test is written against today's xorshift first, to show it detects the problem.
- **F2:** a WAN host sends SYNs to port 80 and must get nothing back. A WAN SYN flood (`hping3 --flood --rand-source`) must not stop a LAN client from loading the dashboard.
- **F3:**
  - a request without a session gets 401
  - a `POST` without the CSRF token gets 403
  - `Host: attacker.example` gets 403
  - a DNS-rebinding proof-of-concept page fails
  - ten wrong passwords trigger a lockout
- **F4:** a self-test writes to `.text` and must get #PF (run in a separate "expect panic" pass of `check_qemu.sh`). A deep-recursion test must hit the guard page instead of corrupting `.bss`.

### WP2: fuzzing the network stack on the host (3 weeks)
- **Build `kernel/net/` as a Linux program** with clang, libFuzzer, ASan and UBSan. Replace the few external dependencies with stubs: `pit.h` (fake clock), `kio` (no-op or a buffer), `heap.h` (`malloc`), `netdev` (records transmitted frames), `cpuload.h`.
  - Bonus: this enforces the rule that `kernel/net` has no x86 dependencies, which the MIPS port needs anyway.
- **Fuzz targets:**
  1. `eth_receive` on a LAN port and on a WAN port (separate harnesses, since the reachable code differs)
  2. the HTTP request parser
  3. the DHCP server
  4. the DHCP client in each state
  5. TCP as a **stateful** target: the input is a sequence of segments driven against the state machine, with the fake clock advancing between them
  6. NAT: outbound and inbound translation, ICMP error translation
- **Seed corpus:** the pcaps `check_net.sh` already writes (`qemu_*_{lan,wan}.pcap`).
- **Process:**
  - Run a fixed CPU budget per target (e.g. 24 core-hours) and report coverage.
  - Triage every crash, fix it and add a regression input; where the code allows, also add a kernel self-test.
  - Optionally, differential testing: the same packets through LazyDaysOS and Linux, then compare which ones each accepts.
- **Deliverable:** `fuzz/` with a `make fuzz` target, the bug list (cause, fix, test) and coverage numbers. This becomes the core of the robustness chapter.

### WP3: in-kernel hardening and its cost (1.5 weeks)
- **Measures:**
  - stack protector (needs `__stack_chk_guard` from the CSPRNG and a `__stack_chk_fail` that panics)
  - read-only text and rodata, guard pages
  - optional bounds assertions in packet parsers, enabled in test builds
  - a minimal UBSan runtime for test builds (`-fsanitize=undefined` with the kernel's own handlers)
- Measure each measure separately with the existing RFC 2544 benchmark (`gns3/lab_ui.py`): pps per frame size, latency under load, image size.
- Limitation to state openly: 32-bit x86 without PAE has no no-execute bit. Whether the MIPS 24KEc offers execute-inhibit needs checking; probably not, which belongs in the discussion.

### WP4: secure management (1.5 weeks)
- Implement the F3 fix fully:
  - first-boot password setup on the LAN, or a password from the kernel command line for tests
  - login page, session cookie, CSRF token
  - Host-header allow-list and login rate limiting
- Scope choice: **HTTPS is optional.** TLS adds a lot of code (BearSSL is the smallest serious option) and a certificate problem. The thesis argues that LAN-only HTTP with these defences is the realistic baseline for this hardware class, and lists TLS as future work.

### WP5: port to the TL-WR841N v14, MT7628 (5–6 weeks; start the hardware bring-up early)
1. **Hardware access:**
   - solder a UART header (3.3 V serial)
   - get a U-Boot prompt
   - load images over TFTP **into RAM only**; never flash until the system is stable, to avoid bricking
   - buy two units (they are cheap secondhand)
2. **Architecture layer** under `kernel/compat/mips/`:
   - boot entry and linker script
   - exceptions and interrupts
   - CPU timer (replace `pit.h` with a generic time API used by `kernel/net`)
   - `cpuload` (MIPS `wait`)
   - serial console
3. **Ethernet driver:** the MT7628 frame engine plus the embedded switch, behind `netdev`. This is the biggest technical risk.
   - Reference: the MT7628 datasheet and programming guide, with OpenWrt/Linux drivers read for understanding only. They are GPL and the kernel claims MIT, so no code is copied.
   - Two ports: WAN, and the LAN switch ports as one port.
4. **Memory budget for 32 MB:** today's heap is a fixed 16 MiB. Size it from the memory map, and measure the peak use of the RX/TX rings, TCP buffers and the NAT table.
5. **Evaluation on the hardware:**
   - boot time
   - pps per frame size (the benchmark can drive real hosts)
   - RAM use
   - the same security tests as in the lab
- **Fallback if the driver slips:**
  - Show the 32 MB fit on x86 (`qemu -m 32`) and the 4 MB flash fit by image size.
  - Present the MIPS port as booting to the serial console with the network stack's self-tests passing.

### WP6 (stretch): signed updates (2 weeks)
- The router accepts a new image over the management API only if it carries a valid Ed25519 signature (Monocypher, a few KB of code) and a version number higher than the running one (anti-rollback).
- **In QEMU:** verification and the API only.
- **On hardware:** write to the flash partition.
- This closes the gap that turns abandoned routers into botnets: the update path itself.

### WP7: evaluation and writing (3–4 weeks, overlapping)
| Measure | LazyDaysOS (before → after hardening) | OpenWrt 19.07 | Linux + nftables | Stock firmware |
|---|---|---|---|---|
| Open TCP/UDP ports, WAN / LAN | | | | |
| Code reachable from WAN input | | | | |
| Image size / RAM in use | | | | |
| Fuzzing: bugs found / coverage | | (n/a) | (n/a) | (n/a) |
| Replayed attacks (SYN flood, ISN injection, CSRF, rebinding, malformed packets) | | | | |
| RFC 2544 pps per frame size, latency under load | | | | |

## 5. Timeline (about 20 weeks; adjust to the actual deadline)

| Weeks | Work |
|---|---|
| 1 | WP0 inventory and baseline; **order the TL-WR841N v14 units and a USB-serial adapter** |
| 2–3 | WP1 fixes F1–F4 with tests |
| 4–6 | WP2 fuzzing; in parallel, solder the UART and get the U-Boot prompt and a RAM boot of a "hello" image |
| 7–8 | WP3 hardening and cost measurements; WP4 secure management |
| 9–14 | WP5 MIPS port and Ethernet driver |
| 15–16 | WP6 signed updates (stretch) or a buffer for WP5 |
| 15–20 | WP7 evaluation runs and thesis writing (start the chapters for WP0–WP3 earlier) |

## 6. Thesis chapters, mapped to the work
1. Introduction: abandoned routers, botnets, the 4/32 gap (Section 1)
2. Background: router architecture, attack classes, related work (seL4-based and unikernel network functions, OpenWrt hardening, fuzzing network stacks)
3. LazyDaysOS design: what exists today (CLAUDE.md is a good source)
4. Threat model and attack-surface analysis (WP0)
5. Hardening: audit findings, fixes, memory protection, management security (WP1, WP3, WP4)
6. Fuzzing the stack (WP2)
7. Port to the MT7628 (WP5) and signed updates (WP6)
8. Evaluation (WP7)
9. Limitations and future work: Wi-Fi, IPv6, TLS, NX, formal verification of the parsers

## 7. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| MT7628 Ethernet driver takes longer than planned | High | Start hardware bring-up in week 4; fallback in WP5 |
| Bricking the router | Medium | RAM boot over TFTP only until stable; two units; serial console |
| Fuzzing finds nothing | Low for a new stack | Still a result if coverage is high; add stateful TCP and differential testing |
| "No Wi-Fi" weakens the home-router claim | Certain | State the scope: wired router or edge device; Wi-Fi as future work |
| No hardware entropy source on the MT7628 | Medium | Jitter entropy from the CPU counter and interrupt timing, documented as a limitation |
| License contamination from GPL drivers | Low, if careful | Use the datasheet for the implementation; note references; add the missing `LICENSE` file |

## 8. First steps
1. Add a `LICENSE` file (the README claims MIT, but no file exists).
2. Write the failing tests for F2 (WAN SYN to port 80) and F1 (ISN predictability), then fix them. Both are small and give the thesis its first before/after results.
3. Set up the OpenWrt 19.07 x86 comparison node in GNS3.
4. Order the hardware.

## Open decisions
- Deadline, which sets the timeline above.
- Is a TL-WR841N v14 already at hand, or does it need ordering?
- HTTPS: out of scope (recommended), or required by the supervisor?
- Is the supervisor fine with "wired router, no Wi-Fi" as the scope?

## Sources
- OpenWrt, [Warning about 4/32 devices](https://openwrt.org/supported_devices/432_warning) and [TP-Link TL-WR841N(D) table of hardware](https://openwrt.org/toh/tp-link/tl-wr841nd)
- OpenWrt forum, [Support for TL-WR841N v14](https://forum.openwrt.org/t/support-for-tl-wr841n-v14/19143)
- RFC 6528 (TCP sequence number attacks), RFC 5961 (blind in-window attacks), RFC 2544 (benchmarking)
