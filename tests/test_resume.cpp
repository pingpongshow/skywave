// Session resume (ROADMAP 4.4, PROTOCOL.md "Session Resume" and
// "Reconnecting to a Peer That Lost Its Session"): wire bodies, a stream
// that survives link death through RESUME, and the ResumeInvalid fallback
// to a clean START. (This file held the modem's own file-transfer
// service, which was removed: the companion app transfers files itself.)
#include <cstdio>
#include <string>
#include <vector>

#include "link/engine.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> randomBytes(size_t n, uint64_t seed) {
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

std::vector<uint8_t> testPsk() {
  std::vector<uint8_t> k(32);
  for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(0x5A ^ (i * 7));
  return k;
}

struct Pair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

  explicit Pair(double snrDb, uint64_t seed, bool crypto = false)
      : a(mk("W1AW", crypto)), b(mk("K6XYZ", crypto)) {
    chanAB.fading = false;
    chanAB.snr3kDb = snrDb;
    chanBA = chanAB;
    chanAB.seed = seed;
    chanBA.seed = seed + 5000;
  }

  static EngineConfig mk(const char* call, bool crypto = false) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    if (crypto) {
      c.policy.caps |= kCapEncryption;
      c.psk = testPsk();
    }
    return c;
  }

  void step(bool dropAB = false, bool dropBA = false) {
    auto burstA = a.tick(now);
    auto burstB = b.tick(now);
    double dt = 0.05;
    auto push = [&](std::vector<cfloat>& burst, LinkEngine& to,
                    ChansimParams& chan) {
      dt += double(burst.size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst.begin(), burst.end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = chan;
      cp.seed = chan.seed + (seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      to.rxBurst(y, now);
    };
    if (burstA && !dropAB) push(*burstA, b, chanAB);
    if (burstB && !dropBA) push(*burstB, a, chanBA);
    now += dt;
  }

  // Total link death until both ends give the session up.
  bool blackout() {
    for (int i = 0; i < 4000 && (a.state() != FsmState::Idle ||
                                 b.state() != FsmState::Idle);
         ++i) {
      step(true, true);
      now += 2.0;  // let ARQ/DISC/idle timers run their course
    }
    return a.state() == FsmState::Idle && b.state() == FsmState::Idle;
  }

  bool connect(int budget = 400) {
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

  // Move bytes A -> B until `want` bytes arrived (or the budget runs out).
  void pump(std::vector<uint8_t>& got, size_t want, int budget) {
    for (int i = 0; i < budget && got.size() < want; ++i) {
      step();
      auto d = b.takeReceivedData();
      got.insert(got.end(), d.begin(), d.end());
      if (a.state() != FsmState::Connected) break;
    }
  }
};

}  // namespace

TEST_CASE(resume_wire_roundtrip) {
  StartAckBody sa;
  sa.finalMode = 8;
  sa.hasResumeToken = true;
  sa.resumeToken = {1, 2, 3, 4, 5, 6, 7, 8};
  auto q = StartAckBody::parse(sa.pack());
  CHECK(q.has_value());
  CHECK(q->hasResumeToken);
  CHECK(q->resumeToken == sa.resumeToken);

  ResumeReqBody rr;
  rr.token = sa.resumeToken;
  rr.lastRxSeq = 555;
  auto r2 = ResumeReqBody::parse(rr.pack());
  CHECK(r2.has_value());
  CHECK(r2->token == rr.token);
  CHECK_EQ(int(r2->lastRxSeq), 555);

  ResumeAckBody ra;
  ra.lastRxSeq = 700;
  auto a2 = ResumeAckBody::parse(ra.pack());
  CHECK(a2.has_value());
  CHECK_EQ(int(a2->lastRxSeq), 700);

  auto nak = StartNakBody::parse(
      StartNakBody{StartNakReason::ResumeInvalid}.pack());
  CHECK(nak.has_value());
  CHECK(nak->reason == StartNakReason::ResumeInvalid);
}

TEST_CASE(stream_resume_after_link_death) {
  // ROADMAP 4.4 gate: kill the link mid-transfer, reconnect inside the
  // resume window, and the stream continues byte-exact from where the
  // sequence spaces left off - no restart, no replay.
  Pair p(25.0, 9100);
  CHECK(p.connect());
  auto blob = randomBytes(24 * 1024, 21);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  p.pump(got, 8 * 1024, 4000);
  CHECK(got.size() >= 8 * 1024);
  CHECK(got.size() < blob.size());
  // The link dies. Both sides give up the session but keep the token and
  // the pipeline (failure-mode teardown, not operator DISC).
  CHECK(p.blackout());
  CHECK(p.a.hasResumeContext());
  CHECK(p.b.hasResumeContext());
  std::printf("resume: link died after %zu bytes\n", got.size());
  // Reconnect well inside the 10 min window.
  p.now += 30.0;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.a.sessionResumed());
  CHECK(p.b.sessionResumed());
  p.pump(got, blob.size(), 6000);
  CHECK_EQ(int(got.size()), int(blob.size()));
  CHECK(got == blob);
  for (auto& e : p.a.takeEvents()) {
    CHECK(e.find("EVT:STREAM_REPLAY") != 0);  // resumed: nothing replayed
  }
}

TEST_CASE(resume_invalid_falls_back_to_clean_start) {
  // The responder lost its state (power cycle): RESUME_REQ draws
  // START_NAK(ResumeInvalid) and the same call completes as a clean
  // session - never a protocol failure - re-sending from the last message
  // the dead peer acknowledged (EVT:STREAM_REPLAY names the repeat).
  Pair p(25.0, 9200);
  CHECK(p.connect());
  auto blob = randomBytes(24 * 1024, 22);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  p.pump(got, 8 * 1024, 4000);
  CHECK(p.blackout());
  CHECK(p.a.hasResumeContext());
  p.b = LinkEngine(Pair::mk("K6XYZ"));  // responder restarted: no token
  p.now += 10.0;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(!p.a.sessionResumed());
  size_t replayed = 0;
  for (auto& e : p.a.takeEvents()) {
    if (e.rfind("EVT:STREAM_REPLAY ", 0) == 0) {
      replayed = size_t(std::stoul(e.substr(18)));
    }
  }
  // The fresh peer receives the rest of the stream from a message
  // boundary at or before where the dead one stopped: nothing is lost, and
  // what repeats is at most the reported replay (it is an upper bound - an
  // unconfirmed message may or may not have reached the dead peer).
  std::vector<uint8_t> got2;
  p.pump(got2, blob.size(), 6000);
  for (int i = 0; i < 400; ++i) {  // drain anything still in flight
    p.step();
    auto d = p.b.takeReceivedData();
    got2.insert(got2.end(), d.begin(), d.end());
  }
  std::printf("fallback: %zu delivered before, %zu replayed, %zu after\n",
              got.size(), replayed, got2.size());
  CHECK(got2.size() <= blob.size());
  const size_t restart = blob.size() - std::min(blob.size(), got2.size());
  CHECK(restart <= got.size());              // no gap
  CHECK(got.size() - restart <= replayed);   // repeat within the report
  CHECK(replayed > 0);
  CHECK(std::equal(got2.begin(), got2.end(), blob.begin() + long(restart)));
}

// A START_NAK(ResumeInvalid) answers a RESUME_REQ. One that arrives while
// we sent only a clean START (a stray, a forgery, or the duplicate NAK of
// a RESUME_REQ retry after we already fell back) must change nothing: it
// used to queue another START and replay the stream pipeline again.
TEST_CASE(unsolicited_resume_invalid_is_ignored) {
  Pair p(25.0, 9300);
  p.a.connectTo("K6XYZ", p.now);
  // Run until A has sent its START and waits for START_ACK; B must not
  // hear it (so nothing answers it).
  bool waiting = false;
  for (int i = 0; i < 400 && !waiting; ++i) {
    auto burstA = p.a.tick(p.now);
    if (p.a.state() == FsmState::WaitStartAck && !burstA) {
      waiting = true;
      break;
    }
    if (burstA && p.a.state() != FsmState::WaitStartAck) {
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burstA->begin(), burstA->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      p.b.rxBurst(Chansim::run(p.chanAB, x), p.now);
    }
    auto burstB = p.b.tick(p.now);
    if (burstB) {
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burstB->begin(), burstB->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      p.a.rxBurst(Chansim::run(p.chanBA, x), p.now);
    }
    p.now += 0.5;
  }
  CHECK(waiting);
  // Inject the stray NAK, addressed to A's session.
  LinkEnvelope env;
  env.type = LinkType::StartNak;
  env.sessionId = p.a.sessionId();
  const auto raw =
      packFrame(env, StartNakBody{StartNakReason::ResumeInvalid}.pack());
  TxFrameParams tp;
  tp.family = Family::Wide;
  tp.sig = ChirpSig::Calling;
  tp.mode = 4;
  tp.frameType = uint8_t(LinkType::StartNak);
  tp.payload = raw;
  auto frame = buildFrameBaseband(tp);
  std::vector<cfloat> x(1500, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), 2000, cfloat(0, 0));
  CHECK(p.a.rxBurst(Chansim::run(p.chanBA, x), p.now));
  // Well inside the START retry timeout: A must stay silent.
  int bursts = 0;
  for (int i = 0; i < 20; ++i) {
    if (p.a.tick(p.now)) ++bursts;
    p.now += 0.2;
  }
  CHECK_EQ(bursts, 0);
  CHECK(p.a.state() == FsmState::WaitStartAck);
}

#ifdef SKW_HAVE_CRYPTO
// The fallback START after ResumeInvalid must carry the KEX like any clean
// START. It was built without it, so when the restarted peer now
// negotiates encryption (here: it gained the capability across its
// restart, while the paused session was cleartext and resumable), the
// fallback was refused for missing KEX material and the reconnect failed.
TEST_CASE(resume_invalid_fallback_keeps_encryption) {
  Pair p(25.0, 9400);
  p.a = LinkEngine(Pair::mk("W1AW", /*crypto=*/true));
  CHECK(p.connect());
  CHECK(!p.a.cryptoActive());  // B does not offer encryption yet
  auto blob = randomBytes(4 * 1024, 23);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  p.pump(got, 1024, 2000);
  CHECK(p.blackout());
  CHECK(p.a.hasResumeContext());
  p.b = LinkEngine(Pair::mk("K6XYZ", /*crypto=*/true));  // restarted
  p.now += 10.0;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
    if (p.a.state() == FsmState::Idle) break;
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.b.state() == FsmState::Connected);
  CHECK(p.a.cryptoActive());
  CHECK(p.b.cryptoActive());
  CHECK(!p.a.lastRefusal().has_value());
}
#endif

SKYTEST_MAIN
