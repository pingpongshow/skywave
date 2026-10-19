// Two-modem integration harness (ROADMAP 2.7, Milestone M2): two
// in-process LinkEngines over two independent chansim directions.
// Scenarios per TESTING.md: connect_floor, bulk_transfer (with injected
// loss), disconnect_paths. CI runs a scaled bulk size; the full 100 KiB
// soak is a manual/nightly run of the same scenario.
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "link/engine.hpp"
#include "link/monitor.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

// Incompressible test payloads: with zstd negotiated (4.1), periodic
// patterns would compress to almost nothing and stop exercising ARQ.
std::vector<uint8_t> noiseBlob(size_t n, uint64_t seed) {
  std::vector<uint8_t> v(n);
  uint64_t s = seed * 2654435761u + 1;
  for (auto& b : v) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    b = uint8_t(s >> 32);
  }
  return v;
}

struct Pair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

  explicit Pair(double snrDb, uint64_t seed = 77,
                Family fam = Family::Wide)
      : a(makeCfg("W1AW", fam)), b(makeCfg("K6XYZ", fam)) {
    chanAB.fading = false;
    chanAB.snr3kDb = snrDb;
    chanBA = chanAB;
    chanAB.seed = seed;
    chanBA.seed = seed + 5000;
  }

  static EngineConfig makeCfg(const char* call, Family fam) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.family = fam;
    c.policy.caps = kCapZstd | kCapResume | kCapMasking;
    return c;
  }

  // One half-duplex exchange step: poll both engines; deliver at most
  // one burst per direction through its channel. Time advances by the
  // burst airtime (plus turnaround).
  void step(bool dropAB = false, bool dropBA = false) {
    auto burstA = a.tick(now);
    auto burstB = b.tick(now);
    double dt = 0.05;
    if (burstA) {
      dt += double(burstA->size()) / kFsBaseband;
      if (!dropAB) {
        std::vector<cfloat> x(1500, cfloat(0, 0));
        x.insert(x.end(), burstA->begin(), burstA->end());
        x.insert(x.end(), 2000, cfloat(0, 0));
        ChansimParams cp = chanAB;
        cp.seed = chanAB.seed + (seedCtr++) * 7919;
        auto y = Chansim::run(cp, x);
        b.rxBurst(y, now);
      }
    }
    if (burstB) {
      dt += double(burstB->size()) / kFsBaseband;
      if (!dropBA) {
        std::vector<cfloat> x(1500, cfloat(0, 0));
        x.insert(x.end(), burstB->begin(), burstB->end());
        x.insert(x.end(), 2000, cfloat(0, 0));
        ChansimParams cp = chanBA;
        cp.seed = chanBA.seed + (seedCtr++) * 7919;
        auto y = Chansim::run(cp, x);
        a.rxBurst(y, now);
      }
    }
    now += dt;
  }

  bool connect(int maxSteps = 400) {
    a.connectTo("K6XYZ", now);
    for (int i = 0; i < maxSteps; ++i) {
      step();
      if (a.state() == FsmState::Connected &&
          b.state() == FsmState::Connected) {
        return true;
      }
      if (a.state() == FsmState::Idle && i > 4) return false;
    }
    return false;
  }
};

}  // namespace

// A caller that stepped down the bandwidth ladder must still be HEARD.
// Before the receiver-side family scan, a listener only ever demodulated
// its own family, so `callFamilyForAttempt()` - which had existed since
// step 2.3 - could never actually land a narrower call.
TEST_CASE(m2_connect_across_families) {
  struct MixedPair : Pair {
    MixedPair() : Pair(30.0, 91) {
      // Rebuild the two engines on DIFFERENT families: the caller on MID,
      // the listener configured for WIDE.
      a = LinkEngine(makeCfg("W1AW", Family::Mid));
      b = LinkEngine(makeCfg("K6XYZ", Family::Wide));
    }
  };
  MixedPair p;
  CHECK(p.connect(400));
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.b.state() == FsmState::Connected);

  // And the session must actually carry traffic on the adopted family.
  const std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};
  p.a.sendData(msg);
  std::vector<uint8_t> got;
  for (int i = 0; i < 400 && got.size() < msg.size(); ++i) {
    p.step();
    auto rx = p.b.takeReceivedData();
    got.insert(got.end(), rx.begin(), rx.end());
  }
  CHECK(got == msg);
}

// The scan must not fire when it is switched off: an operator who pins a
// family expects the modem to stay there.
TEST_CASE(m2_family_scan_respects_fallback_off) {
  struct PinnedPair : Pair {
    PinnedPair() : Pair(30.0, 92) {
      EngineConfig ca = makeCfg("W1AW", Family::Mid);
      EngineConfig cb = makeCfg("K6XYZ", Family::Wide);
      cb.policy.familyFallback = false;   // listener pinned to WIDE
      ca.policy.familyFallback = false;   // caller pinned to MID
      a = LinkEngine(std::move(ca));
      b = LinkEngine(std::move(cb));
    }
  };
  PinnedPair p;
  CHECK(!p.connect(60));   // deaf by configuration, as asked
}


// Responder-side mode pinning. The responder must NOT drive MODE_CHANGE
// itself (two controllers collided in the harness - mode oscillation and
// pin override), so it asks via the ACK's rx_flags byte and the
// initiator remains the only controller on the air.
TEST_CASE(m2_responder_side_mode_pin) {
  Pair p(30.0, 5150);
  CHECK(p.connect(400));
  // Keep traffic flowing: a pin request rides the ACK's rx_flags, and
  // ACKs exist because data does.
  const std::vector<uint8_t> chunk(64, 0x5A);
  for (int i = 0; i < 60; ++i) {
    if (p.a.txBacklog() < 256) p.a.sendData(chunk);
    p.step();
  }

  // b is the responder (a called it).
  CHECK(p.b.pinMode(6, p.now));

  bool applied = false;
  for (int i = 0; i < 400 && !applied; ++i) {
    if (p.a.txBacklog() < 256) p.a.sendData(chunk);
    p.step();
    applied = p.a.sessionMode() == 6 && p.b.sessionMode() == 6;
  }
  CHECK(applied);              // the initiator honoured the request
  CHECK(p.a.modePinned());     // and it is a real pin, not a demote
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.b.state() == FsmState::Connected);

  // Releasing from the responder frees the initiator's pin too.
  p.b.unpinMode();
  bool freed = false;
  for (int i = 0; i < 400 && !freed; ++i) {
    if (p.a.txBacklog() < 256) p.a.sendData(chunk);
    p.step();
    freed = !p.a.modePinned();
  }
  CHECK(freed);
}

// BIDIRECTIONAL FAIRNESS. Standalone ACKs are control frames and outrank
// DATA, so on a two-way link the side that is BEHIND owes an ACK for
// every burst it receives and spends every transmit opportunity
// acknowledging, while the side that is ahead keeps sending - positive
// feedback that ends with one direction at zero. Observed in the churn
// soak as B->A 8500 bytes against A->B 0, both ends healthy at M10.
//
// The soak is real-time paced and too noisy to gate on; this runs the
// same traffic pattern on simulated time, so it is deterministic.
// LOST MODE_ACK MUST NOT SPLIT THE LINK. The responder applies a
// MODE_CHANGE the moment it receives one; the initiator applies only
// when the MODE_ACK returns. So if every MODE_ACK is lost, the
// initiator exhausts its three retries and gives up while the responder
// has ALREADY moved - and the two ends sit on different modes for the
// rest of the session, with ARQ window, timeouts and AMC all diverging.
// Observed live in the floor soak as A on M4 against B on M1.
//
// This is the deterministic instrument for that bug. Soak throughput is
// far too sensitive to attribute a change this small - two attempts at
// fixing it were "measured" against churn and both readings turned out
// to be confounded (see ROADMAP).
TEST_CASE(m2_mode_change_survives_lost_mode_ack) {
  Pair p(30.0, 31337);
  CHECK(p.connect(400));
  const int start = p.a.sessionMode();
  CHECK(p.a.sessionMode() == p.b.sessionMode());

  // Ask for a definite mode change, then black out the REVERSE path so
  // every MODE_ACK is lost while the initiator burns its retry budget.
  CHECK(p.a.pinMode(start >= 6 ? 5 : 6, p.now));
  // Long enough for the initiator's three MODE_CHANGE retries to expire,
  // short enough that the session itself survives: a reverse path held
  // down past the ARQ retry budget is a DEAD link, and tearing that down
  // is correct behaviour rather than the bug under test here.
  for (int i = 0; i < 400; ++i) p.step(false, true);

  // Reverse path restored: the link must reconverge on a single mode.
  for (int i = 0; i < 1200; ++i) p.step();
  std::printf("lost mode_ack: a=%d b=%d (start %d)\n", p.a.sessionMode(),
              p.b.sessionMode(), start);
  CHECK(p.a.sessionMode() == p.b.sessionMode());
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.b.state() == FsmState::Connected);
}

TEST_CASE(m2_bidirectional_bulk_is_fair) {
  Pair p(30.0, 24680);
  CHECK(p.connect(400));

  const std::vector<uint8_t> chunk(200, 0xC3);
  size_t aToB = 0, bToA = 0;
  for (int i = 0; i < 3000; ++i) {
    // BOTH ends keep a backlog: the case that starved.
    if (p.a.txBacklog() < 800) p.a.sendData(chunk);
    if (p.b.txBacklog() < 800) p.b.sendData(chunk);
    p.step();
    aToB += p.b.takeReceivedData().size();
    bToA += p.a.takeReceivedData().size();
  }
  const size_t lo = std::min(aToB, bToA), hi = std::max(aToB, bToA);
  std::printf("bidirectional: A->B=%zu B->A=%zu (ratio %.2f)\n", aToB, bToA,
              hi > 0 ? double(lo) / double(hi) : 0.0);

  CHECK(p.a.state() == FsmState::Connected);
  CHECK(aToB > 0);   // neither direction may be starved outright
  CHECK(bToA > 0);
  // Neither side may take more than ~4x the other. Perfect symmetry is
  // not the goal - half-duplex turn-taking is lumpy - but an order of
  // magnitude means one side has captured the channel.
  CHECK(hi <= 4 * lo);
}

// One burst reaching the receiver through two overlapping captures (the
// detector matched it twice) must be dispatched once; the same bytes a
// whole airtime later are a genuine repeat.
TEST_CASE(m2_overlapping_captures_dispatch_once) {
  Pair p(20.0);
  CHECK(p.connect(400));
  p.a.sendData(noiseBlob(40, 3));
  std::optional<std::vector<cfloat>> burst;
  for (int i = 0; i < 40 && !burst; ++i) {
    burst = p.a.tick(p.now);
    p.now += 0.05;
  }
  CHECK(burst.has_value());
  std::vector<cfloat> x(1500, cfloat(0, 0));
  x.insert(x.end(), burst->begin(), burst->end());
  x.insert(x.end(), 2000, cfloat(0, 0));
  ChansimParams cp = p.chanAB;
  const auto y = Chansim::run(cp, x);
  const double start = 100.0;
  const uint64_t ok0 = p.b.framesOk();
  CHECK(p.b.rxBurst(y, p.now, true, start));
  const uint64_t ok1 = p.b.framesOk();
  CHECK(ok1 == ok0 + 1);
  // A second capture of the SAME burst: its window opens 1000 samples
  // later over the same audio, so the sync point is the same instant.
  const size_t k = 1000;
  CHECK(p.b.rxBurst(std::span<const cfloat>(y).subspan(k), p.now, true,
                    start + double(k) / kFsBaseband));
  CHECK(p.b.framesOk() == ok1);
  // The same bytes a whole airtime later (an identical ACK sent back to
  // back, a re-sent CALL) are a new burst.
  const double air = double(burst->size()) / kFsBaseband;
  CHECK(p.b.rxBurst(y, p.now, true, start + air + 0.2));
  CHECK(p.b.framesOk() == ok1 + 1);
}

// Both ends run the session under the initiator's non-zero id, and an
// in-session frame from another pair (different id) is ignored: another
// link's DISC on the same channel used to tear ours down (every session
// ran as id 0).
TEST_CASE(m2_session_id_filters_other_pairs) {
  Pair p(20.0);
  CHECK(p.connect(400));
  CHECK(p.a.sessionId() != 0);
  CHECK_EQ(int(p.a.sessionId()), int(p.b.sessionId()));
  Pair q(20.0, 91);
  q.now = 777.0;  // a different session id
  CHECK(q.connect(400));
  CHECK(q.a.sessionId() != p.a.sessionId());
  q.b.disconnect(q.now);
  std::optional<std::vector<cfloat>> disc;
  for (int i = 0; i < 40 && !disc; ++i) {
    disc = q.b.tick(q.now);
    q.now += 0.05;
  }
  CHECK(disc.has_value());
  std::vector<cfloat> x(1500, cfloat(0, 0));
  x.insert(x.end(), disc->begin(), disc->end());
  x.insert(x.end(), 2000, cfloat(0, 0));
  ChansimParams cp = p.chanBA;
  p.a.rxBurst(Chansim::run(cp, x), p.now);
  for (int i = 0; i < 20; ++i) p.step();
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.b.state() == FsmState::Connected);
}

// A CALL from a third station while a session is up must not touch it:
// it used to overwrite the peer, drop the initiator role and switch the
// session's family.
TEST_CASE(m2_third_party_call_does_not_hijack) {
  Pair p(20.0);
  CHECK(p.connect(400));
  LinkEngine c(Pair::makeCfg("N0XYZ", Family::Wide));
  c.connectTo("K6XYZ", p.now);
  std::optional<std::vector<cfloat>> call;
  for (int i = 0; i < 40 && !call; ++i) call = c.tick(p.now + 0.05 * i);
  CHECK(call.has_value());
  std::vector<cfloat> x(1500, cfloat(0, 0));
  x.insert(x.end(), call->begin(), call->end());
  x.insert(x.end(), 2000, cfloat(0, 0));
  ChansimParams cp = p.chanAB;
  p.b.rxBurst(Chansim::run(cp, x), p.now);
  CHECK(p.b.remoteCall() == "W1AW");
  CHECK(p.b.state() == FsmState::Connected);
  const auto msg = noiseBlob(300, 5);
  p.a.sendData(msg);
  std::vector<uint8_t> got;
  for (int i = 0; i < 400 && got.size() < msg.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == msg);
}

TEST_CASE(m2_connect_clean) {
  Pair p(30.0);
  CHECK(p.connect());
  CHECK(p.a.sessionMode() == p.b.sessionMode());
  CHECK(p.a.sessionMode() >= kFirstMode);
}

// Turn-taking: the initiator connects holding ISS; a responder with
// data requests the turn via its ACKs, receives TURN_GRANT, and its
// data flows - the reverse direction of the roles the connect assigned.
TEST_CASE(m2_turnsched_grant_handover) {
  Pair p(30.0, 5150);
  CHECK(p.connect(400));
  CHECK(p.a.turnSchedActive());
  CHECK(p.a.turnIss());
  CHECK(!p.b.turnIss());

  auto blob = noiseBlob(4 * 1024, 99);
  p.b.sendData(blob);  // only the IRS has traffic
  std::vector<uint8_t> got;
  for (int i = 0; i < 4000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.a.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK_EQ(got.size(), blob.size());
  CHECK(got == blob);
  CHECK(p.b.turnIss());  // the grant moved the sending role to B
}

// Turn-taking: a grant series lost to the channel must not leave the
// link with two listeners - the granting side reclaims ISS after its
// retries and traffic continues once the channel heals.
TEST_CASE(m2_turnsched_grant_loss_reclaims) {
  Pair p(30.0, 6210);
  CHECK(p.connect(400));
  auto blob = noiseBlob(2 * 1024, 17);
  p.b.sendData(blob);
  // Drop everything A->B while the break/grant exchange happens: B's
  // break request arrives at A, every TURN_GRANT A answers with is lost.
  for (int i = 0; i < 700; ++i) p.step(/*dropAB=*/true);
  // Channel heals. Whatever role state the loss left behind, B's data
  // must still be deliverable end to end.
  std::vector<uint8_t> got;
  for (int i = 0; i < 5000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.a.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK_EQ(got.size(), blob.size());
  CHECK(got == blob);
  CHECK(p.a.state() == FsmState::Connected);
}

TEST_CASE(m2_connect_floor) {
  // Handshake success >= 90% at -8 dB WIDE (AWGN; the handshake rides
  // calling chirp + robust header + M4).
  int ok = 0;
  const int trials = 10;
  for (int t = 0; t < trials; ++t) {
    Pair p(-8.0, 1000 + uint64_t(t) * 331);
    if (p.connect()) ++ok;
  }
  std::printf("connect_floor: %d/%d at -8 dB\n", ok, trials);
  CHECK(ok >= 9);
}

TEST_CASE(m2_bulk_transfer) {
  // Error-free bulk at two SNRs (scaled size for CI; same scenario runs
  // 100 KiB in nightly soak).
  for (double snr : {25.0, 12.0}) {
    Pair p(snr, 42 + uint64_t(snr));
    CHECK(p.connect());
    auto blob = noiseBlob(12 * 1024, 17 + uint64_t(snr));
    p.a.sendData(blob);
    std::vector<uint8_t> got;
    for (int i = 0; i < 4000 && got.size() < blob.size(); ++i) {
      p.step();
      auto d = p.b.takeReceivedData();
      got.insert(got.end(), d.begin(), d.end());
      if (p.a.state() != FsmState::Connected) break;
    }
    CHECK_EQ(int(got.size()), int(blob.size()));
    CHECK(got == blob);
  }
}

TEST_CASE(m2_bulk_transfer_with_loss) {
  // Drop 1 in 7 A->B bursts: ARQ must recover byte-exact.
  Pair p(25.0, 99);
  CHECK(p.connect());
  auto blob = noiseBlob(6 * 1024, 37);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  int n = 0;
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    bool drop = (++n % 7) == 0;
    p.step(drop, false);
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK_EQ(int(got.size()), int(blob.size()));
  CHECK(got == blob);
  CHECK(p.a.state() == FsmState::Connected);
}

TEST_CASE(m2_disconnect_paths) {
  // Orderly teardown: DISC/DISC_ACK both sides.
  {
    Pair p(25.0, 7);
    CHECK(p.connect());
    p.a.disconnect(p.now);
    for (int i = 0; i < 60 && (p.a.state() != FsmState::Idle ||
                               p.b.state() != FsmState::Idle);
         ++i) {
      p.step();
    }
    CHECK(p.a.state() == FsmState::Idle);
    CHECK(p.b.state() == FsmState::Idle);
    CHECK(p.a.lastDiscCause() == FsmDiscCause::LocalRequest);
    CHECK(p.b.lastDiscCause() == FsmDiscCause::PeerRequest);
  }
  // Unilateral: peer vanishes mid-session; DISC retries exhaust.
  {
    Pair p(25.0, 8);
    CHECK(p.connect());
    p.a.disconnect(p.now);
    for (int i = 0; i < 400 && p.a.state() != FsmState::Idle; ++i) {
      p.step(true, true);  // nothing gets through
      p.now += 2.0;        // let retry cadences fire
    }
    CHECK(p.a.state() == FsmState::Idle);
    // The operator asked for the disconnect; an unanswered DISC does not
    // make it a link failure: the cause stays LocalRequest and the resume
    // token is gone (it used to survive as "DiscRetriesExhausted").
    CHECK(p.a.lastDiscCause() == FsmDiscCause::LocalRequest);
    CHECK(!p.a.hasResumeContext());
  }
  // Idle timeout: connected, then total silence for 120 s.
  {
    Pair p(25.0, 9);
    CHECK(p.connect());
    for (int i = 0; i < 300 && p.b.state() != FsmState::Idle; ++i) {
      p.now += 5.0;
      (void)p.b.tick(p.now);
    }
    CHECK(p.b.state() == FsmState::Idle);
    CHECK(p.b.lastDiscCause() == FsmDiscCause::IdleTimeout);
  }
}

TEST_CASE(m3_mode_pin_scenario) {
  // Pin M6 on A: coordinated MODE_CHANGE lands on both sides; no
  // promotion happens afterward despite rich SNR; unpin + traffic lets
  // AMC promote again.
  Pair p(30.0, 55);
  CHECK(p.connect());
  const int before = p.a.sessionMode();
  CHECK(p.a.pinMode(6, p.now));
  for (int i = 0; i < 40 && (p.a.sessionMode() != 6 ||
                             p.b.sessionMode() != 6);
       ++i) {
    p.step();
  }
  CHECK_EQ(p.a.sessionMode(), 6);
  CHECK_EQ(p.b.sessionMode(), 6);
  CHECK(p.a.modePinned());
  // Traffic at high SNR: pinned mode must hold.
  auto blob = noiseBlob(2048, 11);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 1500 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == blob);
  CHECK_EQ(p.a.sessionMode(), 6);
  CHECK_EQ(p.b.sessionMode(), 6);
  (void)before;
  // Unpin: AMC may now promote (dwell + consecutive evals + more data).
  p.a.unpinMode();
  CHECK(!p.a.modePinned());
  std::vector<uint8_t> blob2(4096);
  p.a.sendData(blob2);
  std::vector<uint8_t> got2;
  int stalled = 0;
  size_t lastSize = 0;
  for (int i = 0; i < 6000 && got2.size() < blob2.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got2.insert(got2.end(), d.begin(), d.end());
    stalled = got2.size() == lastSize ? stalled + 1 : 0;
    lastSize = got2.size();
    if (stalled > 800) break;  // genuinely wedged
  }
  CHECK_EQ(int(got2.size()), int(blob2.size()));
  CHECK(p.a.sessionMode() >= 6);
  // Let any in-flight MODE_CHANGE/MODE_ACK exchange settle, then the
  // committed modes must agree.
  for (int i = 0; i < 12; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), p.b.sessionMode());
}

TEST_CASE(m3_promotion_through_probe) {
  // Rich SNR: session starts at the M10 initial cap; promotion into M11
  // must go through a clean PROBE/PROBE_ACK exchange, and the committed
  // modes never diverge.
  Pair p(35.0, 66);
  CHECK(p.connect());
  CHECK(p.a.sessionMode() <= 10);
  auto blob = noiseBlob(16 * 1024, 66);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  int best = p.a.sessionMode();
  for (int i = 0; i < 5000 && got.size() < blob.size(); ++i) {
    p.step();
    best = std::max(best, p.a.sessionMode());
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == blob);
  std::printf("promotion: best mode reached M%d\n", best);
  CHECK(best >= 11);  // crossed the probe gate
  for (int i = 0; i < 12; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), p.b.sessionMode());
}

TEST_CASE(m3_mask_excision) {
  // Persistent narrowband tone in the A->B direction after connect: B's
  // per-carrier SNR collapses around the tone, B sends MASK_UPDATE, A
  // zeroes those carriers on TX, and the transfer completes. The tone
  // sits exactly on a carrier (baseband +300 Hz = bin 8, WIDE idx 40).
  Pair p(16.0, 88);
  CHECK(p.connect());
  CHECK(p.a.pinMode(10, p.now));
  for (int i = 0; i < 40 && p.b.sessionMode() != 10; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), 10);
  // Interference starts.
  p.chanAB.toneHz = 300.0;
  p.chanAB.toneAmp = 0.35;
  // Long enough that the >= 5 s tone-persistence gate trips well before
  // the transfer drains (the 4.1 pipeline moves ~1.5x more bytes/frame).
  auto blob = noiseBlob(24 * 1024, 88);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  bool masked = false;
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    p.step();
    if (!masked && p.a.txMaskedCount() > 0) {
      masked = true;
      std::printf("mask_excision: A masks %d carriers (B rx-side %d)\n",
                  p.a.txMaskedCount(), p.b.rxMaskedCount());
    }
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(masked);                 // the exchange actually happened
  CHECK(p.b.rxMaskedCount() > 0);  // and B expects the masked TX
  CHECK_EQ(int(got.size()), int(blob.size()));
  CHECK(got == blob);
  CHECK(p.a.state() == FsmState::Connected);
}

TEST_CASE(m3_mask_survives_demotion_below_m8) {
  // Regression: a session that established a carrier mask at M10 then
  // demoted into the spread tier (M4-M7, which the TRANSMITTER never
  // masks) must keep moving data. The receiver used to apply the session
  // mask to the payload walk at every mode, so after a mask existed
  // every DATA frame below M8 failed while control frames (header
  // piggyback only) still passed - the churn soak's "usable channel, no
  // progress" stall.
  Pair p(16.0, 89);
  CHECK(p.connect());
  CHECK(p.a.pinMode(10, p.now));
  for (int i = 0; i < 40 && p.b.sessionMode() != 10; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), 10);
  p.chanAB.toneHz = 300.0;
  p.chanAB.toneAmp = 0.35;
  auto blob = noiseBlob(24 * 1024, 89);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  bool masked = false;
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    p.step();
    if (!masked && p.a.txMaskedCount() > 0) masked = true;
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(masked);
  CHECK(p.b.rxMaskedCount() > 0);
  CHECK_EQ(int(got.size()), int(blob.size()));
  // Now demote below the masking tier with the mask still in force on
  // both ends and move a second blob. Both directions: B's TX toward A
  // is masked by A's rx-side mask only at M8+, same rule.
  CHECK(p.a.pinMode(6, p.now));
  for (int i = 0; i < 60 && p.b.sessionMode() != 6; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), 6);
  CHECK_EQ(p.b.sessionMode(), 6);
  CHECK(p.b.rxMaskedCount() > 0);  // the mask did not go away
  auto blob2 = noiseBlob(4 * 1024, 90);
  p.a.sendData(blob2);
  std::vector<uint8_t> got2;
  for (int i = 0; i < 6000 && got2.size() < blob2.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got2.insert(got2.end(), d.begin(), d.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK_EQ(int(got2.size()), int(blob2.size()));
  CHECK(got2 == blob2);
  CHECK(p.a.state() == FsmState::Connected);
}

namespace {

// Airtime to move `bytes` at pinned M13 over a clean 30 dB channel.
double m13TransferAirtime(size_t bytes) {
  Pair p(30.0, 123, Family::Wide);
  if (!p.connect()) return -1;
  if (!p.a.pinMode(13, p.now)) return -1;
  for (int i = 0; i < 40 && p.b.sessionMode() != 13; ++i) p.step();
  auto blob = noiseBlob(bytes, 123);
  const double t0 = p.now;
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  if (got != blob) return -1;
  return p.now - t0;
}

// Payload bytes per second of airtime for one TX unit at M13: a 4-frame
// burst container, or one solo frame (2 codewords per frame either way).
double m13UnitRate(int framesPerBurst) {
  const size_t budget = dataPayloadBudget(13, framesPerBurst,
                                          2 * framesPerBurst);
  std::vector<uint8_t> payload(size_t(framesPerBurst) *
                               (budget + dataFrameOverhead(framesPerBurst)));
  TxFrameParams tp;
  tp.family = Family::Wide;
  tp.sig = ChirpSig::Fast;
  tp.mode = 13;
  tp.frameType = framesPerBurst > 1 ? kBurstContainerFrameType
                                    : uint8_t(LinkType::Data);
  tp.payload = payload;
  return double(framesPerBurst) * double(budget) / frameAirtimeSec(tp);
}

}  // namespace

TEST_CASE(m3_burst_container_throughput) {
  // One preamble/sync/header per 4-frame burst at M13 must beat the
  // per-frame overhead of single-frame transmission by a wide margin
  // (geometry: the engine always aggregates), and a live transfer must
  // realise a healthy share of the table rate.
  const double unitAgg = m13UnitRate(4), unitSolo = m13UnitRate(1);
  const size_t bytes = 18 * 1024;
  const double tAgg = m13TransferAirtime(bytes);
  CHECK(tAgg > 0);
  const double thruAgg = bytes * 8.0 / tAgg;
  const double net = netRate(Family::Wide, 13, CpMode::Medium);
  std::printf("burst container: live %.0f bps; unit %.0f vs solo %.0f B/s "
              "(x%.2f); net table %.0f bps\n",
              thruAgg, unitAgg, unitSolo, unitAgg / unitSolo, net);
  CHECK(unitAgg > 1.25 * unitSolo);
  CHECK(thruAgg > 0.40 * net);

  // Geometry gate: the container frame's payload-section rate must sit
  // within 10% of NET x (1 - container overhead fraction).
  std::vector<uint8_t> payload(4 * 313);
  TxFrameParams tp;
  tp.family = Family::Wide;
  tp.sig = ChirpSig::Fast;
  tp.mode = 13;
  tp.frameType = kBurstContainerFrameType;
  tp.payload = payload;
  auto g = computeGeometry(tp);
  const double paySamples =
      double(kFftBaseband + cpSamplesAudio(CpMode(1)) / kDecimation);
  // Payload-SECTION rate vs the table NET rate, with the structural
  // codeword-padding allowance (0.80, as in the conformance geometry
  // gate). Piggyback bytes ride the header section and are excluded.
  const double sectionSec = g.payloadSymbols * paySamples / kFsBaseband;
  const double sectionBits = (payload.size() - 30) * 8.0;
  const double eff = sectionBits / sectionSec;
  std::printf("container geometry: payload-section %.0f bps vs NET %.0f\n",
              eff, net);
  CHECK(eff > 0.80 * net);
  CHECK(eff < 1.05 * net);
}

TEST_CASE(m3_tx_freq_alignment) {
  // Physical mistuning: A's dial is 40 Hz high, so A->B arrives at
  // +40 Hz and B->A at -40 Hz. Split correction: each side pre-shifts
  // ~half; both signals end centered and residual RX CFO ~ 0.
  // Bidirectional traffic: CFO reports ride ACKs, so each direction's
  // correction converges only when that direction carries acknowledged
  // traffic (one-way sessions align the data direction only - keepalive
  // reports for the quiet direction are Phase-4 polish).
  // +-24 Hz mistuning: inside the +-30 clamp, so full split correction
  // absorbs it (beyond +-30 is EVT:CFO_WARN operator territory).
  Pair p(25.0, 77);
  p.chanAB.cfoHz = 24.0;
  p.chanBA.cfoHz = -24.0;
  CHECK(p.connect());
  auto blob = noiseBlob(8 * 1024, 77);
  p.a.sendData(blob);
  p.b.sendData(blob);
  std::vector<uint8_t> got, gotA;
  for (int i = 0;
       i < 4000 && (got.size() < blob.size() || gotA.size() < blob.size());
       ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
    auto d2 = p.a.takeReceivedData();
    gotA.insert(gotA.end(), d2.begin(), d2.end());
    if (getenv("SKW_TXA_DEBUG") && i % 100 == 0) {
      std::printf("  i=%d t=%.0f got=%zu gotA=%zu modeA=%d modeB=%d "
                  "backB=%zu stA=%d stB=%d shA=%.1f shB=%.1f\n",
                  i, p.now, got.size(), gotA.size(), p.a.sessionMode(),
                  p.b.sessionMode(), p.b.txBacklog(), int(p.a.state()),
                  int(p.b.state()), p.a.txShiftHz(), p.b.txShiftHz());
    }
  }
  CHECK(got == blob);
  CHECK(gotA == blob);
  std::printf("tx align: shiftA=%.1f shiftB=%.1f rxCfoA=%.1f rxCfoB=%.1f\n",
              p.a.txShiftHz(), p.b.txShiftHz(), p.a.lastRxCfoHz(),
              p.b.lastRxCfoHz());
  // Bootstrap applied the full measured offset at connect; tracking
  // slews keep it there. Both TX now arrive near-centered.
  CHECK(p.a.txShiftHz() < -18.0);
  CHECK(p.b.txShiftHz() > 18.0);
  CHECK(std::abs(p.a.lastRxCfoHz()) < 6.0);
  CHECK(std::abs(p.b.lastRxCfoHz()) < 6.0);
}

TEST_CASE(m3_cp_auto_steps_under_multipath) {
  // 4.5 ms delay spread exceeds the MEDIUM CP (4 ms): CPMODE AUTO must
  // step both sides to LONG and keep the transfer alive.
  Pair p(28.0, 91);
  p.chanAB.fading = true;
  p.chanAB.delaySpreadMs = 4.5;
  p.chanAB.dopplerSpreadHz = 0.1;
  p.chanBA = p.chanAB;
  p.chanBA.seed = p.chanAB.seed + 5000;
  CHECK(p.connect(800));  // handshake fights ISI until CP steps up
  // Sized so the transfer itself yields >= 25 DATA frames, which is what
  // the CP streak requires. 3072 B is only ~13 frames: it used to reach
  // the streak solely because the ARQ timeout was shorter than the round
  // trip and tripled every frame through retransmission. With the
  // measured-RTT timeout that storm is gone, so the payload now has to
  // carry the streak on its own.
  auto blob = noiseBlob(8192, 55);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 60000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  // A lost MODE_ACK can leave a transient CP divergence; the
  // rate-limited retry re-converges it. Give it settle time.
  for (int i = 0; i < 400 && (p.a.cpMode() != CpMode::Long ||
                              p.b.cpMode() != CpMode::Long);
       ++i) {
    p.step();
  }
  std::printf("cp auto: cpA=%d cpB=%d got=%zu\n", int(p.a.cpMode()),
              int(p.b.cpMode()), got.size());
  CHECK(p.a.cpMode() == CpMode::Long);
  CHECK(p.a.cpMode() == p.b.cpMode());
  CHECK(got == blob);
}

TEST_CASE(m3_monitor_third_party) {
  // A passive third station decodes both directions of a session it can
  // hear - the self-describing PHY header is the whole mechanism.
  Pair p(25.0, 44);
  Monitor mon(Family::Wide);
  int monLines = 0, binRecords = 0, sawCall = 0, sawData = 0, sawAck = 0;
  auto feed = [&](const std::vector<cfloat>& y) {
    if (auto ev = mon.rxBurst(y)) {
      ++monLines;
      if (!ev->binaryRecord.empty()) {
        ++binRecords;
        CHECK_EQ(int(ev->binaryRecord[0]), 0xFB);
      }
      if (ev->monLine.find(":1:") != std::string::npos) ++sawCall;
      // Data rides bare DATA frames or MAC burst containers (type 62).
      if (ev->monLine.find(":6:") != std::string::npos ||
          ev->monLine.find(":62:") != std::string::npos) {
        ++sawData;
      }
      // B-direction frames: CALL_ACK / START_ACK / ACK / MODE_ACK.
      if (ev->monLine.find(":2:") != std::string::npos ||
          ev->monLine.find(":4:") != std::string::npos ||
          ev->monLine.find(":7:") != std::string::npos ||
          ev->monLine.find(":9:") != std::string::npos) {
        ++sawAck;
      }
    }
  };
  p.a.connectTo("K6XYZ", p.now);
  auto blob = noiseBlob(2048, 44);
  bool sent = false;
  std::vector<uint8_t> got;
  for (int i = 0; i < 1200 && got.size() < blob.size(); ++i) {
    auto bA = p.a.tick(p.now);
    auto bB = p.b.tick(p.now);
    double dt = 0.05;
    if (bA) {
      dt += double(bA->size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), bA->begin(), bA->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = p.chanAB;
      cp.seed = p.chanAB.seed + (p.seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      p.b.rxBurst(y, p.now);
      feed(y);
    }
    if (bB) {
      dt += double(bB->size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), bB->begin(), bB->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = p.chanBA;
      cp.seed = p.chanBA.seed + (p.seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      p.a.rxBurst(y, p.now);
      feed(y);
    }
    p.now += dt;
    if (!sent && p.a.state() == FsmState::Connected &&
        p.b.state() == FsmState::Connected) {
      p.a.sendData(blob);
      sent = true;
    }
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == blob);
  std::printf("monitor: %d lines, %d bin, call=%d data=%d ack=%d\n",
              monLines, binRecords, sawCall, sawData, sawAck);
  CHECK(sawCall >= 1);   // A->B handshake
  CHECK(sawData >= 2);   // A->B data (containers at fast modes)
  CHECK(sawAck >= 2);    // B->A feedback: BOTH directions decoded
  CHECK(binRecords >= monLines / 2);  // not suppressed (monitor idle)
  // Connected-state suppression.
  Monitor mon2(Family::Wide);
  mon2.setLocalConnected(true);
  TxFrameParams tp;
  tp.family = Family::Wide;
  tp.sig = ChirpSig::Fast;
  tp.mode = 8;
  std::vector<uint8_t> pl(60);
  LinkEnvelope env;
  env.type = LinkType::Data;
  auto raw = packFrame(env, DataBody{0, pl}.pack());
  tp.payload = raw;
  auto frame = buildFrameBaseband(tp);
  std::vector<cfloat> x(2000, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), 2500, cfloat(0, 0));
  ChansimParams cs;
  cs.fading = false;
  cs.snr3kDb = 30;
  cs.seed = 3;
  auto y = Chansim::run(cs, x);
  auto ev = mon2.rxBurst(y);
  CHECK(ev.has_value());
  CHECK(ev->binaryRecord.empty());              // suppressed
  CHECK(ev->monLine.rfind("MON:", 0) == 0);     // MON: still flows
}

SKYTEST_MAIN

// A CALL the responder will not answer - here from the placeholder
// callsign - must leave it idle. It used to enter CallAnswered without
// queueing a CALL_ACK: the station showed CONNECTING, sent nothing, and
// ignored every other caller until the answer window ran out.
TEST_CASE(m2_unanswered_call_leaves_responder_idle) {
  struct PlaceholderPair : Pair {
    PlaceholderPair() : Pair(30.0, 93) { a = LinkEngine(makeCfg("N0CALL", Family::Wide)); }
  };
  PlaceholderPair p;
  p.a.connectTo("K6XYZ", p.now);
  bool heard = false;
  for (int i = 0; i < 40; ++i) {
    p.step();
    heard = heard || p.b.state() != FsmState::Idle;
  }
  CHECK(!heard);
  CHECK(p.b.state() == FsmState::Idle);
  CHECK(p.a.state() != FsmState::Connected);
}

namespace {

std::vector<cfloat> padded(const std::vector<cfloat>& burst) {
  std::vector<cfloat> x(1500, cfloat(0, 0));
  x.insert(x.end(), burst.begin(), burst.end());
  x.insert(x.end(), 2000, cfloat(0, 0));
  return x;
}

}  // namespace

// CALL_ACK must report the selectivity the responder measured on the CALL
// (the receiver computes it per frame). The engine handed the handshake a
// hard-coded 0, so the report - and every consumer of it - was blind.
TEST_CASE(m2_call_ack_reports_selectivity) {
  Pair p(25.0, 4242);
  // A narrowband interferer A->B notches carriers of the CALL.
  p.chanAB.toneHz = 300.0;
  p.chanAB.toneAmp = 0.35;
  p.a.connectTo("K6XYZ", p.now);
  std::optional<CallAckBody> ack;
  int callSel = -1;
  for (int i = 0; i < 40 && !ack; ++i) {
    if (auto bA = p.a.tick(p.now)) {
      const auto y = Chansim::run(p.chanAB, padded(*bA));
      if (auto r = receiveFrame(Family::Wide, y)) callSel = r->selectivity;
      p.b.rxBurst(y, p.now);
    }
    if (auto bB = p.b.tick(p.now)) {
      ChansimParams clean;
      clean.fading = false;
      clean.snr3kDb = 30.0;
      auto r = receiveFrame(Family::Wide, Chansim::run(clean, padded(*bB)));
      if (r && r->payloadOk) {
        if (auto f = parseFrame(r->payload);
            f && f->env.type == LinkType::CallAck) {
          ack = CallAckBody::parse(f->body);
        }
      }
    }
    p.now += 0.5;
  }
  CHECK(ack.has_value());
  std::printf("call_ack selectivity: measured %d on the CALL, reported %d\n",
              callSel, ack ? int(ack->selectivity) : -1);
  CHECK(callSel > 0);
  CHECK(ack && int(ack->selectivity) == callSel);
}

// An idle-timeout teardown is a failure-mode drop and the DISC must say
// so. It went out as DISC(Normal): the peer took it as an operator
// disconnect and dropped its resume token while this end kept its own.
TEST_CASE(m2_idle_timeout_disc_keeps_resume_on_both_ends) {
  Pair p(25.0, 5151);
  CHECK(p.connect());
  // B (the IRS) has data, so its ACKs keep asking for the turn: A keeps
  // hearing B while B hears nothing and idles out.
  p.b.sendData(noiseBlob(2048, 7));
  for (int i = 0; i < 6000 && p.b.state() == FsmState::Connected; ++i) {
    p.step(/*dropAB=*/true, false);
  }
  CHECK(p.b.state() == FsmState::Idle);
  CHECK(p.b.lastDiscCause() == FsmDiscCause::IdleTimeout);
  for (int i = 0; i < 400 && p.a.state() != FsmState::Idle; ++i) {
    p.step(true, false);
  }
  CHECK(p.a.state() == FsmState::Idle);
  CHECK(p.a.lastDiscCause() == FsmDiscCause::LinkLost);
  CHECK(p.a.hasResumeContext());
  CHECK(p.b.hasResumeContext());
}

// The responder's max_mode binds the whole session: the initiator's AMC
// may not promote past it, a MODE_CHANGE above it is refused, and neither
// end may pin above it. It was sent in START, parsed, and ignored.
TEST_CASE(m2_responder_max_mode_caps_the_session) {
  struct CappedPair : Pair {
    CappedPair() : Pair(35.0, 6161) {
      auto cb = makeCfg("K6XYZ", Family::Wide);
      cb.policy.maxMode = 7;
      b = LinkEngine(std::move(cb));
    }
  };
  CappedPair p;
  CHECK(p.connect());
  CHECK_EQ(p.a.sessionMaxMode(), 7);
  CHECK_EQ(p.b.sessionMaxMode(), 7);
  auto blob = noiseBlob(16 * 1024, 61);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  int best = std::max(p.a.sessionMode(), p.b.sessionMode());
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    p.step();
    best = std::max({best, p.a.sessionMode(), p.b.sessionMode()});
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  std::printf("responder cap: best mode M%d under a cap of M7\n", best);
  CHECK(got == blob);
  CHECK(best <= 7);
  CHECK(!p.a.pinMode(9, p.now));
  CHECK(!p.b.pinMode(9, p.now));
  CHECK(p.a.pinMode(6, p.now));
}

// Per-channel verdicts start fresh with every session: the channel
// classifier and the advisor history belonged to the last channel.
TEST_CASE(m2_session_state_starts_fresh) {
  Pair p(30.0, 7171);
  CHECK(p.connect());
  auto blob = noiseBlob(12 * 1024, 71);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 6000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == blob);
  std::printf("first session: class %d, advisor span %.1f s\n",
              int(p.b.channelClass()), p.b.advisor().snrHistorySpanSec());
  CHECK(p.b.channelClass() != ChannelClass::Unknown);
  CHECK(p.b.advisor().snrHistorySpanSec() > 0.0);
  p.a.disconnect(p.now);
  for (int i = 0; i < 200 && (p.a.state() != FsmState::Idle ||
                              p.b.state() != FsmState::Idle);
       ++i) {
    p.step();
  }
  CHECK(p.connect());
  CHECK(p.b.channelClass() == ChannelClass::Unknown);
  CHECK(p.b.advisor().snrHistorySpanSec() == 0.0);
}

// A quiet session (no DATA, so no ACKs) still learns the peer's view of
// the channel: the keepalive carries the sender's SNR report. It used to
// carry a constant and was never parsed.
TEST_CASE(m2_keepalive_carries_snr_report) {
  Pair p(20.0, 8181);
  CHECK(p.connect());
  const double before = p.b.remoteSnrDb();
  for (int i = 0; i < 400; ++i) {
    p.step();
    p.now += 0.5;  // an idle link: let the keepalive timer run
  }
  std::printf("keepalive snr: B sees A's report %.1f dB (was %.1f), A sees "
              "%.1f dB\n",
              p.b.remoteSnrDb(), before, p.a.remoteSnrDb());
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(std::abs(p.b.remoteSnrDb() - 20.0) < 8.0);
  CHECK(std::abs(p.a.remoteSnrDb() - 20.0) < 8.0);
}

#ifdef SKW_HAVE_CRYPTO
// A failed key exchange is refused as an authentication failure. The
// refusal reason was chosen before the KEX ran, so it went out as "Busy".
TEST_CASE(m2_kex_failure_is_refused_as_auth_failed) {
  struct KexPair : Pair {
    KexPair() : Pair(25.0, 9191) {
      auto ca = makeCfg("W1AW", Family::Wide);
      auto cb = makeCfg("K6XYZ", Family::Wide);
      ca.policy.caps |= kCapEncryption;
      cb.policy.caps |= kCapEncryption;
      ca.psk.assign(32, 0x42);  // the responder holds no PSK
      a = LinkEngine(std::move(ca));
      b = LinkEngine(std::move(cb));
    }
  };
  KexPair p;
  CHECK(!p.connect(300));
  CHECK(p.a.lastRefusal().has_value());
  CHECK(p.a.lastRefusal() == StartNakReason::AuthFailed);
  CHECK(!p.a.cryptoActive());
  CHECK(!p.b.cryptoActive());
}
#endif

// ARQ exhaustion is a dead link, not an operator disconnect. The engine
// tore the session down through the operator's path, so the FSM recorded
// LocalRequest and the worker reported EVT:DISCONNECTED NORMAL for a link
// that had simply died. It must end as LinkLost (reported as LINK) and,
// being a fault drop, keep the resume token.
TEST_CASE(m2_arq_exhaustion_reports_link_lost) {
  // A one-retry ARQ budget gives up well inside the 120 s idle window
  // (otherwise the idle timer, not ARQ, ends a silent link first).
  struct ShortArqPair : Pair {
    ShortArqPair() : Pair(25.0, 41) {
      EngineConfig c = makeCfg("W1AW", Family::Wide);
      c.arqMaxRetries = 1;
      a = LinkEngine(std::move(c));
    }
  };
  ShortArqPair p;
  CHECK(p.connect());
  std::vector<uint8_t> msg(600, 0x5A);
  p.a.sendData(msg);
  for (int i = 0; i < 2000 && p.a.state() != FsmState::Idle; ++i) {
    p.step(true, true);  // the channel is gone both ways
    p.now += 1.0;
  }
  CHECK(p.a.state() == FsmState::Idle);
  if (p.a.lastDiscCause() != FsmDiscCause::LinkLost) {
    std::printf("  cause %d\n", int(p.a.lastDiscCause()));
  }
  CHECK(p.a.lastDiscCause() == FsmDiscCause::LinkLost);
  CHECK(p.a.hasResumeContext());
}
