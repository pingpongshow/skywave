#include "dsp/zc.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fft.hpp"
#include "dsp/fir.hpp"

namespace skw {
namespace {

// Per-family ZC roots (coprime with the carrier count).
int zcRoot(Family f) {
  switch (f) {
    case Family::Wide: return 25;    // Nzc = 64
    case Family::Mid: return 8;      // Nzc = 27
    case Family::Narrow: return 5;   // Nzc = 13
    case Family::Ultra: return 25;   // Nzc = 72 (25 coprime with 72)
  }
  return 1;
}

// Map an active-carrier index (0-based within the family) to a baseband
// FFT bin of the 320-point transform.
int carrierToBin(Family f, int carrierIdx) {
  int audioBin = plan(f).firstBin + carrierIdx;
  int b = audioBin - kCenterBin;  // baseband bin, may be negative
  return b >= 0 ? b : kFftBaseband + b;
}

}  // namespace

std::vector<cfloat> zcFreqSequence(Family family) {
  const int n = plan(family).carriers();
  const int q = zcRoot(family);
  std::vector<cfloat> seq(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    // Even length: exp(-j pi q i^2 / N); odd: exp(-j pi q i(i+1) / N).
    double num = (n % 2 == 0) ? double(i) * i : double(i) * (i + 1);
    double phase = -M_PI * q * num / n;
    seq[size_t(i)] = cfloat(float(std::cos(phase)), float(std::sin(phase)));
  }
  return seq;
}

std::vector<cfloat> makeZcSymbol(Family family) {
  const auto seq = zcFreqSequence(family);
  const int n = plan(family).carriers();
  Fft fft(kFftBaseband);
  std::vector<cfloat> freq(size_t(kFftBaseband), cfloat(0, 0));
  for (int i = 0; i < n; ++i) {
    freq[size_t(carrierToBin(family, i))] = seq[size_t(i)];
  }
  std::vector<cfloat> core(static_cast<size_t>(kFftBaseband));
  fft.inverse(freq.data(), core.data());

  // Unit RMS over the core.
  double p = 0;
  for (const auto& c : core) p += std::norm(c);
  float scale = float(1.0 / std::sqrt(p / kFftBaseband));
  for (auto& c : core) c *= scale;

  std::vector<cfloat> sym;
  sym.reserve(size_t(kZcSymbolSamples));
  sym.insert(sym.end(), core.end() - kZcCpBaseband, core.end());  // CP
  sym.insert(sym.end(), core.begin(), core.end());
  return sym;
}

std::vector<cfloat> makeZcBlock(Family family, int nSymbols) {
  auto sym = makeZcSymbol(family);
  std::vector<cfloat> block;
  block.reserve(sym.size() * size_t(nSymbols));
  for (int i = 0; i < nSymbols; ++i) {
    block.insert(block.end(), sym.begin(), sym.end());
  }
  return block;
}

ZcSyncResult zcRefine(Family family, std::span<const cfloat> rx,
                      double coarseStart, double priorCfoHz,
                      double priorCfoSigmaHz, int nSymbols,
                      int searchRadius) {
  ZcSyncResult res;
  const int symLen = kZcSymbolSamples;
  const int core = kFftBaseband;
  const auto ref = makeZcSymbol(family);  // CP + core, unit RMS

  // Derotate the whole span by the prior CFO so residual is small.
  // (No pre-filter: a matched correlation already projects noise onto the
  // reference's spectral support, so band-limiting first is a no-op for
  // every estimate formed here.)
  std::vector<cfloat> x(rx.size());
  for (size_t i = 0; i < rx.size(); ++i) {
    double ph = -2.0 * M_PI * priorCfoHz * double(i) / kFsBaseband;
    x[i] = rx[i] * cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }

  // 1) Timing: matched filter against the known symbol core, summed
  //    noncoherently across the block's symbols.
  const int c0 = int(std::lround(coarseStart));
  int bestOff = 0;
  double bestMetric = -1.0;
  std::vector<double> metric(size_t(2 * searchRadius + 1), 0.0);
  for (int off = -searchRadius; off <= searchRadius; ++off) {
    double m = 0;
    for (int s = 0; s < nSymbols; ++s) {
      const int base = c0 + off + s * symLen + kZcCpBaseband;
      if (base < 0 || base + core > int(x.size())) continue;
      cfloat acc(0, 0);
      for (int i = 0; i < core; ++i) {
        acc += x[size_t(base + i)] *
               std::conj(ref[size_t(kZcCpBaseband + i)]);
      }
      m += std::norm(acc);
    }
    metric[size_t(off + searchRadius)] = m;
    if (m > bestMetric) {
      bestMetric = m;
      bestOff = off;
    }
  }
  if (bestMetric <= 0.0) return res;

  // Sub-sample timing comes from the channel impulse peak (step 4); the
  // integer matched-filter argmax is enough here.
  const int t0 = c0 + bestOff;
  res.blockStart = double(t0);

  // 2) CFO residual from the phase progression of per-symbol MATCHED
  //    FILTER outputs m_s = <x_s, ref>. Blind pair autocorrelation is
  //    useless here: at the robust floor the per-sample SNR is ~-15 dB and
  //    the noise x noise term dominates (sigma ~ 3 Hz); the matched filter
  //    is coherent over the known sequence and inherently in-band, giving
  //    sigma ~ 0.2 Hz with 4 symbols. Timing error adds the same phase to
  //    every m_s and cancels in the differences.
  // Correlate over core minus a 16-sample tail margin: if residual timing
  // error is positive, the last symbol's window would otherwise spill into
  // whatever follows the block, biasing its phase and thus the CFO slope.
  const int mLen = core - 16;
  std::vector<cfloat> m(static_cast<size_t>(nSymbols), cfloat(0, 0));
  int mCount = 0;
  for (int s = 0; s < nSymbols; ++s) {
    const int base = t0 + s * symLen + kZcCpBaseband;
    if (base < 0 || base + mLen > int(x.size())) continue;
    cfloat acc(0, 0);
    for (int i = 0; i < mLen; ++i) {
      acc += x[size_t(base + i)] * std::conj(ref[size_t(kZcCpBaseband + i)]);
    }
    m[size_t(s)] = acc;
    ++mCount;
  }
  // ML frequency fit: maximize |sum_s m_s e^{-j w s}| over the residual
  // range (prior error is bounded by the chirp handoff, so +-3 Hz), then
  // parabolic-refine. This uses all symbol pairs at all lags optimally.
  const double stepHz = 0.05;
  const double rangeHz = 3.0;
  auto combinedMag2 = [&](double fHz) {
    cfloat cacc(0, 0);
    for (int s = 0; s < nSymbols; ++s) {
      double ph = -2.0 * M_PI * fHz * double(s) * symLen / kFsBaseband;
      cacc += m[size_t(s)] * cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
    return double(std::norm(cacc));
  };
  double bestF = 0.0, bestM = -1.0;
  for (double f = -rangeHz; f <= rangeHz + 1e-9; f += stepHz) {
    double v = combinedMag2(f);
    if (v > bestM) {
      bestM = v;
      bestF = f;
    }
  }
  {
    double a = combinedMag2(bestF - stepHz);
    double c = combinedMag2(bestF + stepHz);
    double dn = a - 2 * bestM + c;
    if (dn < -1e-18) {
      bestF += stepHz * std::clamp(0.5 * (a - c) / dn, -0.5, 0.5);
    }
  }
  const double residual = bestF;

  // Measurement sigma from per-symbol SNR around the fit (frequency CRLB
  // over mCount uniformly spaced symbols), then variance-weighted fusion
  // with the prior. The ZC measurement is energy-limited - for NARROW at
  // deep SNR the chirp prior legitimately dominates.
  const double nSym = std::max(double(mCount), 2.0);
  // |sum m_s|^2 = (N*A)^2 for per-symbol amplitude A, so A^2 = bestM/N^2;
  // per-symbol noise power = totPow/N - A^2.
  const double sigPow = bestM / (nSym * nSym);
  double totPow = 0;
  for (int s = 0; s < nSymbols; ++s) totPow += std::norm(m[size_t(s)]);
  const double noisePow =
      std::max(totPow / nSym - sigPow, sigPow * 1e-6);
  const double snrSym = sigPow / noisePow;
  const double sigPhi = 1.0 / std::sqrt(std::max(2.0 * snrSym, 1e-6));
  const double slopeSigma =
      sigPhi * std::sqrt(12.0 / (nSym * (nSym * nSym - 1.0)));
  const double measSigma =
      std::max(slopeSigma * kFsBaseband / (2.0 * M_PI * symLen), 1e-3);

  const double wm = 1.0 / (measSigma * measSigma);
  const double wp = 1.0 / (priorCfoSigmaHz * priorCfoSigmaHz);
  const double fusedResidual = residual * wm / (wm + wp);
  res.cfoHz = priorCfoHz + fusedResidual;
  res.cfoSigmaHz = 1.0 / std::sqrt(wm + wp);
  const double q =
      totPow > 0 ? std::sqrt(std::max(bestM, 0.0) / (totPow * nSym)) : 0.0;
  res.quality = mCount > 0 ? float(std::min(q, 1.0)) : 0.0f;

  // 3) Channel estimate: FFT each symbol core (with residual CFO removed),
  //    divide by the known sequence, average across symbols.
  const int n = plan(family).carriers();
  const auto seq = zcFreqSequence(family);
  Fft fft(kFftBaseband);
  std::vector<cfloat> sym(static_cast<size_t>(core));
  std::vector<cfloat> bins(static_cast<size_t>(core));
  std::vector<cfloat> acc(size_t(n), cfloat(0, 0));
  int used = 0;
  for (int s = 0; s < nSymbols; ++s) {
    const int base = t0 + s * symLen + kZcCpBaseband;
    if (base < 0 || base + core > int(x.size())) continue;
    for (int i = 0; i < core; ++i) {
      double ph =
          -2.0 * M_PI * fusedResidual * double(base + i) / kFsBaseband;
      sym[size_t(i)] = x[size_t(base + i)] *
                       cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
    fft.forward(sym.data(), bins.data());
    for (int i = 0; i < n; ++i) {
      acc[size_t(i)] += bins[size_t(carrierToBin(family, i))] /
                        seq[size_t(i)];
    }
    ++used;
  }
  if (used == 0) return res;
  // Normalize so |H| ~ 1 on an ideal channel: the TX symbol is
  // g * ifft(seq-on-carriers) with unit-RMS scale g = N/sqrt(n)
  // (sum|ifft(X)|^2 = sum|X|^2/N = n/N over N samples), and
  // fft(g * ifft(X)) = g * X, so each per-symbol estimate is g * H.
  const float g = float(kFftBaseband) / std::sqrt(float(n));
  for (auto& h : acc) h /= float(used) * g;

  // 4) Timing refinement from the channel estimate's impulse response:
  //    IFFT the per-carrier estimate and pick the peak. This integrates
  //    the whole block coherently and concentrates every carrier's energy
  //    into the peak - decisive for NARROW (27-sample-wide time-domain
  //    correlation lobe), and unlike a per-carrier phase-slope fit it
  //    still works when per-carrier SNR is near unity (WIDE at the floor).
  //    The channel is then de-rotated to represent the corrected timing.
  {
    Fft ifft(kFftBaseband);
    std::vector<cfloat> spec(size_t(kFftBaseband), cfloat(0, 0));
    for (int i = 0; i < n; ++i) {
      spec[size_t(carrierToBin(family, i))] = acc[size_t(i)];
    }
    std::vector<cfloat> h(static_cast<size_t>(kFftBaseband));
    ifft.inverse(spec.data(), h.data());
    auto magAt = [&](int lag) {
      int idx = ((lag % kFftBaseband) + kFftBaseband) % kFftBaseband;
      return std::norm(h[size_t(idx)]);
    };
    int bestLag = 0;
    double best = -1.0;
    for (int lag = -32; lag <= 32; ++lag) {
      double v = magAt(lag);
      if (v > best) {
        best = v;
        bestLag = lag;
      }
    }
    // OFDM window policy: anchor on the EARLIEST significant tap, not the
    // peak or centroid - a window placed after the first tap suffers ISI
    // from the next symbol on every symbol (the CP only guards against
    // taps that arrive AFTER the window start). "Significant tap" means
    // the earliest LOCAL MAXIMUM at >= half the peak power: a threshold
    // crossing would walk down the skirt of the correlation lobe itself
    // (band-limited width ~ N/n, up to ~12 samples for NARROW), and the
    // resulting large apparent delay is both off the tracker's
    // well-conditioned grid region and eats CP margin for nothing.
    int firstLag = bestLag;
    for (int lag = bestLag - 1; lag >= bestLag - 28 && lag >= -31; --lag) {
      if (magAt(lag) >= 0.5 * best && magAt(lag) >= magAt(lag - 1) &&
          magAt(lag) >= magAt(lag + 1)) {
        firstLag = lag;
      }
    }
    const double tau = double(firstLag) - 2.0;
    res.blockStart += tau;
    for (int i = 0; i < n; ++i) {
      int bin = plan(family).firstBin + i - kCenterBin;
      double ph = 2.0 * M_PI * double(bin) * tau / kFftBaseband;
      acc[size_t(i)] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
  }
  res.channel = std::move(acc);
  res.ok = true;
  return res;
}

}  // namespace skw
