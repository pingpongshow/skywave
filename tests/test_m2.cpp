// Two-modem integration harness (ROADMAP 2.7, Milestone M2): two
// in-process LinkEngines over two independent chansim directions.
// Scenarios per TESTING.md: connect_floor, bulk_transfer (with injected
// loss), disconnect_paths. CI runs a scaled bulk size; the full 100 KiB
// soak is a manual/nightly run of the same scenario.
#include <cstdio>
#include <vector>

#include "link/engine.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

struct Pair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

  explicit Pair(double snrDb, uint64_t seed = 77,
                Family fam = Family::Wide, bool aggregate = true)
      : a(makeCfg("W1AW", fam, aggregate)),
        b(makeCfg("K6XYZ", fam, aggregate)) {
    chanAB.fading = false;
    chanAB.snr3kDb = snrDb;
    chanBA = chanAB;
    chanAB.seed = seed;
    chanBA.seed = seed + 5000;
  }

  static EngineConfig makeCfg(const char* call, Family fam,
                              bool aggregate = true) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.family = fam;
    c.policy.caps = kCapZstd | kCapResume | kCapMasking;
    c.burstAggregation = aggregate;
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

TEST_CASE(m2_connect_clean) {
  Pair p(30.0);
  CHECK(p.connect());
  CHECK(p.a.sessionMode() == p.b.sessionMode());
  CHECK(p.a.sessionMode() >= kFirstMode);
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
    std::vector<uint8_t> blob(12 * 1024);
    for (size_t i = 0; i < blob.size(); ++i) {
      blob[i] = uint8_t(i * 131 + 7 + size_t(snr));
    }
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
  std::vector<uint8_t> blob(6 * 1024);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 37 + 3);
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
    CHECK(p.a.lastDiscCause() == FsmDiscCause::DiscRetriesExhausted);
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
  std::vector<uint8_t> blob(2048);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i);
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
  std::vector<uint8_t> blob(16 * 1024);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 7 + 1);
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
  std::vector<uint8_t> blob(10 * 1024);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 61 + 9);
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

namespace {

// Airtime to move `bytes` at pinned M13 over a clean 30 dB channel.
double m13TransferAirtime(bool aggregate, size_t bytes) {
  Pair p(30.0, 123, Family::Wide, aggregate);
  if (!p.connect()) return -1;
  if (!p.a.pinMode(13, p.now)) return -1;
  for (int i = 0; i < 40 && p.b.sessionMode() != 13; ++i) p.step();
  std::vector<uint8_t> blob(bytes);
  for (size_t i = 0; i < bytes; ++i) blob[i] = uint8_t(i * 17 + 3);
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

}  // namespace

TEST_CASE(m3_burst_container_throughput) {
  // One preamble/sync/header per 4-frame burst at M13 must beat the
  // per-frame overhead of single-frame transmission by a wide margin.
  const size_t bytes = 18 * 1024;
  double tAgg = m13TransferAirtime(true, bytes);
  double tSolo = m13TransferAirtime(false, bytes);
  CHECK(tAgg > 0);
  CHECK(tSolo > 0);
  double thruAgg = bytes * 8.0 / tAgg;
  double thruSolo = bytes * 8.0 / tSolo;
  std::printf("burst container: agg %.0f bps vs solo %.0f bps (x%.2f); "
              "net table %.0f bps\n",
              thruAgg, thruSolo, thruAgg / thruSolo,
              netRate(Family::Wide, 13, CpMode::Medium));
  CHECK(thruAgg > 1.25 * thruSolo);

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
      double(kFftBaseband + kCpSamplesAudio[1] / kDecimation);
  // Payload-SECTION rate vs the table NET rate, with the structural
  // codeword-padding allowance (0.80, as in the conformance geometry
  // gate). Piggyback bytes ride the header section and are excluded.
  const double sectionSec = g.payloadSymbols * paySamples / kFsBaseband;
  const double sectionBits = (payload.size() - 30) * 8.0;
  const double eff = sectionBits / sectionSec;
  const double net = netRate(Family::Wide, 13, CpMode::Medium);
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
  std::vector<uint8_t> blob(8 * 1024);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 3 + 1);
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
  std::vector<uint8_t> blob(2048);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 11 + 5);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 8000 && got.size() < blob.size(); ++i) {
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

SKYTEST_MAIN
