#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "core/constants.hpp"
#include "dsp/fft.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {
std::vector<cfloat> ones(size_t n) {
  return std::vector<cfloat>(n, cfloat(1.0f, 0.0f));
}
std::vector<cfloat> tone(size_t n, double freqHz) {
  std::vector<cfloat> x(n);
  for (size_t i = 0; i < n; ++i) {
    double ph = 2.0 * M_PI * freqHz * double(i) / kFsBaseband;
    x[i] = cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }
  return x;
}
}  // namespace

TEST_CASE(chansim_deterministic) {
  ChansimParams p = ccirModerate();
  p.snr3kDb = 10.0;
  p.seed = 42;
  auto in = tone(6000, 300.0);
  auto a = Chansim::run(p, in);
  auto b = Chansim::run(p, in);
  CHECK(a == b);
  p.seed = 43;
  auto c = Chansim::run(p, in);
  CHECK(a != c);
}

TEST_CASE(chansim_unit_energy_fading) {
  // Long-run mean output power ~ input power (tap normalization), no noise.
  ChansimParams p = ccirPoor();  // fastest fading -> best averaging
  p.snr3kDb = 99.0;
  p.seed = 7;
  const size_t n = size_t(kFsBaseband) * 120;  // 120 s
  auto out = Chansim::run(p, ones(n));
  double pow = 0;
  for (auto& v : out) pow += std::norm(v);
  pow /= double(out.size());
  CHECK(pow > 0.7);
  CHECK(pow < 1.4);  // Rayleigh averaging over ~120 fades
}

TEST_CASE(chansim_doppler_spectrum_width) {
  // Gaussian PSD (sigma = spread/2) <-> Gaussian autocorrelation:
  // R(tau)/R(0) = exp(-2 pi^2 sigma^2 tau^2). Autocorrelation estimation
  // avoids the FFT-leakage bias that corrupts spectral second moments.
  for (double spread : {0.5, 1.0}) {
    ChansimParams p;
    p.delaySpreadMs = 0.0;
    p.dopplerSpreadHz = spread;
    p.snr3kDb = 99.0;
    p.seed = 11;
    const size_t n = size_t(kFsBaseband) * 240;
    auto out = Chansim::run(p, ones(n));

    const size_t lag = size_t(0.3 * kFsBaseband);  // tau = 0.3 s
    std::complex<double> num(0, 0);
    double den = 0;
    for (size_t i = 0; i + lag < out.size(); ++i) {
      num += std::complex<double>(out[i + lag]) *
             std::conj(std::complex<double>(out[i]));
      den += std::norm(std::complex<double>(out[i]));
    }
    double r = std::abs(num) / den;
    CHECK(r > 0.05);
    CHECK(r < 0.999);
    double tau = double(lag) / kFsBaseband;
    double sigma = std::sqrt(-std::log(r) / (2.0 * M_PI * M_PI * tau * tau));
    CHECK_NEAR(sigma, spread / 2.0, spread * 0.15);
  }
}

TEST_CASE(chansim_delay_spread_two_taps) {
  // Cross-correlate output with a white-ish input: peaks at 0 and D.
  ChansimParams p = ccirPoor();  // D = 24 samples
  p.dopplerSpreadHz = 0.02;      // nearly static so taps stay strong
  p.snr3kDb = 99.0;
  p.seed = 3;
  // White input (proper generator: a raw LCG has strong lag correlations
  // that swamp the cross-correlation floor).
  std::mt19937 rng(99);
  std::normal_distribution<float> g(0.0f, 1.0f);
  std::vector<cfloat> in(size_t(kFsBaseband) * 2);
  for (auto& v : in) v = cfloat(g(rng), g(rng));
  auto out = Chansim::run(p, in);
  double e0 = 0, eD = 0, eOther = 0;
  for (int lag : {0, 24, 11}) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i + 30 < in.size(); ++i) {
      acc += std::complex<double>(out[i + size_t(lag)]) *
             std::conj(std::complex<double>(in[i]));
    }
    double m = std::abs(acc);
    if (lag == 0) e0 = m;
    else if (lag == 24) eD = m;
    else eOther = m;
  }
  CHECK(e0 > 10.0 * eOther);
  CHECK(eD > 10.0 * eOther);
}

TEST_CASE(chansim_awgn_calibration) {
  // Zero input, refPower 1, snr 0 dB(3k): noise var = 1/3000*12000 = 4.
  ChansimParams p;
  p.fading = false;
  p.snr3kDb = 0.0;
  p.refSignalPower = 1.0;
  p.seed = 5;
  auto out =
      Chansim::run(p, std::vector<cfloat>(size_t(kFsBaseband) * 4, cfloat()));
  double var = 0;
  for (auto& v : out) var += std::norm(v);
  var /= double(out.size());
  CHECK_NEAR(var, 4.0, 0.15);
}

TEST_CASE(chansim_cfo_shift) {
  ChansimParams p;
  p.fading = false;
  p.snr3kDb = 99.0;
  p.cfoHz = 13.0;
  auto out = Chansim::run(p, tone(size_t(kFsBaseband) * 2, 100.0));
  // Measure frequency via average phase increment.
  std::complex<double> rot(0, 0);
  for (size_t i = 1; i < out.size(); ++i) {
    rot += std::complex<double>(out[i]) *
           std::conj(std::complex<double>(out[i - 1]));
  }
  double f = std::atan2(rot.imag(), rot.real()) * kFsBaseband / (2 * M_PI);
  CHECK_NEAR(f, 113.0, 0.5);
}

TEST_CASE(chansim_clock_offset) {
  ChansimParams p;
  p.fading = false;
  p.snr3kDb = 99.0;
  p.clockPpm = 50.0;
  const size_t n = size_t(kFsBaseband) * 10;
  auto out = Chansim::run(p, tone(n, 0.0));
  // Output shorter by ~ppm.
  double expect = double(n) / (1.0 + 50e-6);
  CHECK_NEAR(double(out.size()), expect, 8.0);
}

SKYTEST_MAIN
