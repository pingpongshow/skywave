// Session crypto (SECURITY.md): PSK-rooted identity + ephemeral X25519
// per session, HKDF-SHA256 key schedule, AES-256-GCM per link frame with
// implicit nonces and a replay window. Compiled only under ENABLE_CRYPTO
// (SKW_HAVE_CRYPTO); libsodium provides every primitive.
//
// KEX blobs on the wire (inside StartBody.kexPublic /
// StartAckBody.kexResponse, PROTOCOL.md):
//   E(32 bytes X25519 public) || auth(16 bytes truncated HMAC-SHA256)
//   auth_i covers transcript_i (no E_r yet); auth_r covers the full
//   transcript. Transcript = initCall || respCall || family || caps_i ||
//   caps_r || E_i [|| E_r].
#pragma once
#ifdef SKW_HAVE_CRYPTO

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace skw {

inline constexpr size_t kPskBytes = 32;
inline constexpr size_t kKexPublicBytes = 32;
inline constexpr size_t kKexAuthBytes = 16;
inline constexpr size_t kKexBlobBytes = kKexPublicBytes + kKexAuthBytes;
inline constexpr size_t kGcmTagBytes = 16;
inline constexpr size_t kReplayWindow = 128;

// Handshake context both sides agree on (transcript inputs).
struct KexContext {
  std::string initCall;   // canonical callsigns
  std::string respCall;
  uint8_t family = 0;
  uint16_t initCaps = 0;  // capability bitmaps as sent on the wire
  uint16_t respCaps = 0;
};

class CryptoSession {
 public:
  CryptoSession();
  ~CryptoSession();
  // Non-copyable (key material must not be duplicated silently) but
  // movable: the owning LinkEngine is move-assigned when a station
  // restarts a session object.
  CryptoSession(const CryptoSession&) = delete;
  CryptoSession& operator=(const CryptoSession&) = delete;
  CryptoSession(CryptoSession&&) noexcept = default;
  CryptoSession& operator=(CryptoSession&&) noexcept = default;

  // Hardware/runtime support (AES-NI); false => refuse encryption.
  static bool available();

  bool setPsk(std::span<const uint8_t> psk);  // exactly 32 bytes

  // --- handshake ---------------------------------------------------------
  // Initiator: generate an ephemeral pair, return E_i||auth_i for START.
  std::optional<std::vector<uint8_t>> makeKexInit(const KexContext& ctx);
  // Responder: verify auth_i, generate own ephemeral, derive keys.
  // Returns E_r||auth_r for START_ACK, or nullopt on auth failure.
  std::optional<std::vector<uint8_t>> acceptKexInit(
      const KexContext& ctx, std::span<const uint8_t> initBlob);
  // Initiator: verify auth_r against the full transcript, derive keys.
  bool acceptKexResponse(const KexContext& ctx,
                         std::span<const uint8_t> respBlob);

  bool established() const { return established_; }

  // --- epochs ------------------------------------------------------------
  // Re-derive directional keys for `epoch`; the previous epoch's keys are
  // kept for the RX grace window (SECURITY.md: skew tolerance 1).
  void setEpoch(uint16_t epoch);
  uint16_t epoch() const { return epoch_; }

  // --- AEAD framing (5.2) -------------------------------------------------
  // stream: 0 = ARQ/data space, 1 = voice space. seq48 is the extended
  // sequence (caller tracks rollover from the 16-bit wire field).
  // aad = link envelope bytes 0..7. seal returns ciphertext||tag, or an
  // EMPTY vector when no session key is established (never seal under
  // the zeroed key).
  std::vector<uint8_t> seal(uint8_t stream, uint64_t seq48,
                            std::span<const uint8_t> aad,
                            std::span<const uint8_t> plain) const;
  // open tries the current epoch, then (within the grace window) the
  // previous one. Replay-checked per (direction fixed by role) stream 0;
  // returns nullopt on tag/replay failure.
  std::optional<std::vector<uint8_t>> open(uint8_t stream, uint64_t seq48,
                                           std::span<const uint8_t> aad,
                                           std::span<const uint8_t> cipher);

  uint32_t replayDrops() const { return replayDrops_; }
  uint64_t framesSealed() const { return framesSealed_; }

  // Extend a 16-bit wire sequence to 48 bits near the RX high-water mark.
  static uint64_t extendSeq(uint64_t hiWater, uint16_t wire);

  // Wipe all key material (session end / destructor).
  void zeroize();

 private:
  struct Dir {  // per-direction traffic material
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 12> nonceBase{};
  };
  // False (session left un-established) when the peer's public key is
  // rejected (low-order point): callers must fail the handshake.
  bool deriveMaster(const KexContext& ctx,
                    std::span<const uint8_t, 32> peerPub);
  void deriveEpochKeys(uint16_t epoch, Dir& tx, Dir& rx) const;
  // The epoch is a nonce field, so the grace-window attempt must use the
  // PREVIOUS epoch number with the previous keys - not just the old key.
  std::array<uint8_t, 12> nonceFor(const Dir& d, uint16_t epoch,
                                   uint8_t stream, uint64_t seq48) const;

  bool havePsk_ = false;
  bool established_ = false;
  bool initiator_ = false;
  std::array<uint8_t, 32> psk_{};
  std::array<uint8_t, 32> ephSecret_{};
  std::array<uint8_t, 32> ephPublic_{};
  std::array<uint8_t, 32> master_{};
  uint16_t epoch_ = 1;
  uint16_t prevEpoch_ = 0;
  Dir tx_, rx_, rxPrev_;
  bool havePrevEpoch_ = false;
  mutable uint64_t framesSealed_ = 0;
  // Replay window (per stream): highest seq48 seen + bitmask of the
  // kReplayWindow frames below it.
  struct Replay {
    uint64_t hi = 0;
    bool any = false;
    std::array<uint64_t, kReplayWindow / 64> bits{};
  };
  bool replayCheck(Replay& r, uint64_t seq);
  Replay replay_[2];
  uint32_t replayDrops_ = 0;
};

}  // namespace skw

#endif  // SKW_HAVE_CRYPTO
