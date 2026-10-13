#include "phy/receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "dsp/css.hpp"
#include "dsp/fft.hpp"
#include "dsp/zc.hpp"
#include "fec/crc.hpp"
#include "fec/interleave.hpp"
#include "fec/ldpc.hpp"
#include "fec/whitener.hpp"
#include "phy/equalizer.hpp"
#include "phy/layout.hpp"

namespace skw {
namespace {

// Symbols the tracker must have folded into its noise estimate before
// snrDb() is reported as a measurement (ChannelTracker::noiseUpdates:
// the EMA runs fast for the first 8 symbols, then settles).
constexpr int kSnrConvergedSymbols = 8;

// Phase 8 receiver knobs (GAP_CLOSURE_PLAN.md): on by default; "=0"
// restores the v1.1.0 behaviour for A/B measurement.
bool envKnobOn(const char* name) {
  const char* e = std::getenv(name);
  return !(e && std::atoi(e) == 0);
}
bool rtsSmoothingEnabled() {  // R6
  static const bool on = envKnobOn("SKW_RTS");
  return on;
}
bool qNormEnabled() {  // R5
  static const bool on = envKnobOn("SKW_QNORM");
  return on;
}
// In-frame FLL loop gain (R2), per tracker class. Robust gain measured
// on the full 8.2-8.5 stack (100 trials; ladders on CCIR Poor and
// Moderate, AWGN floor+6 at 1 Hz/s drift), gains 0/0.05/0.1/0.2:
//   0    - no drift tolerance (100% FER at 1 Hz/s)
//   0.05 - best fading, but NARROW M4 47% / MID M4 25% under drift
//   0.1  - fading within 0.5 dB of the best everywhere, better than the
//          loop off on Moderate; drift: all robust modes 0% except
//          NARROW M4 10% (a 40 s frame - 40 Hz of in-frame drift)
//   0.2  - feeds Poor's 1 Hz Doppler back: Poor NARROW M4 -7.7 -> -3.2 dB
// Fast class 0.2 (short frames, high SNR). SKW_FLL=0 disables;
// SKW_FLL_GAIN_FAST / _ROBUST override.
double fllGain(bool robustClass) {
  static const bool on = envKnobOn("SKW_FLL");
  static const double fast = [] {
    const char* e = std::getenv("SKW_FLL_GAIN_FAST");
    return e ? std::atof(e) : 0.2;
  }();
  static const double robust = [] {
    const char* e = std::getenv("SKW_FLL_GAIN_ROBUST");
    return e ? std::atof(e) : 0.1;
  }();
  return on ? (robustClass ? robust : fast) : 0.0;
}

// Drift retry (the OFDM counterpart of the CSS drift search, 8.8): the
// FLL is first-order, so a linear drift r leaves a steady frequency lag
// of r * Tsym / gain. At the robust gain 0.1 that lag killed long frames
// under extreme drift (AWGN floor+6, 40 trials: NARROW M4 @ 1 Hz/s 25%,
// MID M4 @ 2 Hz/s 82.5%, mostly header failures); gain 0.3 recovers
// both (2.5%), but a gain that high everywhere feeds CCIR Poor's Doppler
// back (fllGain above). So a frame that fails at the class gain is
// walked once more at this gain - frames that decode are untouched.
// SKW_FLL_DRIFT_RETRY=0 disables; SKW_FLL_GAIN_DRIFT overrides 0.3.
bool fllDriftRetryEnabled() { return envKnobOn("SKW_FLL_DRIFT_RETRY"); }
double fllDriftGain() {
  static const double g = [] {
    const char* e = std::getenv("SKW_FLL_GAIN_DRIFT");
    return e ? std::atof(e) : 0.3;
  }();
  return g;
}

// Frame-average channel power in tracker H units (R5): mean in-band FFT
// power minus the out-of-band noise floor over up to `maxSyms` symbols
// from the first post-ZC symbol. Guard bins (6 carriers each side) cover
// the +-55 Hz CFO capture so signal leakage is not counted as noise.
// Returns <= 0 when it cannot be measured.
double frameMeanChannelPower(Family family, std::span<const cfloat> sig,
                             int64_t t0, int maxSyms) {
  const int n = plan(family).carriers();
  const int symLen =
      kFftBaseband + cpSamplesAudio(CpMode::Medium) / kDecimation;
  const float invTs = 1.0f / ofdmTimeScale(family);
  std::vector<char> cls(size_t(kFftBaseband), 0);  // 0 noise, 1 in, 2 guard
  for (int c = -6; c < n + 6; ++c) {
    const int b = plan(family).firstBin + c - centerBin();
    cls[size_t(b >= 0 ? b : kFftBaseband + b)] = (c >= 0 && c < n) ? 1 : 2;
  }
  Fft f(kFftBaseband);
  std::vector<cfloat> in(static_cast<size_t>(kFftBaseband)),
      out(static_cast<size_t>(kFftBaseband));
  double pin = 0, pout = 0;
  int nin = 0, nout = 0;
  for (int k = 0; k < maxSyms; ++k) {
    const int64_t st = t0 + int64_t(k) * symLen + (symLen - kFftBaseband);
    if (st < 0 || st + kFftBaseband > int64_t(sig.size())) break;
    std::copy(sig.begin() + st, sig.begin() + st + kFftBaseband, in.begin());
    f.forward(in.data(), out.data());
    for (int b = 0; b < kFftBaseband; ++b) {
      const double v = std::norm(out[size_t(b)] * invTs);
      if (cls[size_t(b)] == 1) {
        pin += v;
        ++nin;
      } else if (cls[size_t(b)] == 0) {
        pout += v;
        ++nout;
      }
    }
  }
  if (nin == 0 || nout == 0) return -1.0;
  const double in_ = pin / nin;
  return std::max(in_ - pout / nout, 0.05 * in_);
}
// Pilot-residual energy per unit of data-aided (EVM) noise variance on
// AWGN: the residual of a Kalman-predicted pilot is measurement noise
// plus the filter's own uncertainty share. Fitted from bertool's `pilot`
// column as raw-ratio x EVM-linear: 0.63-0.75 across M6-M10 from -2.5
// to 18 dB. At 0.7 the AWGN excess reads ~0 at every point (0.001-
// 0.007), CCIR Good 0.001-0.009, CCIR Moderate 0.02-0.06 at M6 and
// 0.008-0.014 at M10. (In the tracker's own raw-noise terms the same
// coefficient is ~1.5, but that figure contains channel motion under
// Doppler and cannot serve as the reference.)
constexpr double kPilotNoiseCoefEvm = 0.7;

// Walks OFDM symbols of one codeword (mirror of appendOfdmSymbols): FFTs
// each group symbol at integer positions, runs the tracker, despreads with
// MRC over the group's carrier pool, and emits LLRs.
struct SymbolWalker {
  Family family;
  std::span<const cfloat> rx;
  double cfoHz;
  int64_t nextSampleStart;  // absolute index of the next symbol's CP start
  int symIdx;               // pilot-pattern symbol number
  ChannelTracker* tracker;
  const CarrierMask* mask = nullptr;  // payload phase only
  // Optional tap for the GUI constellation view: equalized data-carrier
  // symbols, normalized so an ideal point sits on the unit constellation.
  // Null in the hot path - collecting costs one divide per symbol.
  std::vector<cfloat>* constellationOut = nullptr;
  // Optional: the MRC gain `a` of each despread symbol, in step with
  // constellationOut (the equalized symbol is z/a, whose noise variance
  // is the per-carrier noise divided by a). For the data-aided SNR.
  std::vector<float>* gainOut = nullptr;
  Fft fft{kFftBaseband};
  // In-frame FLL state (GAP_CLOSURE_PLAN.md R2): the residual frequency
  // the frame-constant derotation (cfoHz) misses - LO drift accumulates
  // ~1 Hz over a multi-second frame, and a one-shot derotation let NARROW
  // die at 0.1 Hz/s. Fed each symbol from the tracker's channel rotation
  // sum h conj(h_prev); applied phase-continuously.
  double fllHz = 0.0;
  double fllPhase = 0.0;
  std::vector<cfloat> fllPrevH{};
  // Loop gain for this walk; < 0 takes fllGain() (the drift retry in
  // receiveFrame sets fllDriftGain()).
  double fllGainOverride = -1.0;

  // CFO-derotate, FFT and TX-scale-correct the symbol whose FFT window
  // starts at `start`. False when the window runs past the capture.
  bool fftSymbol(int64_t start, std::vector<cfloat>& out) {
    if (start < 0 || start + kFftBaseband > int64_t(rx.size())) return false;
    std::vector<cfloat> in(static_cast<size_t>(kFftBaseband));
    for (int i = 0; i < kFftBaseband; ++i) {
      double ph = -2.0 * M_PI * (cfoHz + fllHz) * double(start + i) /
                      kFsBaseband +
                  fllPhase;
      in[size_t(i)] = rx[size_t(start + i)] *
                      cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
    out.resize(static_cast<size_t>(kFftBaseband));
    fft.forward(in.data(), out.data());
    // Undo the TX time-domain scale so pilot-measured H matches the
    // ZC-seeded estimate (see ofdmTimeScale).
    const float invTs = 1.0f / ofdmTimeScale(family);
    for (auto& b : out) b *= invTs;
    return true;
  }

  // Collect exactly `bits` LLRs coded at the given parameters. Returns
  // false on running out of samples.
  //
  // With smoothing on (default; SKW_RTS=0 disables) the whole span is
  // FFT'd and run through the tracker FIRST, then RTS-smoothed, and every
  // group is demapped with the smoothed H of its last symbol: the frame
  // is fully buffered, so each symbol's estimate can use the pilots
  // AFTER it as well as before (GAP_CLOSURE_PLAN.md R6). Without it the
  // tracker runs causally, interleaved with demapping (v1.1.0).
  bool collect(Constellation cst, int spread, CpMode cp, int bits,
               std::vector<float>& llr) {
    const int cpLen = cpSamplesAudio(cp) / kDecimation;
    const int symLen = kFftBaseband + cpLen;
    const int bps = bitsPerSymbol(cst);
    const int G = spreadGroupSymbols(family, spread, mask);

    // Plan the span: groups until `bits` are covered.
    int nGroups = 0;
    for (int need = 0, s = symIdx; need < bits; s += G, ++nGroups) {
      need += sourceSymbolsPerGroup(family, s, spread, mask) * bps;
    }
    const int nSym = nGroups * G;

    // Forward pass: FFT + track every symbol of the span (the FLL needs
    // each symbol's tracker update before the next symbol's FFT).
    std::vector<std::vector<cfloat>> bins(static_cast<size_t>(nSym));
    std::vector<std::vector<cfloat>> hS(static_cast<size_t>(nSym));
    std::vector<double> nvS(static_cast<size_t>(nSym));
    const bool smooth = rtsSmoothingEnabled();
    const double gain = fllGainOverride >= 0.0
                            ? fllGainOverride
                            : fllGain(tracker->robustClass());
    if (smooth) tracker->beginHistory();
    for (int k = 0; k < nSym; ++k) {
      const int64_t start = nextSampleStart + int64_t(k) * symLen + cpLen;
      if (!fftSymbol(start, bins[size_t(k)])) {
        if (smooth) tracker->smoothHistory(hS, nvS);  // stop recording
        return false;
      }
      tracker->processSymbol(bins[size_t(k)], symIdx + k);
      const auto& h = tracker->h();
      if (!smooth) {
        hS[size_t(k)] = h;
        nvS[size_t(k)] = tracker->noiseVar();
      }
      if (gain > 0.0) {
        if (fllPrevH.size() == h.size()) {
          std::complex<double> r(0, 0);
          for (size_t c = 0; c < h.size(); ++c) {
            r += std::complex<double>(h[c] * std::conj(fllPrevH[c]));
          }
          const double ferr = std::atan2(r.imag(), r.real()) * kFsBaseband /
                              (2.0 * M_PI * symLen);
          const double delta = gain * ferr;
          // Phase continuity at the next symbol's first sample.
          fllPhase += 2.0 * M_PI * delta * double(start + symLen) /
                      kFsBaseband;
          fllHz += delta;
        }
        fllPrevH = h;
      }
    }
    if (smooth) tracker->smoothHistory(hS, nvS);

    int got = 0;
    for (int grp = 0; grp < nGroups; ++grp) {
      std::vector<std::pair<int, int>> pool;
      for (int g = 0; g < G; ++g) {
        const int k = grp * G + g;
        auto lay = symbolLayout(family, symIdx + k, mask);
        for (int c : lay.dataCarriers) pool.push_back({k, c});
      }
      const int last = grp * G + G - 1;
      const auto& h = hS[size_t(last)];
      const float nvar = float(nvS[size_t(last)]);
      const int nSrc = int(pool.size()) / spread;
      for (int i = 0; i < nSrc && got < bits; ++i) {
        cfloat z(0, 0);
        float a = 0;
        for (int k = 0; k < spread; ++k) {
          auto [sk, c] = pool[size_t(i + k * nSrc)];
          cfloat y = bins[size_t(sk)][size_t(carrierToBasebandBin(family, c))];
          cfloat eff = h[size_t(c)] * spreadPhase(i, k, symIdx + grp * G);
          z += y * std::conj(eff);
          a += std::norm(eff);
        }
        if (constellationOut && a > 1e-12f) {
          constellationOut->push_back(z / a);
          if (gainOut) gainOut->push_back(a);
        }
        demapLlr(cst, z, a, nvar, llr);
        got += bps;
      }
    }
    nextSampleStart += int64_t(nSym) * symLen;
    symIdx += nSym;
    // Trim any over-collected LLRs from the last source symbol.
    llr.resize(size_t(bits));
    return true;
  }
};

std::vector<uint8_t> bitsToBytes(std::span<const uint8_t> bits) {
  std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
  for (size_t i = 0; i < bits.size(); ++i) {
    out[i / 8] |= uint8_t((bits[i] & 1) << (7 - i % 8));
  }
  return out;
}

// Deinterleave, optionally add IR info-bit priors at the systematic
// positions (priorOff = this codeword's offset into the payload's info
// stream), decode. sysOut (optional) receives the deinterleaved
// systematic-bit channel LLRs for prior storage.
bool decodeCodeword(const LdpcCode& code, std::vector<float>& llrInterleaved,
                    std::vector<uint8_t>& info,
                    std::span<const float> priors = {}, size_t priorOff = 0,
                    std::vector<float>* sysOut = nullptr) {
  std::vector<float> llr;
  deinterleave<float>(llrInterleaved, llr);
  // HARQ-IR priors are this attempt's CHANNEL LLRs. Step 3.4 recorded
  // that as a deviation from the spec's "accumulated posteriors",
  // upgradeable later - it was MEASURED 2026-08-17 and the upgrade is
  // not one. At M9 floor-5, 40 trials:
  //   channel LLRs (this)             32/40
  //   + decoder extrinsic, clamped    32/40
  //   decoder posteriors alone        worse (saturated the wrong way on
  //                                   a failed decode)
  //   extrinsic alone                  0/40 (discards the observation)
  // The channel LLRs already carry the usable information, so the
  // decoder keeps its simple signature and the deviation is closed
  // rather than left open.
  if (sysOut) {
    sysOut->assign(llr.begin(), llr.begin() + code.k());
  }
  for (size_t i = 0;
       i < size_t(code.k()) && priorOff + i < priors.size(); ++i) {
    llr[i] += priors[priorOff + i];
  }
  return code.decode(llr, info);
}

// CSS frame receive (WAVEFORM.md "CSS Deep Tier"): the chirp preamble
// gave coarse timing and CFO; the two known shift-0 SF8 sync symbols
// refine timing to the sample (CFO<->delay coupling makes any residual
// CFO a pure common shift, absorbed here) and expose the multipath
// profile, which becomes the RAKE tap list for header and payload.
std::optional<RxFrameResult> cssReceiveFrame(
    Family family, std::span<const cfloat> sig, const ChirpDetection& hit,
    double cfoHz, const std::function<void(RxStage)>& stage,
    HarqChase* harq) {
  const CssPlan cp = cssPlan(family);
  const int spc = cp.samplesPerChip;
  const size_t symH = cssSymbolSamples(family, kCssHeaderSf);
  const auto base = cssBaseChirp(family, kCssHeaderSf);

  // Derotate by the preamble CFO estimate once, over the maximum extent
  // a CSS frame can occupy from the coarse start.
  const double coarse =
      hit.startSample +
      double(chirpPattern(ChirpSig::Css).size()) * chirpRepSamples();
  const int W = 2 * spc + 24;  // sync search half-window, samples
  const int64_t region0 = int64_t(std::llround(coarse)) - W;
  if (region0 < 0) return std::nullopt;
  std::vector<cfloat> rx(sig.begin() + long(region0), sig.end());
  for (size_t n = 0; n < rx.size(); ++n) {
    const double ph =
        -2.0 * M_PI * cfoHz * double(int64_t(region0) + int64_t(n)) /
        kFsBaseband;
    rx[n] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }

  // Sync search: non-coherent correlation of the two known symbols over
  // the timing window.
  if (rx.size() < size_t(2 * W) + size_t(kCssSyncSymbols) * symH) {
    return std::nullopt;
  }
  std::vector<float> prof(size_t(2 * W + 1), 0.0f);
  for (int d = -W; d <= W; ++d) {
    double sum = 0.0;
    for (int s = 0; s < kCssSyncSymbols; ++s) {
      cfloat c(0, 0);
      const size_t off = size_t(W + d) + size_t(s) * symH;
      for (size_t n = 0; n < symH; ++n) {
        c += rx[off + n] * std::conj(base[n]);
      }
      sum += std::abs(c);
    }
    prof[size_t(d + W)] = float(sum);
  }
  int dBest = 0;
  float peak = 0.0f;
  for (int d = -W; d <= W; ++d) {
    if (prof[size_t(d + W)] > peak) {
      peak = prof[size_t(d + W)];
      dBest = d;
    }
  }
  // Noise reference: median of the profile outside +-2 chips of the peak.
  std::vector<float> noise;
  for (int d = -W; d <= W; ++d) {
    if (std::abs(d - dBest) > 2 * spc) noise.push_back(prof[size_t(d + W)]);
  }
  float med = 0.0f;
  if (!noise.empty()) {
    std::nth_element(noise.begin(), noise.begin() + long(noise.size() / 2),
                     noise.end());
    med = noise[noise.size() / 2];
  }
  if (getenv("SKW_RX_DEBUG")) {
    fprintf(stderr,
            "skw-css: start=%.1f cfo=%.2f W=%d dBest=%d peak=%.3g med=%.3g "
            "ratio=%.2f\n",
            hit.startSample, cfoHz, W, dBest, double(peak), double(med),
            med > 0 ? double(peak / med) : 0.0);
  }
  if (med <= 0.0f || peak < 3.5f * med) return std::nullopt;  // sync fail

  // Persistent RAKE tap tracker. Rayleigh taps hand dominance back and
  // forth on ~1 s scales while a deep frame runs 10+ s, and a tap that
  // is faded RIGHT NOW must stay in the score (with its remembered
  // strength) or a mid-block dominance switch shifts every symbol by the
  // inter-tap delay. Each pilot refreshes an EMA per tap; delays follow
  // sample-clock drift; unseen taps decay away over a few pilots.
  struct TapTrack {
    int delay;
    float ema;
  };
  std::vector<TapTrack> track;
  std::vector<CssTap> taps{{0, 1.0f}};
  auto updateTaps = [&](const std::vector<float>& pr, int half, int center,
                        float noiseMed, bool initial) {
    const float pk = pr[size_t(center + half)];
    if (pk <= 0.0f) return;
    for (auto& t : track) t.ema *= 0.7f;
    // Candidates must beat BOTH a peak-relative and a noise-relative
    // floor: near the SNR floor 0.12*pk sits inside the noise, and
    // spurious taps poison every symbol score.
    const float cut = std::max((initial ? 0.12f : 0.4f) * pk,
                               (initial ? 1.8f : 2.0f) * noiseMed);
    const int lo = std::max(-half + 1, center - 3 * spc);
    const int hi = std::min(half - 1, center + 3 * spc);
    for (int d = lo; d <= hi; ++d) {
      const float v = pr[size_t(d + half)];
      if (v < cut) continue;
      if (v < pr[size_t(d + half - 1)] || v < pr[size_t(d + half + 1)]) {
        continue;
      }
      const int rel = d - center;
      bool merged = false;
      for (auto& t : track) {
        if (std::abs(t.delay - rel) <= 2) {
          t.ema = std::max(t.ema, v / pk);
          t.delay = rel;  // follow clock drift
          merged = true;
          break;
        }
      }
      if (!merged) track.push_back({rel, v / pk});
    }
    std::sort(track.begin(), track.end(),
              [](const TapTrack& a, const TapTrack& b) {
                return a.ema > b.ema;
              });
    while (track.size() > 3 || (!track.empty() && track.back().ema < 0.15f)) {
      track.pop_back();
    }
    taps.clear();
    for (const auto& t : track) taps.push_back({t.delay, t.ema});
    if (taps.empty()) taps.push_back({0, 1.0f});
  };
  updateTaps(prof, W, dBest, med, true);

  // Per-sample SNR from the sync region: the correlation peak gives the
  // signal amplitude (peak ~ kCssSyncSymbols*symLen*a), total power
  // gives a^2 + sigma^2. (A median-of-|corr| noise reference saturates:
  // chirp correlation SIDELOBES scale with the signal and cap the
  // ratio ~12 dB - AMC needs an unbounded estimate to climb out of the
  // tier when the channel improves.)
  double snr3k;
  {
    const size_t s0 = size_t(W + dBest);
    double pTot = 0.0;
    const size_t nTot = size_t(kCssSyncSymbols) * symH;
    for (size_t n = 0; n < nTot; ++n) pTot += std::norm(rx[s0 + n]);
    pTot /= double(nTot);
    const double a = double(peak) / (double(kCssSyncSymbols) * double(symH));
    const double sig2 = a * a;
    const double noise2 = std::max(pTot - sig2, sig2 * 1e-3 + 1e-12);
    snr3k = 10.0 * std::log10(sig2 / noise2) + 6.02;
  }

  stage(RxStage::HeaderFail);
  // Section walker: a known SF8 pilot precedes every kCssPilotInterval
  // data symbols. Each pilot re-anchors timing (clock drift) and
  // re-estimates the tap profile (fading hands dominance between paths
  // mid-frame); a pilot caught in a deep fade is skipped and the
  // previous anchor carries the block.
  size_t pos = size_t(W + dBest) + size_t(kCssSyncSymbols) * symH;
  auto walkSection = [&](int sectionSf,
                         size_t byteCount) -> std::optional<
                                                 std::vector<uint8_t>> {
    const size_t symLen = cssSymbolSamples(family, sectionSf);
    const size_t nsym = cssSymbolsForBytes(byteCount, sectionSf);
    const int PW = 3 * spc + 6;
    std::vector<uint16_t> syms;
    syms.reserve(nsym);
    for (size_t s = 0; s < nsym; ++s) {
      if (s % size_t(kCssPilotInterval) == 0) {
        if (rx.size() < pos + size_t(2 * PW) + symH) return std::nullopt;
        std::vector<float> pp(size_t(2 * PW + 1), 0.0f);
        for (int d = -PW; d <= PW; ++d) {
          cfloat c(0, 0);
          const cfloat* xr = rx.data() + long(pos) + d;
          for (size_t n = 0; n < symH; ++n) c += xr[n] * std::conj(base[n]);
          pp[size_t(d + PW)] = float(std::abs(c));
        }
        int dp = 0;
        float pk = 0.0f;
        for (int d = -PW; d <= PW; ++d) {
          if (pp[size_t(d + PW)] > pk) {
            pk = pp[size_t(d + PW)];
            dp = d;
          }
        }
        std::vector<float> ns;
        for (int d = -PW; d <= PW; ++d) {
          if (std::abs(d - dp) > spc) ns.push_back(pp[size_t(d + PW)]);
        }
        std::nth_element(ns.begin(), ns.begin() + long(ns.size() / 2),
                         ns.end());
        const float nsMed = ns[ns.size() / 2];
        // A pilot caught in a deep fade updates nothing; the tracker's
        // remembered taps carry the block. The anchor position never
        // jumps - per-tap delay follow absorbs clock drift.
        if (nsMed > 0.0f && pk >= 2.5f * nsMed) {
          updateTaps(pp, PW, 0, nsMed, false);
        }
        if (getenv("SKW_RX_DEBUG")) {
          fprintf(stderr, "skw-css pilot s=%zu dp=%d pk=%.3g med=%.3g taps:",
                  s, dp, double(pk), double(nsMed));
          for (const auto& t : taps) {
            fprintf(stderr, " (%d,%.2f)", t.delay, double(t.weight));
          }
          fprintf(stderr, "\n");
        }
        pos += symH;
      }
      if (rx.size() < pos + symLen) return std::nullopt;
      syms.push_back(cssDemodSymbolTaps(
          family, sectionSf, std::span<const cfloat>(rx).subspan(pos, symLen),
          taps));
      if (getenv("SKW_RX_DEBUG")) {
        fprintf(stderr, "skw-css sym %zu = %u\n", s, unsigned(syms.back()));
      }
      pos += symLen;
    }
    return cssDecode(syms, sectionSf, byteCount);
  };

  // Walk the header copies; the CRC-16 arbitrates - a fade takes at most
  // one copy. The payload starts after ALL copies on air, and copy 1
  // (decoded first) states how many there are: two on the v1 layout, one
  // on the capability-gated connected-session short layout. A frame
  // whose single copy is lost is lost outright - the accepted trade for
  // the saved airtime; a lost first copy on the two-copy layout still
  // recovers from copy 2 as before.
  std::optional<PhyHeader> hdr;
  int copies = kCssHeaderCopies;
  for (int c = 0; c < copies; ++c) {
    auto hdrBytes = walkSection(kCssHeaderSf, kPhyHeaderBytes);
    if (!hdrBytes) {
      if (!hdr) return std::nullopt;
      break;  // ran out of samples but an earlier copy already parsed
    }
    if (!hdr) {
      hdr = parsePhyHeader(*hdrBytes);
      if (hdr) copies = int(hdr->cssHeaderCopies);
    }
  }
  if (!hdr) return std::nullopt;
  if (hdr->waveform != Waveform::Css || cssSf(hdr->mode) == 0 ||
      hdr->family != family) {
    return std::nullopt;
  }
  stage(RxStage::Ok);

  RxFrameResult res;
  res.header = *hdr;
  res.sig = ChirpSig::Css;
  res.inverted = hit.inverted;
  res.cfoHz = cfoHz;
  res.snrDb = snr3k;
  {
    int maxTapDelay = 0;
    for (const auto& t : taps) {
      maxTapDelay = std::max(maxTapDelay, std::abs(t.delay));
    }
    res.delaySpreadMs = 1000.0 * double(maxTapDelay) / kFsBaseband;
  }

  // Payload: LDPC r1/2 over soft-demodulated CSS symbols (same pilot
  // cadence as the header sections), whitened, trailing CRC-16.
  const int sf = cssSf(hdr->mode);
  const size_t symLenP = cssSymbolSamples(family, sf);
  const size_t codedBits = cssPayloadCodedBits(size_t(hdr->length));
  const size_t nsymP = (codedBits + size_t(sf) - 1) / size_t(sf);
  const int PWp = 3 * spc + 6;
  const size_t payloadPos = pos;
  const auto trackAtPayload = track;
  const auto tapsAtPayload = taps;
  // Demodulate the payload from `buf` (same indexing as rx) starting from
  // the tap state the header left; returns false if samples run out.
  // `ratioSum` accumulates each symbol's decided-score/mean - the drift
  // search's figure of merit.
  auto demodPayload = [&](const std::vector<cfloat>& buf,
                          std::vector<float>& llr, double& ratioSum) {
    track = trackAtPayload;
    taps = tapsAtPayload;
    size_t p = payloadPos;
    llr.clear();
    llr.reserve(nsymP * size_t(sf));
    ratioSum = 0.0;
    for (size_t s = 0; s < nsymP; ++s) {
      if (s % size_t(kCssPilotInterval) == 0) {
        if (buf.size() < p + size_t(2 * PWp) + symH) return false;
        std::vector<float> pp(size_t(2 * PWp + 1), 0.0f);
        for (int d = -PWp; d <= PWp; ++d) {
          cfloat c(0, 0);
          const cfloat* xr = buf.data() + long(p) + d;
          for (size_t n = 0; n < symH; ++n) c += xr[n] * std::conj(base[n]);
          pp[size_t(d + PWp)] = float(std::abs(c));
        }
        float pk = 0.0f;
        int dp = 0;
        for (int d = -PWp; d <= PWp; ++d) {
          if (pp[size_t(d + PWp)] > pk) {
            pk = pp[size_t(d + PWp)];
            dp = d;
          }
        }
        std::vector<float> ns;
        for (int d = -PWp; d <= PWp; ++d) {
          if (std::abs(d - dp) > spc) ns.push_back(pp[size_t(d + PWp)]);
        }
        std::nth_element(ns.begin(), ns.begin() + long(ns.size() / 2),
                         ns.end());
        const float nsMed = ns[ns.size() / 2];
        if (nsMed > 0.0f && pk >= 2.5f * nsMed) {
          updateTaps(pp, PWp, 0, nsMed, false);
        }
        p += symH;
      }
      if (buf.size() < p + symLenP) return false;
      CssPeakInfo pk;
      cssSoftDemodSymbol(family, sf,
                         std::span<const cfloat>(buf).subspan(p, symLenP),
                         taps, llr, &pk);
      ratioSum += double(pk.ratio);
      p += symLenP;
    }
    return llr.size() >= codedBits;
  };
  // LDPC + CRC over one LLR set; fills res on success.
  auto decodePayload = [&](std::vector<float> llr, HarqChase* h) -> bool {
    llr.resize(codedBits);  // drop the last symbol's pad bits
    const size_t payBytes = size_t(hdr->length) + kCssPayloadCrcBytes;
    std::vector<uint8_t> infoBits;
    infoBits.reserve(payBytes * 8);
    size_t remaining = payBytes * 8;
    size_t off = 0;
    bool ok = true;
    const LdpcCode& pcode = LdpcCode::get(kLdpcNShort, {1, 2});
    size_t cw = 0;
    // Burst containers repack on retransmission (identical-codeword
    // combining not guaranteed) - same HARQ bypass as the OFDM path.
    if (hdr->frameType == kBurstContainerFrameType) h = nullptr;
    while (remaining > 0 && (ok || h)) {
      std::vector<float> cwLlr(llr.begin() + long(off),
                               llr.begin() + long(off + size_t(pcode.n())));
      std::vector<uint8_t> info;
      bool dec = decodeCodeword(pcode, cwLlr, info);
      if (h) {
        // Chase: a deep-tier retransmission carries identical
        // codewords, and the payload IS LDPC - soft-combine with
        // earlier receptions (~3 dB per doubling at the floor).
        const uint64_t key =
            HarqChase::frameKey(hdr->frameType, hdr->seqLow, hdr->mode,
                                hdr->length, uint32_t(cw));
        if (!dec) {
          if (h->combine(key, cwLlr) > 1) {
            dec = decodeCodeword(pcode, cwLlr, info);
          }
          if (dec) h->onDecodeSuccess(key);
        } else {
          h->onDecodeSuccess(key);
        }
      }
      if (!dec) {
        ok = false;
        if (!h) break;
      } else if (ok) {
        const size_t want = std::min(remaining, size_t(pcode.k()));
        infoBits.insert(infoBits.end(), info.begin(),
                        info.begin() + long(want));
      }
      remaining -= std::min(remaining, size_t(pcode.k()));
      off += size_t(pcode.n());
      ++cw;
    }
    if (!ok) return false;
    auto white = bitsToBytes(infoBits);
    white.resize(payBytes);
    const uint16_t want =
        uint16_t((white[payBytes - 2] << 8) | white[payBytes - 1]);
    white.resize(size_t(hdr->length));
    if (crc16(white) != want) return false;
    Whitener wh;
    wh.apply(white);
    res.payload = std::move(white);
    res.payloadOk = true;
    return true;
  };

  std::vector<float> llr;
  double ratio0 = 0.0;
  const bool samplesOk = demodPayload(rx, llr, ratio0);
  if (samplesOk) decodePayload(llr, harq);

  // DRIFT SEARCH (ROADMAP 8.8). The receiver removes the preamble's CFO
  // once; LO drift leaves a residual r*t that a chirp turns into a TIMING
  // offset proportional to 2^SF - the SF8 pilots move the RAKE taps by
  // 2^(8-SF) times what the SF5-SF7 payload needs (NARROW at 0.1 Hz/s: ~11
  // samples of misplacement per Hz, ~2.5 chips by the end of a 64 s M2
  // frame), and the moving pilot peak spawns spurious taps. Only after
  // the ordinary pass FAILS: derotate the residual quadratic phase for a
  // grid of drift rates, re-run the pilot/tap/demod pass on each, and
  // decode the candidate whose symbols correlate best. The CRC arbitrates,
  // so a frame that decodes today is untouched and a wrong candidate
  // cannot pass. HARQ is not touched by the retry (its store holds
  // pass 1's LLRs). SKW_CSS_DRIFT_SEARCH=0 disables it (A/B).
  static const bool driftSearch = [] {
    const char* e = getenv("SKW_CSS_DRIFT_SEARCH");
    return !(e && std::string(e) == "0");
  }();
  if (!res.payloadOk && samplesOk && driftSearch) {
    const double t0 = hit.startSample;  // drift is measured from the preamble
    // Only the payload span is re-read: derotating the whole capture
    // window (up to the 90 s ladder rung) doubled the cost for nothing.
    const size_t pilotsP = (nsymP + size_t(kCssPilotInterval) - 1) /
                           size_t(kCssPilotInterval);
    const size_t spanLo = payloadPos > size_t(PWp) ? payloadPos - size_t(PWp) : 0;
    const size_t spanHi = std::min(rx.size(), payloadPos + nsymP * symLenP +
                                                  pilotsP * symH + size_t(PWp) + 1);
    auto derotate = [&](double r) {
      std::vector<cfloat> buf(rx.size());
      for (size_t n = spanLo; n < spanHi; ++n) {
        const double t = (double(region0) + double(n) - t0) / kFsBaseband;
        const double ph = -M_PI * r * t * t;
        buf[n] = rx[n] * cfloat(float(std::cos(ph)), float(std::sin(ph)));
      }
      return buf;
    };
    double bestR = 0.0, bestScore = ratio0;
    std::vector<float> bestLlr;
    auto tryRate = [&](double r) {
      std::vector<float> l;
      double sc = 0.0;
      if (demodPayload(derotate(r), l, sc) && sc > bestScore) {
        bestScore = sc;
        bestR = r;
        bestLlr = std::move(l);
      }
    };
    // 0.02 Hz/s grid over +-0.2 Hz/s, then +-0.01 around the winner. (A
    // 0.04 coarse grid lost the right neighbourhood: NARROW M2 at
    // 0.1 Hz/s went 0% -> 60% FER - the figure of merit is too flat away
    // from the true rate to steer a coarse search.)
    for (double r = -0.20; r <= 0.2001; r += 0.02) {
      if (std::abs(r) > 1e-9) tryRate(r);
    }
    {
      const double c = bestR;
      tryRate(c - 0.01);
      tryRate(c + 0.01);
    }
    if (!bestLlr.empty()) decodePayload(bestLlr, nullptr);
    if (getenv("SKW_RX_DEBUG")) {
      fprintf(stderr, "skw-css drift search: best r=%.3f Hz/s score %.1f "
              "(pass 1 %.1f) -> %s\n", bestR, bestScore, ratio0,
              res.payloadOk ? "decoded" : "failed");
    }
  }
  return res;
}

}  // namespace

int irStepDownMode(int mode) {
  if (mode == 9) return 8;    // QPSK r3/4 -> QPSK r1/2
  if (mode == 11) return 10;  // 16QAM r3/4 -> 16QAM r1/2
  return mode;                // r1/2 already, or 64QAM (no r1/2 rung)
}

void HarqChase::storeInfoPriors(uint64_t key, std::vector<float> llrs) {
  if (priors_.size() > 16) priors_.clear();  // bounded
  priors_[key] = std::move(llrs);
}

const std::vector<float>* HarqChase::infoPriors(uint64_t key) const {
  auto it = priors_.find(key);
  return it == priors_.end() ? nullptr : &it->second;
}

int HarqChase::combine(uint64_t key, std::vector<float>& llr) {
  // Poisoned-store guard: retransmissions differ in the rv byte, so a
  // store that keeps failing to combine-decode accumulates mismatched
  // priors. After several failed combines, restart clean.
  if (auto it = store_.find(key);
      it != store_.end() && it->second.count >= 4) {
    store_.erase(it);
  }
  if (store_.size() > 64) {
    // Bounded store: drop the oldest half on overflow.
    uint64_t median = stampCtr_ - 32;
    for (auto it = store_.begin(); it != store_.end();) {
      it = it->second.stamp < median ? store_.erase(it) : std::next(it);
    }
  }
  auto it = store_.find(key);
  if (it != store_.end() && it->second.llr.size() == llr.size()) {
    for (size_t i = 0; i < llr.size(); ++i) llr[i] += it->second.llr[i];
    it->second.llr = llr;
    it->second.stamp = ++stampCtr_;
    return ++it->second.count;
  }
  store_[key] = Entry{llr, 1, ++stampCtr_};
  return 1;
}

// CFO BRANCH RESOLUTION (GAP_CLOSURE_PLAN.md R1). The chirp detector's
// fine CFO is exact only modulo its ambiguity spacing, and the branch it
// picks can be wrong two ways: a slope-less pattern (fast U-U) has no
// coarse estimate at all, so any |CFO| > 1/(2T) = 3.1 Hz aliases; and in
// two-path multipath an up/down dominance swap turns the delay spread
// into a coarse-CFO error (~15 Hz on WIDE's sweep at 2 ms), landing the
// fine estimate several branches off - the whole WIDE/CCIR-Poor header
// outage. ZC refinement could not repair either: its fit spans +-3 Hz and
// trusted the chirp prior at 0.7 Hz.
//
// Every branch c_k = cfo + k*amb within the detector's capture range is
// scored by a local ZC fit (timing corrected for the pattern's
// delay-Doppler bias; residual searched over half a branch spacing, so
// the branches tile the range). On static channels the ZC fit power
// separates branches by 11-35 dB. Under fast fading it does NOT: on CCIR
// Poor the 3-symbol fast ZC block's fit power swings +-10 dB between
// neighbouring branches, so a wrong one scored best on ~25% of fast
// frames at 0 Hz (measured 2026-10, logged per candidate) - and no score
// margin separates that from the genuinely needed moves (static 20 Hz:
// >= 11 dB, Moderate 35 Hz: >= 3 dB, Poor robust: >= 0.3 dB). So the
// score only ORDERS the candidates and the header decode (LDPC + CRC)
// decides: best-scoring branch, then the detector's own branch, then the
// runner-up. A frame the v1.1.0 receiver decoded on the detector's branch
// still decodes; the extra header attempts are spent only on frames that
// would otherwise fail.
std::vector<CfoBranch> resolveCfoBranches(Family family,
                                          std::span<const cfloat> sig,
                                          ChirpSig chirpSig, double cfoHz,
                                          double zcCoarse, int zcCount) {
  const double amb = chirpCfoAmbiguityHz(chirpSig);
  if (!cfoBranchSearchEnabled() || amb <= 0.0) {
    return {CfoBranch{cfoHz, zcCoarse, 3.0}};
  }
  const double maxHz = chirpCfoCaptureMaxHz();
  const double sens = chirpStartCfoSensitivity(family, chirpSig);
  const double localRange = 0.5 * amb + 0.25;
  const double fitRange = std::max(3.0, localRange);
  const int symLen = zcSymbolSamples();
  const int kLo = int(std::ceil((-maxHz - cfoHz) / amb));
  const int kHi = int(std::floor((maxHz - cfoHz) / amb));
  struct Scored {
    CfoBranch b;
    double power;
  };
  std::vector<Scored> scored;
  for (int k = kLo; k <= kHi; ++k) {
    const double c = cfoHz + k * amb;
    // The detector's start carries an uncorrected delay-Doppler bias of
    // -sens * (true CFO); under hypothesis c the unbiased start is
    // zcCoarse + sens * c.
    const double st = zcCoarse + sens * c;
    const int64_t lo = std::max<int64_t>(0, int64_t(st) - 64);
    const int64_t hi = std::min<int64_t>(
        int64_t(sig.size()), int64_t(st) + int64_t(zcCount) * symLen + 64);
    if (hi - lo < symLen) continue;
    auto z = zcRefine(family, sig.subspan(size_t(lo), size_t(hi - lo)),
                      st - double(lo), c, 0.7, zcCount, 8, localRange);
    if (z.ok) scored.push_back({CfoBranch{c, st, fitRange}, z.fitPower});
  }
  const CfoBranch home{cfoHz, zcCoarse + sens * cfoHz, fitRange};
  if (scored.empty()) return {home};
  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& a, const Scored& b) {
                     return a.power > b.power;
                   });
  std::vector<CfoBranch> out;
  auto add = [&](const CfoBranch& b) {
    for (const auto& o : out) {
      if (std::abs(o.cfoHz - b.cfoHz) < 0.25 * amb) return;
    }
    out.push_back(b);
  };
  add(scored[0].b);
  add(home);
  if (scored.size() > 1) add(scored[1].b);
  if (std::getenv("SKW_CFO_DEBUG")) {
    std::fprintf(stderr, "cfo-branch sig=%d chirp=%+.2f amb=%.3f ->", int(chirpSig),
                 cfoHz, amb);
    for (const auto& b : out) std::fprintf(stderr, " %+.2f", b.cfoHz);
    std::fprintf(stderr, "\n");
  }
  return out;
}

namespace {

std::optional<RxFrameResult> receiveFrameImpl(
    Family family, std::span<const cfloat> rx, RxStage* stageOut,
    HarqChase* harq, const CarrierMask* payloadMask, bool wantConstellation,
    double fllGainOverride, bool* retryable) {
  auto stage = [&](RxStage s) {
    if (stageOut) *stageOut = s;
  };
  stage(RxStage::NoDetect);
  // 1) Chirp acquisition.
  ChirpDetector det(family);
  std::vector<ChirpDetection> hits;
  const size_t chunk = 1472;
  for (size_t i = 0; i < rx.size() && hits.empty(); i += chunk) {
    det.process(rx.subspan(i, std::min(chunk, rx.size() - i)), hits);
  }
  if (hits.empty()) return std::nullopt;
  const ChirpDetection& hit = hits.front();
  stage(RxStage::ZcFail);

  // Spectrum inversion auto-correct: conjugate and re-run the whole
  // receive path on the corrected signal.
  std::vector<cfloat> corrected;
  std::span<const cfloat> sig = rx;
  if (hit.inverted) {
    corrected.assign(rx.begin(), rx.end());
    for (auto& v : corrected) v = std::conj(v);
    sig = corrected;
  }
  const double cfoSign = hit.inverted ? -hit.cfoHz : hit.cfoHz;

  // CSS frames (sync arbitration): the chirp signature announced a CSS
  // frame; hand off to the CSS receive path - no ZC, no OFDM.
  if (hit.sig == ChirpSig::Css) {
    return cssReceiveFrame(family, sig, hit, cfoSign, stage, harq);
  }

  // 2) ZC refinement.
  const bool robust = robustHeaderForSig(hit.sig);
  const int zcCount = zcSymbols(robust);
  const int reps = int(chirpPattern(hit.sig).size());
  const double zcCoarse =
      hit.startSample + double(reps) * chirpRepSamples();
  // CFO branch candidates, verified by the header decode (R1): the ZC
  // fit alone cannot pick the branch under fast fading (see
  // resolveCfoBranches), but a header codeword that passes LDPC + CRC can.
  const auto branches =
      resolveCfoBranches(family, sig, hit.sig, cfoSign, zcCoarse, zcCount);
  const LdpcCode& hcode = LdpcCode::get(kLdpcNShort, {1, 2});
  ZcSyncResult zc;
  std::optional<ChannelTracker> trackerSlot;
  std::optional<SymbolWalker> walkerSlot;
  std::vector<uint8_t> hInfo;
  std::optional<PhyHeader> hdr;
  bool anyZc = false;
  // A walk that ran past the capture: the frame is still arriving (the
  // worker re-tries growing captures), so the drift retry cannot help.
  bool truncated = false;
  for (const auto& branch : branches) {
    zc = zcRefine(family, sig, branch.start, branch.cfoHz, 0.7, zcCount, 24,
                  branch.fitRangeHz);
    if (!zc.ok) continue;
    anyZc = true;

    // Integer symbol grid; fold the fractional remainder into H.
    const double firstSym =
        zc.blockStart + double(zcCount) * zcSymbolSamples();
    const int64_t t0 = int64_t(std::llround(firstSym));
    const double delta = double(t0) - firstSym;
    std::vector<cfloat> h0 = zc.channel;
    const int n = plan(family).carriers();
    for (int c = 0; c < n; ++c) {
      int bin = plan(family).firstBin + c - centerBin();  // signed
      double ph = 2.0 * M_PI * double(bin) * delta / kFftBaseband;
      h0[size_t(c)] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }

    // Initial noise guess: generous; the tracker converges within a few
    // symbols and the header codeword is long.
    double h0pow = 0;
    for (const auto& v : h0) h0pow += std::norm(v);
    h0pow /= std::max(n, 1);
    // ZC-measured noise seed, taken only when it LOWERS the estimate:
    // short frames (ACKs) never converge the tracker and the crude
    // 10-dB-below-signal guess froze AMC's effective SNR at ~16 dB on
    // rich channels - but under fading the "identical" ZC symbols differ
    // by CHANNEL MOTION, so the inter-symbol spread overestimates noise
    // and a high seed poisons whole frames (measured: M8+ fading FER
    // 30-70% far above the floors).
    const double nv0 = zc.noiseVar > 0.0
                           ? std::min(zc.noiseVar, h0pow * 0.1)
                           : h0pow * 0.1;
    const double meanChanPow =
        qNormEnabled() ? frameMeanChannelPower(family, sig, t0, 96) : -1.0;
    trackerSlot.emplace(family, robust, std::move(h0), nv0, meanChanPow);
    walkerSlot.emplace(SymbolWalker{family, sig, zc.cfoHz, t0, 0,
                                    &*trackerSlot});
    walkerSlot->fllGainOverride = fllGainOverride;

    // 3) Header codeword.
    std::vector<float> hLlr;
    if (!walkerSlot->collect(Constellation::Qpsk, headerSpread(robust),
                             CpMode::Medium, hcode.n(), hLlr)) {
      truncated = true;
      continue;
    }
    hInfo.clear();
    if (!decodeCodeword(hcode, hLlr, hInfo)) continue;
    auto hdrBytes = bitsToBytes({hInfo.data(), size_t(kPhyHeaderBytes) * 8});
    hdr = parsePhyHeader(hdrBytes);
    if (hdr) break;
  }
  if (!anyZc) return std::nullopt;
  stage(RxStage::HeaderFail);
  if (!hdr) {
    if (retryable && !truncated) *retryable = true;
    return std::nullopt;
  }
  ChannelTracker& tracker = *trackerSlot;
  SymbolWalker& walker = *walkerSlot;
  stage(RxStage::Ok);

  RxFrameResult res;
  res.header = *hdr;
  res.sig = hit.sig;
  res.inverted = hit.inverted;
  res.cfoHz = zc.cfoHz;

  // 4) Payload: piggyback bytes from the header codeword + payload
  // codewords at the header's mode/CP.
  const size_t piggy =
      std::min(size_t(hdr->length), size_t(kHeaderPiggybackBytes));
  std::vector<uint8_t> white;
  {
    auto pb = bitsToBytes(
        {hInfo.data() + size_t(kPhyHeaderBytes) * 8, piggy * 8});
    white.insert(white.end(), pb.begin(), pb.end());
  }

  bool ok = true;
  if (size_t(hdr->length) > piggy) {
    const ModeDef& md = kModes[size_t(hdr->mode)];
    if (md.waveform != Waveform::Ofdm || !md.active) return std::nullopt;
    size_t restBits = (size_t(hdr->length) - piggy) * 8;
    // Payload phase: apply the session carrier mask (header symbols used
    // the full set). The tracker skips masked pilots; its delay-domain
    // model interpolates the gap. Only at the modes the TRANSMITTER masks
    // (modeUsesCarrierMask): applying the mask to a spread-mode frame the
    // sender built unmasked misaligned the whole payload walk - every
    // DATA frame below M8 failed for the rest of the session once a mask
    // existed, while piggyback-only control frames kept passing.
    const CarrierMask* walkMask =
        modeUsesCarrierMask(int(hdr->mode)) ? payloadMask : nullptr;
    walker.mask = walkMask;
    tracker.setMask(walkMask);
    // Burst containers repack on retransmission: identical-codeword
    // combining cannot be guaranteed, so HARQ is bypassed for them.
    if (hdr->frameType == kBurstContainerFrameType) harq = nullptr;
    // Info chunks are k bits each, which is not byte-aligned for every
    // rate (r5/6: 1620 bits): concatenate BITS across codewords, convert
    // to bytes once. Codeword sizing mirrors the TX rule.
    std::vector<uint8_t> restInfoBits;
    restInfoBits.reserve(restBits);
    size_t remaining = restBits;
    size_t cw = 0;
    // Continuous payload stream: collect ALL codewords' coded bits in
    // one walk (TX packs them without per-codeword symbol alignment),
    // then slice per codeword.
    std::vector<int> cwN;
    {
      size_t rem = restBits;
      while (rem > 0) {
        const int n = payloadCodewordN(rem, md.rate);
        cwN.push_back(n);
        rem -= std::min(rem, size_t(n) * size_t(md.rate.num) /
                                 size_t(md.rate.den));
      }
    }
    long totalCoded = 0;
    for (int n : cwN) totalCoded += n;
    std::vector<float> allLlr;
    // Equalized symbols are always collected now: one complex divide per
    // despread symbol against a 320-point FFT and a Kalman update per
    // OFDM symbol, and they feed the data-aided SNR below.
    std::vector<cfloat> constPts;
    std::vector<float> gains;
    constPts.reserve(512);
    gains.reserve(512);
    walker.constellationOut = &constPts;
    walker.gainOut = &gains;
    const bool haveAll = walker.collect(md.constellation, md.spread,
                                        hdr->cpMode, int(totalCoded),
                                        allLlr);
    walker.constellationOut = nullptr;
    walker.gainOut = nullptr;
    std::vector<uint8_t> codedStream;  // re-encoded decoded bits, in air order
    codedStream.reserve(size_t(totalCoded));
    size_t llrOff = 0;
    // IR (rate-stepdown): an r1/2 frame may be a step-down retransmission
    // of an earlier rate>1/2 attempt - add that attempt's stored
    // systematic priors; a failing rate>1/2 payload stores its systematic
    // channel LLRs so the step-down decode can use them.
    const uint64_t ikey =
        HarqChase::infoKey(hdr->frameType, hdr->seqLow, hdr->length);
    const bool halfRate = md.rate.num == 1 && md.rate.den == 2;
    std::span<const float> priors{};
    if (harq && halfRate) {
      if (const auto* p = harq->infoPriors(ikey)) {
        if (p->size() == restBits) priors = *p;
      }
    }
    const bool collectPriors = harq && !halfRate;
    std::vector<float> sysLlrs;
    size_t priorOff = 0;
    while (remaining > 0 && (ok || harq)) {
      const LdpcCode& pcode =
          LdpcCode::get(payloadCodewordN(remaining, md.rate), md.rate);
      if (!haveAll) {
        truncated = true;
        ok = false;
        break;
      }
      std::vector<float> llr(allLlr.begin() + long(llrOff),
                             allLlr.begin() + long(llrOff + size_t(pcode.n())));
      llrOff += size_t(pcode.n());
      std::vector<uint8_t> info;
      std::vector<float> sys;
      bool dec = decodeCodeword(pcode, llr, info, priors, priorOff,
                                collectPriors ? &sys : nullptr);
      if (harq) {
        const uint64_t key = HarqChase::frameKey(
            hdr->frameType, hdr->seqLow, hdr->mode, hdr->length,
            uint32_t(cw));
        if (!dec) {
          // Chase: add earlier receptions' LLRs and retry once.
          if (harq->combine(key, llr) > 1) {
            dec = decodeCodeword(pcode, llr, info, priors, priorOff);
          }
          if (dec) harq->onDecodeSuccess(key);
        } else {
          harq->onDecodeSuccess(key);
        }
      }
      if (getenv("SKW_RX_DEBUG")) {
        fprintf(stderr, "skw-rx: cw=%zu n=%d dec=%d nvar=%.3g snr=%.1f\n",
                cw, pcode.n(), int(dec), tracker.noiseVar(),
                tracker.snrDb());
      }
      if (!dec) ok = false;
      if (dec) {
        // Rebuild this codeword's on-air bits for the data-aided SNR.
        std::vector<uint8_t> cwBits, il;
        pcode.encode(info, cwBits);
        interleaveBits(cwBits, il);
        codedStream.insert(codedStream.end(), il.begin(), il.end());
      }
      size_t want = std::min(remaining, size_t(pcode.k()));
      if (collectPriors && sys.size() >= want) {
        sysLlrs.insert(sysLlrs.end(), sys.begin(), sys.begin() + long(want));
      }
      restInfoBits.insert(restInfoBits.end(), info.begin(),
                          info.begin() + long(want));
      remaining -= want;
      priorOff += want;
      ++cw;
    }
    if (harq) {
      if (ok) {
        harq->dropInfoPriors(ikey);
      } else if (collectPriors && sysLlrs.size() == restBits) {
        harq->storeInfoPriors(ikey, std::move(sysLlrs));
      }
    }
    if (ok) {
      auto bytes = bitsToBytes(restInfoBits);
      white.insert(white.end(), bytes.begin(), bytes.end());
      // Data-aided SNR: error vector of each equalized symbol against
      // the ideal point of its decoded bits. z/a = s + w with
      // var(w) = sigma^2 / a, so the per-carrier noise is a * |w|^2;
      // signal is the tracker's mean |H|^2, the same numerator the
      // tracker's own snrDb() uses.
      const int bps = bitsPerSymbol(md.constellation);
      const size_t nSym = std::min(constPts.size(), codedStream.size() / size_t(bps));
      if (nSym > 0 && gains.size() >= nSym) {
        double nv = 0.0;
        for (size_t i = 0; i < nSym; ++i) {
          uint32_t v = 0;
          for (int b = 0; b < bps; ++b) {
            v = (v << 1) | (codedStream[i * size_t(bps) + size_t(b)] & 1);
          }
          const cfloat e = constPts[i] - mapSymbol(md.constellation, v);
          nv += double(gains[i]) * std::norm(e);
        }
        nv /= double(nSym);
        double p = 0.0;
        for (const auto& v : tracker.h()) p += std::norm(v);
        p /= double(std::max<size_t>(tracker.h().size(), 1));
        res.evmSnrDb = 10.0 * std::log10(std::max(p / std::max(nv, 1e-12), 1e-9));
      }
    }
    if (wantConstellation) res.constellation = std::move(constPts);
  }
  res.payloadOk = ok && white.size() >= size_t(hdr->length);
  if (!res.payloadOk && retryable && !truncated) *retryable = true;
  if (res.payloadOk) {
    white.resize(size_t(hdr->length));
    Whitener wh;
    wh.apply(white);
    res.payload = std::move(white);
  }
  // Report SNR in the 3 kHz-noise convention of the floor tables (and
  // of the CSS path's snr3k): the tracker's per-carrier estimate sits a
  // measured, family-dependent offset above it (ofdmSnrCalDb).
  res.snrDb = tracker.snrDb() - ofdmSnrCalDb(family);
  res.snrConverged = tracker.noiseUpdates() >= kSnrConvergedSymbols;
  res.delaySpreadMs = tracker.delaySpreadMs();
  {
    const auto& h = tracker.h();
    const auto& re = tracker.carrierResid();
    const double nv = std::max(tracker.noiseVar(), 1e-12);
    res.carrierSnrDb.resize(h.size());
    double chanPow = 0.0, residSum = 0.0;
    int residN = 0;
    for (size_t c = 0; c < h.size(); ++c) {
      const double denom =
          std::max(re.size() == h.size() && re[c] >= 0 ? re[c] : nv, 1e-12);
      res.carrierSnrDb[c] = float(
          10.0 * std::log10(std::max(double(std::norm(h[c])) / denom,
                                     1e-9)));
      chanPow += double(std::norm(h[c]));
      if (re.size() == h.size() && re[c] >= 0) {
        residSum += re[c];
        ++residN;
      }
    }
    // Residual energy relative to channel power. This is the quantity
    // the AMC's pilotStabilityMax gate was always specified against;
    // nothing ever computed it, so that gate has been evaluating a
    // constant 0 and never firing.
    if (residN > 0 && chanPow > 1e-12) {
      const double ratio =
          (residSum / double(residN)) / (chanPow / double(h.size()));
      // The raw ratio is NOT a channel-motion indicator by itself: on
      // pure AWGN the pilot residual is measurement noise, so the ratio
      // reads ~1.5 / SNR (measured with bertool's `pilot` column: 2.95
      // at M4 -7.5 dB, 1.0 at M6 -2.5 dB, 0.043 at M8 12 dB, 0.006 at
      // M11 22 dB - all with zero Doppler). Uncompensated, the
      // classifier called every robust-tier link below ~-3 dB SEVERE
      // and could never call a static channel STATIC below ~13 dB (its
      // 0.03 bound), so the AMC used fading floors on AWGN paths at the
      // robust tier - a 12 dB penalty at M6. Report the EXCESS over the
      // noise share: what channel motion adds. The calibration table
      // (AmcConfig::pilotStabilityMax, M10 15-25 dB) is unaffected to
      // within its own spread - the noise share there is <= 0.05.
      // The noise share must come from a MOTION-FREE noise figure: the
      // tracker's own nv already contains the prediction lag under
      // Doppler, and subtracting it cancelled the very excess being
      // measured (Moderate read 0.000 too). Use the data-aided EVM
      // noise when the payload decoded (coefficient re-fitted in those
      // terms: ~0.6 at the floor region), and fall back to the tracker
      // figure otherwise - a failed frame's zero excess is harmless,
      // the FER gate keeps such a channel from reading Static.
      if (res.evmSnrDb > -90.0) {
        const double noiseShare =
            kPilotNoiseCoefEvm / std::pow(10.0, res.evmSnrDb / 10.0);
        res.pilotStability = std::max(0.0, ratio - noiseShare);
      } else {
        // No decoded payload (control frame, or a failed one): no
        // motion-free noise figure, so no sample - the tracker-based
        // share over-subtracts under fading and read a flat 0 that
        // diluted the classifier's EMA with every ACK.
        res.pilotStability = -1.0;
      }
    }
    // Selectivity: peak-to-median carrier SNR spread, scaled to the
    // 0..255 index units the M12+ gate uses.
    // Selectivity means DEEP NOTCHES - the thing that makes 64QAM
    // pointless regardless of average SNR - so the statistic is the
    // FRACTION of carriers sitting >=10 dB below the band median, not a
    // percentile spread (which failed to separate CCIR Good from
    // Moderate at all: 62 vs 67 on a 0-255 scale).
    if (!res.carrierSnrDb.empty()) {
      std::vector<float> s(res.carrierSnrDb);
      std::sort(s.begin(), s.end());
      const float med = s[s.size() / 2];
      int notched = 0;
      for (float v : res.carrierSnrDb) {
        if (v <= med - 10.0f) ++notched;
      }
      res.selectivity = uint8_t(std::clamp(
          255.0 * double(notched) / double(res.carrierSnrDb.size()), 0.0,
          255.0));
    }
  }
  return res;
}

}  // namespace

std::optional<RxFrameResult> receiveFrame(Family family,
                                          std::span<const cfloat> rx,
                                          RxStage* stageOut, HarqChase* harq,
                                          const CarrierMask* payloadMask,
                                          bool wantConstellation) {
  bool retryable = false;
  auto res = receiveFrameImpl(family, rx, stageOut, harq, payloadMask,
                              wantConstellation, -1.0, &retryable);
  if (!retryable || !fllDriftRetryEnabled()) return res;
  // Drift retry (see fllDriftGain). Without HARQ: the first pass already
  // stored this reception's LLRs, and combining them with a second walk
  // of the same samples would count it twice. A frame that decodes here
  // leaves its HARQ entries to age out, as the CSS drift search does.
  RxStage st2 = RxStage::NoDetect;
  auto res2 = receiveFrameImpl(family, rx, &st2, nullptr, payloadMask,
                               wantConstellation, fllDriftGain(), nullptr);
  if (!res2) return res;
  if (res && res->payloadOk) return res;
  if (res && !res2->payloadOk) return res;  // keep the first header pass
  if (std::getenv("SKW_RX_DEBUG")) {
    std::fprintf(stderr, "skw-rx: drift retry recovered %s\n",
                 res ? "payload" : "header");
  }
  if (stageOut) *stageOut = st2;
  return res2;
}

}  // namespace skw
