// Handshake policy (ROADMAP 2.3): happy path, every NAK reason, ULTRA
// fallback schedule, quality-seeded initial mode.
#include <string>

#include "link/amc.hpp"
#include "link/handshake.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

HandshakePolicy stationA() {
  HandshakePolicy p;
  p.ownCall = "W1AW";
  p.family = Family::Wide;
  p.caps = kCapZstd | kCapResume | kCapVoice;
  return p;
}

HandshakePolicy stationB() {
  HandshakePolicy p;
  p.ownCall = "K6XYZ";
  p.family = Family::Wide;
  p.caps = kCapZstd | kCapResume | kCapHarqIr;
  return p;
}

}  // namespace

TEST_CASE(initial_mode_seeding) {
  // At the M4 fading floor + margin: M4 (or a same-floor neighbor - the
  // measured WIDE table has M4 and M5 at equal floors, and picking the
  // faster of two equally-robust modes is correct).
  const double margin = AmcConfig{}.promoteMarginDb;  // one operating point
  double m4 = fadingFloorDb(Family::Wide, 4);
  int atFloor = initialModeFromQuality(Family::Wide, m4 + margin, 0, 13);
  CHECK(atFloor >= 4 && atFloor <= 5);
  // Below the M6 requirement, M6+ is never proposed.
  double m6 = fadingFloorDb(Family::Wide, 6);
  CHECK(initialModeFromQuality(Family::Wide, m6 + margin - 0.1, 0, 13) < 6);
  // Very low SNR still yields M4 (never below the ladder).
  CHECK_EQ(int(initialModeFromQuality(Family::Wide, -30.0, 0, 13)), 4);
  // Rich SNR caps at M10 (fragile modes need live gates), and respects
  // the config cap.
  CHECK_EQ(int(initialModeFromQuality(Family::Wide, 60.0, 0, 13)),
           int(kInitialModeCap));
  CHECK_EQ(int(initialModeFromQuality(Family::Wide, 60.0, 0, 7)), 7);
  // Monotone in SNR across the ladder.
  int prev = 4;
  for (double snr = -10; snr <= 40; snr += 1.0) {
    int m = initialModeFromQuality(Family::Wide, snr, 0, 13);
    CHECK(m >= prev);
    prev = m;
  }
  // kFadingNA rows are never proposed even with absurd SNR (NARROW M13
  // is NA; cap already prevents it, so check via the NA row directly:
  // a maxMode of 13 with 99-dB SNR still respects the cap).
  CHECK(initialModeFromQuality(Family::Narrow, 99.0, 0, 13) <=
        kInitialModeCap);
}

TEST_CASE(happy_path_negotiation) {
  auto A = stationA();
  auto B = stationB();

  CallBody call = makeCall(A, "K6XYZ");
  CHECK(call.srcCall == "W1AW" && call.dstCall == "K6XYZ");
  CHECK_EQ(call.version, kProtocolVersion);

  // B measures the CALL at 12.6 dB, CFO -3.2 Hz.
  QualityMeasurement bMeas{12.6, 20, -3.2};
  auto ackOpt = respondToCall(B, call, bMeas);
  CHECK(ackOpt.has_value());
  CHECK_EQ(int(ackOpt->snrHalfDb), 25);   // 12.6 * 2 rounded
  CHECK_EQ(int(ackOpt->cfoHalfHz), -6);   // -3.2 / 0.5 rounded
  CHECK_EQ(ackOpt->caps, B.caps);

  // A measures the CALL_ACK at 15 dB; effective = min(12.5, 15).
  QualityMeasurement aMeas{15.0, 10, 3.0};
  StartBody start = makeStart(A, *ackOpt, aMeas);
  CHECK_EQ(start.caps, uint16_t(A.caps & B.caps));  // zstd + resume
  CHECK_EQ(int(start.initialMode),
           int(initialModeFromQuality(Family::Wide, 12.5, 20, 13)));
  CHECK_EQ(int(start.macProfile), int(macProfileForMode(start.initialMode)));

  // B accepts; final mode may only go DOWN from the proposal.
  auto res = evaluateStart(B, call, start, bMeas);
  CHECK(std::holds_alternative<StartAckBody>(res));
  auto& ok = std::get<StartAckBody>(res);
  CHECK(ok.finalMode <= start.initialMode);
  CHECK(ok.finalMode >= kFirstMode);
}

TEST_CASE(responder_silence_paths) {
  auto B = stationB();
  QualityMeasurement q{10, 0, 0};
  // Not addressed to us.
  {
    auto call = makeCall(stationA(), "VE3AAA");
    CHECK(!respondToCall(B, call, q).has_value());
  }
  // Placeholder callsign involved.
  {
    auto A = stationA();
    A.ownCall = kPlaceholderCall;
    auto call = makeCall(A, "K6XYZ");
    CHECK(!respondToCall(B, call, q).has_value());
  }
  // ULTRA call to a non-ULTRA station.
  {
    auto A = stationA();
    A.family = Family::Ultra;
    auto call = makeCall(A, "K6XYZ");
    CHECK(!respondToCall(B, call, q).has_value());
    auto Bu = stationB();
    Bu.ultraEnable = true;
    CHECK(respondToCall(Bu, call, q).has_value());
  }
}

TEST_CASE(every_nak_reason) {
  auto A = stationA();
  auto B = stationB();
  QualityMeasurement q{12, 0, 0};
  CallBody call = makeCall(A, "K6XYZ");
  CallAckBody ack = *respondToCall(B, call, q);
  StartBody start = makeStart(A, ack, q);

  auto reason = [&](const HandshakePolicy& p, const CallBody& c,
                    const StartBody& s) -> std::optional<StartNakReason> {
    auto r = evaluateStart(p, c, s, q);
    if (std::holds_alternative<StartNakBody>(r)) {
      return std::get<StartNakBody>(r).reason;
    }
    return std::nullopt;
  };

  // Version mismatch.
  {
    CallBody c2 = call;
    c2.version = 2;
    CHECK(reason(B, c2, start) == StartNakReason::VersionMismatch);
  }
  // Family mismatch.
  {
    CallBody c2 = call;
    c2.family = uint8_t(Family::Narrow);
    CHECK(reason(B, c2, start) == StartNakReason::FamilyMismatch);
  }
  // WIDE call must land on an ULTRA-configured responder (fallback).
  {
    auto Bu = stationB();
    Bu.family = Family::Ultra;
    Bu.ultraEnable = true;
    CHECK(!reason(Bu, call, start).has_value());
  }
  // Busy.
  {
    auto Bb = stationB();
    Bb.busy = true;
    CHECK(reason(Bb, call, start) == StartNakReason::Busy);
  }
  // Policy refusal.
  {
    auto Bp = stationB();
    Bp.policyRefuse = true;
    CHECK(reason(Bp, call, start) == StartNakReason::PolicyRefusal);
  }
  // Capability conflict: initiator commits something B never offered.
  {
    StartBody s2 = start;
    s2.caps = kCapVoice;  // B does not advertise voice
    CHECK(reason(B, call, s2) == StartNakReason::CapabilityConflict);
  }
}

TEST_CASE(ultra_fallback_schedule) {
  auto A = stationA();
  A.family = Family::Ultra;
  A.ultraEnable = true;
  // Fallback on: first 2 of 4 attempts ULTRA, rest WIDE.
  CHECK(callFamilyForAttempt(A, 0, 4) == Family::Ultra);
  CHECK(callFamilyForAttempt(A, 1, 4) == Family::Ultra);
  CHECK(callFamilyForAttempt(A, 2, 4) == Family::Wide);
  CHECK(callFamilyForAttempt(A, 3, 4) == Family::Wide);
  // Fallback off: all ULTRA.
  A.familyFallback = false;
  CHECK(callFamilyForAttempt(A, 3, 4) == Family::Ultra);

  // EVERY family now steps one rung down for its later attempts, not just
  // ULTRA. This was previously ULTRA-only because a responder could not
  // hear a narrower call at all; with the receiver-side family scan in
  // LinkEngine::rxBurst the narrower attempts actually land, and a
  // narrower family is the measured way to close a marginal handshake
  // (docs/conformance/handshake_chain.txt).
  auto W = stationA();
  CHECK(W.family == Family::Wide);
  CHECK(callFamilyForAttempt(W, 0, 4) == Family::Wide);
  CHECK(callFamilyForAttempt(W, 1, 4) == Family::Wide);
  CHECK(callFamilyForAttempt(W, 2, 4) == Family::Mid);
  CHECK(callFamilyForAttempt(W, 3, 4) == Family::Mid);
  W.familyFallback = false;
  CHECK(callFamilyForAttempt(W, 3, 4) == Family::Wide);

  // NARROW has nowhere to go and must stay put.
  auto N = stationA();
  N.family = Family::Narrow;
  CHECK(callFamilyForAttempt(N, 3, 4) == Family::Narrow);
  CHECK(narrowerFamily(Family::Narrow) == Family::Narrow);
  CHECK(narrowerFamily(Family::Ultra) == Family::Wide);
  CHECK(narrowerFamily(Family::Wide) == Family::Mid);
  CHECK(narrowerFamily(Family::Mid) == Family::Narrow);
}

TEST_CASE(final_mode_only_lowers) {
  auto B = stationB();
  QualityMeasurement poor{fadingFloorDb(Family::Wide, 4) + 3.0, 0, 0};
  CallBody call = makeCall(stationA(), "K6XYZ");
  StartBody start;
  start.initialMode = 9;  // optimistic proposal
  auto r = evaluateStart(B, call, start, poor);
  CHECK(std::holds_alternative<StartAckBody>(r));
  // B's own view at ~floor(M4)+3 justifies only the bottom rungs; the
  // optimistic M9 proposal must be LOWERED to B's own assessment.
  uint8_t ownView = initialModeFromQuality(Family::Wide, poor.snrDb,
                                           poor.selectivity, B.maxMode);
  CHECK_EQ(int(std::get<StartAckBody>(r).finalMode), int(ownView));
  CHECK(std::get<StartAckBody>(r).finalMode < 9);
}


// Channel memory clamps the handshake on BOTH sides (LINK_ADAPTATION.md:
// "the LOWER of the two wins"). Before this was wired, the store was built,
// tested, and never consulted by anything.
TEST_CASE(chanmem_seed_clamps_the_initial_mode) {
  HandshakePolicy p;
  p.ownCall = "W1AW";
  p.family = Family::Wide;
  p.caps = kCapZstd;
  p.maxMode = 13;

  CallAckBody peerAck;
  peerAck.caps = kCapZstd;
  peerAck.snrHalfDb = int8_t(40);   // +20 dB: a healthy channel
  peerAck.selectivity = 0;
  QualityMeasurement own;
  own.snrDb = 20.0;
  own.selectivity = 0;

  const uint8_t unclamped = makeStart(p, peerAck, own).initialMode;
  CHECK(unclamped > 4);  // a clean measurement justifies a fast mode

  // A memory of trouble on this channel pulls the proposal down...
  p.chanMemMode = 5;
  CHECK_EQ(int(makeStart(p, peerAck, own).initialMode), 5);

  // ...but never UP: the store only ever lowers.
  p.chanMemMode = 13;
  CHECK_EQ(int(makeStart(p, peerAck, own).initialMode), int(unclamped));
}

TEST_CASE(chanmem_seed_clamps_the_responder_view) {
  HandshakePolicy p;
  p.ownCall = "K6XYZ";
  p.family = Family::Wide;
  p.caps = kCapZstd;
  p.maxMode = 13;

  CallBody call;
  call.srcCall = "W1AW";
  call.dstCall = "K6XYZ";
  call.version = kProtocolVersion;
  call.family = uint8_t(Family::Wide);
  call.caps = kCapZstd;

  StartBody start;
  start.caps = kCapZstd;
  start.initialMode = 9;
  start.maxMode = 13;

  QualityMeasurement good;
  good.snrDb = 20.0;
  good.selectivity = 0;

  // Without memory the responder accepts the (lower) proposal.
  auto r = evaluateStart(p, call, start, good);
  CHECK(std::holds_alternative<StartAckBody>(r));
  CHECK_EQ(int(std::get<StartAckBody>(r).finalMode), 9);

  // With a memory of trouble, its own view drags the session lower still.
  p.chanMemMode = 6;
  r = evaluateStart(p, call, start, good);
  CHECK(std::holds_alternative<StartAckBody>(r));
  CHECK_EQ(int(std::get<StartAckBody>(r).finalMode), 6);
}

SKYTEST_MAIN
