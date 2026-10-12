#!/usr/bin/env python3
"""ST3 - real/virtual audio device gate (TESTING.md, Phase 5 exit).

Launches TWO real `skywave` processes and links them through REAL audio
devices, one loopback device per direction, so each process runs its own
audio clock and its own worker thread:

    A  --out-->  BlackHole 2ch  --in-->  B
    B  --out-->  VB-Cable       --in-->  A

Everything is driven the way an application would drive it: control-port
commands over TCP, payload through the data port. Nothing in this script
reaches into modem internals.

Usage:
    tests/st3_hardware.py [--soak-sec N] [--bytes N] [--binary PATH]

Exit status 0 = gate passed. Requires the loopback devices to exist; skips
(exit 77, the ctest "not run" convention) when they do not, so the suite
stays meaningful on machines without them.
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

SKIP = 77


def list_devices(binary):
    out = subprocess.run([binary, "--list-audio"], capture_output=True,
                         text=True, timeout=60).stdout
    devs = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+(.*?)(?:\s*\[.*\])?$", line)
        if m:
            devs.append({"index": int(m.group(1)), "in": int(m.group(2)),
                         "out": int(m.group(3)), "name": m.group(4).strip()})
    return devs


def find(devs, name, need_in=0, need_out=0):
    for d in devs:
        if d["name"].startswith(name) and d["in"] >= need_in and \
                d["out"] >= need_out:
            return d["index"]
    return None


def connect_local(port, wait=30.0):
    """Connect to a modem TCP port, waiting for it to come up. A freshly
    launched modem opens its audio device before its ports, and CoreAudio
    can take several seconds under load (a fixed sleep raced it: the
    2026-10 kill/resume gate failed on "connection refused" with the
    relaunched modem still opening its device)."""
    deadline = time.time() + wait
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=10)
        except ConnectionRefusedError:
            if time.time() >= deadline:
                raise
            time.sleep(0.25)


class Ctrl:
    """Control-port client: one command, one response line."""

    def __init__(self, port, timeout=20.0):
        self.sock = connect_local(port)
        self.sock.settimeout(timeout)
        self.buf = b""

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode())
        deadline = time.time() + 20.0
        while time.time() < deadline:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                if text.startswith(("EVT:", "MON:")):
                    continue          # async notification, not our answer
                return text
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("control port closed")
            self.buf += chunk
        raise TimeoutError(f"no response to {line!r}")

    def wait_state(self, want, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.cmd("STATE") == f"STATE {want}":
                return True
            time.sleep(0.5)
        return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def launch(binary, call, in_dev, out_dev, data_port, ctrl_port, logdir,
           impair=None):
    # One numbered log per launch: st3_killresume relaunches the same
    # callsign every cycle, and reopening a fixed name with "w" erased the
    # previous run's evidence (the archived st3_K6XYZ.log was 7 lines).
    n = 1
    while os.path.exists(os.path.join(logdir, f"st3_{call}_{n:02d}.log")):
        n += 1
    log = open(os.path.join(logdir, f"st3_{call}_{n:02d}.log"), "w")
    # Devices are passed by NAME: the modem resolves them when it opens
    # the stream. Indices shift when any device comes or goes (an iPhone
    # Continuity microphone, an SDR client's virtual DAX devices), and a
    # relaunch with indices listed minutes earlier opened the wrong devices
    # (2026-10-12: st3_killresume's relaunched B came up on BlackHole 16ch
    # / AetherSDR TX, deaf and mute, and never reconnected).
    env = dict(os.environ)
    if impair:
        # SKW_TX_IMPAIR: each modem impairs its OWN transmit audio
        # (worker-level Watterson tap), so both directions of the real
        # device path carry controlled-impairment signal.
        env["SKW_TX_IMPAIR"] = impair
    proc = subprocess.Popen(
        [binary, "--callsign", call,
         "--audio-in", str(in_dev), "--audio-out", str(out_dev),
         "--data-port", str(data_port), "--ctrl-port", str(ctrl_port),
         "--voice-port", str(ctrl_port + 1), "--log-level", "1"],
        stdout=log, stderr=subprocess.STDOUT, env=env)
    return proc, log


def transfer(a_data, b_data, payload, timeout):
    """Send payload A->B over the data ports; return what arrived."""
    a_data.sendall(payload)
    got = b""
    deadline = time.time() + timeout
    b_data.settimeout(2.0)
    while len(got) < len(payload) and time.time() < deadline:
        try:
            chunk = b_data.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        got += chunk
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./skywave")
    ap.add_argument("--soak-sec", type=float,
                    default=float(os.environ.get("SKW_ST3_SOAK", "60")))
    ap.add_argument("--bytes", type=int, default=2048)
    ap.add_argument("--impair",
                    default=os.environ.get("SKW_ST3_IMPAIR", ""),
                    help="SKW_TX_IMPAIR spec applied to BOTH modems, "
                         "e.g. 'preset=good,snr=15,cfo=1.0,ppm=100'")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print(f"ST3 skip: binary not found at {binary}")
        return SKIP

    devs = list_devices(binary)
    fwd = find(devs, "BlackHole 2ch", need_in=1, need_out=1)
    rev = find(devs, "VB-Cable", need_in=1, need_out=1)
    if fwd is None or rev is None:
        print("ST3 skip: needs two duplex loopback devices "
              "(BlackHole 2ch + VB-Cable); found: " +
              ", ".join(d["name"] for d in devs))
        return SKIP
    print(f"ST3: forward device idx {fwd}, reverse device idx {rev}")
    # launch() re-resolves the names at every launch (indices shift).
    fwd, rev = "BlackHole 2ch", "VB-Cable"

    logdir = os.environ.get("SKW_ST3_LOGDIR", ".")
    # A transmits into the forward device and listens on the reverse one;
    # B is the mirror image. Two processes, two audio clocks.
    if args.impair:
        print(f"ST3: TX impairment active: {args.impair}")
    a_proc, a_log = launch(binary, "W1AW", rev, fwd, 9200, 9201, logdir,
                           args.impair)
    b_proc, b_log = launch(binary, "K6XYZ", fwd, rev, 9210, 9211, logdir,
                           args.impair)
    a_ctrl = b_ctrl = None
    a_data = b_data = None
    rc = 1
    try:
        time.sleep(3.0)  # let PortAudio open the devices
        for p, name in ((a_proc, "A"), (b_proc, "B")):
            if p.poll() is not None:
                print(f"ST3 FAIL: process {name} exited early "
                      f"(rc={p.returncode}); see st3_*.log")
                return 1
        a_ctrl = Ctrl(9201)
        b_ctrl = Ctrl(9211)
        print("ST3:", a_ctrl.cmd("VERSION"))
        print("ST3: A", a_ctrl.cmd("STATUS"))
        print("ST3: B", b_ctrl.cmd("STATUS"))
        assert b_ctrl.cmd("LISTEN ON") == "LISTEN ON"

        # --- connect over real audio ---------------------------------------
        t0 = time.time()
        assert a_ctrl.cmd("CONNECT K6XYZ") == "CONNECT K6XYZ"
        if not a_ctrl.wait_state("CONNECTED", 90):
            print("ST3 FAIL: no connection over the audio devices")
            print("   A:", a_ctrl.cmd("STATUS"))
            print("   B:", b_ctrl.cmd("STATUS"))
            return 1
        if not b_ctrl.wait_state("CONNECTED", 30):
            print("ST3 FAIL: peer never reached CONNECTED")
            return 1
        print(f"ST3: connected in {time.time() - t0:.1f} s")

        # --- payload through the data ports --------------------------------
        a_data = connect_local(9200)
        b_data = connect_local(9210)
        payload = bytes((i * 37 + 11) % 251 for i in range(args.bytes))
        got = transfer(a_data, b_data, payload, timeout=240)
        if got != payload:
            print(f"ST3 FAIL: payload mismatch ({len(got)}/{len(payload)} B)")
            return 1
        print(f"ST3: {len(payload)} B byte-exact over real devices")

        # --- soak: hold the session, keep traffic moving, count drops ------
        drops = 0
        cycles = 0
        deadline = time.time() + args.soak_sec
        while time.time() < deadline:
            state = a_ctrl.cmd("STATE")
            if state != "STATE CONNECTED":
                drops += 1
                print(f"ST3: session dropped ({state}) at "
                      f"{time.time() - t0:.0f} s - reconnecting")
                a_ctrl.cmd("CONNECT K6XYZ")
                if not a_ctrl.wait_state("CONNECTED", 90):
                    print("ST3 FAIL: could not re-establish after a drop")
                    return 1
            chunk = bytes((cycles + i) % 251 for i in range(512))
            back = transfer(a_data, b_data, chunk, timeout=180)
            if back != chunk:
                print(f"ST3 FAIL: soak payload mismatch at cycle {cycles} "
                      f"({len(back)}/{len(chunk)} B)")
                return 1
            cycles += 1
            if cycles % 5 == 0:
                print(f"ST3: {time.time() - t0:.0f}s cycles={cycles} "
                      f"drops={drops} | {a_ctrl.cmd('STATUS')}")

        print(f"ST3: soak done - {cycles} cycles, {drops} drops, "
              f"{time.time() - t0:.0f} s")
        assert a_ctrl.cmd("DISCONNECT") == "DISCONNECT"
        a_ctrl.wait_state("LISTENING", 30)
        if drops:
            print(f"ST3 FAIL: {drops} session drop(s) during the soak")
            return 1
        print("ST3 PASS")
        rc = 0
    except AssertionError as e:
        print(f"ST3 FAIL: assertion {e}")
    except (OSError, RuntimeError, TimeoutError) as e:
        print(f"ST3 FAIL: {type(e).__name__}: {e}")
    finally:
        for s in (a_data, b_data):
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        for c in (a_ctrl, b_ctrl):
            if c:
                c.close()
        for p in (a_proc, b_proc):
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        a_log.close()
        b_log.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
