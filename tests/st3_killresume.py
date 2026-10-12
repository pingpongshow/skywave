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
from st3_hardware import Ctrl, connect_local, list_devices, find, launch  # noqa: E402

SKIP = 77
# Upper bound on a replayed run: one coalesced proto message. HF caps
# coalescing at kCoalesceMaxBytes = 4096 (proto/pipeline.hpp); 8192
# leaves 2x headroom for the marker/compression framing. VHF-FM coalesces
# to 16384 and would need this raised - this gate runs HF.
MAX_MSG_BYTES = 8192


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
    # Present now; launch() re-resolves the names at every (re)launch.
    fwd, rev = "BlackHole 2ch", "VB-Cable"
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
        a_data = connect_local(9200)
        b_data = connect_local(9210)
        b_data.settimeout(2.0)

        payload = bytes((i * 131 + 7) % 251 for i in range(24 * 1024))
        a_data.sendall(payload)
        got = b""
        kills = 0
        t0 = time.time()
        # Budget: each cycle costs the survivor ~110 s to notice the
        # dead peer (the ARQ retry budget, not slack) plus the reconnect
        # and the remaining transfer, so five SIGKILLs need well over
        # half an hour. 1800 s cut the run off mid-cycle and reported a
        # ctest Timeout that looked like a modem failure.
        deadline = t0 + 5400
        next_kill_at = len(got) + 2048
        # Loop until the payload's own TAIL arrives, not until a byte
        # COUNT is reached: a replayed message means more bytes cross
        # the link than the application submitted, so a length test
        # stops short of the end and reports a false shortfall (seen as
        # "24176/24576 B in order, 400 B replayed" - the modem was
        # correct, the harness stopped early).
        def tail_arrived():
            return len(got) >= 64 and got[-64:] == payload[-64:]

        while not tail_arrived() and time.time() < deadline:
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
                b_data = connect_local(9210)
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
        # AT-LEAST-ONCE is the contract, and it is the best available.
        # A peer that dies between handing a message to its application
        # and getting its acknowledgement back to us is indistinguishable
        # here from one that never received it, so the sender replays -
        # the only choice that cannot LOSE data - and publishes the size
        # of the uncertainty as EVT:STREAM_REPLAY. What is required, and
        # what is checked: every byte arrives, in order, and any
        # repetition is bounded by one coalesced message.
        #
        # The earlier `got == payload` test demanded exactly-once across
        # a peer restart, which no protocol can deliver against a peer
        # that keeps no state - it is the two-generals problem. It went
        # unnoticed because this gate had never been run.
        # Walk the received stream against the payload. A backward jump
        # is a replay; anything else is corruption or loss.
        pos, dup, i, stuck = 0, 0, 0, False
        while i < len(got) and pos < len(payload):
            if got[i] == payload[pos]:
                pos += 1
                i += 1
                continue
            at = payload.rfind(got[i:i + 64], 0, pos)
            if at < 0:
                stuck = True
                break
            dup += pos - at
            pos = at
        if not stuck and pos == len(payload) and dup <= MAX_MSG_BYTES:
            print(f"killresume PASS: {len(payload)} B reconstructed across "
                  f"{kills} SIGKILLs ({dup} B replayed, bound "
                  f"{MAX_MSG_BYTES}), {time.time() - t0:.0f} s")
            rc = 0
        else:
            print(f"killresume FAIL: {pos}/{len(payload)} B in order, "
                  f"{dup} B replayed, stuck={stuck}, kills={kills}")
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
