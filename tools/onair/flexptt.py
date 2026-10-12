#!/usr/bin/env python3
"""Skywave TCP PTT server for FlexRadio 6000/8000 (docs/ONAIR_TEST_PLAN.md).

Speaks the modem's PTT protocol on the client side (INTERFACES.md,
`PTTMETHOD TCP`: one `KEY\\n` / `UNKEY\\n` per line) and keys the radio
through the SmartSDR network API (TCP 4992, `C<seq>|xmit 1`). It runs
alongside SmartSDR for Mac: the API accepts several clients, and this one
only ever sends `xmit`. TX audio still comes from SmartSDR's DAX TX
device - select DAX as the transmit source in SmartSDR first.

Clients: the modem (`PTTMETHOD TCP`, `PTTPORT 127.0.0.1:4532`) and
`onair_sounder play --ptt 127.0.0.1:4532`. The radio stays keyed while
ANY client holds a key; a client that disconnects releases its key.

Safety: a watchdog unkeys the radio after --max-key seconds of continuous
transmission, and every exit path (Ctrl-C, SIGTERM, a lost client) sends
`xmit 0`.

Usage:
  tools/onair/flexptt.py --discover
  tools/onair/flexptt.py --radio 192.0.2.10
  tools/onair/flexptt.py --kenwood 127.0.0.1:4533   # generic Kenwood TCP CAT
  tools/onair/flexptt.py --dry-run                  # log only, key nothing
"""
import argparse
import asyncio
import re
import signal
import socket
import sys
import time

FLEX_API_PORT = 4992


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def discover(timeout):
    """Listen for FlexRadio VITA-49 discovery broadcasts on UDP 4992."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", FLEX_API_PORT))
    s.settimeout(0.5)
    found = {}
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        text = data.decode("ascii", "replace")
        fields = dict(re.findall(r"(\w+)=(\S+)", text))
        if "ip" in fields:
            found[fields["ip"]] = fields
    s.close()
    return found


class FlexBackend:
    """SmartSDR API client: connect, read the V/H banner, send xmit."""

    name = "flex"

    def __init__(self, host):
        self.host = host
        self.reader = self.writer = None
        self.seq = 0

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, FLEX_API_PORT), 5)
        banner = []
        # The radio greets with "V<version>" then "H<handle>".
        while len(banner) < 2:
            line = await asyncio.wait_for(self.reader.readline(), 5)
            if not line:
                raise ConnectionError("radio closed the API connection")
            line = line.decode("ascii", "replace").strip()
            if line[:1] in ("V", "H"):
                banner.append(line)
        log(f"flex: connected to {self.host} ({' '.join(banner)})")
        asyncio.ensure_future(self._drain())

    async def _drain(self):
        # Replies to our commands; status messages are not subscribed.
        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break
                line = line.decode("ascii", "replace").strip()
                m = re.match(r"R(\d+)\|([0-9A-Fa-f]+)\|?(.*)", line)
                if m and int(m.group(2), 16) != 0:
                    log(f"flex: command {m.group(1)} FAILED "
                        f"(error 0x{m.group(2)}) {m.group(3)}")
        except (ConnectionError, OSError):
            pass
        log("flex: API connection closed")
        self.writer = None

    async def set_tx(self, on):
        if self.writer is None:
            await self.connect()
        self.seq += 1
        self.writer.write(f"C{self.seq}|xmit {1 if on else 0}\n".encode())
        await self.writer.drain()

    def connected(self):
        return self.writer is not None


class KenwoodBackend:
    """Generic Kenwood-style TCP CAT (`TX;` / `RX;`)."""

    name = "kenwood"

    def __init__(self, target):
        host, _, port = target.rpartition(":")
        self.host, self.port = host or "127.0.0.1", int(port)
        self.writer = None

    async def connect(self):
        _, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), 5)
        log(f"kenwood: connected to {self.host}:{self.port}")

    async def set_tx(self, on):
        if self.writer is None:
            await self.connect()
        self.writer.write(b"TX;" if on else b"RX;")
        await self.writer.drain()

    def connected(self):
        return self.writer is not None


class DryBackend:
    name = "dry-run"

    async def connect(self):
        log("dry-run: no radio, keying is logged only")

    async def set_tx(self, on):
        pass

    def connected(self):
        return True


class Bridge:
    def __init__(self, backend, max_key):
        self.backend = backend
        self.max_key = max_key
        self.holders = set()  # clients currently holding a key
        self.keyed = False
        self.keyed_at = 0.0
        self.lock = asyncio.Lock()
        self.tripped = False

    async def update(self):
        want = bool(self.holders) and not self.tripped
        if want == self.keyed:
            return
        async with self.lock:
            try:
                await self.backend.set_tx(want)
            except (OSError, asyncio.TimeoutError, ConnectionError) as e:
                log(f"{self.backend.name}: {'KEY' if want else 'UNKEY'} "
                    f"failed: {e}")
                self.backend.writer = None
                if want:
                    return
            self.keyed = want
            if want:
                self.keyed_at = time.monotonic()
                log(f"TX  ({len(self.holders)} holder(s))")
            else:
                log(f"RX  (keyed {time.monotonic() - self.keyed_at:.1f} s)")

    async def watchdog(self):
        while True:
            await asyncio.sleep(0.25)
            if self.keyed and time.monotonic() - self.keyed_at > self.max_key:
                log(f"WATCHDOG: keyed > {self.max_key:.0f} s, forcing RX; "
                    "clients must UNKEY before keying again")
                self.tripped = True
                await self.update()
            if self.tripped and not self.holders:
                self.tripped = False

    async def client(self, reader, writer):
        peer = writer.get_extra_info("peername")
        tag = f"{peer[0]}:{peer[1]}" if peer else "client"
        log(f"client {tag} connected")
        cid = object()
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                cmd = line.decode("ascii", "replace").strip().upper()
                if cmd == "KEY":
                    self.holders.add(cid)
                elif cmd == "UNKEY":
                    self.holders.discard(cid)
                elif cmd:
                    log(f"client {tag}: ignoring '{cmd}'")
                    continue
                await self.update()
        except (ConnectionError, OSError):
            pass
        finally:
            self.holders.discard(cid)
            await self.update()
            log(f"client {tag} disconnected")
            writer.close()

    async def shutdown(self):
        self.holders.clear()
        self.tripped = True
        if self.keyed:
            await self.update()
        elif self.backend.connected():
            # Belt and braces: never leave the radio in TX on exit.
            try:
                await self.backend.set_tx(False)
            except Exception:
                pass


async def main_async(args):
    if args.dry_run:
        backend = DryBackend()
    elif args.kenwood:
        backend = KenwoodBackend(args.kenwood)
    else:
        backend = FlexBackend(args.radio)
    await backend.connect()
    bridge = Bridge(backend, args.max_key)
    server = await asyncio.start_server(bridge.client, args.listen, args.port)
    log(f"PTT server on {args.listen}:{args.port} "
        f"(watchdog {args.max_key:.0f} s); Ctrl-C to stop")
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    wd = asyncio.ensure_future(bridge.watchdog())
    await stop.wait()
    wd.cancel()
    server.close()
    await bridge.shutdown()
    log("stopped (radio unkeyed)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio", help="FlexRadio IP address (SmartSDR API)")
    ap.add_argument("--kenwood", metavar="HOST:PORT",
                    help="key through a Kenwood-style TCP CAT port instead")
    ap.add_argument("--dry-run", action="store_true",
                    help="log KEY/UNKEY without touching a radio")
    ap.add_argument("--discover", action="store_true",
                    help="list FlexRadios broadcasting on the LAN and exit")
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4532,
                    help="PTT server port (modem default 4532)")
    ap.add_argument("--max-key", type=float, default=45.0,
                    help="watchdog: longest continuous TX, seconds")
    args = ap.parse_args()
    if args.discover:
        radios = discover(3.0)
        if not radios:
            print("no FlexRadio discovery broadcasts heard in 3 s")
            return 1
        for ip, f in radios.items():
            print(f"{ip}  model={f.get('model', '?')} "
                  f"nickname={f.get('nickname', '?')} "
                  f"serial={f.get('serial', '?')} "
                  f"version={f.get('version', '?')}")
        return 0
    if not (args.radio or args.kenwood or args.dry_run):
        ap.error("one of --radio, --kenwood, --dry-run is required "
                 "(find the radio with --discover)")
    try:
        asyncio.run(main_async(args))
    except (OSError, asyncio.TimeoutError, ConnectionError) as e:
        print(f"flexptt: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
