#include "dsp/fir.hpp"

#include <cmath>

namespace skw {
namespace {

double besselI0(double x) {
  // Series expansion; converges quickly for the beta range we use.
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

int kaiserLowpassLength(double sampleRate, double transitionHz,
                        double attenDb) {
  const double dw = 2.0 * M_PI * transitionHz / sampleRate;
  int n = int(std::ceil((attenDb - 8.0) / (2.285 * dw)));
  if (n % 2 == 0) ++n;  // odd length -> integer group delay
  return n;
}

std::vector<float> designKaiserLowpass(double sampleRate, double passHz,
                                       double stopHz, double attenDb,
                                       double gain) {
  const double fc = (passHz + stopHz) / 2.0 / sampleRate;  // normalized cutoff
  // Kaiser estimates.
  double beta;
  if (attenDb > 50.0) beta = 0.1102 * (attenDb - 8.7);
  else if (attenDb >= 21.0) beta = 0.5842 * std::pow(attenDb - 21.0, 0.4) +
                                   0.07886 * (attenDb - 21.0);
  else beta = 0.0;
  const int n = kaiserLowpassLength(sampleRate, stopHz - passHz, attenDb);
  const int mid = n / 2;

  std::vector<float> h(static_cast<size_t>(n));
  const double i0b = besselI0(beta);
  for (int i = 0; i < n; ++i) {
    const int k = i - mid;
    double sinc = (k == 0) ? 2.0 * fc
                           : std::sin(2.0 * M_PI * fc * k) / (M_PI * k);
    double r = double(k) / mid;
    double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    h[size_t(i)] = float(gain * sinc * w);
  }
  return h;
}

// ------------------------------------------------------------- FirDecimator

FirDecimator::FirDecimator(std::vector<float> taps, int factor)
    : taps_(std::move(taps)), factor_(factor) {
  hist_.assign(taps_.size() - 1, cfloat(0.0f, 0.0f));
}

void FirDecimator::process(std::span<const cfloat> in,
                           std::vector<cfloat>& out) {
  // Simple history-buffer convolution; CPU budget is trivial at our rates.
  const size_t nt = taps_.size();
  std::vector<cfloat> buf(hist_.size() + in.size());
  std::copy(hist_.begin(), hist_.end(), buf.begin());
  std::copy(in.begin(), in.end(), buf.begin() + long(hist_.size()));

  for (size_t i = 0; i + nt <= buf.size() + 0; ++i) {
    // Output aligned to every factor-th input sample.
    if (phase_ == 0) {
      cfloat acc(0.0f, 0.0f);
      for (size_t t = 0; t < nt; ++t) acc += buf[i + t] * taps_[nt - 1 - t];
      out.push_back(acc);
    }
    phase_ = (phase_ + 1) % factor_;
  }
  // Keep last nt-1 samples as history.
  const size_t keep = nt - 1;
  std::copy(buf.end() - long(keep), buf.end(), hist_.begin());
}

// ----------------------------------------------------------- FirInterpolator

FirInterpolator::FirInterpolator(std::vector<float> taps, int factor)
    : taps_(std::move(taps)), factor_(factor) {
  // Pad taps to a multiple of factor for clean polyphase split.
  while (taps_.size() % size_t(factor_) != 0) taps_.push_back(0.0f);
  hist_.assign(taps_.size() / size_t(factor_) - 1, cfloat(0.0f, 0.0f));
}

void FirInterpolator::process(std::span<const cfloat> in,
                              std::vector<cfloat>& out) {
  // Polyphase: output phase p uses taps p, p+F, p+2F, ... over input history.
  const size_t perPhase = taps_.size() / size_t(factor_);
  std::vector<cfloat> buf(hist_.size() + in.size());
  std::copy(hist_.begin(), hist_.end(), buf.begin());
  std::copy(in.begin(), in.end(), buf.begin() + long(hist_.size()));

  for (size_t i = 0; i + perPhase <= buf.size(); ++i) {
    for (int p = 0; p < factor_; ++p) {
      cfloat acc(0.0f, 0.0f);
      for (size_t t = 0; t < perPhase; ++t) {
        acc += buf[i + t] * taps_[size_t(factor_) * (perPhase - 1 - t) + size_t(p)];
      }
      out.push_back(acc);
    }
  }
  const size_t keep = perPhase - 1;
  if (keep > 0) std::copy(buf.end() - long(keep), buf.end(), hist_.begin());
}

}  // namespace skw
