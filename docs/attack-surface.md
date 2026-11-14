# Attack surface: LazyDaysOS vs OpenWrt 19.07.10 (WP0)

Measured 2026-11-14. Reproduce with `sh measure/attack_surface.sh lazydays|openwrt` and
`python3 measure/reachable.py`. Raw output: [`measure/results/`](../measure/results/).

## Method

- **Topology.** Both routers boot in QEMU between a LAN host and a WAN host in network namespaces, the same topology as `check_net.sh`.
  - The WAN host runs dnsmasq, which gives the router `10.0.8.2`.
  - Both routers get 128 MB of RAM and two e1000 NICs.
- **Configurations:**
  - **LazyDaysOS**, current `master`, in its home-router configuration: `eth1=dhcp nat=eth1 dhcpd=eth0,... http admin_password=...`.
  - **OpenWrt 19.07.10** x86-64 (`openwrt-19.07.10-x86-64-combined-ext4.img`, SHA-256 checked against downloads.openwrt.org) with **factory defaults**. This is the last OpenWrt release for 4 MB flash / 32 MB RAM devices such as the TL-WR841N v14. The MIPS build for that device was never official for v14, so the x86 build of the same release stands in for it.
- **From outside:** nmap 7.98 scans of all 65,535 TCP and UDP ports, from the LAN host and from the WAN host. Also a scan of the LAN address routed through the WAN (a WAN neighbour addressing the inside directly), and IPv6 on the LAN link.
- **OpenWrt's rate limits were lifted for the scans.** Its kernel limits ICMP errors and its default firewall limits incoming SYNs to 25/s (`syn_flood`). At scan speed, probes hitting those limits look "filtered", and an open port could hide among them.
  - The script lifts the limits over the serial console before scanning and records that it did.
  - No service or firewall rule changes.
  - LazyDaysOS has no such limits.
- **From inside:**
  - OpenWrt: its own shell over the serial console (`netstat`, `ps`, `free`, `df`, `opkg`).
  - LazyDaysOS: `/api/stats` and the image itself.
- **Code reachable by input:** the fuzz corpus (WP2) under gcov, minus what an empty input executes (boot and timers). This is a lower bound, but every line counted is demonstrably reachable. There is no OpenWrt equivalent at line level; its exposed components are given by binary size instead.

## Results

### What each side can reach

| | LazyDaysOS | OpenWrt 19.07.10 (factory defaults) |
|---|---|---|
| **LAN, TCP** (IPv4) | **80** (management, login required) | **22** (SSH, Dropbear), **53** (DNS, dnsmasq), **80** (LuCI, uHTTPd) |
| **LAN, UDP** (IPv4) | 67, 68 (DHCP; no answer to a probe, so "open\|filtered") | 53 (DNS), 67 (DHCP), one high port (dnsmasq's upstream socket) |
| **LAN, IPv6** | none (no IPv6) | TCP 22, 53, 80; UDP 53 (DHCPv6 546/547 also listening) |
| **WAN, TCP** | none open; 80 silently dropped, all others reset | none open; all rejected |
| **WAN, UDP** | 67, 68 "open\|filtered" (no answer), all others closed | all closed (the DHCP client uses a raw socket) |
| **WAN → LAN address** | nothing: all 65,535 dropped | nothing: all rejected |
| Services listening on the **WAN address** (inside view) | DHCP client | dnsmasq on `10.0.8.2:53` (TCP and UDP), kept away from the WAN only by the firewall |

### Credentials out of the box

| | LazyDaysOS | OpenWrt 19.07.10 |
|---|---|---|
| Default password | none shared: `admin_password=`, or a random 12-character one printed on the console at boot | **none**: root has no password until the owner sets one |
| Login with no or empty password | `401` / `401` | **SSH: root shell** (`logged in as root on OpenWrt`); **web: admin session** (`302`, `Set-Cookie: sysauth=...`) |
| Password guessing | 5 tries per address, then lockout (60 s, doubling) | not measured (moot while there is no password) |

Any device on a factory-default OpenWrt LAN, including a compromised IoT device, becomes root on the router until the owner sets a password. OpenWrt documents this and the web interface shows a warning. ETSI EN 303 645 provision 5.1 and the UK PSTI regulations prohibit universal default passwords for exactly this reason.

### Size and code

| | LazyDaysOS | OpenWrt 19.07.10 (x86-64) |
|---|---|---|
| Image | **111 KB** stripped (54 KB gzipped) | 12.6 MB root filesystem in use (8.2 MB compressed image); the MIPS 4/32 build no longer fits 4 MB flash |
| RAM in use after boot | ~0.7 MB (356 KB kernel image incl. buffers + 346 KB heap) | 23 MB (`free`, at idle) |
| Packages / third-party components | 0 (libgcc: one division helper) | 117 packages |
| Software listening to the network | the kernel itself | Linux 4.14 (IPv4, IPv6, netfilter/conntrack), dnsmasq (227 KB), dropbear (185 KB), uhttpd (54 KB) + LuCI (404 KB Lua) + rpcd, odhcpd (75 KB), odhcp6c, udhcpc in BusyBox (377 KB), netifd (147 KB), musl libc (460 KB) |
| Source code | ~8,600 lines of C and assembly (5,600 in the network stack) | Linux alone has millions of lines; each daemon above is tens of thousands |

### LazyDaysOS: code reached by outside input (lower bound, from the fuzz corpus)

| | lines |
|---|---|
| Executable lines in the network stack | 3,454 |
| Executed because of input from the **WAN** (`wan` and `dhcp_client` targets) | **692 (20%)** |
| Executed because of input from anywhere (all six targets) | 2,452 (71%) |

- **From the WAN**, the input reaches IPv4 forwarding and NAT, ICMP, ARP, UDP demultiplexing and the DHCP client.
- **The HTTP parser, login and management API are not reachable from the WAN.** The only HTTP lines WAN input runs are the 3 of `admit()`, which drops the connection. Per file: [`measure/results/reachable.txt`](../measure/results/reachable.txt).

### Vulnerability history

| Software | Notable CVEs |
|---|---|
| **Stock TP-Link TL-WR841N firmware** (the hardware LazyDaysOS targets) | CVE-2023-39471: command injection in `ated_tp`, unauthenticated code execution as root from the adjacent network. CVE-2022-30024: httpd buffer overflow. CVE-2019-17147: buffer overflow, code execution. CVE-2022-42433 and CVE-2025-9377: OS command injection |
| **dnsmasq** (OpenWrt's DNS and DHCP) | DNSpooq, CVE-2020-25681 to -25687: heap overflows and DNS cache poisoning. OpenWrt 19.07.0 to 19.07.5 were affected, fixed in 19.07.6 |
| **uHTTPd** (OpenWrt's web server) | CVE-2019-19945: crash via a crafted POST |
| **netifd / odhcp6c** | CVE-2021-22161: routing loop |
| **Linux** (19.07.0 to .4) | ICMP rate limit side channel used for DNS poisoning (SAD DNS), OpenWrt advisory 2020-12-09 |
| **LazyDaysOS** | found by this project and fixed, each with a test that fails on the old code: WAN access to the management API through the LAN address (F2b), a remote crash from one UDP packet to port 0 (Z3), WAN-held TCP slots via port 0 (Z4), and LAN-side connection-slot lockouts (F2, Z1, Z2) |

## Observations

1. **Exposure by default.**
   - OpenWrt puts three network services on the LAN over IPv4 and IPv6, with no password on two of them, and runs a DNS server on the WAN address behind the firewall.
   - LazyDaysOS has one LAN service, behind a login that has no default password, and nothing listening on the WAN except the DHCP client.
   - On the WAN both are closed to scans. OpenWrt relies on its firewall for that, LazyDaysOS on simply not running WAN services.
2. **Less code in the path.** The code a packet from the internet can reach in LazyDaysOS is a few hundred lines that have been fuzzed with millions of inputs. In OpenWrt it is the Linux network stack plus netfilter, plus every daemon bound to the WAN address that the firewall has to shield.
3. **Weaknesses on the LazyDaysOS side that the scan shows:**
   - From the WAN, UDP 67/68 and TCP 80 look "open|filtered" / "filtered" rather than closed, which makes the router easy to fingerprint. Answering them like any closed port (ICMP unreachable, RST) would hide that.
   - The 692 WAN-reachable lines are a measured lower bound. Fuzzing longer, or a static reachability analysis, would bound it from above.
4. **Missing features the comparison must state:** LazyDaysOS has no IPv6, no Wi-Fi, no DNS forwarder and no SSH. Part of the smaller attack surface is fewer features. The thesis argument is that the remaining surface is the minimum a home router needs, and that each part of it is tested and fuzzed.

## Limitations

- x86 builds in QEMU stand in for the MIPS TL-WR841N. Service sets are the same, but sizes differ: the MIPS OpenWrt image is smaller, and the MT7628 port of LazyDaysOS isn't done (WP5).
- The stock TP-Link firmware wasn't measured, since there's no device yet. Its CVE history stands in until the hardware arrives.
- OpenWrt was measured in factory defaults. Setting a password and disabling services shrinks its surface, but the default is what most devices run.

## Sources

- OpenWrt: [4/32 warning](https://openwrt.org/supported_devices/432_warning), [advisory 2021-01-19-1 (dnsmasq)](https://openwrt.org/advisory/2021-01-19-1), [advisory 2020-01-13-1 (uhttpd, CVE-2019-19945)](https://openwrt.org/advisory/2020-01-13-1), [19.07.7 release notes (CVE-2021-22161)](https://lists.infradead.org/pipermail/openwrt-devel/2021-February/033876.html)
- [CVE-2023-39471 (NVD)](https://nvd.nist.gov/vuln/detail/CVE-2023-39471), [CVE-2022-30024](https://cvepremium.circl.lu/vuln/CVE-2022-30024), [CVE-2019-17147](https://vulmon.com/vulnerabilitydetails?qid=CVE-2019-17147), [CVE-2022-42433](https://cvefeed.io/vuln/detail/CVE-2022-42433), [CVE-2025-9377](https://www.sentinelone.com/vulnerability-database/cve-2025-9377/)
- [Tenable: DNSpooq](https://www.tenable.com/blog/dnspooq-seven-vulnerabilities-identified-in-dnsmasq), [CISA ICSA-21-019-01](https://us-cert.cisa.gov/ics/advisories/icsa-21-019-01)
