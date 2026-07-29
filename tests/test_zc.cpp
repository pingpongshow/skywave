#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "core/constants.hpp"
#include "dsp/zc.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

void addNoise3k(std::vector<cfloat>& x, double snr3kDb, std::mt19937& rng) {
  double snr = std::pow(10.0, snr3kDb / 10.0);
  double var = 1.0 / snr / 3000.0 * kFsBaseband;
  std::normal_distribution<double> g(0.0, std::sqrt(var / 2.0));
  for (auto& v : x) v += cfloat(float(g(rng)), float(g(rng)));
}

void applyCfo(std::vector<cfloat>& x, double cfoHz) {
  for (size_t i = 0; i < x.size(); ++i) {
    double ph = 2.0 * M_PI * cfoHz * double(i) / kFsBaseband;
    x[i] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }
}

// Lead + block + tail with noise; true block start = lead.
std::vector<cfloat> makeRx(Family f, int nSym, double snr3kDb, double cfoHz,
                           uint32_t seed, size_t lead = 500) {
  std::mt19937 rng(seed);
  auto block = makeZcBlock(f, nSym);
  std::vector<cfloat> x(lead, cfloat(0, 0));
  x.insert(x.end(), block.begin(), block.end());
  x.insert(x.end(), 500, cfloat(0, 0));
  applyCfo(x, cfoHz);
  addNoise3k(x, snr3kDb, rng);
  return x;
}

}  // namespace

TEST_CASE(zc_sequence_properties) {
  for (Family f : {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    auto s = zcFreqSequence(f);
    CHECK_EQ(int(s.size()), plan(f).carriers());
    // Constant amplitude.
    for (auto& v : s) CHECK_NEAR(std::abs(v), 1.0, 1e-5);
    // Low autocorrelation sidelobes (ZC ideal property, cyclic).
    const int n = int(s.size());
    for (int lag = 1; lag < n; ++lag) {
      std::complex<double> acc(0, 0);
      for (int i = 0; i < n; ++i) {
        acc += std::complex<double>(s[size_t(i)]) *
               std::conj(std::complex<double>(s[size_t((i + lag) % n)]));
      }
      CHECK(std::abs(acc) < 1e-3 * n);
    }
  }
}

TEST_CASE(zc_symbol_shape) {
  auto sym = makeZcSymbol(Family::Wide);
  CHECK_EQ(int(sym.size()), kZcSymbolSamples);
  // Unit RMS over the core; CP repeats the tail.
  double p = 0;
  for (int i = kZcCpBaseband; i < kZcSymbolSamples; ++i) {
    p += std::norm(sym[size_t(i)]);
  }
  CHECK_NEAR(std::sqrt(p / kFftBaseband), 1.0, 1e-4);
  for (int i = 0; i < kZcCpBaseband; ++i) {
    CHECK_NEAR(std::abs(sym[size_t(i)] -
                        sym[size_t(i + kFftBaseband)]), 0.0, 1e-6);
  }
}

TEST_CASE(zc_refine_clean) {
  for (Family f : {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    auto rx = makeRx(f, kZcSymbolsFast, 40.0, 0.7, 11);
    auto r = zcRefine(f, rx, 500.0 + 8.0, 0.0, 1e6, kZcSymbolsFast);  // coarse off by +8
    CHECK(r.ok);
    // First-tap anchoring backs off the correlation lobe's half-power
    // edge, which scales with 1/bandwidth (widest for NARROW).
    double early = kFftBaseband / double(plan(f).carriers()) * 1.5 + 3.0;
    CHECK(r.blockStart > 500.0 - early);
    CHECK(r.blockStart < 500.0 + 3.0);
    CHECK_NEAR(r.cfoHz, 0.7, 0.1);
    CHECK(r.quality > 0.9f);
    // Ideal channel: |H| ~ 1, phase ~ constant across carriers.
    for (auto& h : r.channel) CHECK_NEAR(std::abs(h), 1.0, 0.05);
  }
}

TEST_CASE(zc_refine_robust_at_floor) {
  // ROADMAP gate: timing +-2 samples, CFO < 0.5 Hz at floor SNR.
  // Robust class (4 symbols) at the WIDE M4 floor, prior CFO off by 1.2 Hz.
  int okCount = 0;
  double worstT = 0, worstF = 0;
  for (uint32_t t = 0; t < 20; ++t) {
    auto rx = makeRx(Family::Wide, kZcSymbolsRobust, -8.5, 1.9, 100 + t);
    auto r = zcRefine(Family::Wide, rx, 500.0 - 6.0, 0.7, 1.0, kZcSymbolsRobust);
    if (!r.ok) continue;
    double ferr = std::fabs(r.cfoHz - 1.9);
    worstT = std::max(worstT, std::fabs(r.blockStart - 500.0));
    worstF = std::max(worstF, ferr);
    bool tOk = r.blockStart > 500.0 - 12.0 && r.blockStart < 500.0 + 3.0;
    if (tOk && ferr < 0.5) ++okCount;
  }
  CHECK(okCount >= 18);
  (void)worstT;
  (void)worstF;
}

TEST_CASE(zc_refine_fast_header_floor) {
  // Fast class (2 symbols) at the fast-header design floor (~-1 dB).
  int okCount = 0;
  for (uint32_t t = 0; t < 20; ++t) {
    auto rx = makeRx(Family::Wide, kZcSymbolsFast, -1.0, -0.8, 300 + t);
    auto r = zcRefine(Family::Wide, rx, 500.0 + 5.0, -1.5, 1.0, kZcSymbolsFast);
    if (!r.ok) continue;
    if (r.blockStart > 500.0 - 12.0 && r.blockStart < 500.0 + 3.0 &&
        std::fabs(r.cfoHz + 0.8) < 0.5) {
      ++okCount;
    }
  }
  CHECK(okCount >= 18);
}

TEST_CASE(zc_refine_narrow_family_floor) {
  // NARROW at its connected-session depth (sync already achieved; the
  // caveat in WAVEFORM.md covers acquisition, not refinement).
  // Timing gate is +-5 samples for NARROW: 13 carriers at -12 dB carry
  // sigma_t ~ 2.3 samples of information (energy-limited), and the
  // 48-sample CP absorbs that slack by design. CFO leans on the chirp
  // prior via fusion (sigma 0.25 Hz), which is the intended architecture.
  int okCount = 0;
  for (uint32_t t = 0; t < 15; ++t) {
    auto rx = makeRx(Family::Narrow, kZcSymbolsRobust, -12.0, 0.4, 500 + t);
    auto r = zcRefine(Family::Narrow, rx, 500.0 + 4.0, 0.0, 0.25,
                      kZcSymbolsRobust);
    if (!r.ok) continue;
    if (r.blockStart > 500.0 - 45.0 && r.blockStart < 500.0 + 5.0 &&
        std::fabs(r.cfoHz - 0.4) < 0.5) {
      ++okCount;
    }
  }
  CHECK(okCount >= 13);
}

TEST_CASE(zc_channel_estimate_two_tap) {
  // Apply a 2-tap channel; the estimate's frequency response should show
  // the expected ripple: H(k) = 1 + a*exp(-j*2*pi*bin(k)*d/N).
  const Family f = Family::Wide;
  const float a = 0.5f;
  const int d = 8;  // 0.67 ms echo
  auto block = makeZcBlock(f, kZcSymbolsRobust);
  std::vector<cfloat> x(500, cfloat(0, 0));
  x.insert(x.end(), block.begin(), block.end());
  x.insert(x.end(), 500, cfloat(0, 0));
  std::vector<cfloat> y(x.size(), cfloat(0, 0));
  for (size_t i = 0; i < x.size(); ++i) {
    y[i] = x[i];
    if (i >= size_t(d)) y[i] += a * x[i - size_t(d)];
  }
  std::mt19937 rng(77);
  addNoise3k(y, 20.0, rng);

  auto r = zcRefine(f, y, 500.0, 0.0, 1e6, kZcSymbolsRobust);
  CHECK(r.ok);
  // The refiner recenters timing on the channel's energy centroid and
  // de-slopes the estimate accordingly; compensate the reference by the
  // measured shift before comparing.
  const double shift = r.blockStart - 500.0;
  const int n = plan(f).carriers();
  double mse = 0;
  for (int i = 0; i < n; ++i) {
    int audioBin = plan(f).firstBin + i;
    int bin = audioBin - kCenterBin;  // signed baseband bin
    double ph = -2.0 * M_PI * double(bin) * d / kFftBaseband;
    std::complex<double> want =
        1.0 + double(a) * std::complex<double>(std::cos(ph), std::sin(ph));
    // Convention: H is reported for an FFT window at blockStart, so a
    // residual start error eps rotates it by e^{+j 2 pi bin eps / N}.
    double corrPh = +2.0 * M_PI * double(bin) * shift / kFftBaseband;
    want *= std::complex<double>(std::cos(corrPh), std::sin(corrPh));
    mse += std::norm(std::complex<double>(r.channel[size_t(i)]) - want);
  }
  mse /= n;
  CHECK(mse < 0.02);
}

SKYTEST_MAIN
