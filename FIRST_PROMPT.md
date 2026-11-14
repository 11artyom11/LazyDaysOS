# First prompt for a new assistant

Paste everything below the line as your first message to the AI assistant taking over LazyDaysOS. It assumes the assistant can read files and run shell commands in the repository (for example Claude Code started in `~/LazyDaysOS` inside WSL). If it can't, attach `docs/HANDOVER.md`, `CLAUDE.md` and `docs/thesis-plan.md` instead.

---

You're taking over **LazyDaysOS**, my degree-thesis project. It's a 32-bit x86 router kernel in C and GNU `as`, booted by GRUB. It has its own network stack:

- ARP, IPv4 forwarding, ICMP and UDP;
- a DHCP client and server, NAT with a firewall;
- a TCP server and an HTTP management API with login;
- RIPv2.

All of it runs on e1000 NICs in QEMU, and is tested against real Linux tools and in GNS3 labs of up to 84 routers. The thesis is *"a minimal, hardened router OS for abandoned 4/32 hardware"*. The end goal is porting it to a TP-Link TL-WR841N v14 (MediaTek MT7628, MIPS, 4 MB flash, 32 MB RAM).

The repository is `~/LazyDaysOS` in WSL2 (Ubuntu 26.04) on my Windows PC. A previous assistant worked on it with me for a long session and left a handover.

**Before doing anything else, read these, in this order:**

1. `docs/HANDOVER.md`: the state of the project, how the machine is set up (every third-party dependency), the GNS3 labs, the gotchas that already cost time, open items, and how I like to work.
2. `CLAUDE.md`: the technical reference (code map, boot flow, kernel command line, tests, fuzzing, labs, low-level traps). Follow its conventions exactly.
3. `docs/thesis-plan.md`: the thesis framing, threat model, audit findings, work packages and results so far. `docs/attack-surface.md` is the comparison with OpenWrt.

**Then check that the environment still works,** without changing anything:

- `export PATH="$HOME/opt/cross/bin:$PATH"; cd ~/LazyDaysOS && git status && git log --oneline -5`
- `make && make check`. It takes a few minutes; exit 0 means everything passed.
- `lsmod | grep -E 'ifb|netem|macvlan'`, and whether the GNS3 server answers on `http://127.0.0.1:3080/v2/version`. If it's down, see HANDOVER §3.4 for how to start it.

If something is missing or failing, compare against the setup in HANDOVER §3 before suspecting the kernel.

**Then report back briefly:**

- whether the environment is healthy;
- your understanding of where the project stands;
- which open item from HANDOVER §6 you'd do next, and why.

Wait for my decision before starting larger work.

**Rules that always apply:**

- **Never push** to GitHub; I do that myself.
  - Work on a feature branch.
  - Commit with clear messages.
  - Merge into `master` only when I ask.
- I don't have passwordless sudo. If something needs root (apt, modprobe, setcap, virsh), give me the exact command to run.
- Run `make check` after every kernel change, and keep the build warning-free.
- Every fix gets a test that fails on the old code, including a negative case. Network features get a `check_net.sh` test with real Linux tools.
- Everything above `kernel/net`'s `netdev` layer must stay free of x86 and e1000 specifics: it has to port to MIPS unchanged.
- My GNS3 labs and dashboards may be in live use. Don't run tests against them or restart their dashboards without asking. Use the "(dev)" lab copy for development (HANDOVER §5).
- Measurements are thesis material:
  - keep them reproducible (commit, build flags, kernel hash);
  - save the raw results under `gns3/results/` or `measure/results/`;
  - write findings into `docs/thesis-plan.md`.
- For visual changes to the dashboards, look at screenshots before telling me they're done (`tools/labshot.ps1`, HANDOVER §3.6). I want designs that are clean and informative, not "AI-looking".
- Tell me plainly when something failed, was skipped, or was your own mistake.
- Keep `docs/HANDOVER.md` and `CLAUDE.md` current as things change, so the next handover is as easy as this one.
