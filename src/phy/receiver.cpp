#include "phy/receiver.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fft.hpp"
#include "dsp/zc.hpp"
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
    const int cpLen = kCpSamplesAudio[size_t(cp)] / kDecimation;
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

  // 2) ZC refinement.
  const bool robust = robustHeaderForSig(hit.sig);
  const int zcCount = robust ? kZcSymbolsRobust : kZcSymbolsFast;
  const int reps = int(chirpPattern(hit.sig).size());
  const double zcCoarse =
      hit.startSample + double(reps) * chirpRepSamples();
  auto zc = zcRefine(family, sig, zcCoarse, cfoSign, 0.7, zcCount);
  if (!zc.ok) return std::nullopt;
  stage(RxStage::HeaderFail);

  // Integer symbol grid; fold the fractional remainder into H.
  const double firstSym = zc.blockStart + double(zcCount) * kZcSymbolSamples;
  const int64_t t0 = int64_t(std::llround(firstSym));
  const double delta = double(t0) - firstSym;
  std::vector<cfloat> h0 = zc.channel;
  const int n = plan(family).carriers();
  for (int c = 0; c < n; ++c) {
    int bin = plan(family).firstBin + c - kCenterBin;  // signed
    double ph = 2.0 * M_PI * double(bin) * delta / kFftBaseband;
    h0[size_t(c)] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }

  // Initial noise guess: generous; the tracker converges within a few
  // symbols and the header codeword is long.
  double h0pow = 0;
  for (const auto& v : h0) h0pow += std::norm(v);
  h0pow /= std::max(n, 1);
  ChannelTracker tracker(family, robust, std::move(h0), h0pow * 0.1);

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
      std::vector<float> llr;
      if (!walker.collect(md.constellation, md.spread, hdr->cpMode,
                          pcode.n(), llr)) {
        ok = false;
        break;
      }
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
