// HARQ chase combining (ROADMAP 3.3). Gate: combined decode at 2
// transmissions succeeds >= 2 dB below the single-transmission point.
#include <cstdio>
#include <vector>

#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<cfloat> makeFrame(std::vector<uint8_t>& payload) {
  payload.assign(150, 0);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 29 + 5);
  TxFrameParams tp;
  tp.family = Family::Wide;
  tp.sig = ChirpSig::Fast;
  tp.mode = 8;
  tp.frameType = uint8_t(6);
  tp.seqLow = 0x42;
  tp.payload = payload;
  auto frame = buildFrameBaseband(tp);
  std::vector<cfloat> x(2000, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), 2500, cfloat(0, 0));
  return x;
}

// Success rate over trials: nTx receptions of the same frame with
// independent noise, chase-combined when harq is on.
int successes(double snrDb, int nTx, bool useHarq, int trials,
              uint64_t seedBase) {
  std::vector<uint8_t> payload;
  auto x = makeFrame(payload);
  int ok = 0;
  for (int t = 0; t < trials; ++t) {
    HarqChase harq;
    bool decoded = false;
    for (int k = 0; k < nTx && !decoded; ++k) {
      ChansimParams cp;
      cp.fading = false;
      cp.snr3kDb = snrDb;
      cp.seed = seedBase + uint64_t(t) * 7919 + uint64_t(k) * 104729;
      auto y = Chansim::run(cp, x);
      auto r = receiveFrame(Family::Wide, y, nullptr,
                            useHarq ? &harq : nullptr);
      decoded = r && r->payloadOk && r->payload == payload;
    }
    if (decoded) ++ok;
  }
  return ok;
}

}  // namespace

TEST_CASE(chase_combining_gain) {
  // Reference: near the single-shot knee the first transmission mostly
  // fails; the second, chase-combined, must recover the frame. 2 dB
  // below the single-tx point, combined decode must beat what a single
  // transmission achieves AT the point.
  const double base = floorDb(Family::Wide, 8) + 1.0;  // single-tx ~ok
  const int trials = 12;
  int single = successes(base, 1, false, trials, 500);
  int combinedDeep = successes(base - 2.5, 2, true, trials, 900);
  int singleDeep = successes(base - 2.5, 1, false, trials, 900);
  std::printf("harq: single@%.1f=%d/%d single@%.1f=%d/%d "
              "combined@%.1f=%d/%d\n",
              base, single, trials, base - 2.5, singleDeep, trials,
              base - 2.5, combinedDeep, trials);
  CHECK(single >= trials - 2);          // sanity: knee placement
  CHECK(singleDeep <= trials / 2 + 1);  // deep point mostly fails alone
  CHECK(combinedDeep >= trials - 2);    // >= 2.5 dB combining gain
}

TEST_CASE(chase_two_receptions_without_retx_flag) {
  // Two receptions of the SAME transmission (no rv bump needed at the
  // PHY): identity key matches, LLRs add.
  const double snr = floorDb(Family::Wide, 8) - 1.5;
  int ok = successes(snr, 2, true, 10, 1300);
  int okNoHarq = successes(snr, 2, false, 10, 1300);
  std::printf("harq: 2rx combined=%d/10 no-harq=%d/10 at %.1f dB\n", ok,
              okNoHarq, snr);
  CHECK(ok > okNoHarq);
}

TEST_CASE(harq_store_hygiene) {
  HarqChase h;
  std::vector<float> a(100, 1.0f), b(100, 2.0f);
  uint64_t k = HarqChase::frameKey(6, 1, 8, 150, 0);
  CHECK_EQ(h.combine(k, a), 1);
  CHECK_EQ(h.combine(k, b), 2);
  CHECK_NEAR(b[0], 3.0, 1e-6);  // 1 + 2 summed
  h.onDecodeSuccess(k);
  CHECK_EQ(int(h.entries()), 0);
  // Size mismatch starts a fresh accumulator instead of corrupting.
  std::vector<float> c(50, 1.0f);
  h.combine(k, c);
  std::vector<float> d(100, 1.0f);
  CHECK_EQ(h.combine(k, d), 1);
  // Bounded store: 200 distinct keys never exceed ~64 entries by much.
  HarqChase big;
  for (int i = 0; i < 200; ++i) {
    std::vector<float> v(10, 0.5f);
    big.combine(HarqChase::frameKey(6, uint8_t(i), 8, uint16_t(i), 0), v);
  }
  CHECK(big.entries() <= 66);
}

namespace {

// One trial: first transmission at `mode1`, retransmission at `mode2`
// (same payload identity), shared HarqChase. True when either decodes.
bool irTrial(int mode1, int mode2, double snrDb, uint64_t seed) {
  std::vector<uint8_t> payload(150);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 29 + 5);
  auto build = [&](int mode) {
    TxFrameParams tp;
    tp.family = Family::Wide;
    tp.sig = ChirpSig::Fast;
    tp.mode = mode;
    tp.frameType = uint8_t(6);
    tp.seqLow = 0x42;
    tp.payload = payload;
    auto frame = buildFrameBaseband(tp);
    std::vector<cfloat> x(2000, cfloat(0, 0));
    x.insert(x.end(), frame.begin(), frame.end());
    x.insert(x.end(), 2500, cfloat(0, 0));
    return x;
  };
  HarqChase harq;
  int modes[2] = {mode1, mode2};
  for (int k = 0; k < 2; ++k) {
    ChansimParams cp;
    cp.fading = false;
    cp.snr3kDb = snrDb;
    cp.seed = seed + uint64_t(k) * 104729;
    auto y = Chansim::run(cp, build(modes[k]));
    auto r = receiveFrame(Family::Wide, y, nullptr, &harq);
    if (r && r->payloadOk && r->payload == payload) return true;
  }
  return false;
}

}  // namespace

TEST_CASE(ir_stepdown_mapping) {
  CHECK_EQ(irStepDownMode(9), 8);
  CHECK_EQ(irStepDownMode(11), 10);
  CHECK_EQ(irStepDownMode(8), 8);    // already r1/2: chase
  CHECK_EQ(irStepDownMode(12), 12);  // no 64QAM r1/2 rung: chase
  CHECK_EQ(irStepDownMode(13), 13);
}

TEST_CASE(ir_beats_chase_for_high_rate_modes) {
  // A/B at an SNR where M9 (QPSK r3/4) fails outright: chase combines
  // two M9 receptions; IR retransmits at M8 with systematic priors from
  // the failed M9 attempt. Gate: IR > chase.
  const double snr = floorDb(Family::Wide, 9) - 5.0;  // deep for M9
  const int trials = 10;
  int chase = 0, ir = 0, m8alone = 0;
  for (int t = 0; t < trials; ++t) {
    uint64_t seed = 4000 + uint64_t(t) * 7919;
    if (irTrial(9, 9, snr, seed)) ++chase;
    if (irTrial(9, 8, snr, seed)) ++ir;
    // Reference: step-down without priors = fresh M8 only.
    HarqChase none;
    (void)none;
    if (irTrial(8, 8, snr, seed + 1)) ++m8alone;
  }
  std::printf("ir A/B at %.1f dB: chase=%d/%d ir=%d/%d m8ref=%d/%d\n", snr,
              chase, trials, ir, trials, m8alone, trials);
  CHECK(ir > chase);
  CHECK(ir >= trials - 2);
}

SKYTEST_MAIN
