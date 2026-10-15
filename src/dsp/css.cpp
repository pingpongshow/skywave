#include "dsp/css.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "dsp/fft.hpp"

namespace skw {
namespace {

constexpr double kTwoPi = 2.0 * M_PI;

// SF is stored in kModes.spread for M0..M3: M0=8, M1=7, M2=6, M3=5.
// The base up-chirp sweeps the family CSS band over 2^SF chips.

// Gray code so an off-by-one demod bin costs a single bit: the data bits
// carried by bin n are toGray(n) (adjacent n differ in one bit of
// toGray(n)), so the modulator picks bin = fromGray(bits).
uint16_t toGray(uint16_t v) { return uint16_t(v ^ (v >> 1)); }
uint16_t fromGray(uint16_t g) {
  uint16_t v = g;
  for (uint16_t s = v >> 1; s; s >>= 1) v ^= s;
  return v;
}

// Hamming(7,4): encode a nibble to 7 bits, decode 7 bits correcting one
// error. Bit layout: p1 p2 d1 p4 d2 d3 d4 (positions 1..7).
uint8_t hamming74Encode(uint8_t nib) {
  const uint8_t d1 = (nib >> 0) & 1, d2 = (nib >> 1) & 1,
                d3 = (nib >> 2) & 1, d4 = (nib >> 3) & 1;
  const uint8_t p1 = d1 ^ d2 ^ d4;
  const uint8_t p2 = d1 ^ d3 ^ d4;
  const uint8_t p4 = d2 ^ d3 ^ d4;
  return uint8_t(p1 | (p2 << 1) | (d1 << 2) | (p4 << 3) | (d2 << 4) |
                 (d3 << 5) | (d4 << 6));
}
uint8_t hamming74Decode(uint8_t code) {
  auto bit = [&](int pos) { return (code >> (pos - 1)) & 1; };
  const int s1 = bit(1) ^ bit(3) ^ bit(5) ^ bit(7);
  const int s2 = bit(2) ^ bit(3) ^ bit(6) ^ bit(7);
  const int s4 = bit(4) ^ bit(5) ^ bit(6) ^ bit(7);
  const int syn = s1 | (s2 << 1) | (s4 << 2);
  if (syn) code ^= uint8_t(1u << (syn - 1));  // flip the implicated bit
  return uint8_t((((code >> 2) & 1) << 0) | (((code >> 4) & 1) << 1) |
                 (((code >> 5) & 1) << 2) | (((code >> 6) & 1) << 3));
}

}  // namespace

CssPlan cssPlan(Family f) {
  // Chip rate = family occupied bandwidth, snapped DOWN (ceil on the
  // samples-per-chip) so the chirp never exceeds the family band and the
  // TX band filter passes it intact; kFsBaseband stays an exact integer
  // number of samples per chip.
  const double bw = plan(f).carriers() * kSubcarrierHz;
  int spc = std::max(2, int(std::ceil(double(kFsBaseband) / bw - 1e-9)));
  return {double(kFsBaseband) / spc, spc};
}

size_t cssSymbolSamples(Family f, int sf) {
  return size_t(1u << sf) * size_t(cssPlan(f).samplesPerChip);
}

namespace {
// Base up-chirp at the full sample rate, DC-centered like everything in
// the 12 kHz complex baseband (the audio converters place DC at
// kCenterHz). Phase is the PROPER integral of a linear frequency sweep
// (-B/2 .. +B/2 over the symbol), so a cyclic time shift maps cleanly to
// a single matched-filter lag.
std::vector<cfloat> baseChirp(const CssPlan& p, int N) {
  const int spc = p.samplesPerChip;
  const size_t symLen = size_t(N) * spc;
  const double fStart = -p.bandHz / 2.0;
  const double Tsym = double(symLen) / kFsBaseband;
  const double mu = p.bandHz / Tsym;  // sweep rate, Hz/s
  std::vector<cfloat> c(symLen);
  for (size_t n = 0; n < symLen; ++n) {
    const double t = double(n) / kFsBaseband;
    const double ph = kTwoPi * (fStart * t + 0.5 * mu * t * t);
    c[n] = cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }
  return c;
}
}  // namespace

std::vector<cfloat> cssModulate(Family f, int sf,
                                std::span<const uint16_t> symbols) {
  const CssPlan p = cssPlan(f);
  const int N = 1 << sf;                 // chips per symbol
  const int spc = p.samplesPerChip;
  const size_t symLen = size_t(N) * spc;
  const auto base = baseChirp(p, N);
  std::vector<cfloat> out(symbols.size() * symLen);
  for (size_t s = 0; s < symbols.size(); ++s) {
    // A CSS symbol of value v is the base chirp cyclically shifted by v
    // chips (v*spc samples). A pure rotation of the base sequence, so the
    // dechirp lands its energy in exactly one bin.
    const size_t shift = (size_t(symbols[s]) % size_t(N)) * size_t(spc);
    cfloat* dst = out.data() + s * symLen;
    for (size_t n = 0; n < symLen; ++n) {
      dst[n] = base[(n + shift) % symLen];
    }
  }
  return out;
}

uint16_t cssDemodSymbol(Family f, int sf, std::span<const cfloat> sym,
                        float* peakRatio) {
  return cssDemodSymbolTaps(f, sf, sym, {}, peakRatio);
}

std::vector<cfloat> cssBaseChirp(Family f, int sf) {
  return baseChirp(cssPlan(f), 1 << sf);
}

namespace {
// Tap-combined matched-filter scores for every cyclic shift; returns the
// mean score (normalization reference for peak ratio and soft LLRs).
double cssShiftScores(Family f, int sf, std::span<const cfloat> sym,
                      std::span<const CssTap> taps,
                      std::vector<double>& scores,
                      CssPeakInfo* peak = nullptr) {
  const CssPlan p = cssPlan(f);
  const int N = 1 << sf;
  const int spc = p.samplesPerChip;
  const size_t symLen = size_t(N) * spc;
  // Non-coherent matched-filter bank over all N cyclic shifts, evaluated
  // as one FFT cross-correlation: corr = IFFT(FFT(sym) . conj(FFT(base)))
  // and |corr| sampled at the N chip-aligned lags. Optimal for AWGN -
  // unlike dechirp + chip-rate decimation there is no aliasing of large
  // shifts and no split-bin combining penalty at the oversampled rate.
  //
  // The (family, sf) -> symLen map is collision-free across the four
  // family plans, so symLen alone keys the cache.
  static thread_local std::vector<cfloat> baseSpecConj;
  static thread_local std::unique_ptr<Fft> fft;
  if (baseSpecConj.size() != symLen) {
    const auto base = baseChirp(p, N);
    fft = std::make_unique<Fft>(int(symLen));
    std::vector<cfloat> spec(symLen);
    fft->forward(base.data(), spec.data());
    baseSpecConj.resize(symLen);
    for (size_t k = 0; k < symLen; ++k) baseSpecConj[k] = std::conj(spec[k]);
  }
  std::vector<cfloat> in(symLen, cfloat(0, 0));
  const size_t have = std::min(symLen, sym.size());
  std::copy(sym.begin(), sym.begin() + long(have), in.begin());
  std::vector<cfloat> spec(symLen), corr(symLen);
  fft->forward(in.data(), spec.data());
  for (size_t k = 0; k < symLen; ++k) spec[k] *= baseSpecConj[k];
  fft->inverse(spec.data(), corr.data());
  // corr[d] = sum_n sym[n] conj(base[n-d]): a shift-v symbol (tx[n] =
  // base[n + v*spc]) peaks at lag d = -v*spc, i.e. symLen - v*spc; a
  // channel path delayed by tau moves that to symLen - v*spc + tau. RAKE:
  // sum tap-weighted magnitudes across the resolved paths.
  static constexpr CssTap kSingleTap[1] = {{0, 1.0f}};
  if (taps.empty()) taps = kSingleTap;
  const long L = long(symLen);
  scores.assign(size_t(N), 0.0);
  double sumMag = 0.0;
  for (int v = 0; v < N; ++v) {
    const long lag0 = L - long(v) * spc;
    double m = 0.0;
    for (const CssTap& t : taps) {
      const long idx = ((lag0 + t.delay) % L + L) % L;
      m += double(t.weight) * std::abs(corr[size_t(idx)]);
    }
    scores[size_t(v)] = m;
    sumMag += m;
  }
  if (peak) {
    int best = 0;
    for (int v = 1; v < N; ++v) {
      if (scores[size_t(v)] > scores[size_t(best)]) best = v;
    }
    const CssTap* strongest = &taps[0];
    for (const CssTap& t : taps) {
      if (t.weight > strongest->weight) strongest = &t;
    }
    const long at = L - long(best) * spc + strongest->delay;
    auto mag = [&](long d) { return std::abs(corr[size_t(((at + d) % L + L) % L)]); };
    long dm = 0;
    float vm = mag(0);
    for (long d = -spc; d <= spc; ++d) {
      const float v = mag(d);
      if (v > vm) {
        vm = v;
        dm = d;
      }
    }
    const float a = mag(dm - 1), b = vm, c = mag(dm + 1);
    const float den = a - 2.0f * b + c;
    const float frac = std::abs(den) > 1e-12f ? 0.5f * (a - c) / den : 0.0f;
    peak->offset = float(dm) + std::clamp(frac, -0.5f, 0.5f);
    const double meanScore = sumMag / N;
    peak->ratio = meanScore > 1e-12 ? float(scores[size_t(best)] / meanScore) : 0.0f;
  }
  return sumMag / N;
}
}  // namespace

uint16_t cssDemodSymbolTaps(Family f, int sf, std::span<const cfloat> sym,
                            std::span<const CssTap> taps, float* peakRatio) {
  std::vector<double> scores;
  const double mean = cssShiftScores(f, sf, sym, taps, scores);
  int best = 0;
  double bestMag = 0.0;
  for (size_t v = 0; v < scores.size(); ++v) {
    if (scores[v] > bestMag) {
      bestMag = scores[v];
      best = int(v);
    }
  }
  if (peakRatio) {
    *peakRatio = mean > 1e-12 ? float(bestMag / mean) : 0.0f;
  }
  return uint16_t(best);
}

std::vector<uint16_t> cssMapCodedBits(std::span<const uint8_t> bits, int sf) {
  // Pack sf coded bits per symbol LSB-first, bin = fromGray(bits): the
  // same gray convention as the Hamming layer, shared by soft demod.
  const size_t nsym = (bits.size() + size_t(sf) - 1) / size_t(sf);
  std::vector<uint16_t> syms;
  syms.reserve(nsym);
  size_t bi = 0;
  for (size_t s = 0; s < nsym; ++s) {
    uint16_t v = 0;
    for (int b = 0; b < sf; ++b) {
      const uint16_t bit = bi < bits.size() ? (bits[bi] & 1) : 0;
      v |= uint16_t(bit << b);
      ++bi;
    }
    syms.push_back(fromGray(v));
  }
  return syms;
}

void cssSoftDemodSymbol(Family f, int sf, std::span<const cfloat> sym,
                        std::span<const CssTap> taps,
                        std::vector<float>& llr, CssPeakInfo* peak) {
  // Dual-max soft metric: per coded bit, the margin between the best
  // shift carrying bit=0 and the best carrying bit=1, normalized by the
  // mean score (positive LLR = bit 0, the LDPC convention). A faded or
  // ambiguous symbol yields small margins - low confidence - instead of
  // hard wrong bits, which is what buys the coding gain over Hamming.
  std::vector<double> scores;
  const double mean = cssShiftScores(f, sf, sym, taps, scores, peak);
  const int N = 1 << sf;
  std::vector<double> m0(size_t(sf), 0.0), m1(size_t(sf), 0.0);
  for (int v = 0; v < N; ++v) {
    const uint16_t bitsOfV = toGray(uint16_t(v));
    const double s = scores[size_t(v)];
    for (int b = 0; b < sf; ++b) {
      if ((bitsOfV >> b) & 1) {
        m1[size_t(b)] = std::max(m1[size_t(b)], s);
      } else {
        m0[size_t(b)] = std::max(m0[size_t(b)], s);
      }
    }
  }
  const double norm = std::max(mean, 1e-12);
  for (int b = 0; b < sf; ++b) {
    llr.push_back(float((m0[size_t(b)] - m1[size_t(b)]) / norm));
  }
}

size_t cssSymbolsForBytes(size_t byteCount, int sf) {
  // 2 nibbles/byte, one Hamming codeword per nibble, 7 bits each,
  // full-message row-column interleave; each symbol carries sf bits.
  const size_t bits = byteCount * 2 * 7;
  return (bits + size_t(sf) - 1) / size_t(sf);
}

std::vector<uint16_t> cssEncode(std::span<const uint8_t> bytes, int sf) {
  // Nibbles -> Hamming codewords.
  std::vector<uint8_t> cw;
  cw.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    cw.push_back(hamming74Encode(b & 0x0F));
    cw.push_back(hamming74Encode(b >> 4));
  }
  // FULL-MESSAGE row-column interleave: bit i of codeword j goes to
  // stream position i*ncw + j. A contiguous fade of L symbols (L*sf
  // consecutive stream bits) then spreads over ~L*sf/ncw bits per
  // codeword instead of wiping whole codewords - HF fade dwells are
  // ~1 s, far longer than any small fixed interleave depth at deep-tier
  // symbol times.
  const size_t ncw = cw.size();
  std::vector<uint8_t> bits;
  bits.reserve(ncw * 7);
  for (int i = 0; i < 7; ++i) {
    for (size_t j = 0; j < ncw; ++j) {
      bits.push_back((cw[j] >> i) & 1);
    }
  }
  // Pack sf bits per symbol; bin = fromGray(bits) so an adjacent-bin
  // demod error costs exactly one data bit.
  std::vector<uint16_t> syms;
  const size_t total = cssSymbolsForBytes(bytes.size(), sf);
  syms.reserve(total);
  size_t bi = 0;
  for (size_t s = 0; s < total; ++s) {
    uint16_t v = 0;
    for (int b = 0; b < sf; ++b) {
      const uint16_t bit = bi < bits.size() ? bits[bi] : 0;
      v |= uint16_t(bit << b);
      ++bi;
    }
    syms.push_back(fromGray(v));
  }
  return syms;
}

std::vector<uint8_t> cssDecode(std::span<const uint16_t> symbols, int sf,
                               size_t byteCount) {
  // Unpack sf bits per symbol (bits = toGray(bin)).
  std::vector<uint8_t> bits;
  bits.reserve(symbols.size() * size_t(sf));
  for (uint16_t g : symbols) {
    const uint16_t v = toGray(g);
    for (int b = 0; b < sf; ++b) bits.push_back((v >> b) & 1);
  }
  const size_t nibbles = byteCount * 2;
  const size_t ncw = nibbles;
  std::vector<uint8_t> cw(ncw, 0);
  // De-interleave (inverse of the full-message row-column map).
  size_t bi = 0;
  for (int i = 0; i < 7; ++i) {
    for (size_t j = 0; j < ncw; ++j) {
      if (bi < bits.size()) {
        cw[j] |= uint8_t(bits[bi] << i);
      }
      ++bi;
    }
  }
  std::vector<uint8_t> out(byteCount, 0);
  for (size_t n = 0; n < nibbles; ++n) {
    const uint8_t nib = hamming74Decode(cw[n]);
    if ((n & 1) == 0) {
      out[n / 2] = nib;
    } else {
      out[n / 2] |= uint8_t(nib << 4);
    }
  }
  return out;
}

std::vector<uint8_t> cssDecodeSoft(std::span<const float> llr, int sf,
                                   size_t byteCount) {
  (void)sf;  // bits are already in stream order, sf per symbol
  const size_t ncw = byteCount * 2;
  // De-interleave: stream position i*ncw + j is bit i of codeword j.
  std::vector<float> cwLlr(ncw * 7, 0.0f);
  for (int i = 0; i < 7; ++i) {
    for (size_t j = 0; j < ncw; ++j) {
      const size_t bi = size_t(i) * ncw + j;
      if (bi < llr.size()) cwLlr[j * 7 + size_t(i)] = llr[bi];
    }
  }
  std::vector<uint8_t> out(byteCount, 0);
  for (size_t n = 0; n < ncw; ++n) {
    int bestNib = 0;
    float bestMetric = -1e30f;
    for (int nib = 0; nib < 16; ++nib) {
      const uint8_t c = hamming74Encode(uint8_t(nib));
      float m = 0.0f;
      for (int i = 0; i < 7; ++i) {
        const float l = cwLlr[n * 7 + size_t(i)];
        m += ((c >> i) & 1) ? -l : l;
      }
      if (m > bestMetric) {
        bestMetric = m;
        bestNib = nib;
      }
    }
    if ((n & 1) == 0) {
      out[n / 2] = uint8_t(bestNib);
    } else {
      out[n / 2] |= uint8_t(bestNib << 4);
    }
  }
  return out;
}

}  // namespace skw
