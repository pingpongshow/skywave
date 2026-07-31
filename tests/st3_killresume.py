#!/usr/bin/env python3
"""ST3 kill-9 / resume hammer (stress campaign #52).

Two real skywave processes over the virtual-audio pair. Repeatedly:
start a long transfer A->B, SIGKILL B mid-flight, relaunch B, and
require the session to re-establish and the byte stream to continue
EXACTLY where it left off (session resume, no duplicated or lost app
bytes). Needs SKW_HARDWARE_TESTS=1 and the same devices as
st3_hardware.py.

Usage: st3_killresume.py [--binary PATH] [--cycles N]
"""
import argparse
import os
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from st3_hardware import Ctrl, list_devices, find, launch  # noqa: E402

SKIP = 77


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./skywave")
    ap.add_argument("--cycles", type=int, default=5)
    args = ap.parse_args()
    if os.environ.get("SKW_HARDWARE_TESTS") != "1":
        print("killresume skip: SKW_HARDWARE_TESTS != 1")
        return SKIP
    binary = os.path.abspath(args.binary)
    devs = list_devices(binary)
    fwd = find(devs, "BlackHole 2ch", need_in=1, need_out=1)
    rev = find(devs, "VB-Cable", need_in=1, need_out=1)
    if fwd is None or rev is None:
        print("killresume skip: loopback devices missing")
        return SKIP
    logdir = os.environ.get("SKW_ST3_LOGDIR", ".")

    a_proc, a_log = launch(binary, "W1AW", rev, fwd, 9200, 9201, logdir)
    b_proc, b_log = launch(binary, "K6XYZ", fwd, rev, 9210, 9211, logdir)
    rc = 1
    try:
        time.sleep(3.0)
        a_ctrl = Ctrl(9201)
        assert a_ctrl.cmd("LISTEN ON") == "LISTEN ON"
        b_ctrl = Ctrl(9211)
        assert b_ctrl.cmd("LISTEN ON") == "LISTEN ON"
        assert a_ctrl.cmd("CONNECT K6XYZ") == "CONNECT K6XYZ"
        if not a_ctrl.wait_state("CONNECTED", 120):
            print("killresume FAIL: initial connect")
            return 1
        a_data = socket.create_connection(("127.0.0.1", 9200), timeout=10)
        b_data = socket.create_connection(("127.0.0.1", 9210), timeout=10)
        b_data.settimeout(2.0)

        payload = bytes((i * 131 + 7) % 251 for i in range(24 * 1024))
        a_data.sendall(payload)
        got = b""
        kills = 0
        t0 = time.time()
        deadline = t0 + 1800
        next_kill_at = len(got) + 2048
        while len(got) < len(payload) and time.time() < deadline:
            try:
                chunk = b_data.recv(65536)
                if chunk:
                    got += chunk
            except socket.timeout:
                pass
            if kills < args.cycles and len(got) >= next_kill_at:
                kills += 1
                print(f"killresume: SIGKILL B at {len(got)} B "
                      f"(kill {kills}/{args.cycles}, "
                      f"{time.time() - t0:.0f}s)")
                b_proc.send_signal(signal.SIGKILL)
                b_proc.wait()
                b_data.close()
                time.sleep(1.5)
                b_proc, b_log = launch(binary, "K6XYZ", fwd, rev, 9210,
                                       9211, logdir)
                time.sleep(3.0)
                b_ctrl = Ctrl(9211)
                b_ctrl.cmd("LISTEN ON")
                b_data = socket.create_connection(("127.0.0.1", 9210),
                                                  timeout=10)
                b_data.settimeout(2.0)
                # Resume happens ACROSS A RECONNECT. Do NOT abort: an
                # operator ABORT discards the resumable context and the
                # queued stream with it. Let A's ARQ declare the peer
                # dead (failure-mode teardown KEEPS the resume context),
                # then call again - exactly as an application would.
                def a_state():
                    try:
                        return a_ctrl.cmd("STATE")
                    except (TimeoutError, OSError):
                        return "STATE CONNECTED"  # busy: poll again
                waited = time.time()
                while (a_state() == "STATE CONNECTED"
                       and time.time() - waited < 600):
                    time.sleep(2.0)
                if a_state() == "STATE CONNECTED":
                    print("killresume FAIL: A never noticed the dead peer (600 s)")
                    return 1
                print(f"killresume: A dropped at {time.time() - t0:.0f}s; "
                      "reconnecting")
                a_ctrl.cmd("CONNECT K6XYZ")
                if not a_ctrl.wait_state("CONNECTED", 240):
                    print("killresume FAIL: no reconnect after SIGKILL")
                    return 1
                print(f"killresume: reconnected at {time.time() - t0:.0f}s")
                next_kill_at = len(got) + 3072
        if got == payload:
            print(f"killresume PASS: {len(payload)} B byte-exact across "
                  f"{kills} SIGKILLs, {time.time() - t0:.0f} s")
            rc = 0
        else:
            ok = got == payload[:len(got)]
            print(f"killresume FAIL: {len(got)}/{len(payload)} B, "
                  f"prefix_exact={ok}, kills={kills}")
    finally:
        for p in (a_proc, b_proc):
            try:
                p.terminate()
                p.wait(timeout=5)
            except Exception:
                p.kill()
    return rc


if __name__ == "__main__":
    sys.exit(main())
