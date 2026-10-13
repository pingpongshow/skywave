#!/usr/bin/env python3
"""Track the TX->RX lag through one sounding frame (wide search, 0.5 s
blocks) to measure audio discontinuities in the transmit chain.

  tools/onair/jumpfind.py <name> <seq> <rx_start_s>

<name>.wav / <name>.manifest are the sounding, <name>_rx.wav the receiver
recording (24 kHz); <rx_start_s> comes from `score` (SKW_SOUNDER_DEBUG=1
or the CSV). Prints every lag step: a step of k x 5.33 ms is k DAX TX
packets (128 samples at 24 kHz) of silence the radio inserted.
"""
import sys, numpy as np, scipy.io.wavfile as wf, scipy.signal as ss
name, seq, rxStart = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
man = {int(l.split()[1]): (float(l.split()[5]), float(l.split()[6])) for l in open(f"{name}.manifest") if l.startswith("frame")}
t0, air = man[seq]; fs = 48000
_, tx = wf.read(f"{name}.wav"); tx = tx.astype(float)/32768
_, rx = wf.read(f"{name}_rx.wav"); rx = ss.resample_poly(rx.astype(float)/32768, 2, 1)
R = ss.hilbert(tx); A = ss.hilbert(rx)
base = int(rxStart*fs) - int(t0*fs)   # rx index = tx index + base (approx)
blk = int(0.5*fs); W = int(1.5*fs); prev=None
for i in range(int(t0*fs), int((t0+air)*fs)-blk, blk):
    b = R[i:i+blk]; c = i + base
    a = A[max(0,c-W):c+blk+W]
    cc = np.abs(ss.fftconvolve(a, np.conj(b[::-1]), mode='valid'))
    j = int(np.argmax(cc)); lag = j - (c - max(0,c-W))
    corr = cc[j]/(np.linalg.norm(b)*np.linalg.norm(a[j:j+blk])+1e-12)
    if prev is None or abs(lag-prev) > 2 or corr < 0.9:
        print(f"  t+{(i-int(t0*fs))/fs:5.1f}s lag {lag:+7d} samples ({lag/48:+7.1f} ms) corr {corr:.2f}")
    prev = lag
