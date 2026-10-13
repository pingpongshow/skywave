// CSS frame path (ROADMAP 6.2): chirp-Css preamble -> sync symbols ->
// SF8 header -> SF(mode) payload, through the full one-shot receiver
// with sync arbitration - clean, CFO/clock offset, inverted spectrum,
// AWGN floor, and CCIR Moderate fading behavior.
#include <cstdio>
#include <vector>

#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> noiseBytes(size_t n, uint64_t seed) {
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

struct TrialResult {
  bool detected = false;
  bool headerOk = false;
  bool payloadOk = false;
};

TrialResult cssFrameTrial(Family fam, int mode, const ChansimParams& cp,
                          size_t payloadBytes, uint64_t seed,
                          bool invert = false) {
  auto payload = noiseBytes(payloadBytes, seed);
  TxFrameParams tp;
  tp.family = fam;
  tp.sig = ChirpSig::Css;
  tp.mode = mode;
  tp.frameType = 10;
  tp.seqLow = uint8_t(seed & 0xFF);
  tp.payload = payload;
  auto frame = buildFrameBaseband(tp);

  std::vector<cfloat> x(3000, cfloat(0, 0));
  x.insert(x.end(), frame.begin(), frame.end());
  x.insert(x.end(), 3000, cfloat(0, 0));
  auto y = Chansim::run(cp, x);
  if (invert) {
    for (auto& v : y) v = std::conj(v);
  }

  TrialResult r;
  RxStage st = RxStage::NoDetect;
  auto res = receiveFrame(fam, y, &st);
  r.detected = st != RxStage::NoDetect;
  if (!res) return r;
  r.headerOk = res->header.mode == mode &&
               res->header.waveform == Waveform::Css &&
               res->header.length == payloadBytes &&
               res->header.frameType == 10;
  r.payloadOk = res->payloadOk && res->payload == payload &&
                res->sig == ChirpSig::Css;
  return r;
}

ChansimParams awgn(double snr, uint64_t seed) {
  ChansimParams cp;
  cp.fading = false;
  cp.snr3kDb = snr;
  cp.seed = seed;
  return cp;
}

}  // namespace

TEST_CASE(css_frame_clean_roundtrip_all_families) {
  for (Family f :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    for (int mode : {0, 3}) {
      auto r = cssFrameTrial(f, mode, awgn(30.0, 11), 40,
                             uint64_t(100 + mode));
      CHECK(r.headerOk);
      CHECK(r.payloadOk);
    }
  }
}

TEST_CASE(css_frame_geometry_matches_build) {
  // computeGeometry's totalSamples must match the modulated length
  // (pre-filter): timers and capture windows derive from it.
  for (Family f : {Family::Wide, Family::Narrow}) {
    for (int mode : {0, 2}) {
      auto payload = noiseBytes(52, 5);
      TxFrameParams tp;
      tp.family = f;
      tp.sig = ChirpSig::Css;
      tp.mode = mode;
      tp.payload = payload;
      auto g = computeGeometry(tp);
      auto bb = buildFrameBaseband(tp);
      // The band filter adds its (bounded) tail; geometry counts the
      // modulated content.
      CHECK(bb.size() >= size_t(g.totalSamples));
      CHECK(bb.size() <= size_t(g.totalSamples) + 400);
    }
  }
}

TEST_CASE(css_frame_cfo_clock_tolerance) {
  for (double cfo : {-4.0, 4.0}) {
    auto cp = awgn(0.0, 21);
    cp.cfoHz = cfo;
    cp.clockPpm = 30.0;
    auto r = cssFrameTrial(Family::Wide, 0, cp, 40, 7);
    CHECK(r.payloadOk);
  }
  // Calling-grade CFO: the preamble estimator hands the CSS path a
  // residual small enough to decode.
  {
    auto cp = awgn(5.0, 22);
    cp.cfoHz = 25.0;
    auto r = cssFrameTrial(Family::Wide, 0, cp, 40, 8);
    CHECK(r.payloadOk);
  }
}

// 8.8 (ROADMAP): LO drift through a long deep-tier frame. The receiver
// removes the preamble's CFO once; the residual r*t moves the SF8 pilots
// 2^(8-SF) times further than the payload needs, and before the drift
// search NARROW/MID M1-M3 failed 88-100% at 0.1 Hz/s. Floor + 3 dB, both
// drift signs, the two families the coupling hurts.
TEST_CASE(css_frame_lo_drift) {
  int ok = 0, total = 0;
  for (Family fam : {Family::Narrow, Family::Mid}) {
    for (int mode : {1, 2, 3}) {
      for (double drift : {0.1, -0.1}) {
        for (uint64_t seed = 1; seed <= 3; ++seed) {
          ChansimParams cp = awgn(floorDb(fam, mode) + 3.0, seed * 31 + uint64_t(mode));
          cp.cfoDriftHzPerSec = drift;
          ++total;
          if (cssFrameTrial(fam, mode, cp, 60, seed).payloadOk) ++ok;
        }
      }
    }
  }
  std::printf("css drift 0.1 Hz/s: %d/%d frames\n", ok, total);
  CHECK(ok >= total - 2);  // floor+3 allows a stray loss; was ~0/36
}

TEST_CASE(css_frame_inverted_spectrum) {
  auto r = cssFrameTrial(Family::Wide, 1, awgn(10.0, 31), 40, 9, true);
  CHECK(r.payloadOk);
}

TEST_CASE(css_frame_awgn_floor) {
  // Full-frame FER at the modem design floor: the frame adds preamble
  // detection and sync to the 6.1 aligned-payload gate.
  int fails = 0;
  const int kTrials = 15;
  for (int t = 0; t < kTrials; ++t) {
    auto r = cssFrameTrial(Family::Wide, 0, awgn(-13.0, uint64_t(41 + t)),
                           32, uint64_t(500 + t));
    if (!r.payloadOk) ++fails;
  }
  std::printf("  WIDE M0 full-frame FER @ -13 dB AWGN: %d/%d\n", fails,
              kTrials);
  CHECK(fails <= 6);
}

TEST_CASE(css_frame_fading_profile) {
  // CCIR Moderate behavior (the 6.2 link gate runs at -13 dB WIDE):
  // measure and print the curve; gates are the observed envelope with
  // slack. RAKE taps from the sync profile carry the 2-tap channel.
  for (double snr : {-13.0, -10.0}) {
    int det = 0, pay = 0;
    const int kTrials = 12;
    for (int t = 0; t < kTrials; ++t) {
      ChansimParams cp = ccirModerate();
      cp.snr3kDb = snr;
      cp.seed = uint64_t(900 + t * 17);
      auto r = cssFrameTrial(Family::Wide, 0, cp, 32, uint64_t(700 + t));
      det += r.detected ? 1 : 0;
      pay += r.payloadOk ? 1 : 0;
    }
    std::printf("  WIDE M0 CCIR-Mod @ %+.0f dB: detect %d/%d payload %d/%d\n",
                snr, det, kTrials, pay, kTrials);
    if (snr == -10.0) CHECK(pay >= kTrials / 3);
  }
  // NARROW's designated fading channel is CCIR Good (kFadingFloors
  // convention: a 500 Hz band sees the 2-path channel as flat Rayleigh).
  int pay = 0;
  for (int t = 0; t < 8; ++t) {
    ChansimParams cp = ccirGood();
    cp.snr3kDb = -12.0;
    cp.seed = uint64_t(1300 + t * 13);
    auto r = cssFrameTrial(Family::Narrow, 0, cp, 32, uint64_t(60 + t));
    pay += r.payloadOk ? 1 : 0;
  }
  std::printf("  NARROW M0 CCIR-Good @ -12 dB: payload %d/8\n", pay);
  CHECK(pay >= 3);
}

SKYTEST_MAIN
