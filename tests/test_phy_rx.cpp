#include <cmath>
#include <vector>

#include "core/constants.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> testPayload(size_t n, uint8_t salt = 0) {
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t(i * 37 + 11 + salt);
  return p;
}

std::vector<cfloat> withPadding(std::vector<cfloat> frame, size_t lead,
                                size_t tail) {
  std::vector<cfloat> x(lead, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), tail, cfloat(0, 0));
  return x;
}

bool runOne(Family fam, ChirpSig sig, int mode, size_t payloadLen,
            const ChansimParams& cp, uint8_t salt = 0) {
  auto payload = testPayload(payloadLen, salt);
  TxFrameParams p;
  p.family = fam;
  p.sig = sig;
  p.mode = mode;
  p.frameType = 9;
  p.seqLow = 0x5A;
  p.payload = payload;
  auto frame = buildFrameBaseband(p);
  auto x = withPadding(std::move(frame), 3000, 6000);
  auto y = Chansim::run(cp, x);
  auto r = receiveFrame(fam, y);
  if (!r) return false;
  if (r->header.mode != mode || r->header.length != payloadLen) return false;
  if (!r->payloadOk) return false;
  return r->payload == payload;
}

ChansimParams cleanChannel(double snr3k = 99.0) {
  ChansimParams c;
  c.fading = false;
  c.snr3kDb = snr3k;
  c.seed = 1;
  return c;
}

}  // namespace

TEST_CASE(rx_loopback_clean_families_modes) {
  for (Family fam :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    // Fast frame at M8 and M13.
    CHECK(runOne(fam, ChirpSig::Fast, 8, 120, cleanChannel()));
    CHECK(runOne(fam, ChirpSig::Fast, 13, 300, cleanChannel()));
  }
  // Robust frames (piggyback-only control payloads).
  CHECK(runOne(Family::Wide, ChirpSig::Robust, 4, 24, cleanChannel()));
  CHECK(runOne(Family::Narrow, ChirpSig::Robust, 4, 20, cleanChannel()));
}

TEST_CASE(rx_loopback_calling_with_cfo) {
  for (double cfo : {-40.0, 25.0}) {
    ChansimParams c = cleanChannel(15.0);
    c.cfoHz = cfo;
    c.seed = 3;
    CHECK(runOne(Family::Wide, ChirpSig::Calling, 5, 40, c));
  }
}

TEST_CASE(rx_loopback_inverted_spectrum) {
  // Inversion is modeled by conjugating the channel output.
  auto payload = testPayload(60);
  TxFrameParams p;
  p.family = Family::Wide;
  p.sig = ChirpSig::Fast;
  p.mode = 9;
  p.payload = payload;
  auto frame = buildFrameBaseband(p);
  auto x = withPadding(std::move(frame), 3000, 6000);
  ChansimParams c = cleanChannel(20.0);
  c.cfoHz = 3.0;
  auto y = Chansim::run(c, x);
  for (auto& v : y) v = std::conj(v);
  auto r = receiveFrame(Family::Wide, y);
  CHECK(r.has_value());
  if (r) {
    CHECK(r->inverted);
    CHECK(r->payloadOk);
    CHECK(r->payload == payload);
  }
}

TEST_CASE(rx_loopback_awgn_midladder) {
  // M8 at floor+6 dB AWGN: must decode reliably.
  int ok = 0;
  for (uint32_t t = 0; t < 5; ++t) {
    ChansimParams c = cleanChannel(8.0);  // M8 floor +2 -> +6 margin
    c.seed = 10 + t;
    ok += runOne(Family::Wide, ChirpSig::Fast, 8, 150, c, uint8_t(t)) ? 1 : 0;
  }
  CHECK(ok >= 5);
}

TEST_CASE(rx_loopback_robust_awgn_near_floor) {
  // M4 robust at -6 dB(3k) (floor + 2.5): decode most trials.
  int ok = 0;
  for (uint32_t t = 0; t < 5; ++t) {
    ChansimParams c = cleanChannel(-6.0);
    c.seed = 20 + t;
    ok += runOne(Family::Wide, ChirpSig::Robust, 4, 28, c, uint8_t(t)) ? 1 : 0;
  }
  CHECK(ok >= 4);
}

TEST_CASE(rx_loopback_ccir_moderate) {
  // Fading channel at comfortable margin: M8 at +12 dB (floor +10).
  int ok = 0;
  for (uint32_t t = 0; t < 5; ++t) {
    ChansimParams c = ccirModerate();
    c.snr3kDb = 12.0;
    c.seed = 30 + t;
    ok += runOne(Family::Wide, ChirpSig::Fast, 8, 150, c, uint8_t(t)) ? 1 : 0;
  }
  CHECK(ok >= 4);
}

SKYTEST_MAIN
