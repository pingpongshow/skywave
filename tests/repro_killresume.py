#!/usr/bin/env python3
"""Focused repro for the kill/resume reconnect stall.

st3_killresume.py fails at "no reconnect after SIGKILL". This script
does the same sequence but PRINTS the control-port replies and both
stations' states throughout, so the stall can be located instead of
guessed at. Not a gate; a diagnostic.
"""
import os
import signal
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from st3_hardware import Ctrl, list_devices, find, launch  # noqa: E402


def st(ctrl, who):
    try:
        return ctrl.cmd("STATE")
    except (TimeoutError, OSError) as e:
        return f"<{who} ctrl busy: {e}>"


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                             else "build/skywave")
    devs = list_devices(binary)
    fwd = find(devs, "BlackHole 2ch", need_in=1, need_out=1)
    rev = find(devs, "VB-Cable", need_in=1, need_out=1)
    if fwd is None or rev is None:
        print("skip: loopback devices missing")
        return 77
    logdir = os.environ.get("SKW_ST3_LOGDIR", ".")

    a_proc, _ = launch(binary, "W1AW", rev, fwd, 9200, 9201, logdir)
    b_proc, _ = launch(binary, "K6XYZ", fwd, rev, 9210, 9211, logdir)
    try:
        time.sleep(3.0)
        a = Ctrl(9201)
        b = Ctrl(9211)
        print("A LISTEN:", a.cmd("LISTEN ON"))
        print("B LISTEN:", b.cmd("LISTEN ON"))
        print("A CONNECT:", a.cmd("CONNECT K6XYZ"))
        if not a.wait_state("CONNECTED", 120):
            print("FAIL: initial connect")
            return 1
        print("connected")

        a_data = socket.create_connection(("127.0.0.1", 9200), timeout=10)
        b_data = socket.create_connection(("127.0.0.1", 9210), timeout=10)
        b_data.settimeout(2.0)
        a_data.sendall(bytes((i * 131 + 7) % 251 for i in range(24 * 1024)))

        got = 0
        t0 = time.time()
        while got < 2048 and time.time() - t0 < 300:
            try:
                c = b_data.recv(65536)
                got += len(c)
            except socket.timeout:
                pass
        print(f"got {got} B in {time.time() - t0:.0f}s; killing B")

        b_proc.send_signal(signal.SIGKILL)
        b_proc.wait()
        b_data.close()
        time.sleep(1.5)
        b_proc, _ = launch(binary, "K6XYZ", fwd, rev, 9210, 9211, logdir)
        time.sleep(3.0)
        b = Ctrl(9211)
        print("B relaunched, LISTEN:", b.cmd("LISTEN ON"))
        print("B STATE:", st(b, "B"))

        # Watch A leave CONNECTED, printing every distinct state.
        seen = None
        t1 = time.time()
        while time.time() - t1 < 600:
            s = st(a, "A")
            if s != seen:
                print(f"  [{time.time() - t1:6.0f}s] A {s}")
                seen = s
            if s not in ("STATE CONNECTED", "<A ctrl busy") and \
               "CONNECTED" not in s and "busy" not in s:
                break
            time.sleep(2.0)
        print(f"A left CONNECTED after {time.time() - t1:.0f}s: {st(a, 'A')}")

        # THE POINT OF THE REPRO: what does A say to CONNECT, and what do
        # both ends do afterwards? st3_killresume never checks the reply.
        reply = a.cmd("CONNECT K6XYZ")
        print(f"A CONNECT reply: {reply!r}")

        t2 = time.time()
        sa = sb = None
        while time.time() - t2 < 240:
            na, nb = st(a, "A"), st(b, "B")
            if na != sa or nb != sb:
                print(f"  [{time.time() - t2:6.0f}s] A {na} | B {nb}")
                sa, sb = na, nb
            if "CONNECTED" in na and "DISCONNECT" not in na:
                print(f"RECONNECTED after {time.time() - t2:.0f}s")
                return 0
            time.sleep(2.0)
        print("FAIL: no reconnect in 240 s")
        return 1
    finally:
        for p in (a_proc, b_proc):
            try:
                p.terminate()
                p.wait(timeout=10)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
