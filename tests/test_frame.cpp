// Link envelope + frame bodies (ROADMAP 2.1): round-trips for every
// PROTOCOL.md frame type, malformed-input rejection, and fuzz (parsers
// must never crash and must cleanly reject garbage).
#include <cstring>
#include <string>
#include <vector>

#include "core/constants.hpp"
#include "link/frame.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

uint64_t rngState = 0x9E3779B97F4A7C15ull;
uint64_t nextRng() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 7;
  rngState ^= rngState << 17;
  return rngState;
}

std::vector<uint8_t> randomBytes(size_t n) {
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = uint8_t(nextRng());
  return v;
}

}  // namespace

TEST_CASE(envelope_round_trip) {
  LinkEnvelope e;
  e.type = LinkType::Data;
  e.flags = kFlagCompressed | kFlagMoreFragments;
  e.sessionId = 0xBEEF;
  e.sequence = 0x1234;
  e.ackBase = 0x00FF;
  std::vector<uint8_t> body = {1, 2, 3, 4, 5};
  auto raw = packFrame(e, body);
  CHECK_EQ(int(raw.size()), int(kFrameOverhead + body.size()));
  auto p = parseFrame(raw);
  CHECK(p.has_value());
  CHECK(p->env.type == LinkType::Data);
  CHECK_EQ(p->env.flags, e.flags);
  CHECK_EQ(p->env.sessionId, e.sessionId);
  CHECK_EQ(p->env.sequence, e.sequence);
  CHECK_EQ(p->env.ackBase, e.ackBase);
  CHECK(p->body == body);
}

TEST_CASE(envelope_rejects) {
  LinkEnvelope e;
  e.type = LinkType::Ack;
  auto raw = packFrame(e, AckBody{}.pack());
  // CRC flip anywhere.
  for (size_t i = 0; i < raw.size(); ++i) {
    auto bad = raw;
    bad[i] ^= 0x40;
    CHECK(!parseFrame(bad).has_value());
  }
  // Truncations.
  for (size_t n = 0; n < raw.size(); ++n) {
    CHECK(!parseFrame({raw.data(), n}).has_value());
  }
  // Unknown type id with a VALID CRC (packFrame will happily emit it):
  // parseFrame must reject on the type gate, not just the CRC.
  {
    LinkEnvelope u = e;
    u.type = LinkType(63);
    auto bad = packFrame(u, AckBody{}.pack());
    CHECK(!parseFrame(bad).has_value());
  }
  // Reserved flag bits set (valid CRC).
  {
    LinkEnvelope u = e;
    u.flags = 0x10;
    auto bad = packFrame(u, AckBody{}.pack());
    CHECK(!parseFrame(bad).has_value());
  }
}

TEST_CASE(callsign_rules) {
  CHECK(validateCallsign("w1aw").value() == "W1AW");
  CHECK(validateCallsign("VE3/W1AW").has_value());
  CHECK(!validateCallsign("AB").has_value());          // too short
  CHECK(!validateCallsign("ABCDEFGHIJKLM").has_value());  // too long
  CHECK(!validateCallsign("W1 AW").has_value());       // space
  CHECK(!validateCallsign("W1-AW").has_value());       // dash
  // Hash: 24-bit, stable, differs across calls.
  uint32_t h1 = callsignHash24("W1AW");
  uint32_t h2 = callsignHash24("K6XYZ");
  CHECK(h1 <= 0xFFFFFF);
  CHECK(h1 == callsignHash24("W1AW"));
  CHECK(h1 != h2);
}

TEST_CASE(body_round_trips) {
  {
    CallBody b;
    b.srcCall = "W1AW";
    b.dstCall = "VE3ABC/P";
    b.version = 1;
    b.family = 2;
    b.caps = kCapZstd | kCapVoice;
    auto p = CallBody::parse(b.pack());
    CHECK(p && p->srcCall == b.srcCall && p->dstCall == b.dstCall &&
          p->family == b.family && p->caps == b.caps);
  }
  {
    CallAckBody b{-17, 42, -6, kCapZstd | kCapResume};
    auto p = CallAckBody::parse(b.pack());
    CHECK(p && p->snrHalfDb == -17 && p->selectivity == 42 &&
          p->cfoHalfHz == -6 && p->caps == b.caps);
  }
  {
    StartBody b;
    b.caps = kCapZstd;
    b.initialMode = 7;
    b.cpMode = 2;
    b.maxMode = 11;
    b.macProfile = 1;
    b.hasResumeToken = true;
    for (int i = 0; i < 8; ++i) b.resumeToken[size_t(i)] = uint8_t(i * 3);
    b.kexPublic = randomBytes(32);
    auto p = StartBody::parse(b.pack());
    CHECK(p && p->initialMode == 7 && p->hasResumeToken &&
          p->resumeToken == b.resumeToken && p->kexPublic == b.kexPublic);
  }
  {
    StartBody b;  // no optionals
    auto p = StartBody::parse(b.pack());
    CHECK(p && !p->hasResumeToken && p->kexPublic.empty());
  }
  {
    StartAckBody b;
    b.finalMode = 6;
    b.kexResponse = randomBytes(32);
    auto p = StartAckBody::parse(b.pack());
    CHECK(p && p->finalMode == 6 && p->kexResponse == b.kexResponse);
  }
  {
    StartNakBody b{StartNakReason::Busy};
    auto p = StartNakBody::parse(b.pack());
    CHECK(p && p->reason == StartNakReason::Busy);
  }
  {
    DataBody b;
    b.rv = 2;
    b.payload = randomBytes(300);
    auto p = DataBody::parse(b.pack());
    CHECK(p && p->rv == 2 && p->payload == b.payload);
  }
  {
    AckBody b{-8, 0x03, 1000, 0x0005, 4};
    auto raw = b.pack();
    CHECK_EQ(int(raw.size()), int(kAckBodyBytes));
    auto p = AckBody::parse(raw);
    CHECK(p && p->ackBase == 1000 && p->gapBitmap == 5 && p->isNak() &&
          p->cfoHalfHz == 4);
    AckBody pure{0, 0, 7, 0, 0};
    CHECK(!AckBody::parse(pure.pack())->isNak());
  }
  {
    ModeChangeBody b{9, 1, 3};
    auto p = ModeChangeBody::parse(b.pack());
    CHECK(p && p->targetMode == 9 && p->epoch == 3);
    CHECK(ModeAckBody::parse(ModeAckBody{3}.pack())->epoch == 3);
  }
  {
    ProbeBody b{7, 10};
    auto p = ProbeBody::parse(b.pack());
    CHECK(p && p->probeId == 7 && p->probeMode == 10);
    ProbeAckBody a{7, 21, 1};
    auto pa = ProbeAckBody::parse(a.pack());
    CHECK(pa && pa->probeId == 7 && pa->snrHalfDb == 21 && pa->decodedOk);
  }
  {
    MaskUpdateBody b;
    b.epoch = 5;
    b.family = 0;
    for (int i = 0; i < 9; ++i) b.mask[size_t(i)] = uint8_t(0xFF >> (i % 3));
    auto p = MaskUpdateBody::parse(b.pack());
    CHECK(p && p->epoch == 5 && p->mask == b.mask);
    CHECK(MaskAckBody::parse(MaskAckBody{5}.pack())->epoch == 5);
  }
  {
    CHECK(KeepaliveBody::parse(KeepaliveBody{33}.pack())->remoteSnr == 33);
  }
  {
    ResumeReqBody b;
    for (int i = 0; i < 8; ++i) b.token[size_t(i)] = uint8_t(200 + i);
    b.lastRxSeq = 0xABCD;
    auto p = ResumeReqBody::parse(b.pack());
    CHECK(p && p->token == b.token && p->lastRxSeq == 0xABCD);
    CHECK(ResumeAckBody::parse(ResumeAckBody{77}.pack())->lastRxSeq == 77);
  }
  {
    auto p = DiscBody::parse(DiscBody{DiscReason::IdleTimeout}.pack());
    CHECK(p && p->reason == DiscReason::IdleTimeout);
  }
  {
    BeaconBody b;
    b.callsign = "K6XYZ";
    b.snrFloorPlus20 = 12;
    b.family = 1;
    b.swVersion = 1;
    auto p = BeaconBody::parse(b.pack());
    CHECK(p && p->callsign == "K6XYZ" && p->snrFloorPlus20 == 12);
  }
  {
    CqBody b;
    b.callsign = "W1AW";
    b.caps = kCapVoice;
    auto p = CqBody::parse(b.pack());
    CHECK(p && p->callsign == "W1AW" && p->caps == kCapVoice);
  }
  {
    BcastDataBody b;
    b.fragIndex = 2;
    b.fragCount = 4;
    b.payload = randomBytes(100);
    auto p = BcastDataBody::parse(b.pack());
    CHECK(p && p->fragIndex == 2 && p->fragCount == 4 &&
          p->payload == b.payload);
  }
  {
    auto p = VoiceOpenBody::parse(VoiceOpenBody{VoiceCodec::C2400}.pack());
    CHECK(p && p->codec == VoiceCodec::C2400);
  }
  {
    CHECK(KeyUpdateBody::parse(KeyUpdateBody{9}.pack())->newEpoch == 9);
  }
}

TEST_CASE(body_malformed_rejects) {
  // Wrong sizes.
  CHECK(!AckBody::parse(randomBytes(6)).has_value());
  CHECK(!AckBody::parse(randomBytes(8)).has_value());
  CHECK(!ModeChangeBody::parse(randomBytes(2)).has_value());
  CHECK(!KeepaliveBody::parse(randomBytes(2)).has_value());
  CHECK(!ResumeReqBody::parse(randomBytes(9)).has_value());
  CHECK(!DataBody::parse({}).has_value());
  // Range violations.
  CHECK(!StartNakBody::parse(std::vector<uint8_t>{0}).has_value());
  CHECK(!StartNakBody::parse(std::vector<uint8_t>{9}).has_value());
  CHECK(!DiscBody::parse(std::vector<uint8_t>{200}).has_value());
  CHECK(!VoiceOpenBody::parse(std::vector<uint8_t>{7}).has_value());
  {
    ModeChangeBody b{13, 3, 0};  // cpMode 3 invalid
    CHECK(!ModeChangeBody::parse(b.pack()).has_value());
  }
  {
    CallBody b;  // reserved caps bit
    b.srcCall = "W1AW";
    b.dstCall = "K6XYZ";
    b.caps = 0x0100;
    CHECK(!CallBody::parse(b.pack()).has_value());
  }
  {
    MaskUpdateBody b;
    b.family = 7;
    CHECK(!MaskUpdateBody::parse(b.pack()).has_value());
  }
  {
    BcastDataBody b;
    b.fragIndex = 4;
    b.fragCount = 3;  // index >= count
    CHECK(!BcastDataBody::parse(b.pack()).has_value());
  }
  {
    // Bad bcast magic.
    BcastDataBody b;
    b.payload = {1, 2, 3};
    auto raw = b.pack();
    raw[0] = 0x00;
    CHECK(!BcastDataBody::parse(raw).has_value());
  }
  // M0-M3 are live wire values as of 6.2 (CSS tier): they parse intact.
  {
    StartBody b;
    b.initialMode = 2;
    auto p = StartBody::parse(b.pack());
    CHECK(p && p->initialMode == 2);
  }
  // Out-of-table modes still reject.
  {
    StartBody b;
    b.initialMode = uint8_t(kNumModes);
    CHECK(!StartBody::parse(b.pack()).has_value());
  }
}

TEST_CASE(fuzz_no_crash_clean_reject) {
  int accepted = 0;
  for (int iter = 0; iter < 4000; ++iter) {
    auto buf = randomBytes(nextRng() % 96);
    if (parseFrame(buf)) ++accepted;
    // Body parsers on raw garbage.
    (void)CallBody::parse(buf);
    (void)CallAckBody::parse(buf);
    (void)StartBody::parse(buf);
    (void)StartAckBody::parse(buf);
    (void)StartNakBody::parse(buf);
    (void)DataBody::parse(buf);
    (void)AckBody::parse(buf);
    (void)ModeChangeBody::parse(buf);
    (void)ModeAckBody::parse(buf);
    (void)ProbeBody::parse(buf);
    (void)ProbeAckBody::parse(buf);
    (void)MaskUpdateBody::parse(buf);
    (void)MaskAckBody::parse(buf);
    (void)KeepaliveBody::parse(buf);
    (void)ResumeReqBody::parse(buf);
    (void)ResumeAckBody::parse(buf);
    (void)DiscBody::parse(buf);
    (void)BeaconBody::parse(buf);
    (void)CqBody::parse(buf);
    (void)BcastDataBody::parse(buf);
    (void)VoiceOpenBody::parse(buf);
    (void)KeyUpdateBody::parse(buf);
  }
  // Random garbage passing the CRC-32 + type + flag gates should be
  // essentially impossible in 4000 draws.
  CHECK_EQ(accepted, 0);

  // Mutation fuzz: valid frames with random single-byte damage must
  // never crash and (almost) always reject; accepted mutants must at
  // least carry a valid type (CRC collision is the only path in).
  LinkEnvelope e;
  e.type = LinkType::Call;
  CallBody cb;
  cb.srcCall = "W1AW";
  cb.dstCall = "K6XYZ";
  auto base = packFrame(e, cb.pack());
  for (int iter = 0; iter < 4000; ++iter) {
    auto mut = base;
    mut[nextRng() % mut.size()] ^= uint8_t(1 + (nextRng() % 255));
    auto p = parseFrame(mut);
    if (p) {
      CHECK(isKnownLinkType(uint8_t(p->env.type)));
    }
  }
}

TEST_CASE(sequence_space_rule) {
  CHECK(usesVoiceSequence(LinkType::VoiceData));
  CHECK(!usesVoiceSequence(LinkType::Data));
  CHECK(!usesVoiceSequence(LinkType::Ack));
  CHECK(!usesVoiceSequence(LinkType::VoiceOpen));  // control is ARQ space
}

SKYTEST_MAIN
