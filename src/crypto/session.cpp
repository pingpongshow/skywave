#ifdef SKW_HAVE_CRYPTO

#include "crypto/session.hpp"

#include <sodium.h>

#include <cstring>

namespace skw {
namespace {

bool sodiumReady() {
  static const bool ok = sodium_init() >= 0;
  return ok;
}

// Canonical transcript bytes. includeResp adds E_r (full transcript).
std::vector<uint8_t> transcriptBytes(const KexContext& ctx,
                                     std::span<const uint8_t, 32> initPub,
                                     const uint8_t* respPub) {
  std::vector<uint8_t> t;
  t.reserve(96 + ctx.initCall.size() + ctx.respCall.size());
  auto str = [&](const std::string& s) {
    t.push_back(uint8_t(s.size()));
    t.insert(t.end(), s.begin(), s.end());
  };
  str(ctx.initCall);
  str(ctx.respCall);
  t.push_back(ctx.family);
  t.push_back(uint8_t(ctx.initCaps));
  t.push_back(uint8_t(ctx.initCaps >> 8));
  t.push_back(uint8_t(ctx.respCaps));
  t.push_back(uint8_t(ctx.respCaps >> 8));
  t.insert(t.end(), initPub.begin(), initPub.end());
  if (respPub) t.insert(t.end(), respPub, respPub + 32);
  return t;
}

void hmacTrunc16(std::span<const uint8_t, 32> key,
                 std::span<const uint8_t> msg, uint8_t out[16]) {
  uint8_t full[crypto_auth_hmacsha256_BYTES];
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, key.data(), key.size());
  crypto_auth_hmacsha256_update(&st, msg.data(), msg.size());
  crypto_auth_hmacsha256_final(&st, full);
  std::memcpy(out, full, 16);
  sodium_memzero(full, sizeof(full));
}

}  // namespace

CryptoSession::CryptoSession() { (void)sodiumReady(); }

CryptoSession::~CryptoSession() { zeroize(); }

bool CryptoSession::available() {
  return sodiumReady() && crypto_aead_aes256gcm_is_available() == 1;
}

bool CryptoSession::setPsk(std::span<const uint8_t> psk) {
  if (psk.size() != kPskBytes) return false;
  std::memcpy(psk_.data(), psk.data(), kPskBytes);
  havePsk_ = true;
  return true;
}

std::optional<std::vector<uint8_t>> CryptoSession::makeKexInit(
    const KexContext& ctx) {
  if (!havePsk_ || !available()) return std::nullopt;
  initiator_ = true;
  randombytes_buf(ephSecret_.data(), ephSecret_.size());
  crypto_scalarmult_base(ephPublic_.data(), ephSecret_.data());
  auto t = transcriptBytes(ctx, ephPublic_, nullptr);
  std::vector<uint8_t> blob(kKexBlobBytes);
  std::memcpy(blob.data(), ephPublic_.data(), kKexPublicBytes);
  hmacTrunc16(psk_, t, blob.data() + kKexPublicBytes);
  return blob;
}

std::optional<std::vector<uint8_t>> CryptoSession::acceptKexInit(
    const KexContext& ctx, std::span<const uint8_t> initBlob) {
  if (!havePsk_ || !available() || initBlob.size() != kKexBlobBytes) {
    return std::nullopt;
  }
  std::array<uint8_t, 32> peerPub;
  std::memcpy(peerPub.data(), initBlob.data(), 32);
  // Verify auth_i over transcript_i (constant time).
  auto ti = transcriptBytes(ctx, peerPub, nullptr);
  uint8_t want[16];
  hmacTrunc16(psk_, ti, want);
  if (sodium_memcmp(want, initBlob.data() + 32, 16) != 0) {
    return std::nullopt;
  }
  initiator_ = false;
  randombytes_buf(ephSecret_.data(), ephSecret_.size());
  crypto_scalarmult_base(ephPublic_.data(), ephSecret_.data());
  // Full transcript: E_i then E_r.
  auto tf = transcriptBytes(ctx, peerPub, ephPublic_.data());
  std::vector<uint8_t> blob(kKexBlobBytes);
  std::memcpy(blob.data(), ephPublic_.data(), kKexPublicBytes);
  hmacTrunc16(psk_, tf, blob.data() + kKexPublicBytes);
  // A rejected peer key (low-order point) leaves no session key: refuse
  // the exchange rather than answer it and run "encrypted" under zeros.
  if (!deriveMaster(ctx, peerPub)) return std::nullopt;
  return blob;
}

bool CryptoSession::acceptKexResponse(const KexContext& ctx,
                                      std::span<const uint8_t> respBlob) {
  if (!initiator_ || !havePsk_ || respBlob.size() != kKexBlobBytes) {
    return false;
  }
  std::array<uint8_t, 32> peerPub;
  std::memcpy(peerPub.data(), respBlob.data(), 32);
  auto tf = transcriptBytes(ctx, ephPublic_, peerPub.data());
  uint8_t want[16];
  hmacTrunc16(psk_, tf, want);
  if (sodium_memcmp(want, respBlob.data() + 32, 16) != 0) return false;
  return deriveMaster(ctx, peerPub);
}

bool CryptoSession::deriveMaster(const KexContext& ctx,
                                 std::span<const uint8_t, 32> peerPub) {
  uint8_t ss[crypto_scalarmult_BYTES];
  if (crypto_scalarmult(ss, ephSecret_.data(), peerPub.data()) != 0) {
    // Low-order peer public: leave un-established; the caller fails the
    // handshake (acceptKexInit / acceptKexResponse report it).
    sodium_memzero(ss, sizeof(ss));
    sodium_memzero(ephSecret_.data(), ephSecret_.size());
    established_ = false;
    return false;
  }
  // Full transcript hash binds the exchange into the schedule. Both
  // sides hash E_i then E_r, so order by role.
  const uint8_t* initPub =
      initiator_ ? ephPublic_.data() : peerPub.data();
  const uint8_t* respPub =
      initiator_ ? peerPub.data() : ephPublic_.data();
  std::array<uint8_t, 32> ip;
  std::memcpy(ip.data(), initPub, 32);
  auto tf = transcriptBytes(ctx, ip, respPub);
  uint8_t th[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(th, tf.data(), tf.size());
  // K_master = HKDF-SHA256(ikm=ss, salt=PSK, info="skw-v1"||th).
  uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  crypto_kdf_hkdf_sha256_extract(prk, psk_.data(), psk_.size(), ss,
                                 sizeof(ss));
  std::vector<uint8_t> info;
  const char* label = "skw-v1";
  info.insert(info.end(), label, label + 6);
  info.insert(info.end(), th, th + sizeof(th));
  crypto_kdf_hkdf_sha256_expand(master_.data(), master_.size(),
                                reinterpret_cast<const char*>(info.data()),
                                info.size(), prk);
  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(ss, sizeof(ss));
  established_ = true;
  havePrevEpoch_ = false;
  epoch_ = 1;
  deriveEpochKeys(1, tx_, rx_);
  replay_[0] = Replay{};
  replay_[1] = Replay{};
  // The ephemeral secret has done its job; keeping it let a replayed
  // START_ACK re-run the exchange and reset the epoch mid-session.
  sodium_memzero(ephSecret_.data(), ephSecret_.size());
  return true;
}

void CryptoSession::deriveEpochKeys(uint16_t epoch, Dir& tx, Dir& rx) const {
  // K_i2r / K_r2i + nonce bases, both from K_master and the epoch. Which
  // one is TX depends on the role.
  auto expand = [&](const char* dirLabel, Dir& d) {
    uint8_t okm[44];
    std::vector<uint8_t> info;
    info.insert(info.end(), dirLabel, dirLabel + 3);
    info.push_back(uint8_t(epoch));
    info.push_back(uint8_t(epoch >> 8));
    crypto_kdf_hkdf_sha256_expand(
        okm, sizeof(okm), reinterpret_cast<const char*>(info.data()),
        info.size(), master_.data());
    std::memcpy(d.key.data(), okm, 32);
    std::memcpy(d.nonceBase.data(), okm + 32, 12);
    sodium_memzero(okm, sizeof(okm));
  };
  expand(initiator_ ? "i2r" : "r2i", tx);
  expand(initiator_ ? "r2i" : "i2r", rx);
}

void CryptoSession::setEpoch(uint16_t epoch) {
  if (!established_ || epoch == epoch_) return;
  rxPrev_ = rx_;
  prevEpoch_ = epoch_;
  havePrevEpoch_ = true;
  epoch_ = epoch;
  deriveEpochKeys(epoch, tx_, rx_);
}

std::array<uint8_t, 12> CryptoSession::nonceFor(const Dir& d,
                                                uint16_t epoch,
                                                uint8_t stream,
                                                uint64_t seq48) const {
  // nonce = base XOR (stream:u8 || epoch:u16 || seq:u48), fields packed
  // little-endian into the low 9 bytes; base randomizes the rest.
  std::array<uint8_t, 12> n = d.nonceBase;
  n[0] ^= stream;
  n[1] ^= uint8_t(epoch);
  n[2] ^= uint8_t(epoch >> 8);
  for (int i = 0; i < 6; ++i) {
    n[size_t(3 + i)] ^= uint8_t(seq48 >> (8 * i));
  }
  return n;
}

std::vector<uint8_t> CryptoSession::seal(uint8_t stream, uint64_t seq48,
                                         std::span<const uint8_t> aad,
                                         std::span<const uint8_t> plain) const {
  if (!established_) return {};
  std::vector<uint8_t> out(plain.size() + kGcmTagBytes);
  const auto n = nonceFor(tx_, epoch_, stream, seq48);
  unsigned long long clen = 0;
  crypto_aead_aes256gcm_encrypt(out.data(), &clen, plain.data(),
                                plain.size(), aad.data(), aad.size(),
                                nullptr, n.data(), tx_.key.data());
  out.resize(size_t(clen));
  ++framesSealed_;
  return out;
}

bool CryptoSession::replayCheck(Replay& r, uint64_t seq) {
  if (!r.any) {
    r.any = true;
    r.hi = seq;
    r.bits = {};
    r.bits[0] = 1;  // bit for hi itself
    return true;
  }
  if (seq > r.hi) {
    const uint64_t shift = seq - r.hi;
    if (shift >= kReplayWindow) {
      r.bits = {};
    } else {
      // Shift the window up by `shift` bits (bit 0 = hi).
      for (uint64_t s = 0; s < shift; ++s) {
        uint64_t carry = 0;
        for (size_t w = 0; w < r.bits.size(); ++w) {
          const uint64_t nc = r.bits[w] >> 63;
          r.bits[w] = (r.bits[w] << 1) | carry;
          carry = nc;
        }
      }
    }
    r.hi = seq;
    r.bits[0] |= 1;
    return true;
  }
  const uint64_t back = r.hi - seq;
  if (back >= kReplayWindow) return false;  // too old
  const size_t w = size_t(back / 64), b = size_t(back % 64);
  if (r.bits[w] & (1ull << b)) return false;  // duplicate
  r.bits[w] |= 1ull << b;
  return true;
}

std::optional<std::vector<uint8_t>> CryptoSession::open(
    uint8_t stream, uint64_t seq48, std::span<const uint8_t> aad,
    std::span<const uint8_t> cipher) {
  if (!established_ || cipher.size() < kGcmTagBytes) return std::nullopt;
  std::vector<uint8_t> out(cipher.size() - kGcmTagBytes);
  unsigned long long mlen = 0;
  const auto n = nonceFor(rx_, epoch_, stream, seq48);
  int rc = crypto_aead_aes256gcm_decrypt(
      out.data(), &mlen, nullptr, cipher.data(), cipher.size(), aad.data(),
      aad.size(), n.data(), rx_.key.data());
  if (rc != 0 && havePrevEpoch_) {
    // Grace window: a frame sealed under the previous epoch.
    const auto np = nonceFor(rxPrev_, prevEpoch_, stream, seq48);
    rc = crypto_aead_aes256gcm_decrypt(out.data(), &mlen, nullptr,
                                       cipher.data(), cipher.size(),
                                       aad.data(), aad.size(), np.data(),
                                       rxPrev_.key.data());
  }
  if (rc != 0) return std::nullopt;
  if (!replayCheck(replay_[stream & 1], seq48)) {
    ++replayDrops_;
    return std::nullopt;
  }
  out.resize(size_t(mlen));
  return out;
}

uint64_t CryptoSession::extendSeq(uint64_t hiWater, uint16_t wire) {
  // Choose the 48-bit sequence with these low 16 bits closest to the
  // high-water mark (handles 16-bit rollover in both directions).
  const uint64_t base = hiWater & ~0xFFFFull;
  uint64_t pick = base | wire;
  uint64_t bestDist =
      pick > hiWater ? pick - hiWater : hiWater - pick;
  auto consider = [&](uint64_t c) {
    if (c > 0xFFFFFFFFFFFFull) return;
    const uint64_t dist = c > hiWater ? c - hiWater : hiWater - c;
    if (dist < bestDist) {
      bestDist = dist;
      pick = c;
    }
  };
  if (base >= 0x10000) consider((base - 0x10000) | wire);
  consider((base + 0x10000) | wire);
  return pick;
}

void CryptoSession::zeroize() {
  sodium_memzero(psk_.data(), psk_.size());
  sodium_memzero(ephSecret_.data(), ephSecret_.size());
  sodium_memzero(ephPublic_.data(), ephPublic_.size());
  sodium_memzero(master_.data(), master_.size());
  for (Dir* d : {&tx_, &rx_, &rxPrev_}) {
    sodium_memzero(d->key.data(), d->key.size());
    sodium_memzero(d->nonceBase.data(), d->nonceBase.size());
  }
  havePsk_ = false;
  established_ = false;
  havePrevEpoch_ = false;
}

}  // namespace skw

#endif  // SKW_HAVE_CRYPTO
