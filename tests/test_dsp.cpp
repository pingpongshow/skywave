#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "core/constants.hpp"
#include "dsp/agc.hpp"
#include "dsp/fft.hpp"
#include "dsp/updown.hpp"
#include "dsp/window.hpp"
#include "skytest.hpp"

using namespace skw;
using cd = std::complex<double>;

namespace {

// Naive double-precision DFT reference.
std::vector<cd> naiveDft(const std::vector<cfloat>& x) {
  const size_t n = x.size();
  std::vector<cd> out(n);
  for (size_t k = 0; k < n; ++k) {
    cd acc(0.0, 0.0);
    for (size_t j = 0; j < n; ++j) {
      double a = -2.0 * M_PI * double(j * k % n) / double(n);
      acc += cd(x[j].real(), x[j].imag()) * cd(std::cos(a), std::sin(a));
    }
    out[k] = acc;
  }
  return out;
}

double relError(const std::vector<cfloat>& got, const std::vector<cd>& ref) {
  double errSq = 0.0, refSq = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    cd d = cd(got[i].real(), got[i].imag()) - ref[i];
    errSq += std::norm(d);
    refSq += std::norm(ref[i]);
  }
  return std::sqrt(errSq / (refSq > 0 ? refSq : 1.0));
}

// Fit a complex tone of known frequency to data; return residual SNR in dB.
double toneSnrDb(const std::vector<cfloat>& x, double freqHz, double rate,
                 size_t skip) {
  cd corr(0.0, 0.0);
  double power = 0.0;
  size_t n = 0;
  for (size_t i = skip; i < x.size(); ++i) {
    double ph = 2.0 * M_PI * freqHz * double(i) / rate;
    cd tone(std::cos(ph), std::sin(ph));
    cd v(x[i].real(), x[i].imag());
    corr += v * std::conj(tone);
    power += std::norm(v);
    ++n;
  }
  if (n == 0) return -999.0;
  cd amp = corr / double(n);           // best-fit complex amplitude
  double sigPow = std::norm(amp) * double(n);
  double errPow = power - sigPow;      // residual after removing the tone
  if (errPow <= 0) return 200.0;
  return 10.0 * std::log10(sigPow / errPow);
}

}  // namespace

TEST_CASE(fft_impulse_and_dc) {
  Fft fft(320);
  std::vector<cfloat> x(320, cfloat(0, 0)), X(320);
  x[0] = cfloat(1, 0);
  fft.forward(x.data(), X.data());
  for (auto& v : X) CHECK_NEAR(std::abs(std::complex<float>(v) - cfloat(1, 0)), 0.0, 1e-4);

  std::fill(x.begin(), x.end(), cfloat(1, 0));
  fft.forward(x.data(), X.data());
  CHECK_NEAR(X[0].real(), 320.0, 1e-2);
  for (size_t k = 1; k < 320; ++k) CHECK(std::abs(X[k]) < 1e-3f);
}

TEST_CASE(fft_matches_naive_dft) {
  std::mt19937 rng(1234);
  std::normal_distribution<float> g(0.0f, 1.0f);
  for (int n : {320, 240, 60, 256, 1024, 27, 100}) {
    Fft fft(n);
    std::vector<cfloat> x(static_cast<size_t>(n)), X(static_cast<size_t>(n));
    for (auto& v : x) v = cfloat(g(rng), g(rng));
    fft.forward(x.data(), X.data());
    CHECK(relError(X, naiveDft(x)) < 2e-6);
  }
}

TEST_CASE(fft_roundtrip_and_parseval) {
  std::mt19937 rng(99);
  std::normal_distribution<float> g(0.0f, 1.0f);
  Fft fft(320);
  std::vector<cfloat> x(320), X(320), y(320);
  for (auto& v : x) v = cfloat(g(rng), g(rng));
  fft.forward(x.data(), X.data());
  fft.inverse(X.data(), y.data());
  double err = 0, ref = 0, tp = 0, fp = 0;
  for (size_t i = 0; i < 320; ++i) {
    err += std::norm(std::complex<double>(y[i].real() - x[i].real(),
                                          y[i].imag() - x[i].imag()));
    ref += std::norm(std::complex<double>(x[i].real(), x[i].imag()));
    tp += std::norm(std::complex<double>(x[i].real(), x[i].imag()));
    fp += std::norm(std::complex<double>(X[i].real(), X[i].imag()));
  }
  CHECK(std::sqrt(err / ref) < 1e-6);
  CHECK_NEAR(fp / 320.0, tp, tp * 1e-5);  // Parseval
}

TEST_CASE(fft_rejects_bad_size) {
  bool threw = false;
  try {
    Fft f(322);  // 2 * 7 * 23: not smooth
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST_CASE(windows_sane) {
  auto h = hannWindow(64);
  CHECK_NEAR(h[0], 0.0, 1e-6);
  CHECK_NEAR(h[32], 1.0, 1e-6);
  auto r = raisedCosineRamp(16);
  CHECK(r[0] > 0.0f && r[0] < 0.05f);
  CHECK(r[15] > 0.95f && r[15] < 1.0f);
  for (size_t i = 1; i < r.size(); ++i) CHECK(r[i] > r[i - 1]);
}

TEST_CASE(downconverter_tone_snr) {
  // ROADMAP 1.2 gate: chain SNR > 60 dB across the band.
  for (double f : {300.0, 1000.0, 1500.0, 2500.0, 2850.0}) {
    Downconverter dc;
    std::vector<float> in(48000);  // 1 s
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = float(std::cos(2.0 * M_PI * f * double(i) / kFsAudio));
    }
    std::vector<cfloat> out;
    dc.process(in, out);
    double snr = toneSnrDb(out, f - kCenterHz, kFsBaseband, 3000);
    CHECK(snr > 60.0);
  }
}

TEST_CASE(down_up_roundtrip_snr) {
  // Complex baseband tone -> TX -> RX -> should recover the same tone.
  for (double off : {-1200.0, -400.0, 0.0, 700.0, 1300.0}) {
    Upconverter up;
    Downconverter down;
    std::vector<cfloat> in(12000);  // 1 s at 12 kHz
    for (size_t i = 0; i < in.size(); ++i) {
      double ph = 2.0 * M_PI * off * double(i) / kFsBaseband;
      in[i] = cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
    std::vector<float> mid;
    up.process(in, mid);
    std::vector<cfloat> out;
    down.process(mid, out);
    double snr = toneSnrDb(out, off, kFsBaseband, 4000);
    CHECK(snr > 60.0);
  }
}

TEST_CASE(dc_blocker_removes_offset) {
  DcBlocker dcb;
  std::vector<float> buf(48000);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = 0.5f + 0.25f * float(std::sin(2.0 * M_PI * 1000.0 * double(i) / 48000.0));
  }
  dcb.process(buf);
  // Mean of the tail should be ~0 while the tone survives.
  double mean = 0, power = 0;
  for (size_t i = 24000; i < buf.size(); ++i) {
    mean += buf[i];
    power += double(buf[i]) * buf[i];
  }
  mean /= 24000.0;
  power /= 24000.0;
  CHECK(std::fabs(mean) < 1e-3);
  CHECK_NEAR(power, 0.25 * 0.25 / 2.0, 0.002);  // tone power intact
}

TEST_CASE(agc_dynamics) {
  Agc agc(48000.0);
  // Strong constant tone at -6 dBFS: gain must pull it toward -18 dBFS.
  std::vector<float> buf(48000);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = 0.5f * float(std::sin(2.0 * M_PI * 1000.0 * double(i) / 48000.0));
  }
  agc.process(buf);
  double rms = 0;
  for (size_t i = 40000; i < buf.size(); ++i) rms += double(buf[i]) * buf[i];
  rms = std::sqrt(rms / 8000.0);
  double dbfs = 20.0 * std::log10(rms);
  CHECK(dbfs > -22.0);
  CHECK(dbfs < -14.0);

  // Freeze: gain must not move while frozen.
  agc.setFrozen(true);
  float g0 = agc.gain();
  std::vector<float> loud(4800, 0.9f);
  agc.process(loud);
  CHECK_NEAR(agc.gain(), g0, 1e-9);
}

SKYTEST_MAIN
