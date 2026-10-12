#!/usr/bin/env python3
"""Record audio from a Web-888 / KiwiSDR to WAV (docs/ONAIR_TEST_PLAN.md).

Receive-only, standard library only (no kiwiclient install needed). Opens
the receiver's SND websocket, tunes USB at the given dial frequency with
AGC OFF (fixed manual gain) and audio compression OFF, and writes mono
16-bit WAV. The receiver's true sample rate (KiwiSDRs run near, not at,
their nominal rate) goes into a sidecar `<out>.rate` file and is printed:
pass it to `onair_sounder score --rate`.

Usage:
  tools/onair/kiwirec.py --host 192.0.2.210 --freq 14105.0 --seconds 300 \\
      --out rec.wav [--low 200 --high 2900] [--gain 50]
"""
import argparse
import base64
import os
import re
import socket
import struct
import sys
import time
import wave


class WebSocket:
    """Minimal RFC 6455 client: enough for the Kiwi's SND stream."""

    def __init__(self, host, port, path, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during websocket handshake")
            resp += chunk
        head, _, self.buf = resp.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError("websocket refused: " +
                                  head.split(b"\r\n")[0].decode(errors="replace"))

    def send_text(self, text):
        data = text.encode()
        mask = os.urandom(4)
        hdr = bytearray([0x81])
        n = len(data)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes(hdr) + mask + masked)

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("receiver closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        """Returns (opcode, payload); reassembles fragments."""
        payload = b""
        opcode = None
        while True:
            b0, b1 = self._read(2)
            op = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if op == 0x9:  # ping -> pong
                self._control(0xA, data)
                continue
            if op == 0x8:
                raise ConnectionError("receiver closed the websocket")
            if op != 0:
                opcode = op
            payload += data
            if b0 & 0x80:
                return opcode, payload

    def _control(self, op, data):
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes([0x80 | op, 0x80 | len(data)]) + mask + masked)

    def close(self):
        try:
            self._control(0x8, b"")
        except OSError:
            pass
        self.sock.close()


def parse_msg(text):
    return dict(re.findall(r"(\w+)=(\S*)", text))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=8073)
    ap.add_argument("--freq", type=float, required=True,
                    help="USB dial frequency, kHz (same as the transmitter's)")
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--low", type=int, default=200, help="passband low, Hz")
    ap.add_argument("--high", type=int, default=2900, help="passband high, Hz")
    ap.add_argument("--gain", type=int, default=50,
                    help="manual gain with AGC off (0-120)")
    ap.add_argument("--password", default="")
    ap.add_argument("--user", default="skywave-onair")
    args = ap.parse_args()

    ws = WebSocket(args.host, args.port, f"/{int(time.time())}/SND")
    ws.send_text(f"SET auth t=kiwi p={args.password}")
    rate = None
    true_rate = None
    configured = False
    wav = None
    frames = 0
    start = None
    last_keepalive = 0.0
    smeter = []
    try:
        while True:
            op, data = ws.recv()
            tag, body = data[:3], data[3:]
            if tag == b"MSG":
                msg = parse_msg(body.decode(errors="replace"))
                if msg.get("badp") == "1":
                    sys.exit("kiwirec: receiver wants a password (--password)")
                if "too_busy" in msg and msg["too_busy"] not in ("", "0"):
                    sys.exit("kiwirec: receiver has no free channel")
                if "audio_rate" in msg:
                    rate = int(float(msg["audio_rate"]))
                if "sample_rate" in msg:
                    true_rate = float(msg["sample_rate"])
                if rate and not configured:
                    configured = True
                    for cmd in (
                            f"SET AR OK in={rate} out=48000",
                            f"SET ident_user={args.user}",
                            f"SET mod=usb low_cut={args.low} high_cut={args.high} "
                            f"freq={args.freq:.3f}",
                            f"SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 "
                            f"manGain={args.gain}",
                            "SET compression=0",
                            "SET squelch=0 max=0",
                    ):
                        ws.send_text(cmd)
                    wav = wave.open(args.out, "wb")
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(rate)
                    print(f"kiwirec: {args.host} USB {args.freq:.3f} kHz, "
                          f"passband {args.low}-{args.high} Hz, AGC off gain "
                          f"{args.gain}, audio {rate} Hz", flush=True)
            elif tag == b"SND" and wav is not None:
                # flags(1) seq(4, LE) smeter(2, BE) then PCM16 BE (uncompressed)
                flags = body[0]
                if flags & 0x10:  # compressed despite the request
                    sys.exit("kiwirec: receiver sent compressed audio")
                sm = struct.unpack(">H", body[5:7])[0]
                smeter.append(0.1 * sm - 127.0)
                pcm = body[7:]
                if len(pcm) % 2:
                    pcm = pcm[:-1]
                samples = struct.unpack(f">{len(pcm) // 2}h", pcm)
                wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))
                frames += len(samples)
                if start is None:
                    start = time.time()
                if frames >= args.seconds * rate:
                    break
            now = time.time()
            if now - last_keepalive > 5.0:
                ws.send_text("SET keepalive")
                last_keepalive = now
    except KeyboardInterrupt:
        pass
    finally:
        ws.close()
        if wav is not None:
            wav.close()
    if wav is None:
        sys.exit("kiwirec: no audio received")
    elapsed = time.time() - (start or time.time())
    sr = true_rate or float(rate)
    with open(os.path.splitext(args.out)[0] + ".rate", "w") as f:
        f.write(f"{sr:.4f}\n")
    sm = sorted(smeter)
    print(f"kiwirec: wrote {args.out}: {frames} samples ({frames / rate:.1f} s "
          f"in {elapsed:.1f} s wall), true sample rate {sr:.4f} Hz -> "
          f"score with --rate {sr:.4f}; S-meter median "
          f"{sm[len(sm) // 2] if sm else float('nan'):.1f} dBm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
