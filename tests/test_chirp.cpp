#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>

#include "core/constants.hpp"
#include "dsp/chirp.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

// AWGN at a target SNR referenced to 3 kHz noise bandwidth: noise density
// N0 = Pn3k / 3000 with Pn3k = Psig / snr3k. Signal is unit-RMS complex, so
// complex noise variance across the 12 kHz baseband = N0 * 12000.
void addNoise3k(std::vector<cfloat>& x, double snr3kDb, std::mt19937& rng) {
  double snr = std::pow(10.0, snr3kDb / 10.0);
  double n0 = 1.0 / snr / 3000.0;
  double var = n0 * kFsBaseband;
  std::normal_distribution<double> g(0.0, std::sqrt(var / 2.0));
  for (auto& v : x) v += cfloat(float(g(rng)), float(g(rng)));
}

std::vector<cfloat> noiseOnly(size_t n, double power, std::mt19937& rng) {
  std::normal_distribution<double> g(0.0, std::sqrt(power / 2.0));
  std::vector<cfloat> x(n);
  for (auto& v : x) v = cfloat(float(g(rng)), float(g(rng)));
  return x;
}

void applyCfo(std::vector<cfloat>& x, double cfoHz) {
  for (size_t i = 0; i < x.size(); ++i) {
    double ph = 2.0 * M_PI * cfoHz * double(i) / kFsBaseband;
    x[i] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }
}

// Builds lead noise + preamble + tail, runs the detector, returns detections.
std::vector<ChirpDetection> runOne(Family fam, ChirpSig sig, double snr3kDb,
                                   double cfoHz, bool invert, uint32_t seed,
                                   size_t leadSamples = 6000) {
  std::mt19937 rng(seed);
  auto pre = makeChirpPreamble(fam, sig);
  applyCfo(pre, cfoHz);
  if (invert) {
    for (auto& v : pre) v = std::conj(v);
  }
  std::vector<cfloat> sig12(leadSamples, cfloat(0, 0));
  sig12.insert(sig12.end(), pre.begin(), pre.end());
  // Tail: a preamble is always followed by frame content, and the 4-rep
  // decision waits for the longest pattern's horizon (NARROW's 12-rep CSS
  // preamble: 11 reps + suppression), so give it 14 reps.
  sig12.insert(sig12.end(), size_t(14) * size_t(chirpRepSamples()), cfloat(0, 0));
  addNoise3k(sig12, snr3kDb, rng);

  ChirpDetector det(fam);
  std::vector<ChirpDetection> out;
  // Feed in modem-worker-sized chunks.
  const size_t chunk = 368;
  for (size_t i = 0; i < sig12.size(); i += chunk) {
    size_t n = std::min(chunk, sig12.size() - i);
    det.process({sig12.data() + i, n}, out);
  }
  return out;
}

}  // namespace

TEST_CASE(chirp_patterns_distinct_under_inversion) {
  // All 8 observable slope sequences (4-rep CSS on WIDE) must be
  // mutually distinct.
  auto seq = [](ChirpSig s, bool inv) {
    auto p = chirpPattern(Family::Wide, s);
    if (inv) {
      for (size_t i = 0; i < p.size(); ++i) p[i] = !p[i];
    }
    return p;
  };
  std::vector<std::vector<bool>> all;
  for (auto s : {ChirpSig::Fast, ChirpSig::Robust, ChirpSig::Calling,
                 ChirpSig::Css}) {
    for (bool inv : {false, true}) all.push_back(seq(s, inv));
  }
  for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = i + 1; j < all.size(); ++j) CHECK(all[i] != all[j]);
  }
}

TEST_CASE(chirp_detects_clean) {
  for (Family fam : {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    for (ChirpSig sig : {ChirpSig::Fast, ChirpSig::Robust, ChirpSig::Calling}) {
      auto out = runOne(fam, sig, 30.0, 0.0, false, 1);
      CHECK_EQ(out.size(), 1u);
      if (out.size() == 1) {
        CHECK(out[0].sig == sig);
        CHECK(!out[0].inverted);
        CHECK_NEAR(out[0].startSample, 6000.0, 12.0);  // +-1 ms
        CHECK_NEAR(out[0].cfoHz, 0.0, 1.0);
      }
    }
  }
}

TEST_CASE(chirp_detection_at_floor) {
  // Calling preamble at the family acquisition floors (3 kHz ref).
  struct P { Family f; double snr; };
  const P pts[] = {{Family::Wide, -12.0}, {Family::Mid, -14.0},
                   {Family::Narrow, -15.0}, {Family::Ultra, -12.0}};
  for (const auto& p : pts) {
    int hits = 0;
    const int trials = 25;
    for (int t = 0; t < trials; ++t) {
      auto out = runOne(p.f, ChirpSig::Calling, p.snr, 0.0, false,
                        1000 + uint32_t(t));
      if (out.size() == 1 && out[0].sig == ChirpSig::Calling) ++hits;
    }
    CHECK(hits >= 23);  // >= 92% with 25 trials (spec 99%; CI-safe bound)
  }
}

TEST_CASE(chirp_fast_at_connected_floor) {
  int hits = 0;
  for (int t = 0; t < 20; ++t) {
    auto out = runOne(Family::Wide, ChirpSig::Fast, -9.0, 0.0, false,
                      2000 + uint32_t(t));
    if (out.size() == 1 && out[0].sig == ChirpSig::Fast) ++hits;
  }
  CHECK(hits >= 18);
}

TEST_CASE(chirp_cfo_estimation_wide_capture) {
  // Calling signature must resolve CFO across +-50 Hz to < 1.5 Hz at 0 dB.
  for (double cfo : {-50.0, -30.0, -12.5, 0.0, 7.0, 25.0, 50.0}) {
    auto out = runOne(Family::Wide, ChirpSig::Calling, 0.0, cfo, false,
                      uint32_t(3000 + int(cfo) + 60));
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1) {
      CHECK(out[0].sig == ChirpSig::Calling);  // must not fall back to Fast
      CHECK_NEAR(out[0].cfoHz, cfo, 1.5);
    }
  }
  // Narrow family: much larger delay shift per Hz; same requirement.
  auto out = runOne(Family::Narrow, ChirpSig::Calling, 0.0, 20.0, false, 3999);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1) {
    CHECK(out[0].sig == ChirpSig::Calling);
    CHECK_NEAR(out[0].cfoHz, 20.0, 1.5);
  }
}

TEST_CASE(chirp_robust_cfo_connected_range) {
  for (double cfo : {-5.0, -2.0, 3.0, 5.0}) {
    auto out = runOne(Family::Wide, ChirpSig::Robust, 5.0, cfo, false,
                      uint32_t(4000 + int(cfo) + 10));
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1) {
      CHECK(out[0].sig == ChirpSig::Robust);
      CHECK_NEAR(out[0].cfoHz, cfo, 1.0);
    }
  }
}

TEST_CASE(chirp_inversion_detected) {
  for (ChirpSig sig : {ChirpSig::Fast, ChirpSig::Robust, ChirpSig::Calling}) {
    auto out = runOne(Family::Wide, sig, 10.0, 0.0, true, 5001);
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1) {
      CHECK(out[0].sig == sig);
      CHECK(out[0].inverted);
    }
  }
}

TEST_CASE(chirp_no_false_alarms_in_noise) {
  // 30 s of pure noise per family; zero classifications allowed.
  std::mt19937 rng(6001);
  for (Family fam : {Family::Wide, Family::Narrow}) {
    auto x = noiseOnly(size_t(30) * kFsBaseband, 1.0, rng);
    ChirpDetector det(fam);
    std::vector<ChirpDetection> out;
    const size_t chunk = 4096;
    for (size_t i = 0; i < x.size(); i += chunk) {
      det.process({x.data() + i, std::min(chunk, x.size() - i)}, out);
    }
    CHECK_EQ(out.size(), 0u);
  }
}

// Regression (ST3 kill/resume stall, 2026-10): EXACT digital silence - a
// virtual audio cable whose writer is idle or dead, a muted interface -
// collapsed the noise-NCC floor to 0 and the detector then emitted a
// phantom q=0.00 detection every ~0.65 s, indefinitely. Each phantom costs
// the worker a capture climbing the full 90 s ladder; on the real device
// path that put the receiver behind real time and lost handshake frames.
// Silence must produce nothing, and a preamble after long silence (and
// after an unrelated burst) must still be detected exactly once.
TEST_CASE(chirp_no_phantoms_in_digital_silence) {
  std::mt19937 rng(7001);
  for (Family fam : {Family::Wide, Family::Narrow}) {
    const size_t fs = size_t(kFsBaseband);
    std::vector<cfloat> x(120 * fs, cfloat(0, 0));       // 120 s of zeros
    auto burst = noiseOnly(4 * fs, 1.0, rng);             // non-preamble burst
    x.insert(x.end(), burst.begin(), burst.end());
    x.insert(x.end(), 40 * fs, cfloat(0, 0));             // 40 s of zeros
    const size_t at = x.size();
    auto pre = makeChirpPreamble(fam, ChirpSig::Calling);
    x.insert(x.end(), pre.begin(), pre.end());
    x.insert(x.end(), 5 * fs, cfloat(0, 0));
    ChirpDetector det(fam);
    std::vector<ChirpDetection> out;
    const size_t chunk = 368;
    for (size_t i = 0; i < x.size(); i += chunk) {
      det.process({x.data() + i, std::min(chunk, x.size() - i)}, out);
    }
    std::printf("digital silence %s: %zu detection(s)\n",
                fam == Family::Wide ? "wide" : "narrow", out.size());
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1) {
      CHECK(out[0].sig == ChirpSig::Calling);
      CHECK_NEAR(out[0].startSample, double(at), 12.0);
    }
  }
}

TEST_CASE(chirp_timing_accuracy_with_noise) {
  for (int t = 0; t < 6; ++t) {
    auto out = runOne(Family::Wide, ChirpSig::Calling, -6.0, 0.0, false,
                      7000 + uint32_t(t));
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1) CHECK_NEAR(out[0].startSample, 6000.0, 12.0);
  }
}

SKYTEST_MAIN

// NARROW and MID CSS frames carry a 12-rep preamble (the 4-rep signature
// was the deep-tier acquisition limit on narrow bands). At the full-frame
// M0 floors (NARROW -18.5, MID -17.5 dB) the 12-rep preamble is detected
// and classified CSS 25/25 and 23/25; the 4-rep signature managed 8/25
// and 13/25 (SKW_CSS_PREAMBLE_<FAMILY>=UDDU).
TEST_CASE(chirp_long_css_preamble_at_floor) {
  struct P { Family f; double snr; };
  for (const P& p : {P{Family::Narrow, -18.5}, P{Family::Mid, -17.5}}) {
    CHECK_EQ(chirpPattern(p.f, ChirpSig::Css).size(), size_t(12));
    int hits = 0;
    const int trials = 25;
    for (int t = 0; t < trials; ++t) {
      auto out = runOne(p.f, ChirpSig::Css, p.snr, 0.0, false,
                        5000 + uint32_t(t));
      // Detected and classified CSS, start inside the receiver's sync
      // window; the CFO branch (6.25 Hz ambiguity) is the receiver's to
      // resolve (sync branch check / branch retry), not the detector's.
      if (!out.empty() && out[0].sig == ChirpSig::Css &&
          std::abs(out[0].startSample - 6000.0) < 100.0) {
        ++hits;
      } else if (std::getenv("SKW_TEST_VERBOSE")) {
        std::printf("    miss t=%d n=%zu", t, out.size());
        for (auto& d : out) std::printf(" [sig=%d start=%.0f cfo=%.1f]", int(d.sig), d.startSample, d.cfoHz);
        std::printf("\n");
      }
    }
    std::printf("  %s CSS 12-rep @ %.1f dB: %d/%d\n", plan(p.f).name, p.snr,
                hits, trials);
    CHECK(hits >= 22);
  }
  // WIDE/ULTRA keep the 4-rep CSS signature.
  CHECK_EQ(chirpPattern(Family::Wide, ChirpSig::Css).size(), size_t(4));
  CHECK_EQ(chirpPattern(Family::Ultra, ChirpSig::Css).size(), size_t(4));
}
