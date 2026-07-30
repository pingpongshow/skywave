#include "phy/receiver.hpp"

#include <algorithm>
#include <cmath>
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
  Fft fft{kFftBaseband};

  // Collect exactly `bits` LLRs coded at the given parameters. Returns
  // false on running out of samples.
  bool collect(Constellation cst, int spread, CpMode cp, int bits,
               std::vector<float>& llr) {
    const int cpLen = cpSamplesAudio(cp) / kDecimation;
    const int symLen = kFftBaseband + cpLen;
    const int bps = bitsPerSymbol(cst);
    const int G = spreadGroupSymbols(family, spread, mask);
    const int n = plan(family).carriers();

    int got = 0;
    while (got < bits) {
      // FFT the group's symbols.
      std::vector<std::vector<cfloat>> bins;
      std::vector<std::pair<int, int>> pool;
      for (int g = 0; g < G; ++g) {
        int64_t start = nextSampleStart + int64_t(g) * symLen + cpLen;
        if (start < 0 || start + kFftBaseband > int64_t(rx.size())) {
          return false;
        }
        std::vector<cfloat> in(static_cast<size_t>(kFftBaseband));
        for (int i = 0; i < kFftBaseband; ++i) {
          double ph = -2.0 * M_PI * cfoHz * double(start + i) / kFsBaseband;
          in[size_t(i)] = rx[size_t(start + i)] *
                          cfloat(float(std::cos(ph)), float(std::sin(ph)));
        }
        std::vector<cfloat> out(static_cast<size_t>(kFftBaseband));
        fft.forward(in.data(), out.data());
        // Undo the TX time-domain scale so pilot-measured H matches the
        // ZC-seeded estimate (see ofdmTimeScale).
        const float invTs = 1.0f / ofdmTimeScale(family);
        for (auto& b : out) b *= invTs;
        tracker->processSymbol(out, symIdx + g);
        auto lay = symbolLayout(family, symIdx + g, mask);
        for (int c : lay.dataCarriers) pool.push_back({g, c});
        bins.push_back(std::move(out));
      }
      const int nSrc = int(pool.size()) / spread;
      const auto& h = tracker->h();
      const float nvar = float(tracker->noiseVar());
      for (int i = 0; i < nSrc && got < bits; ++i) {
        cfloat z(0, 0);
        float a = 0;
        for (int k = 0; k < spread; ++k) {
          auto [g, c] = pool[size_t(i + k * nSrc)];
          cfloat y = bins[size_t(g)][size_t(carrierToBasebandBin(family, c))];
          cfloat eff = h[size_t(c)] * spreadPhase(i, k, symIdx);
          z += y * std::conj(eff);
          a += std::norm(eff);
        }
        demapLlr(cst, z, a, nvar, llr);
        got += bps;
      }
      (void)n;
      nextSampleStart += int64_t(G) * symLen;
      symIdx += G;
    }
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

  // Both header copies must be walked (the payload starts after them);
  // the CRC-16 arbitrates - a fade takes at most one copy.
  std::optional<PhyHeader> hdr;
  for (int c = 0; c < kCssHeaderCopies; ++c) {
    auto hdrBytes = walkSection(kCssHeaderSf, kPhyHeaderBytes);
    if (!hdrBytes) {
      if (!hdr) return std::nullopt;
      break;  // ran out of samples but an earlier copy already parsed
    }
    if (!hdr) hdr = parsePhyHeader(*hdrBytes);
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
  {
    const size_t symLen = cssSymbolSamples(family, sf);
    const size_t codedBits = cssPayloadCodedBits(size_t(hdr->length));
    const size_t nsym = (codedBits + size_t(sf) - 1) / size_t(sf);
    const int PW = 3 * spc + 6;
    std::vector<float> llr;
    llr.reserve(nsym * size_t(sf));
    bool samplesOk = true;
    for (size_t s = 0; s < nsym && samplesOk; ++s) {
      if (s % size_t(kCssPilotInterval) == 0) {
        if (rx.size() < pos + size_t(2 * PW) + symH) {
          samplesOk = false;
          break;
        }
        std::vector<float> pp(size_t(2 * PW + 1), 0.0f);
        for (int d = -PW; d <= PW; ++d) {
          cfloat c(0, 0);
          const cfloat* xr = rx.data() + long(pos) + d;
          for (size_t n = 0; n < symH; ++n) c += xr[n] * std::conj(base[n]);
          pp[size_t(d + PW)] = float(std::abs(c));
        }
        float pk = 0.0f;
        int dp = 0;
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
        if (nsMed > 0.0f && pk >= 2.5f * nsMed) {
          updateTaps(pp, PW, 0, nsMed, false);
        }
        pos += symH;
      }
      if (rx.size() < pos + symLen) {
        samplesOk = false;
        break;
      }
      cssSoftDemodSymbol(family, sf,
                         std::span<const cfloat>(rx).subspan(pos, symLen),
                         taps, llr);
      pos += symLen;
    }
    if (samplesOk && llr.size() >= codedBits) {
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
      if (hdr->frameType == kBurstContainerFrameType) harq = nullptr;
      while (remaining > 0 && (ok || harq)) {
        std::vector<float> cwLlr(llr.begin() + long(off),
                                 llr.begin() + long(off + size_t(pcode.n())));
        std::vector<uint8_t> info;
        bool dec = decodeCodeword(pcode, cwLlr, info);
        if (harq) {
          // Chase: a deep-tier retransmission carries identical
          // codewords, and the payload IS LDPC - soft-combine with
          // earlier receptions (~3 dB per doubling at the floor).
          const uint64_t key =
              HarqChase::frameKey(hdr->frameType, hdr->seqLow, hdr->mode,
                                  hdr->length, uint32_t(cw));
          if (!dec) {
            if (harq->combine(key, cwLlr) > 1) {
              dec = decodeCodeword(pcode, cwLlr, info);
            }
            if (dec) harq->onDecodeSuccess(key);
          } else {
            harq->onDecodeSuccess(key);
          }
        }
        if (!dec) {
          ok = false;
          if (!harq) break;
        } else if (ok) {
          const size_t want = std::min(remaining, size_t(pcode.k()));
          infoBits.insert(infoBits.end(), info.begin(),
                          info.begin() + long(want));
        }
        remaining -= std::min(remaining, size_t(pcode.k()));
        off += size_t(pcode.n());
        ++cw;
      }
      if (ok) {
        auto white = bitsToBytes(infoBits);
        white.resize(payBytes);
        const uint16_t want =
            uint16_t((white[payBytes - 2] << 8) | white[payBytes - 1]);
        white.resize(size_t(hdr->length));
        if (crc16(white) == want) {
          Whitener wh;
          wh.apply(white);
          res.payload = std::move(white);
          res.payloadOk = true;
        }
      }
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

std::optional<RxFrameResult> receiveFrame(Family family,
                                          std::span<const cfloat> rx,
                                          RxStage* stageOut, HarqChase* harq,
                                          const CarrierMask* payloadMask) {
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
  auto zc = zcRefine(family, sig, zcCoarse, cfoSign, 0.7, zcCount);
  if (!zc.ok) return std::nullopt;
  stage(RxStage::HeaderFail);

  // Integer symbol grid; fold the fractional remainder into H.
  const double firstSym = zc.blockStart + double(zcCount) * zcSymbolSamples();
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
  // ZC-measured noise seed when available: short frames (ACKs) never
  // converge the tracker, and a crude 10-dB-below-signal guess froze
  // AMC's effective SNR at ~16 dB on rich channels.
  const double nv0 =
      zc.noiseVar > 0.0 ? zc.noiseVar : h0pow * 0.1;
  ChannelTracker tracker(family, robust, std::move(h0), nv0);

  SymbolWalker walker{family, sig, zc.cfoHz, t0, 0, &tracker};

  // 3) Header codeword.
  const LdpcCode& hcode = LdpcCode::get(kLdpcNShort, {1, 2});
  std::vector<float> hLlr;
  if (!walker.collect(Constellation::Qpsk, headerSpread(robust),
                      CpMode::Medium, hcode.n(), hLlr)) {
    return std::nullopt;
  }
  std::vector<uint8_t> hInfo;
  if (!decodeCodeword(hcode, hLlr, hInfo)) return std::nullopt;
  auto hdrBytes = bitsToBytes({hInfo.data(), size_t(kPhyHeaderBytes) * 8});
  auto hdr = parsePhyHeader(hdrBytes);
  if (!hdr) return std::nullopt;
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
    // model interpolates the gap.
    walker.mask = payloadMask;
    tracker.setMask(payloadMask);
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
    const bool haveAll = walker.collect(md.constellation, md.spread,
                                        hdr->cpMode, int(totalCoded),
                                        allLlr);
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
    }
  }
  res.payloadOk = ok && white.size() >= size_t(hdr->length);
  if (res.payloadOk) {
    white.resize(size_t(hdr->length));
    Whitener wh;
    wh.apply(white);
    res.payload = std::move(white);
  }
  res.snrDb = tracker.snrDb();
  res.delaySpreadMs = tracker.delaySpreadMs();
  {
    const auto& h = tracker.h();
    const auto& re = tracker.carrierResid();
    const double nv = std::max(tracker.noiseVar(), 1e-12);
    res.carrierSnrDb.resize(h.size());
    for (size_t c = 0; c < h.size(); ++c) {
      const double denom =
          std::max(re.size() == h.size() && re[c] >= 0 ? re[c] : nv, 1e-12);
      res.carrierSnrDb[c] = float(
          10.0 * std::log10(std::max(double(std::norm(h[c])) / denom,
                                     1e-9)));
    }
  }
  return res;
}

}  // namespace skw
