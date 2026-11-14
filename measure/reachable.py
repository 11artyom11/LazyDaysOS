#!/usr/bin/env python3
"""Which lines of the network stack do outside inputs actually execute?

Uses the fuzz harness's gcov build (fuzz/, V=cov): runs a target over a corpus in
forked children, then reads per-line execution from gcov. For each target a run with an
empty input (boot plus 20 idle minutes of timers, same configuration) is the
baseline; lines executed beyond it run only because of the input. This is a lower bound on reachable code (what the corpus
reaches), but every counted line is demonstrably reachable.

    python3 measure/reachable.py        (from the repository root; builds fuzz V=cov)
Prints, per source file and in total: executable lines, lines reached from the WAN
(wan and dhcp_client targets) and lines reached from anywhere (all targets).
"""
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ = os.path.join(ROOT, "fuzz")
COV = os.path.join(FUZZ, "build", "cov")
SOURCES = sorted(glob.glob(os.path.join(ROOT, "kernel/net/*.c"))) + [
    os.path.join(ROOT, p) for p in ("kernel/random.c", "kernel/cmdline.c", "libk/util/strbuf.c")]
TARGETS = ["net", "wan", "http", "tcp", "dhcp_server", "dhcp_client", "rip"]


def reset():
    for f in glob.glob(os.path.join(COV, "stack", "*.gcda")):
        os.remove(f)


def run(target, inputs):
    subprocess.run([os.path.join(COV, "fuzz_" + target), "--fork", inputs], cwd=FUZZ, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def executed():
    """{source: (set of executed lines, number of executable lines)}"""
    out = {}
    for src in SOURCES:
        obj = os.path.join(COV, "stack", os.path.splitext(os.path.basename(src))[0] + ".raw.o")
        text = subprocess.run(["gcov", "-t", "-o", obj, src], cwd=FUZZ, capture_output=True, text=True).stdout
        hit, total = set(), 0
        for line in text.splitlines():
            parts = line.split(":", 2)
            if len(parts) < 3:
                continue
            count, number = parts[0].strip(), parts[1].strip()
            if count == "-" or not number.isdigit():
                continue
            total += 1
            if count not in ("#####", "====="):
                hit.add(int(number))
        out[src] = (hit, total)
    return out


WAN_TARGETS = ["wan", "dhcp_client"]          # inputs that arrive on the WAN port (the DHCP client
                                              # serves the WAN in the home configuration)


def attributable(target):
    """Lines the target's corpus executes beyond an empty input with the same configuration"""
    with tempfile.TemporaryDirectory() as empty:
        open(os.path.join(empty, "empty"), "wb").close()
        reset()
        run(target, empty)
        base = executed()
    reset()
    run(target, os.path.join(FUZZ, "corpus", target))
    full = executed()
    return {src: full[src][0] - base[src][0] for src in SOURCES}, {src: full[src][1] for src in SOURCES}


def main():
    subprocess.run(["make", "-s", "V=cov", "bins"], cwd=FUZZ, check=True, stdout=subprocess.DEVNULL)
    per_target, totals_by_file = {}, {}
    for t in TARGETS:
        per_target[t], totals_by_file = attributable(t)

    rows, totals = [], [0, 0, 0]
    for src in SOURCES:
        wan = set().union(*(per_target[t][src] for t in WAN_TARGETS))
        anywhere = set().union(*(per_target[t][src] for t in TARGETS))
        total = totals_by_file[src]
        rows.append((os.path.relpath(src, ROOT), total, len(wan), len(anywhere)))
        totals = [totals[0] + total, totals[1] + len(wan), totals[2] + len(anywhere)]
    print(f"{'file':28} {'executable':>10} {'from WAN':>9} {'from anywhere':>14}   (lines run only because of input)")
    for name, total, w, a in sorted(rows, key=lambda r: (-r[2], -r[3])):
        print(f"{name:28} {total:10} {w:9} {a:14}")
    print(f"{'total':28} {totals[0]:10} {totals[1]:9} {totals[2]:14}")
    print(f"WAN = targets {', '.join(WAN_TARGETS)}; anywhere = all {len(TARGETS)} targets; "
          "each minus an empty input with the same configuration")


if __name__ == "__main__":
    sys.exit(main())
