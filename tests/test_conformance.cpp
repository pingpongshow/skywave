// CI conformance quick profile (ROADMAP 1.13): representative FER points
// plus geometry-derived NET-rate protection for every config. The full
// family x mode x SNR sweep is bertool's job (run manually / nightly);
// this keeps CI bounded while still catching regressions in the floors.
#include <cmath>
#include <vector>

#include "core/constants.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

int runTrials(Family fam, int mode, double snr3k, int trials, uint64_t seed,
              bool fadingModerate = true) {
  const bool robust = mode <= 7;
  const size_t payloadLen = robust ? 60 : 300;
  std::vector<uint8_t> payload(payloadLen);
  for (size_t i = 0; i < payloadLen; ++i) payload[i] = uint8_t(i * 41 + 3);
  TxFrameParams tp;
  tp.family = fam;
  tp.sig = robust ? ChirpSig::Robust : ChirpSig::Fast;
  tp.mode = mode;
  tp.payload = payload;
  auto frame = buildFrameBaseband(tp);
  std::vector<cfloat> x(2000, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), 2500, cfloat(0, 0));

  int ok = 0;
  for (int t = 0; t < trials; ++t) {
    ChansimParams cp = fadingModerate ? ccirModerate() : ChansimParams{};
    if (!fadingModerate) cp.fading = false;
    cp.snr3kDb = snr3k;
    cp.seed = seed + uint64_t(t) * 7919;
    auto y = Chansim::run(cp, x);
    auto r = receiveFrame(fam, y);
    if (r && r->payloadOk && r->payload == payload) ++ok;
  }
  return ok;
}

}  // namespace

TEST_CASE(conformance_net_rates_geometry) {
  // Payload-section throughput derived from geometry must track the table
  // NET rates. Known structural allowances: codeword zero-padding (worst
  // for small robust frames) and NARROW-S16 group pooling.
  for (int f = 0; f < kNumFamilies; ++f) {
    for (int mode = kFirstMode; mode < kNumModes; ++mode) {
      Family fam = Family(f);
      const bool robust = mode <= 7;
      // Large payload so padding amortizes.
      std::vector<uint8_t> payload(robust ? 200 : 1200);
      TxFrameParams tp;
      tp.family = fam;
      tp.sig = robust ? ChirpSig::Robust : ChirpSig::Fast;
      tp.mode = mode;
      tp.payload = payload;
      auto g = computeGeometry(tp);
      double paySec = double(g.payloadSymbols) *
                      double(kFftBaseband + kCpSamplesAudio[1] / kDecimation) /
                      kFsBaseband;
      double eff = double(payload.size() - 30) * 8.0 / paySec;
      double table = netRate(fam, mode, CpMode::Medium);
      // Table NET rates are asymptotic (payload >> codeword). Spread
      // modes carry few info bits per codeword at r1/2, so a 200-byte
      // robust frame pays real zero-padding: measured intrinsic ratios
      // are 0.47-0.70 (worst: NARROW/MID S16). Unspread fast frames with
      // 1200-byte payloads amortize to >0.80.
      double floorRatio = robust ? 0.42 : 0.80;
      CHECK(eff > floorRatio * table);
      CHECK(eff < 1.05 * table);
    }
  }
}

// FER regression smokes run on AWGN (stable reference) while the fading
// floor tables are still being established empirically (bertool campaign,
// ROADMAP 1.13 status). Fading conformance is bertool's job.
TEST_CASE(conformance_quick_wide_m8_awgn) {
  int ok = runTrials(Family::Wide, 8, floorDb(Family::Wide, 8) + 1.0, 10,
                     500, false);
  CHECK(ok >= 9);
}

TEST_CASE(conformance_quick_wide_m4_awgn) {
  int ok = runTrials(Family::Wide, 4, floorDb(Family::Wide, 4) + 1.0, 8, 600,
                     false);
  CHECK(ok >= 7);
}

TEST_CASE(conformance_quick_ultra_m13_awgn) {
  int ok = runTrials(Family::Ultra, 13, floorDb(Family::Ultra, 13) + 3.0, 8,
                     700, false);
  CHECK(ok >= 7);
}

TEST_CASE(conformance_quick_narrow_m6_awgn) {
  int ok = runTrials(Family::Narrow, 6, floorDb(Family::Narrow, 6) + 1.0, 8,
                     800, false);
  CHECK(ok >= 7);
}

SKYTEST_MAIN
