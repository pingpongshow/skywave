# Skywave Security Design

## Posture and Legal Note

Content encryption over amateur radio is prohibited in most jurisdictions.
Skywave therefore ships crypto as a **build-time option** (`ENABLE_CRYPTO`,
default `OFF`), disabled by default at runtime even when compiled in, and
intended for services where it is legal (MARS/SHARES-type services,
commercial/government HF, unlicensed experimentation over audio links).
Non-crypto builds compile out the subsystem entirely — no dead command
surface, no GUI controls.

## Model

- **PSK-rooted identity + ephemeral session keys.** Both stations hold a
  shared pre-provisioned secret (PSK). Each session performs an ephemeral
  X25519 exchange authenticated by the PSK, yielding forward secrecy: a
  captured PSK alone cannot decrypt recorded past sessions without the
  ephemeral private keys.
- AEAD: AES-256-GCM per link frame.
- Directional keys: independent TX/RX subkeys per direction.
- Epochs: in-session rekeying without teardown.

## Handshake Integration

Carried inside `START` / `START_ACK` ([PROTOCOL.md](PROTOCOL.md)) when both
peers advertise the encryption capability:

1. `START` carries initiator ephemeral public key `E_i` (32 B) and
   `auth_i = HMAC-SHA256(PSK, transcript_i)` (truncated 16 B)
2. `START_ACK` carries `E_r` and `auth_r` over the full transcript
3. both derive `ss = X25519(e, E_peer)`, then
   `K_master = HKDF-SHA256(ikm = ss, salt = PSK, info = "skw-v1" || transcript_hash)`
4. directional traffic keys:
   `K_i2r = HKDF(K_master, "i2r" || epoch)`, `K_r2i = HKDF(K_master, "r2i" || epoch)`

Transcript = both callsigns, family, capability bitmaps, both ephemeral
publics. Auth failure → `START_NAK(auth)`; the link never silently falls
back to cleartext when encryption was requested.

`AUTH` state (reported by `SECURITY`) is true only when the HMAC exchange
verified — it is identity assurance, distinct from "encryption on".

## Frame Protection

Applied in the payload pipeline after compression, before segmentation:

- algorithm: AES-256-GCM
- **implicit nonces**: nonce = `nonce_base_dir XOR (stream:u8 || epoch:u16
  || seq:u48)` where `stream` is `0` for the ARQ/data space and `1` for the
  voice-stream space (the two sequence spaces are independent —
  [PROTOCOL.md](PROTOCOL.md) — so the stream id is required for nonce
  uniqueness); `nonce_base_dir` derived per direction from HKDF; each 48-bit
  extended sequence is maintained by both sides from its 16-bit wire
  sequence + rollover tracking — **no nonce bytes on the wire**
- AAD: link envelope bytes 0–7 (type, flags, session, sequence) — header
  tampering breaks authentication
- per-frame overhead: **16 bytes** (tag only)
- replay: 128-frame sliding window per direction; duplicates and
  out-of-window frames are dropped and counted
- **the epoch is a nonce field**, so a frame accepted through the
  rotation grace window must be opened with the previous epoch *number*
  as well as the previous key — swapping the key alone produces a wrong
  nonce and a silent authentication failure
- retransmissions re-seal under the **same** sequence number, so the
  nonce and resulting ciphertext are byte-identical; HARQ chase
  combining across attempts is unaffected by encryption
- a cleartext `DATA` frame inside an encrypted session (and an encrypted
  frame with no session crypto) is dropped, not interpreted: there is no
  per-frame downgrade path

Encrypted frames set envelope flag bit 1. Handshake and broadcast frames
(`CALL`, `CQ`, `BEACON`, `BCAST_DATA`) are never encrypted; `DATA`,
`VOICE_DATA`, and file transfer always are once the session established
crypto.

## Epochs and Rotation

- epoch starts at 1 per session; u16, monotonically increasing
- `KEYROTATE` (manual) or automatic rotation every 2^20 frames or 4 hours,
  whichever first
- rotation: initiator sends `KEY_UPDATE(new_epoch)` encrypted under the
  current keys; both derive the new directional keys from `K_master` and the
  new epoch; a 8-frame grace window accepts either epoch during transition
- max tolerated epoch skew: 1 (grace window only); larger skew forces
  disconnect

## Key Material Handling

- `ENCRYPTKEY <hex>` loads a 32-byte hex PSK; stored in the INI (`[crypto]
  psk`) — file permissions are the user's responsibility, documented
- `ENCRYPTION ON` requires PSK present **and hardware AES** (the
  AES-NI runtime check); both commands rejected while
  `CALLING`/`CONNECTING`/`CONNECTED`
- if encryption is negotiated but the exchange cannot complete (no PSK,
  auth failure, missing kex material), the session is **refused** —
  `START_NAK` on the responder side, teardown on the initiator side.
  Session end zeroizes the keys, which also invalidates any resume
  context (a crypto state change is a resume invalidator)
- zeroization: session keys and `K_master` are wiped (`sodium_memzero`) at
  session end; PSK is wiped at process exit
- library: libsodium for X25519/HKDF primitives and constant-time helpers;
  AES-256-GCM via libsodium (`crypto_aead_aes256gcm`, hardware-accelerated)
  with a runtime check and graceful refusal on CPUs without AES-NI

## Runtime Surface

| Surface | Behavior |
|---|---|
| `SECURITY` | `SECURITY AUTH=<ON\|OFF> ENCRYPT=<ON\|OFF> EPOCH=<n>` |
| `ENCRYPTION ON\|OFF` | request/withdraw encryption for future connects |
| `ENCRYPTKEY <hex>` | install PSK |
| `KEYROTATE` | manual epoch bump on live encrypted session |
| INI `[crypto]` | `enabled`, `psk` |
| GUI | Security page, crypto builds only |

## Threat Model Notes (Explicit Non-Goals)

- no protection against RF traffic analysis (frame sizes/timing visible)
- no anonymity: callsigns are cleartext by design (and by regulation)
- PSK compromise allows active MITM of *future* sessions (auth is
  PSK-rooted); PFS protects past traffic only
- no multi-party/group keying in v1; one PSK per station pair
