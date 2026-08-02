#!/usr/bin/env python3
"""Skywave companion app (INTERFACES.md "companion split").

The modem owns waveform, sessions, timing, audio and PTT; this app is
the application layer on top of the TCP ports: chat over the DATA
port, session control + telemetry over the CONTROL port, dial
frequency reporting (FREQINFO <hz>, informational - keys the modem's
channel-memory buckets).

Usage:
  tools/companion.py [--host 127.0.0.1] [--data 9100] [--ctrl 9101]
                     [--title NAME]

Run one instance per modem, e.g. against the two bench stations:
  tools/companion.py --data 9200 --ctrl 9201 --title W1AW
  tools/companion.py --data 9210 --ctrl 9211 --title K6XYZ
"""
import argparse
import queue
import socket
import threading
import tkinter as tk
from tkinter import ttk, scrolledtext


class PortReader(threading.Thread):
    """Background socket reader feeding a queue of (kind, payload)."""

    def __init__(self, host, port, kind, q):
        super().__init__(daemon=True)
        self.host, self.port, self.kind, self.q = host, port, kind, q
        self.sock = None
        self.alive = False

    def run(self):
        while True:
            try:
                self.sock = socket.create_connection((self.host, self.port),
                                                     timeout=5)
                self.sock.settimeout(0.5)
                self.alive = True
                self.q.put((self.kind + ":open", b""))
                buf = b""
                while True:
                    try:
                        chunk = self.sock.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    if self.kind == "ctrl":
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            self.q.put(("ctrl", line))
                    else:
                        self.q.put(("data", chunk))
            except OSError:
                pass
            self.alive = False
            self.q.put((self.kind + ":closed", b""))
            threading.Event().wait(2.0)  # retry cadence

    def send(self, payload: bytes):
        try:
            if self.sock:
                self.sock.sendall(payload)
                return True
        except OSError:
            pass
        return False


class Companion(tk.Tk):
    POLL_MS = 100
    STATUS_MS = 2000

    def __init__(self, host, data_port, ctrl_port, title):
        super().__init__()
        self.title(f"Skywave companion - {title}")
        self.geometry("760x560")
        self.q = queue.Queue()
        self.data = PortReader(host, data_port, "data", self.q)
        self.ctrl = PortReader(host, ctrl_port, "ctrl", self.q)
        self.data.start()
        self.ctrl.start()
        self._build()
        self.after(self.POLL_MS, self._drain)
        self.after(self.STATUS_MS, self._poll_status)

    def _build(self):
        top = ttk.Frame(self)
        top.pack(fill="x", padx=6, pady=4)
        self.remote = tk.StringVar(value="")
        ttk.Label(top, text="Remote:").pack(side="left")
        ttk.Entry(top, textvariable=self.remote, width=12).pack(side="left")
        ttk.Button(top, text="Connect",
                   command=lambda: self._cmd("CONNECT " +
                                             self.remote.get().strip())
                   ).pack(side="left", padx=2)
        ttk.Button(top, text="Disconnect",
                   command=lambda: self._cmd("DISCONNECT")).pack(side="left")
        ttk.Button(top, text="Abort",
                   command=lambda: self._cmd("ABORT")).pack(side="left",
                                                            padx=2)
        self.dial = tk.StringVar(value="")
        ttk.Label(top, text="  Dial Hz:").pack(side="left")
        ttk.Entry(top, textvariable=self.dial, width=10).pack(side="left")
        ttk.Button(top, text="Report",
                   command=lambda: self._cmd("FREQINFO " +
                                             self.dial.get().strip())
                   ).pack(side="left", padx=2)

        self.status = tk.StringVar(value="(connecting to modem...)")
        ttk.Label(self, textvariable=self.status, anchor="w",
                  font=("Menlo", 11)).pack(fill="x", padx=6)

        panes = ttk.PanedWindow(self, orient="vertical")
        panes.pack(fill="both", expand=True, padx=6, pady=4)

        chat_frame = ttk.LabelFrame(panes, text="Chat (DATA port)")
        self.chat = scrolledtext.ScrolledText(chat_frame, height=12,
                                              state="disabled",
                                              font=("Menlo", 12))
        self.chat.pack(fill="both", expand=True)
        panes.add(chat_frame, weight=3)

        evt_frame = ttk.LabelFrame(panes, text="Events (CONTROL port)")
        self.events = scrolledtext.ScrolledText(evt_frame, height=8,
                                                state="disabled",
                                                font=("Menlo", 10))
        self.events.pack(fill="both", expand=True)
        panes.add(evt_frame, weight=2)

        entry = ttk.Frame(self)
        entry.pack(fill="x", padx=6, pady=4)
        self.msg = tk.StringVar()
        e = ttk.Entry(entry, textvariable=self.msg, font=("Menlo", 12))
        e.pack(side="left", fill="x", expand=True)
        e.bind("<Return>", lambda _ev: self._send_chat())
        ttk.Button(entry, text="Send",
                   command=self._send_chat).pack(side="left", padx=4)

    def _append(self, widget, text, tag=None):
        widget.configure(state="normal")
        widget.insert("end", text)
        widget.see("end")
        widget.configure(state="disabled")

    def _cmd(self, line):
        line = line.strip()
        if line:
            self.ctrl.send((line + "\n").encode())

    def _send_chat(self):
        text = self.msg.get()
        if not text:
            return
        if self.data.send((text + "\n").encode()):
            self._append(self.chat, f"<me> {text}\n")
            self.msg.set("")
        else:
            self._append(self.chat, "[data port not connected]\n")

    def _poll_status(self):
        self._cmd("STATUS")
        self.after(self.STATUS_MS, self._poll_status)

    def _drain(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "data":
                    self._append(self.chat,
                                 payload.decode("utf-8", "replace"))
                elif kind == "ctrl":
                    line = payload.decode("utf-8", "replace")
                    if line.startswith("STATUS "):
                        self.status.set(line[7:])
                    elif line.startswith("MON:"):
                        pass  # monitor spectra: not rendered here
                    else:
                        self._append(self.events, line + "\n")
                elif kind.endswith(":open"):
                    self._append(self.events,
                                 f"[{kind.split(':')[0]} port connected]\n")
                elif kind.endswith(":closed"):
                    self._append(self.events,
                                 f"[{kind.split(':')[0]} port lost - "
                                 "retrying]\n")
        except queue.Empty:
            pass
        self.after(self.POLL_MS, self._drain)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--data", type=int, default=9100)
    ap.add_argument("--ctrl", type=int, default=9101)
    ap.add_argument("--title", default="modem")
    a = ap.parse_args()
    Companion(a.host, a.data, a.ctrl, a.title).mainloop()


if __name__ == "__main__":
    main()
