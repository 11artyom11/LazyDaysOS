#!/usr/bin/env python3
"""Run shell commands on a router's serial console (QEMU -serial unix:SOCK,server,nowait).

    console.py SOCK wait PATTERN TIMEOUT      wait until PATTERN appears in the output
    console.py SOCK run 'cmd' ['cmd' ...]     press Enter, wait for the shell prompt, run each
                                              command and print '### cmd' then its output

The prompt is "root@...:...# " (OpenWrt's ash). Used by attack_surface.sh for the
router's own view: listening sockets, processes, memory, flash use.
"""
import re
import socket
import sys
import time

PROMPT = re.compile(rb"root@[^:\s]+:[^#\n]*# $")


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(0.2)
    return s


def read_until(s, pattern, timeout):
    buf, deadline = b"", time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            chunk = b""
        if chunk:
            buf += chunk
            if pattern.search(buf):
                return buf
    raise TimeoutError("no %r within %d s; last output: %r" % (pattern.pattern, timeout, buf[-300:]))


def main():
    path, mode = sys.argv[1], sys.argv[2]
    s = connect(path)
    if mode == "wait":
        read_until(s, re.compile(sys.argv[3].encode()), float(sys.argv[4]))
        return
    s.sendall(b"\n")
    read_until(s, PROMPT, 30)
    for cmd in sys.argv[3:]:
        s.sendall(cmd.encode() + b"\n")
        out = read_until(s, PROMPT, 120)
        lines = out.decode(errors="replace").replace("\r", "").split("\n")
        print("### " + cmd)
        print("\n".join(lines[1:-1]))          # drop the echoed command and the next prompt


if __name__ == "__main__":
    main()
