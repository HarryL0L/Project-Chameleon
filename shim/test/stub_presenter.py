#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stand-in for the Chameleon app in desktop tests: listens where the shim
expects the app, reports window sizes (CONFIG), and answers every PRESENT the
way the app does (RELEASE of the previous buffer, then FRAME_DONE).

usage: stub_presenter.py <socket> <WxH@Hz> [<seconds> <WxH@Hz>]...
  e.g. stub_presenter.py /tmp/presenter 1280x720@60 5 720x1280@60 10 1280x720@90
Prints one line per message kind and a summary on exit."""
import os
import socket
import struct
import sys
import time
from collections import Counter

MSG = struct.Struct("=IIQQ")
HELLO, BUFFER_ADD, BUFFER_REMOVE, PRESENT = 1, 2, 3, 4
CONFIG, RELEASE, FRAME_DONE = 100, 101, 102


def parse(mode):
    size, _, hz = mode.partition("@")
    w, h = (int(v) for v in size.split("x"))
    return w, h, int(float(hz or 60) * 1000)


def main():
    path, first = sys.argv[1], parse(sys.argv[2])
    plan = [(float(sys.argv[i]), parse(sys.argv[i + 1])) for i in range(3, len(sys.argv) - 1, 2)]
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    srv.bind(path)
    srv.listen(1)
    conn, _ = srv.accept()
    start = time.monotonic()

    def config(w, h, mhz):
        conn.send(MSG.pack(CONFIG, 0, w | h << 32, mhz))
        print(f"presenter: CONFIG {w}x{h}@{mhz / 1000:g}", flush=True)

    config(*first)
    counts, shown, expect_ahb = Counter(), None, False
    conn.settimeout(0.05)
    while True:
        if plan and time.monotonic() - start >= plan[0][0]:
            config(*plan.pop(0)[1])
        try:
            data, anc, _, _ = conn.recvmsg(64, socket.CMSG_SPACE(4 * 4))
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        for level, kind, fds in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                for i in range(0, len(fds) - len(fds) % 4, 4):
                    os.close(int.from_bytes(fds[i:i + 4], sys.byteorder))
        if expect_ahb:  # the AHardwareBuffer packet after BUFFER_ADD
            expect_ahb = False
            continue
        if len(data) != MSG.size:
            continue
        kind, id_, a, b = MSG.unpack(data)
        counts[kind] += 1
        if kind == BUFFER_ADD:
            expect_ahb = True
        elif kind == PRESENT:
            if shown is not None and shown != id_:
                conn.send(MSG.pack(RELEASE, shown, 0, 0))
            shown = id_
            conn.send(MSG.pack(FRAME_DONE, 0, a, time.monotonic_ns()))
    names = {HELLO: "HELLO", BUFFER_ADD: "BUFFER_ADD", BUFFER_REMOVE: "BUFFER_REMOVE", PRESENT: "PRESENT"}
    print("presenter: " + ", ".join(f"{names.get(k, k)} x{v}" for k, v in sorted(counts.items())), flush=True)


main()
