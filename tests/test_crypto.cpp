// Crypto (ROADMAP 5.1 + 5.2): primitive pinning vectors (X25519 RFC 7748,
// HKDF-SHA256 RFC 5869), the full KEX role-play, tampered-transcript
// rejection, and AEAD framing properties (tamper, replay window, nonce
// uniqueness across streams/epochs, epoch grace window).
#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto/session.hpp"
#include "link/engine.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> hexBytes(const char* h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    auto nib = [](char c) {
      return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
    };
    v.push_back(uint8_t(nib(h[i]) << 4 | nib(h[i + 1])));
  }
  return v;
}

std::vector<uint8_t> testPsk(uint8_t fill = 0x42) {
  return std::vector<uint8_t>(kPskBytes, fill);
}

KexContext ctx() {
  KexContext c;
  c.initCall = "W1AW";
  c.respCall = "K6XYZ";
  c.family = 0;
  c.initCaps = 0x0037;
  c.respCaps = 0x0037;
  return c;
}

// Run a complete two-party handshake; returns both established sessions.
bool handshake(CryptoSession& i, CryptoSession& r,
               const std::vector<uint8_t>& pskI,
               const std::vector<uint8_t>& pskR) {
  CHECK(i.setPsk(pskI));
  CHECK(r.setPsk(pskR));
  auto blobI = i.makeKexInit(ctx());
  if (!blobI) return false;
  auto blobR = r.acceptKexInit(ctx(), *blobI);
  if (!blobR) return false;
  return i.acceptKexResponse(ctx(), *blobR);
}

}  // namespace

TEST_CASE(crypto_primitive_vectors) {
  CHECK(sodium_init() >= 0);
  CHECK(CryptoSession::available());
  // X25519: RFC 7748 section 6.1 (Alice/Bob DH test vector).
  auto aPriv = hexBytes(
      "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  auto bPub = hexBytes(
      "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  auto want = hexBytes(
      "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  uint8_t ss[32];
  CHECK(crypto_scalarmult(ss, aPriv.data(), bPub.data()) == 0);
  CHECK(std::memcmp(ss, want.data(), 32) == 0);
  // HKDF-SHA256: RFC 5869 appendix A.1.
  auto ikm = hexBytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  auto salt = hexBytes("000102030405060708090a0b0c");
  auto info = hexBytes("f0f1f2f3f4f5f6f7f8f9");
  auto okmWant = hexBytes(
      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
      "34007208d5b887185865");
  uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  crypto_kdf_hkdf_sha256_extract(prk, salt.data(), salt.size(), ikm.data(),
                                 ikm.size());
  uint8_t okm[42];
  crypto_kdf_hkdf_sha256_expand(okm, sizeof(okm),
                                reinterpret_cast<const char*>(info.data()),
                                info.size(), prk);
  CHECK(std::memcmp(okm, okmWant.data(), sizeof(okm)) == 0);
}

TEST_CASE(crypto_kex_roleplay_and_keys_agree) {
  CryptoSession i, r;
  CHECK(handshake(i, r, testPsk(), testPsk()));
  CHECK(i.established());
  CHECK(r.established());
  // Directional keys agree: what I seals under stream 0, R opens - and
  // the reverse direction uses DIFFERENT keys (a frame sealed by I must
  // not open as if R had sealed it).
  const std::vector<uint8_t> aad = {6, 0, 1, 0, 2, 0, 3, 0};
  const std::vector<uint8_t> msg = {'s', 'e', 'c', 'r', 'e', 't'};
  auto c1 = i.seal(0, 1, aad, msg);
  CHECK_EQ(int(c1.size()), int(msg.size() + kGcmTagBytes));
  auto p1 = r.open(0, 1, aad, c1);
  CHECK(p1.has_value());
  CHECK(*p1 == msg);
  CryptoSession i2, r2;
  CHECK(handshake(i2, r2, testPsk(), testPsk()));
  auto cr = r2.seal(0, 1, aad, msg);
  auto selfOpen = r2.open(0, 1, aad, cr);  // own TX key != own RX key
  CHECK(!selfOpen.has_value());
  auto crossOpen = i2.open(0, 1, aad, cr);
  CHECK(crossOpen.has_value());
}

TEST_CASE(crypto_tampered_transcript_rejected) {
  // Wrong PSK on the responder: auth_i verification fails.
  {
    CryptoSession i, r;
    CHECK(i.setPsk(testPsk(0x42)));
    CHECK(r.setPsk(testPsk(0x43)));
    auto blobI = i.makeKexInit(ctx());
    CHECK(blobI.has_value());
    CHECK(!r.acceptKexInit(ctx(), *blobI).has_value());
  }
  // Transcript field mutated in flight (capability bitmap downgrade):
  // responder computes auth over what IT saw -> mismatch.
  {
    CryptoSession i, r;
    CHECK(i.setPsk(testPsk()));
    CHECK(r.setPsk(testPsk()));
    auto blobI = i.makeKexInit(ctx());
    KexContext seen = ctx();
    seen.initCaps = 0x0001;  // attacker stripped capabilities
    CHECK(!r.acceptKexInit(seen, *blobI).has_value());
  }
  // Ephemeral public bit-flipped in flight: auth_i covers E_i.
  {
    CryptoSession i, r;
    CHECK(i.setPsk(testPsk()));
    CHECK(r.setPsk(testPsk()));
    auto blobI = i.makeKexInit(ctx());
    (*blobI)[3] ^= 0x80;
    CHECK(!r.acceptKexInit(ctx(), *blobI).has_value());
  }
  // auth_r tampered: initiator rejects the response.
  {
    CryptoSession i, r;
    CHECK(i.setPsk(testPsk()));
    CHECK(r.setPsk(testPsk()));
    auto blobI = i.makeKexInit(ctx());
    auto blobR = r.acceptKexInit(ctx(), *blobI);
    CHECK(blobR.has_value());
    (*blobR)[kKexPublicBytes + 2] ^= 1;
    CHECK(!i.acceptKexResponse(ctx(), *blobR));
  }
}

TEST_CASE(crypto_aead_tamper_and_aad_binding) {
  CryptoSession i, r;
  CHECK(handshake(i, r, testPsk(), testPsk()));
  const std::vector<uint8_t> aad = {6, 2, 9, 0, 5, 0, 1, 0};
  std::vector<uint8_t> msg(200);
  for (size_t k = 0; k < msg.size(); ++k) msg[k] = uint8_t(k * 7);
  auto c = i.seal(0, 7, aad, msg);
  // Ciphertext tamper.
  auto cBad = c;
  cBad[10] ^= 1;
  CHECK(!r.open(0, 7, aad, cBad).has_value());
  // Tag tamper.
  auto cTag = c;
  cTag[cTag.size() - 1] ^= 1;
  CHECK(!r.open(0, 7, aad, cTag).has_value());
  // AAD (envelope) tamper: sequence field rewritten by an attacker.
  auto aadBad = aad;
  aadBad[4] ^= 1;
  CHECK(!r.open(0, 7, aadBad, c).has_value());
  // Wrong sequence (nonce mismatch).
  CHECK(!r.open(0, 8, aad, c).has_value());
  // Pristine frame still opens.
  CHECK(r.open(0, 7, aad, c).has_value());
}

TEST_CASE(crypto_replay_window) {
  CryptoSession i, r;
  CHECK(handshake(i, r, testPsk(), testPsk()));
  const std::vector<uint8_t> aad = {6, 2, 9, 0, 5, 0, 1, 0};
  const std::vector<uint8_t> msg = {1, 2, 3};
  auto c5 = i.seal(0, 5, aad, msg);
  auto c6 = i.seal(0, 6, aad, msg);
  CHECK(r.open(0, 5, aad, c5).has_value());
  CHECK(!r.open(0, 5, aad, c5).has_value());  // exact replay dropped
  CHECK(r.open(0, 6, aad, c6).has_value());
  CHECK(!r.open(0, 5, aad, c5).has_value());  // still remembered
  // Out-of-order within the window is fine, replay of it is not.
  auto c2 = i.seal(0, 2, aad, msg);
  CHECK(r.open(0, 2, aad, c2).has_value());
  CHECK(!r.open(0, 2, aad, c2).has_value());
  // Far beyond the window: too old, dropped even though never seen.
  auto cOld = i.seal(0, 1, aad, msg);
  auto cNew = i.seal(0, 5 + kReplayWindow + 10, aad, msg);
  CHECK(r.open(0, 5 + kReplayWindow + 10, aad, cNew).has_value());
  CHECK(!r.open(0, 1, aad, cOld).has_value());
  CHECK_EQ(int(r.replayDrops()), 4);
}

TEST_CASE(crypto_nonce_uniqueness_streams_epochs) {
  CryptoSession i, r;
  CHECK(handshake(i, r, testPsk(), testPsk()));
  const std::vector<uint8_t> aad = {0, 0, 0, 0, 0, 0, 0, 0};
  const std::vector<uint8_t> msg(32, 0xAB);
  // Same seq, different stream id -> different nonce -> different
  // ciphertext (voice and data spaces never collide).
  auto cData = i.seal(0, 9, aad, msg);
  auto cVoice = i.seal(1, 9, aad, msg);
  CHECK(cData != cVoice);
  CHECK(r.open(1, 9, aad, cVoice).has_value());
  CHECK(!r.open(0, 9, aad, cVoice).has_value());
  // Epoch rotation changes keys: old-epoch ciphertext opens only through
  // the grace path, and the same seq under the new epoch is distinct.
  auto cE1 = i.seal(0, 20, aad, msg);
  i.setEpoch(2);
  r.setEpoch(2);
  auto cE2 = i.seal(0, 21, aad, msg);
  CHECK(cE1 != cE2);
  CHECK(r.open(0, 21, aad, cE2).has_value());
  CHECK(r.open(0, 20, aad, cE1).has_value());  // grace window
  CHECK(!i.seal(0, 22, aad, msg).empty());
  // Zeroization kills the session.
  r.zeroize();
  CHECK(!r.established());
  CHECK(!r.open(0, 23, aad, i.seal(0, 23, aad, msg)).has_value());
}

TEST_CASE(crypto_seq_extension) {
  CHECK_EQ(int(CryptoSession::extendSeq(0, 5)), 5);
  CHECK_EQ(int(CryptoSession::extendSeq(100, 101)), 101);
  // Rollover forward: hiWater near the 16-bit boundary.
  CHECK(CryptoSession::extendSeq(0xFFF0, 0x0002) == 0x10002);
  // Slightly-behind wire seq stays in the current window.
  CHECK(CryptoSession::extendSeq(0x10005, 0xFFFE) == 0xFFFE);
  // Deep into the space.
  CHECK(CryptoSession::extendSeq(0x50000 + 0x8000, 0x7FFF) ==
        0x50000 + 0x7FFF);
}

// --- 5.3 gate: two-modem encrypted session ---------------------------------

namespace {

std::vector<uint8_t> pskBytes() { return testPsk(0x5C); }

struct CryptoPair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

  CryptoPair(bool aPsk, bool bPsk, bool bWants = true)
      : a(mk("W1AW", aPsk, true)), b(mk("K6XYZ", bPsk, bWants)) {
    chanAB.fading = false;
    chanAB.snr3kDb = 25.0;
    chanBA = chanAB;
    chanAB.seed = 3300;
    chanBA.seed = 8300;
  }

  static EngineConfig mk(const char* call, bool withPsk, bool wants) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    if (wants) c.policy.caps |= kCapEncryption;
    if (withPsk) c.psk = pskBytes();
    c.encryptionRequested = wants;
    return c;
  }

  void step() {
    auto bA = a.tick(now);
    auto bB = b.tick(now);
    double dt = 0.05;
    auto push = [&](std::vector<cfloat>& burst, LinkEngine& to,
                    ChansimParams& ch) {
      dt += double(burst.size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst.begin(), burst.end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = ch;
      cp.seed = ch.seed + (seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      to.rxBurst(y, now);
    };
    if (bA) push(*bA, b, chanAB);
    if (bB) push(*bB, a, chanBA);
    now += dt;
  }

  bool connect(int budget = 600) {
    a.connectTo("K6XYZ", now);
    for (int i = 0; i < budget; ++i) {
      step();
      if (a.state() == FsmState::Connected &&
          b.state() == FsmState::Connected) {
        return true;
      }
    }
    return false;
  }
};

std::vector<uint8_t> noise(size_t n, uint64_t seed) {
  std::vector<uint8_t> v(n);
  uint64_t s = seed * 2654435761u + 1;
  for (auto& x : v) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    x = uint8_t(s >> 32);
  }
  return v;
}

}  // namespace

TEST_CASE(crypto_session_two_modem_with_rotation) {
  // Both stations hold the same PSK and request encryption: the session
  // comes up authenticated + encrypted, bulk data survives a mid-transfer
  // KEYROTATE, and the payload arrives byte-exact.
  CryptoPair p(true, true);
  CHECK(p.connect());
  CHECK(p.a.cryptoActive());
  CHECK(p.b.cryptoActive());
  CHECK(p.a.cryptoAuth());
  CHECK_EQ(int(p.a.cryptoEpoch()), 1);

  auto blob = noise(12 * 1024, 61);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  bool rotated = false;
  for (int i = 0; i < 8000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
    if (!rotated && got.size() > blob.size() / 3) {
      rotated = p.a.rotateKeys(p.now);  // mid-transfer rekey
    }
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(rotated);
  CHECK_EQ(int(p.a.cryptoEpoch()), 2);
  CHECK(got == blob);
  std::printf("crypto session: %zu B through epoch rotation, epoch=%d/%d\n",
              got.size(), int(p.a.cryptoEpoch()), int(p.b.cryptoEpoch()));
  CHECK_EQ(int(p.b.cryptoEpoch()), 2);  // peer followed the KEY_UPDATE
}

TEST_CASE(crypto_psk_mismatch_refuses_session) {
  // Responder holds no PSK: the negotiated encryption cannot complete, so
  // the session is REFUSED - it must never come up in cleartext.
  CryptoPair p(true, false);
  const bool up = p.connect(400);
  std::printf("psk mismatch: connected=%d cryptoA=%d\n", up ? 1 : 0,
              p.a.cryptoActive() ? 1 : 0);
  CHECK(!(up && !p.a.cryptoActive()));  // never cleartext-with-crypto-asked
  CHECK(!p.a.cryptoActive());
  CHECK(!p.b.cryptoActive());
}

TEST_CASE(crypto_peer_without_capability_stays_cleartext) {
  // Peer does not advertise encryption: the capability intersection drops
  // it and the session proceeds in the clear (a legal, explicit outcome).
  CryptoPair p(true, false, /*bWants=*/false);
  CHECK(p.connect());
  CHECK(!p.a.cryptoActive());
  CHECK(!p.b.cryptoActive());
  const std::vector<uint8_t> msg = {'c', 'l', 'e', 'a', 'r'};
  p.a.sendData(msg);
  std::vector<uint8_t> got;
  for (int i = 0; i < 600 && got.size() < msg.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == msg);
}

SKYTEST_MAIN
