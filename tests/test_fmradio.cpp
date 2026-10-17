// FM radio emulator (sim/fmradio.hpp): the model the VHF tests, fmsweep
// and the ST2 VHF world rely on must behave like an FM link - a clean
// path returns the audio, above threshold the output SNR follows FM
// theory, below it the noise turns impulsive (clicks), emphasis pairs
// cancel, and the voice-path preset removes the upper VHF band.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/constants.hpp"
#include "sim/fmradio.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

struct ToneFit {
  double gainDb, snrDb, kurtosis;
};

// One second of a tone through the radio; least-squares fit of the input
// to the delay-aligned output, residual = noise + distortion.
ToneFit toneThrough(const FmRadioParams& p, double hz, double amp) {
  const size_t n = size_t(kFsAudio);
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2 * M_PI * hz * double(i) / kFsAudio));
  const auto y = fmRadio(p, x);
  const size_t lo = 4000, hi = n - 4000;
  double best = -1e30;
  size_t delay = 0;
  for (size_t d = 0; d < 3000; ++d) {
    double c = 0;
    for (size_t i = lo; i < hi; i += 7) c += double(x[i]) * y[i + d];
    if (c > best) { best = c; delay = d; }
  }
  double sxx = 0, sxy = 0;
  for (size_t i = lo; i < hi; ++i) {
    sxx += double(x[i]) * x[i];
    sxy += double(x[i]) * y[i + delay];
  }
  const double g = sxy / sxx;
  double e2 = 0, e4 = 0;
  for (size_t i = lo; i < hi; ++i) {
    const double r = y[i + delay] - g * x[i];
    e2 += r * r;
    e4 += r * r * r * r;
  }
  const double m = double(hi - lo);
  return {20 * std::log10(std::fabs(g) + 1e-30), 10 * std::log10(g * g * sxx / e2),
          (e4 / m) / ((e2 / m) * (e2 / m))};
}

}  // namespace

TEST_CASE(fm_clean_path_returns_the_audio) {
  for (double hz : {500.0, 2000.0, 6000.0}) {
    const ToneFit f = toneThrough(fmDataPort(), hz, 0.3);
    std::printf("  clean %4.0f Hz: gain %+.2f dB, SNR %.0f dB\n", hz, f.gainDb, f.snrDb);
    CHECK_NEAR(f.gainDb, 0.0, 0.2);
    CHECK(f.snrDb > 60.0);
  }
  // An RF offset is DC after the discriminator; the data-port high-pass
  // removes it.
  FmRadioParams p = fmDataPort();
  p.rfOffsetHz = 1000.0;
  CHECK(toneThrough(p, 2000.0, 0.3).snrDb > 50.0);
}

TEST_CASE(fm_noise_follows_theory_and_thresholds) {
  // Tone at 0.3 full scale = 900 Hz deviation, 300-7000 Hz detection,
  // 16 kHz IF: SNRout = 1.5 (900/6700)^2 * CNR * 16000/6700 = CNR - 11.9 dB.
  FmRadioParams p = fmDataPort();
  for (double cnr : {20.0, 30.0}) {
    p.cnrDb = cnr;
    const ToneFit f = toneThrough(p, 2000.0, 0.3);
    std::printf("  CNR %.0f: SNR %.1f dB (theory %.1f), kurtosis %.1f\n", cnr, f.snrDb,
                cnr - 11.9, f.kurtosis);
    CHECK_NEAR(f.snrDb, cnr - 11.9, 1.5);
    CHECK_NEAR(f.kurtosis, 3.0, 0.5);  // Gaussian
  }
  // Below the ~10 dB threshold the discriminator clicks: worse than the
  // linear law and impulsive.
  p.cnrDb = 4.0;
  const ToneFit f = toneThrough(p, 2000.0, 0.3);
  std::printf("  CNR 4: SNR %.1f dB (linear law %.1f), kurtosis %.1f\n", f.snrDb, 4.0 - 11.9,
              f.kurtosis);
  CHECK(f.snrDb < 4.0 - 11.9 - 2.0);
  CHECK(f.kurtosis > 4.0);
}

TEST_CASE(fm_emphasis_pair_is_flat_and_voice_path_cuts_the_top) {
  FmRadioParams p = fmDataPort();
  p.preEmphasisUs = p.deEmphasisUs = 750.0;
  for (double hz : {500.0, 2500.0, 6000.0}) {
    CHECK_NEAR(toneThrough(p, hz, 0.05).gainDb, 0.0, 0.3);
  }
  // The rig's voice/1200 path: 300-3000 Hz audio.
  CHECK(toneThrough(fmVoicePort(), 1000.0, 0.05).gainDb > -0.5);
  CHECK(toneThrough(fmVoicePort(), 5000.0, 0.05).gainDb < -40.0);
}

SKYTEST_MAIN
