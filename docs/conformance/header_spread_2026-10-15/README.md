# Robust header spread vs M5-M7 floors (2026-10-15)

Question: the robust header is spread x16 for every M4-M7 frame (sized for
M4, whose payload is QPSK r1/2 x16). M5/M6/M7 payloads are x8/x4/x2, so an
M7 session's header is 8x more spread than its payload - ~20 s per frame on
NARROW. Would a header matched to the session mode work?

Method: `bertool` full frames, a temporary `SKW_HDR_SPREAD` override on TX
and RX, every family, M5-M7, header x16 vs matched vs one step heavier,
CCIR Moderate / Poor (class tables) and AWGN, floor +0/+1/+3 dB, 100
trials per point, seed 4242 (paired). `header_spread.csv` has every point.

Result: no. A lighter header costs 1-3 dB on every M5-M7 floor, and not
only through header failures: the header symbols are also the channel and
noise training the payload decodes on. NARROW M7 AWGN at floor+1: x16 0%
FER; x4 35% with ZERO header failures; x2 96%. NARROW M7 Moderate at the
floor: 17% / 25% / 43%. MID M7 AWGN floor+1: 4% / 68% / 94%. The fix went
to the mode-control protocol instead (PROTOCOL.md: promotions ride the
target mode).
