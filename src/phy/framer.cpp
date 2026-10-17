#include "phy/framer.hpp"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "dsp/css.hpp"
#include "dsp/fft.hpp"
#include "dsp/fir.hpp"
#include "dsp/zc.hpp"
#include "fec/crc.hpp"
#include "fec/interleave.hpp"
#include "fec/ldpc.hpp"
#include "fec/whitener.hpp"
#include "phy/layout.hpp"
#include "phy/mapper.hpp"

namespace skw {
namespace {

// MSB-first bit packing helpers.
struct BitWriter {
  std::vector<uint8_t> bytes;
  int bitPos = 0;
  void put(uint32_t v, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
      if (bitPos % 8 == 0) bytes.push_back(0);
      if ((v >> i) & 1) bytes.back() |= uint8_t(0x80 >> (bitPos % 8));
      ++bitPos;
    }
  }
};
struct BitReader {
  std::span<const uint8_t> bytes;
  int bitPos = 0;
  uint32_t get(int nbits) {
    uint32_t v = 0;
    for (int i = 0; i < nbits; ++i) {
      size_t byte = size_t(bitPos / 8);
      int bit = bitPos % 8;
      v = (v << 1) | ((bytes[byte] >> (7 - bit)) & 1);
      ++bitPos;
    }
    return v;
  }
};

std::vector<uint8_t> bytesToBits(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> bits;
  bits.reserve(bytes.size() * 8);
  for (uint8_t b : bytes) {
    for (int i = 7; i >= 0; --i) bits.push_back((b >> i) & 1);
  }
  return bits;
}

std::vector<float> familyTxBandTaps(Family family) {
  // Guard between the outermost carrier and the filter's passband edge.
  // A Kaiser response is not flat right up to its edge, so a carrier
  // sitting in the knee is attenuated and phase-distorted - and ULTRA's
  // outermost carriers sit closest to the edge of the rig passband the
  // family exists to fill. Overridable for measurement.
  double margin = 40.0;
  if (const char* e = getenv("SKW_TXBAND_MARGIN_HZ")) {
    const double v = std::atof(e);
    if (v > 0.0) margin = v;
  }
  const double half = plan(family).carriers() * kSubcarrierHz / 2.0 + margin;
  return designKaiserLowpass(kFsBaseband, half, half + 300.0, 50.0);
}

// Samples the occupied-band filter flush appends after the last symbol.
int txBandTailSamples(Family family) {
  return int(familyTxBandTaps(family).size() / 2) * 2;
}

}  // namespace

void packPhyHeader(const PhyHeader& h, uint8_t out[kPhyHeaderBytes]) {
  BitWriter w;
  w.put(kPhyMagic, 8);
  w.put(h.version, 4);
  w.put(uint32_t(h.waveform), 4);
  w.put(uint32_t(h.family), 2);
  w.put(h.mode, 4);
  w.put(uint32_t(h.cpMode), 2);
  w.put(h.frameType, 6);
  w.put(h.length, 12);
  w.put(h.seqLow, 8);
  w.put(h.cssHeaderCopies == 1 ? 1u : 0u, 1);  // CSS single-header layout
  w.put(h.poll ? 1u : 0u, 1);
  w.put(0, 4);  // pad to 56 bits = 7 bytes
  uint16_t crc = crc16({w.bytes.data(), w.bytes.size()});
  w.put(crc, 16);
  std::copy(w.bytes.begin(), w.bytes.end(), out);
}

std::optional<PhyHeader> parsePhyHeader(std::span<const uint8_t> bytes) {
  if (bytes.size() < kPhyHeaderBytes) return std::nullopt;
  uint16_t want = crc16(bytes.subspan(0, 7));
  uint16_t got = uint16_t((bytes[7] << 8) | bytes[8]);
  if (want != got) return std::nullopt;
  BitReader r{bytes.subspan(0, 7)};
  if (r.get(8) != kPhyMagic) return std::nullopt;
  PhyHeader h;
  h.version = uint8_t(r.get(4));
  if (h.version != kWireVersion) return std::nullopt;
  h.waveform = Waveform(r.get(4));
  h.family = Family(r.get(2));
  h.mode = uint8_t(r.get(4));
  h.cpMode = CpMode(r.get(2));
  h.frameType = uint8_t(r.get(6));
  h.length = uint16_t(r.get(12));
  h.seqLow = uint8_t(r.get(8));
  h.cssHeaderCopies = r.get(1) ? 1 : 2;
  h.poll = r.get(1) != 0;
  // A CRC-16 pass is no guarantee of sane fields (any station can send a
  // well-formed header, and 1 in 65536 noise headers passes): every field
  // that indexes a table is range-checked here, once, for all callers.
  // mode is 4 bits but the ladder has kNumModes rungs (mode 15 read past
  // kModes and hung the payload loop on a zero code rate); cpMode is 2
  // bits over 3 CP modes.
  if (h.mode >= kNumModes || int(h.cpMode) > int(CpMode::Long) ||
      (h.waveform != Waveform::Ofdm && h.waveform != Waveform::Css)) {
    return std::nullopt;
  }
  return h;
}

FrameBits buildFrameBits(const TxFrameParams& p) {
  if (p.payload.size() > 4095) throw std::invalid_argument("frame too large");
  FrameBits fb;

  // Whiten a copy of the payload (fresh whitener per frame).
  std::vector<uint8_t> white(p.payload.begin(), p.payload.end());
  Whitener wh;
  wh.apply(white);

  PhyHeader h;
  h.family = p.family;
  h.mode = uint8_t(p.mode);
  h.cpMode = p.cpMode;
  h.frameType = p.frameType;
  h.length = uint16_t(p.payload.size());
  h.seqLow = p.seqLow;
  h.poll = p.poll;
  uint8_t hdr[kPhyHeaderBytes];
  packPhyHeader(h, hdr);

  // Header codeword: 9 header bytes + up to 30 piggyback payload bytes,
  // zero-padded to the N=648 r1/2 info size (324 bits).
  const LdpcCode& hcode = LdpcCode::get(kLdpcNShort, {1, 2});
  const size_t piggy = std::min(white.size(), size_t(kHeaderPiggybackBytes));
  std::vector<uint8_t> hinfo(size_t(hcode.k()), 0);
  {
    auto hb = bytesToBits({hdr, kPhyHeaderBytes});
    auto pb = bytesToBits({white.data(), piggy});
    std::copy(hb.begin(), hb.end(), hinfo.begin());
    std::copy(pb.begin(), pb.end(), hinfo.begin() + long(hb.size()));
  }
  std::vector<uint8_t> coded;
  hcode.encode(hinfo, coded);
  interleaveBits(coded, fb.headerCoded);

  // Remaining payload -> codewords at the payload mode's rate, sized by
  // payloadCodewordN, zero-padded (not true-shortened; simplicity over the
  // last few % of efficiency - revisit with HARQ-IR in 3.4).
  const ModeDef& md = kModes[size_t(p.mode)];
  std::vector<uint8_t> rest(white.begin() + long(piggy), white.end());
  auto restBits = bytesToBits(rest);
  size_t off = 0;
  while (off < restBits.size()) {
    const size_t remaining = restBits.size() - off;
    const LdpcCode& pcode =
        LdpcCode::get(payloadCodewordN(remaining, md.rate), md.rate);
    std::vector<uint8_t> info(size_t(pcode.k()), 0);
    size_t nCopy = std::min(remaining, size_t(pcode.k()));
    std::copy(restBits.begin() + long(off),
              restBits.begin() + long(off + nCopy), info.begin());
    std::vector<uint8_t> cw, il;
    pcode.encode(info, cw);
    interleaveBits(cw, il);
    fb.payloadCoded.push_back(std::move(il));
    off += nCopy;
  }
  return fb;
}

int appendOfdmSymbols(Family family, Constellation constellation, int spread,
                      CpMode cp, std::span<const uint8_t> codedBits,
                      int symIdx0, std::vector<cfloat>& out,
                      const CarrierMask* mask, bool leftoverCopies) {
  const int bps = bitsPerSymbol(constellation);
  Fft fft(kFftBaseband);
  const int cpLen = cpSamplesAudio(cp) / kDecimation;
  const int G = spreadGroupSymbols(family, spread, mask);

  size_t bitPos = 0;
  int symIdx = symIdx0;
  int emitted = 0;
  while (bitPos < codedBits.size()) {
    // Pool the group's data carriers: (symbol offset in group, carrier).
    std::vector<std::pair<int, int>> pool;
    std::vector<std::vector<cfloat>> freq(
        size_t(G),
        std::vector<cfloat>(size_t(kFftBaseband), cfloat(0, 0)));
    for (int g = 0; g < G; ++g) {
      auto lay = symbolLayout(family, symIdx + g, mask);
      for (int c : lay.pilotCarriers) {
        freq[size_t(g)][size_t(carrierToBasebandBin(family, c))] =
            pilotValue(c, symIdx + g);
      }
      for (int c : lay.dataCarriers) pool.push_back({g, c});
    }
    const int nSrc = int(pool.size()) / spread;
    std::vector<cfloat> srcVal;
    srcVal.reserve(size_t(nSrc));

    for (int i = 0; i < nSrc; ++i) {
      uint32_t v = 0;
      for (int b = 0; b < bps; ++b) {
        uint8_t bit = bitPos < codedBits.size() ? codedBits[bitPos] & 1 : 0;
        ++bitPos;
        v = (v << 1) | bit;
      }
      cfloat s = mapSymbol(constellation, v);
      for (int k = 0; k < spread; ++k) {
        auto [g, c] = pool[size_t(i + k * nSrc)];
        freq[size_t(g)][size_t(carrierToBasebandBin(family, c))] =
            s * spreadPhase(i, k, symIdx);
      }
      srcVal.push_back(s);
    }
    // Leftover cells: extra copies (layout.hpp leftoverCopiesEnabled).
    if (nSrc > 0 && leftoverCopies && leftoverCopiesForSpread(spread)) {
      const int rem = int(pool.size()) - nSrc * spread;
      for (int j = 0; j < rem; ++j) {
        const int i = j % nSrc;
        auto [g, c] = pool[size_t(nSrc * spread + j)];
        freq[size_t(g)][size_t(carrierToBasebandBin(family, c))] =
            srcVal[size_t(i)] * spreadPhase(i, spread + j / nSrc, symIdx);
      }
    }
    const float ts = ofdmTimeScale(family);
    for (int g = 0; g < G; ++g) {
      std::vector<cfloat> core(static_cast<size_t>(kFftBaseband));
      fft.inverse(freq[size_t(g)].data(), core.data());
      for (auto& v : core) v *= ts;
      out.insert(out.end(), core.end() - cpLen, core.end());
      out.insert(out.end(), core.begin(), core.end());
    }
    symIdx += G;
    emitted += G;
  }
  return emitted;
}

FrameGeometry computeGeometry(const TxFrameParams& p) {
  if (kModes[size_t(p.mode)].waveform == Waveform::Css) {
    FrameGeometry g;
    const int sf = cssSf(p.mode);
    const size_t symH = cssSymbolSamples(p.family, kCssHeaderSf);
    const size_t symP = cssSymbolSamples(p.family, sf);
    auto pilots = [](size_t n) {
      return (n + size_t(kCssPilotInterval) - 1) / kCssPilotInterval;
    };
    const size_t hSyms = cssSymbolsForBytes(kPhyHeaderBytes, kCssHeaderSf);
    const size_t pSyms =
        (cssPayloadCodedBits(p.payload.size()) + size_t(sf) - 1) /
        size_t(sf);
    g.preambleSamples =
        int(chirpPattern(p.family, ChirpSig::Css).size()) * chirpRepSamples();
    g.zcSymbols = 0;  // CSS frames sync on their own known symbols
    const size_t copies = p.cssHeaderCopies == 1 ? 1 : 2;
    const size_t hAll = copies * (hSyms + pilots(hSyms));
    g.headerSymbols = int(kCssSyncSymbols + hAll);
    g.payloadSymbols = int(pSyms + pilots(pSyms));
    g.payloadCodewords = 0;  // Hamming, not LDPC
    g.totalSamples =
        g.preambleSamples +
        int(symH) * int(kCssSyncSymbols + hAll + pilots(pSyms)) +
        int(symP) * int(pSyms) + txBandTailSamples(p.family);
    return g;
  }
  FrameGeometry g;
  auto pre = makeChirpPreamble(p.family, p.sig);
  g.preambleSamples = int(pre.size());
  g.zcSymbols = zcSymbols(robustHeaderForSig(p.sig));

  const bool robust = robustHeaderForSig(p.sig);
  const int hSpread = headerSpread(robust);

  // Header symbols: walk group capacities (QPSK, MEDIUM CP).
  const int hG = spreadGroupSymbols(p.family, hSpread);
  int bits = 648;
  int sym = 0;
  while (bits > 0) {
    bits -= sourceSymbolsPerGroup(p.family, sym, hSpread) * 2;
    sym += hG;
  }
  g.headerSymbols = sym;

  const ModeDef& md = kModes[size_t(p.mode)];
  const size_t piggy =
      std::min(p.payload.size(), size_t(kHeaderPiggybackBytes));
  size_t remaining = (p.payload.size() - piggy) * 8;
  // Payload symbols: ONE continuous coded bit stream across codewords
  // (mirrors buildFrameBaseband).
  long totalCoded = 0;
  while (remaining > 0) {
    const LdpcCode& pcode =
        LdpcCode::get(payloadCodewordN(remaining, md.rate), md.rate);
    ++g.payloadCodewords;
    remaining -= std::min(remaining, size_t(pcode.k()));
    totalCoded += pcode.n();
  }
  const CarrierMask* pmask = effectiveMask(p.mask, p.family, p.mode);
  const int pG = spreadGroupSymbols(p.family, md.spread, pmask);
  int psym = 0;
  while (totalCoded > 0) {
    totalCoded -= sourceSymbolsPerGroup(p.family, g.headerSymbols + psym,
                                        md.spread, pmask) *
                  bitsPerSymbol(md.constellation);
    psym += pG;
  }
  g.payloadSymbols = psym;

  const int hdrSymSamples =
      kFftBaseband + cpSamplesAudio(CpMode::Medium) / kDecimation;
  const int paySymSamples =
      kFftBaseband + cpSamplesAudio(p.cpMode) / kDecimation;
  g.totalSamples = g.preambleSamples + g.zcSymbols * zcSymbolSamples() +
                   g.headerSymbols * hdrSymSamples +
                   g.payloadSymbols * paySymSamples +
                   txBandTailSamples(p.family);
  return g;
}

namespace {

// CSS frame: preamble + known sync symbols + SF8 header + SF(mode)
// payload (+CRC-16). Constant-envelope, so no PAPR clip; the family band
// filter still smooths the cyclic-shift wrap discontinuities.
std::vector<cfloat> buildCssFrameBaseband(const TxFrameParams& p) {
  if (p.payload.size() > 4095) throw std::invalid_argument("frame too large");
  const int sf = cssSf(p.mode);

  std::vector<uint8_t> white(p.payload.begin(), p.payload.end());
  Whitener wh;
  wh.apply(white);
  const uint16_t crc = crc16(white);
  white.push_back(uint8_t(crc >> 8));
  white.push_back(uint8_t(crc & 0xFF));

  PhyHeader h;
  h.waveform = Waveform::Css;
  h.family = p.family;
  h.mode = uint8_t(p.mode);
  h.cpMode = p.cpMode;  // carried but unused by CSS transport
  h.frameType = p.frameType;
  h.length = uint16_t(p.payload.size());
  h.seqLow = p.seqLow;
  h.cssHeaderCopies = p.cssHeaderCopies == 1 ? 1 : 2;
  h.poll = p.poll;
  uint8_t hdr[kPhyHeaderBytes];
  packPhyHeader(h, hdr);

  std::vector<cfloat> frame;
  auto pre = makeChirpPreamble(p.family, ChirpSig::Css);
  frame.insert(frame.end(), pre.begin(), pre.end());
  const uint16_t syncVals[kCssSyncSymbols] = {0, 0};
  auto sync = cssModulate(p.family, kCssHeaderSf, syncVals);
  frame.insert(frame.end(), sync.begin(), sync.end());

  // Sections carry a known SF8 pilot before every kCssPilotInterval data
  // symbols (timing re-anchor + tap re-estimation at the receiver).
  const auto pilot = cssBaseChirp(p.family, kCssHeaderSf);
  auto appendSymbols = [&](int sectionSf,
                           std::span<const uint16_t> symVals) {
    auto data = cssModulate(p.family, sectionSf, symVals);
    const size_t symLen = cssSymbolSamples(p.family, sectionSf);
    for (size_t s = 0; s < symVals.size(); ++s) {
      if (s % size_t(kCssPilotInterval) == 0) {
        frame.insert(frame.end(), pilot.begin(), pilot.end());
      }
      frame.insert(frame.end(), data.begin() + long(s * symLen),
                   data.begin() + long((s + 1) * symLen));
    }
  };
  // Header: Hamming bit layer, duplicated for time diversity (or sent
  // once on the capability-gated connected-session short layout - the
  // second SF8 copy is ~1.9 s on WIDE and ~9.6 s on NARROW per frame).
  const auto hdrSyms =
      cssEncode({hdr, size_t(kPhyHeaderBytes)}, kCssHeaderSf);
  for (int c = 0; c < int(h.cssHeaderCopies); ++c) {
    appendSymbols(kCssHeaderSf, hdrSyms);
  }
  // Payload: LDPC r1/2 (N=648 only) coded bits over soft symbols.
  {
    auto restBits = bytesToBits(white);
    std::vector<uint8_t> coded;
    coded.reserve(cssPayloadCodedBits(p.payload.size()));
    const LdpcCode& pcode = LdpcCode::get(kLdpcNShort, {1, 2});
    size_t off = 0;
    while (off < restBits.size()) {
      const size_t remaining = restBits.size() - off;
      std::vector<uint8_t> info(size_t(pcode.k()), 0);
      const size_t nCopy = std::min(remaining, size_t(pcode.k()));
      std::copy(restBits.begin() + long(off),
                restBits.begin() + long(off + nCopy), info.begin());
      std::vector<uint8_t> cw, il;
      pcode.encode(info, cw);
      interleaveBits(cw, il);
      coded.insert(coded.end(), il.begin(), il.end());
      off += nCopy;
    }
    appendSymbols(sf, cssMapCodedBits(coded, sf));
  }

  FirDecimator bandFilt(familyTxBandTaps(p.family), 1);
  std::vector<cfloat> filtered;
  filtered.reserve(frame.size());
  bandFilt.process(frame, filtered);
  std::vector<cfloat> zeros(size_t(txBandTailSamples(p.family)), cfloat(0, 0));
  bandFilt.process(zeros, filtered);

  double pow = 0;
  for (const auto& v : filtered) pow += std::norm(v);
  double rms = std::sqrt(pow / double(filtered.size()));
  if (rms > 1e-12) {
    float g = float(1.0 / rms);
    for (auto& v : filtered) v *= g;
  }
  return filtered;
}

}  // namespace

std::vector<cfloat> buildFrameBaseband(const TxFrameParams& p) {
  if (kModes[size_t(p.mode)].waveform == Waveform::Css) {
    return buildCssFrameBaseband(p);
  }
  FrameBits fb = buildFrameBits(p);
  const bool robust = robustHeaderForSig(p.sig);
  const ModeDef& md = kModes[size_t(p.mode)];

  std::vector<cfloat> frame;
  auto pre = makeChirpPreamble(p.family, p.sig);
  frame.insert(frame.end(), pre.begin(), pre.end());

  // ZC block and chirp are unit-RMS; OFDM symbols are scaled to unit RMS
  // in appendOfdmSymbols (ofdmTimeScale), so the frame is level-uniform
  // and the ZC-seeded channel estimate matches the pilot convention.
  const int zcCount = zcSymbols(robust);
  auto zc = makeZcBlock(p.family, zcCount);
  frame.insert(frame.end(), zc.begin(), zc.end());

  int sym = appendOfdmSymbols(p.family, Constellation::Qpsk,
                              headerSpread(robust), CpMode::Medium,
                              fb.headerCoded, 0, frame);
  // Payload codewords ride ONE continuous bit stream (no per-codeword
  // symbol alignment): at 176 carriers a 1944-bit codeword spans 1.58
  // symbols and per-codeword rounding wasted ~21% of the payload air.
  {
    std::vector<uint8_t> allBits;
    for (const auto& cw : fb.payloadCoded) {
      allBits.insert(allBits.end(), cw.begin(), cw.end());
    }
    if (!allBits.empty()) {
      sym += appendOfdmSymbols(p.family, md.constellation, md.spread,
                               p.cpMode, allBits, sym, frame,
                               effectiveMask(p.mask, p.family, p.mode),
                               leftoverCopiesEnabled());
    }
  }

  // PAPR soft clip at +8 dB over current RMS.
  double pow = 0;
  for (const auto& v : frame) pow += std::norm(v);
  double rms = std::sqrt(pow / double(frame.size()));
  const float ceiling = float(rms * std::pow(10.0, activeProfile().txPaprCeilingDb / 20.0));
  for (auto& v : frame) {
    float mag = std::abs(v);
    if (mag > ceiling) v *= ceiling / mag;
  }

  // Occupied-band filter, then normalize to unit RMS.
  FirDecimator bandFilt(familyTxBandTaps(p.family), 1);
  std::vector<cfloat> filtered;
  filtered.reserve(frame.size());
  bandFilt.process(frame, filtered);
  // Flush the filter tail so the frame end is not truncated.
  std::vector<cfloat> zeros(size_t(txBandTailSamples(p.family)), cfloat(0, 0));
  bandFilt.process(zeros, filtered);

  pow = 0;
  for (const auto& v : filtered) pow += std::norm(v);
  rms = std::sqrt(pow / double(filtered.size()));
  if (rms > 1e-12) {
    float g = float(1.0 / rms);
    for (auto& v : filtered) v *= g;
  }
  return filtered;
}

}  // namespace skw
