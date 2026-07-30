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
  // Chip rate = family occupied bandwidth, snapped so kFsBaseband is an
  // exact integer number of samples per chip (the dechirp needs integer
  // chip spacing to land energy in one FFT bin).
  const double bw = plan(f).carriers() * kSubcarrierHz;
  int spc = std::max(2, int(std::lround(double(kFsBaseband) / bw)));
  return {double(kFsBaseband) / spc, spc};
}

size_t cssSymbolSamples(Family f, int sf) {
  return size_t(1u << sf) * size_t(cssPlan(f).samplesPerChip);
}

namespace {
// Base up-chirp at the full sample rate, complex baseband centered on the
// waveform center. Phase is the PROPER integral of a linear frequency
// sweep (fStart .. fStart+B over the symbol), so a cyclic time shift maps
// cleanly to a single dechirp-FFT bin.
std::vector<cfloat> baseChirp(const CssPlan& p, int N) {
  const int spc = p.samplesPerChip;
  const size_t symLen = size_t(N) * spc;
  const double fStart = kCenterHz - p.bandHz / 2.0;
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
  // base[n + v*spc]) peaks at lag d = -v*spc, i.e. symLen - v*spc.
  int best = 0;
  double bestMag = 0.0, sumMag = 0.0;
  for (int v = 0; v < N; ++v) {
    const size_t lag = v == 0 ? 0 : symLen - size_t(v) * size_t(spc);
    const double m = std::abs(corr[lag]);
    sumMag += m;
    if (m > bestMag) {
      bestMag = m;
      best = v;
    }
  }
  if (peakRatio) {
    const double mean = sumMag / N;
    *peakRatio = mean > 1e-12 ? float(bestMag / mean) : 0.0f;
  }
  return uint16_t(best);
}

size_t cssSymbolsForBytes(size_t byteCount, int sf) {
  // 2 nibbles/byte, one Hamming codeword per nibble, interleaved 7 deep;
  // each symbol carries sf bits. Round the codeword count up to a
  // multiple of 7, then to whole symbols.
  const size_t nibbles = byteCount * 2;
  const size_t codewords = ((nibbles + 6) / 7) * 7;  // pad to interleave
  const size_t bits = codewords * 7;
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
  while (cw.size() % 7) cw.push_back(hamming74Encode(0));  // interleave pad
  // Block interleave depth 7: bit i of codeword j goes to stream position
  // i*depth + j, so a lost symbol (a run of bits) hits each codeword once.
  const size_t blocks = cw.size() / 7;
  std::vector<uint8_t> bits;
  bits.reserve(cw.size() * 7);
  for (size_t blk = 0; blk < blocks; ++blk) {
    for (int i = 0; i < 7; ++i) {
      for (int j = 0; j < 7; ++j) {
        bits.push_back((cw[blk * 7 + size_t(j)] >> i) & 1);
      }
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
  const size_t codewords = ((nibbles + 6) / 7) * 7;
  const size_t blocks = codewords / 7;
  std::vector<uint8_t> cw(codewords, 0);
  // De-interleave (inverse of the depth-7 block map).
  size_t bi = 0;
  for (size_t blk = 0; blk < blocks; ++blk) {
    for (int i = 0; i < 7; ++i) {
      for (int j = 0; j < 7; ++j) {
        if (bi < bits.size()) {
          cw[blk * 7 + size_t(j)] |= uint8_t(bits[bi] << i);
        }
        ++bi;
      }
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

std::vector<cfloat> cssModulatePayload(Family f, int mode,
                                       std::span<const uint8_t> bytes) {
  const int sf = cssSf(mode);
  return cssModulate(f, sf, cssEncode(bytes, sf));
}

std::vector<uint8_t> cssDemodPayload(Family f, int mode,
                                     std::span<const cfloat> section,
                                     size_t byteCount) {
  const int sf = cssSf(mode);
  const size_t symLen = cssSymbolSamples(f, sf);
  const size_t nsym = section.size() / symLen;
  std::vector<uint16_t> syms(nsym);
  for (size_t s = 0; s < nsym; ++s) {
    syms[s] = cssDemodSymbol(
        f, sf, section.subspan(s * symLen, symLen), nullptr);
  }
  return cssDecode(syms, sf, byteCount);
}

}  // namespace skw
