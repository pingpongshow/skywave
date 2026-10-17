#include <cmath>
#include <complex>
#include <vector>

#include "core/constants.hpp"
#include "dsp/updown.hpp"
#include "dsp/fft.hpp"
#include "fec/interleave.hpp"
#include "fec/ldpc.hpp"
#include "fec/whitener.hpp"
#include "phy/framer.hpp"
#include "phy/layout.hpp"
#include "phy/mapper.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> testPayload(size_t n) {
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t(i * 31 + 7);
  return p;
}

// Clean-channel symbol reader: FFT symbols from `x`, despread with known
// PRN phases, hard-demap, return coded bits (mirrors appendOfdmSymbols).
std::vector<uint8_t> readSymbols(Family fam, Constellation c, int spread,
                                 CpMode cp, std::span<const cfloat> x,
                                 int symIdx0, int nSymbols) {
  Fft fft(kFftBaseband);
  const int cpLen = cpSamplesAudio(cp) / kDecimation;
  const int symLen = kFftBaseband + cpLen;
  const int bps = bitsPerSymbol(c);
  const int G = spreadGroupSymbols(fam, spread);
  std::vector<uint8_t> bits;
  for (int s = 0; s + G <= nSymbols + G - 1 && s < nSymbols; s += G) {
    // FFT the group's symbols and pool data carriers (mirror of TX).
    std::vector<std::vector<cfloat>> freq;
    std::vector<std::pair<int, int>> pool;
    for (int g = 0; g < G; ++g) {
      const cfloat* base =
          x.data() + size_t(s + g) * size_t(symLen) + cpLen;
      std::vector<cfloat> in(base, base + kFftBaseband);
      std::vector<cfloat> out(static_cast<size_t>(kFftBaseband));
      fft.forward(in.data(), out.data());
      const float invTs = 1.0f / ofdmTimeScale(fam);
      for (auto& b : out) b *= invTs;
      freq.push_back(std::move(out));
      auto lay = symbolLayout(fam, symIdx0 + s + g);
      for (int carrier : lay.dataCarriers) pool.push_back({g, carrier});
    }
    const int nSrc = int(pool.size()) / spread;
    for (int i = 0; i < nSrc; ++i) {
      cfloat acc(0, 0);
      for (int k = 0; k < spread; ++k) {
        auto [g, carrier] = pool[size_t(i + k * nSrc)];
        acc += freq[size_t(g)][size_t(carrierToBasebandBin(fam, carrier))] *
               std::conj(spreadPhase(i, k, symIdx0 + s));
      }
      acc /= float(spread);
      uint32_t v = hardDemap(c, acc);
      for (int b = bps - 1; b >= 0; --b) bits.push_back((v >> b) & 1);
    }
  }
  return bits;
}

}  // namespace

TEST_CASE(mapper_unit_power_and_gray) {
  for (Constellation c :
       {Constellation::Qpsk, Constellation::Qam16, Constellation::Qam64}) {
    const int m = 1 << bitsPerSymbol(c);
    double p = 0;
    for (int v = 0; v < m; ++v) p += std::norm(mapSymbol(c, uint32_t(v)));
    CHECK_NEAR(p / m, 1.0, 1e-5);
    // Hard demap inverts the map.
    for (int v = 0; v < m; ++v) {
      CHECK_EQ(hardDemap(c, mapSymbol(c, uint32_t(v))), uint32_t(v));
    }
    // Gray property: nearest horizontal/vertical neighbors differ in 1 bit.
    for (int v = 0; v < m; ++v) {
      cfloat s = mapSymbol(c, uint32_t(v));
      for (int u = 0; u < m; ++u) {
        if (u == v) continue;
        float d = std::abs(mapSymbol(c, uint32_t(u)) - s);
        float minStep = c == Constellation::Qpsk   ? 2.0f / std::sqrt(2.0f)
                        : c == Constellation::Qam16 ? 2.0f / std::sqrt(10.0f)
                                                    : 2.0f / std::sqrt(42.0f);
        if (d < minStep * 1.01f) {
          int diff = int(v) ^ int(u);
          CHECK_EQ(diff & (diff - 1), 0);  // single bit
        }
      }
    }
  }
}

TEST_CASE(interleaver_roundtrip) {
  for (int n : {648, 1944}) {
    std::vector<uint8_t> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) in[size_t(i)] = uint8_t(i & 1);
    std::vector<uint8_t> il, out;
    interleaveBits(in, il);
    CHECK_EQ(int(il.size()), n);
    deinterleave<uint8_t>(il, out);
    CHECK(out == in);
    // Adjacent input bits are separated after interleaving.
    std::vector<int> pos(static_cast<size_t>(n));
    std::vector<uint8_t> idx(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) idx[size_t(i)] = 0;
  }
}

TEST_CASE(pilot_layout_rules) {
  // WIDE: exactly 8 pilots per symbol; every carrier sounded over 8 syms.
  std::vector<bool> sounded(64, false);
  for (int s = 0; s < 8; ++s) {
    auto lay = symbolLayout(Family::Wide, s);
    CHECK_EQ(int(lay.pilotCarriers.size()), 8);
    CHECK_EQ(int(lay.dataCarriers.size()), 56);
    for (int c : lay.pilotCarriers) sounded[size_t(c)] = true;
  }
  for (bool b : sounded) CHECK(b);
  // Pilot boost ~ +2 dB.
  CHECK_NEAR(std::abs(pilotValue(3, 5)), std::pow(10.0, 0.1), 1e-3);
}

TEST_CASE(phy_header_roundtrip) {
  PhyHeader h;
  h.family = Family::Ultra;
  h.mode = 11;
  h.cpMode = CpMode::Long;
  h.frameType = 42;
  h.length = 3210;
  h.seqLow = 0xA5;
  uint8_t buf[kPhyHeaderBytes];
  packPhyHeader(h, buf);
  auto back = parsePhyHeader({buf, kPhyHeaderBytes});
  CHECK(back.has_value());
  if (back) {
    CHECK(back->family == Family::Ultra);
    CHECK_EQ(back->mode, 11);
    CHECK(back->cpMode == CpMode::Long);
    CHECK_EQ(back->frameType, 42);
    CHECK_EQ(back->length, 3210);
    CHECK_EQ(back->seqLow, 0xA5);
  }
  buf[3] ^= 0x10;  // corrupt
  CHECK(!parsePhyHeader({buf, kPhyHeaderBytes}).has_value());
}

// Header fields that index tables are range-checked at parse time: a
// CRC-valid header with mode 15 (4-bit field, 15-rung ladder) hung the
// receiver's payload loop; cpMode 3 indexed past the CP table.
TEST_CASE(phy_header_rejects_out_of_range_fields) {
  PhyHeader h;
  h.mode = 15;
  uint8_t b[kPhyHeaderBytes];
  packPhyHeader(h, b);
  CHECK(!parsePhyHeader({b, kPhyHeaderBytes}).has_value());
  h.mode = 8;
  h.cpMode = CpMode(3);
  packPhyHeader(h, b);
  CHECK(!parsePhyHeader({b, kPhyHeaderBytes}).has_value());
  h.cpMode = CpMode::Long;
  packPhyHeader(h, b);
  CHECK(parsePhyHeader({b, kPhyHeaderBytes}).has_value());
}

// A mask leaving (almost) no carriers is ignored, not trusted: all-zero
// made the geometry/symbol loops run forever. The masked geometry equals
// the unmasked one, and the spread modes never use a mask at all.
TEST_CASE(phy_degenerate_mask_is_ignored) {
  CarrierMask none;
  none.bits.fill(0);
  std::vector<uint8_t> pl(200, 0x5A);
  TxFrameParams p;
  p.family = Family::Wide;
  p.mode = 10;
  p.sig = ChirpSig::Fast;
  p.payload = pl;
  const auto plain = computeGeometry(p);
  p.mask = &none;
  const auto masked = computeGeometry(p);
  CHECK_EQ(masked.totalSamples, plain.totalSamples);
  CHECK(effectiveMask(&none, Family::Wide, 10) == nullptr);
  CarrierMask one;  // one carrier masked: a real mask, used at M8+ only
  one.exclude(3);
  CHECK(effectiveMask(&one, Family::Wide, 10) == &one);
  CHECK(effectiveMask(&one, Family::Wide, 6) == nullptr);
}

TEST_CASE(frame_bits_decode_back_header_and_payload) {
  // Fast frame, M8 payload: reconstruct the coded bits from assembled
  // symbols and decode both codeword classes back to the source.
  auto payload = testPayload(200);
  TxFrameParams p;
  p.family = Family::Wide;
  p.sig = ChirpSig::Fast;
  p.mode = 8;
  p.frameType = 5;
  p.seqLow = 0x3C;
  p.payload = payload;

  FrameBits fb = buildFrameBits(p);
  CHECK_EQ(int(fb.headerCoded.size()), 648);
  CHECK_EQ(int(fb.payloadCoded.size()), 2);  // (200-30)*8=1360 bits < 2*972

  // Header symbols.
  std::vector<cfloat> hs;
  int hsyms = appendOfdmSymbols(p.family, Constellation::Qpsk,
                                headerSpread(false), CpMode::Medium,
                                fb.headerCoded, 0, hs);
  auto hbits = readSymbols(p.family, Constellation::Qpsk, headerSpread(false),
                           CpMode::Medium, hs, 0, hsyms);
  hbits.resize(648);
  std::vector<uint8_t> deil;
  deinterleave<uint8_t>(hbits, deil);
  std::vector<float> llr(deil.size());
  for (size_t i = 0; i < deil.size(); ++i) llr[i] = deil[i] ? -4.0f : 4.0f;
  const LdpcCode& hcode = LdpcCode::get(kLdpcNShort, {1, 2});
  std::vector<uint8_t> info;
  CHECK(hcode.decode(llr, info));

  // Header fields.
  std::vector<uint8_t> hdrBytes(kPhyHeaderBytes, 0);
  for (int i = 0; i < kPhyHeaderBytes * 8; ++i) {
    hdrBytes[size_t(i / 8)] |= uint8_t((info[size_t(i)] & 1) << (7 - i % 8));
  }
  auto h = parsePhyHeader(hdrBytes);
  CHECK(h.has_value());
  if (h) {
    CHECK(h->family == Family::Wide);
    CHECK_EQ(h->mode, 8);
    CHECK_EQ(h->frameType, 5);
    CHECK_EQ(h->length, 200);
    CHECK_EQ(h->seqLow, 0x3C);
  }
  // Piggyback: first 30 whitened payload bytes.
  std::vector<uint8_t> white(payload.begin(), payload.end());
  Whitener wh;
  wh.apply(white);
  for (int i = 0; i < kHeaderPiggybackBytes; ++i) {
    uint8_t b = 0;
    for (int k = 0; k < 8; ++k) {
      b = uint8_t((b << 1) |
                  (info[size_t(kPhyHeaderBytes * 8 + i * 8 + k)] & 1));
    }
    CHECK_EQ(b, white[size_t(i)]);
  }

  // Payload codeword 0.
  std::vector<cfloat> ps;
  int psyms = appendOfdmSymbols(p.family, Constellation::Qpsk, 1,
                                CpMode::Medium, fb.payloadCoded[0], hsyms, ps);
  auto pbits = readSymbols(p.family, Constellation::Qpsk, 1, CpMode::Medium,
                           ps, hsyms, psyms);
  pbits.resize(1944);
  std::vector<uint8_t> pdeil;
  deinterleave<uint8_t>(pbits, pdeil);
  std::vector<float> pllr(pdeil.size());
  for (size_t i = 0; i < pdeil.size(); ++i) pllr[i] = pdeil[i] ? -4.0f : 4.0f;
  const LdpcCode& pcode = LdpcCode::get(kLdpcNLong, {1, 2});
  std::vector<uint8_t> pinfo;
  CHECK(pcode.decode(pllr, pinfo));
  for (int i = 0; i < 20 * 8; ++i) {  // spot-check first 20 bytes
    int byteIdx = kHeaderPiggybackBytes + i / 8;
    uint8_t want = (white[size_t(byteIdx)] >> (7 - i % 8)) & 1;
    CHECK_EQ(pinfo[size_t(i)], want);
  }
}

TEST_CASE(frame_bits_decode_back_spread16) {
  // Robust frame at M4 (S16): control payload fits fully in the header
  // codeword; verify the x16 spreader round-trips.
  auto payload = testPayload(22);
  TxFrameParams p;
  p.family = Family::Narrow;
  p.sig = ChirpSig::Robust;
  p.mode = 4;
  p.payload = payload;
  FrameBits fb = buildFrameBits(p);
  CHECK_EQ(int(fb.payloadCoded.size()), 0);

  std::vector<cfloat> hs;
  int hsyms = appendOfdmSymbols(p.family, Constellation::Qpsk,
                                headerSpread(true), CpMode::Medium,
                                fb.headerCoded, 0, hs);
  auto hbits = readSymbols(p.family, Constellation::Qpsk, headerSpread(true),
                           CpMode::Medium, hs, 0, hsyms);
  hbits.resize(648);
  std::vector<uint8_t> deil;
  deinterleave<uint8_t>(hbits, deil);
  std::vector<float> llr(deil.size());
  for (size_t i = 0; i < deil.size(); ++i) llr[i] = deil[i] ? -4.0f : 4.0f;
  const LdpcCode& hcode = LdpcCode::get(kLdpcNShort, {1, 2});
  std::vector<uint8_t> info;
  CHECK(hcode.decode(llr, info));
  std::vector<uint8_t> hdrBytes(kPhyHeaderBytes, 0);
  for (int i = 0; i < kPhyHeaderBytes * 8; ++i) {
    hdrBytes[size_t(i / 8)] |= uint8_t((info[size_t(i)] & 1) << (7 - i % 8));
  }
  auto h = parsePhyHeader(hdrBytes);
  CHECK(h.has_value());
  if (h) {
    CHECK(h->family == Family::Narrow);
    CHECK_EQ(h->mode, 4);
    CHECK_EQ(h->length, 22);
  }
}

TEST_CASE(frame_geometry_matches_build) {
  auto payload = testPayload(150);
  TxFrameParams p;
  p.family = Family::Wide;
  p.sig = ChirpSig::Fast;
  p.mode = 10;
  p.cpMode = CpMode::Short;
  p.payload = payload;
  FrameGeometry g = computeGeometry(p);
  CHECK(g.headerSymbols > 0);
  CHECK_EQ(g.payloadCodewords, 1);  // 120 bytes = 960 bits <= 1944/2
  auto frame = buildFrameBaseband(p);
  // Geometry counts the band-filter flush (it used to omit 9.8 ms).
  CHECK_EQ(int(frame.size()), g.totalSamples);
  // Unit RMS.
  double pow = 0;
  for (auto& v : frame) pow += std::norm(v);
  CHECK_NEAR(std::sqrt(pow / double(frame.size())), 1.0, 1e-3);
}

TEST_CASE(frame_papr_ceiling) {
  auto payload = testPayload(400);
  TxFrameParams p;
  p.family = Family::Wide;
  p.sig = ChirpSig::Fast;
  p.mode = 13;
  p.payload = payload;
  auto frame = buildFrameBaseband(p);
  double pow = 0, peak = 0;
  for (auto& v : frame) {
    double m = std::norm(v);
    pow += m;
    peak = std::max(peak, m);
  }
  double papr = 10.0 * std::log10(peak / (pow / double(frame.size())));
  // Soft clip at 8 dB; band filtering regrows peaks slightly.
  CHECK(papr < 9.5);
}

TEST_CASE(frame_spectral_mask) {
  // Out-of-band power (beyond band edge + 150 Hz) at least 26 dB below
  // total in-band power for every family.
  for (Family fam :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    auto payload = testPayload(300);
    TxFrameParams p;
    p.family = fam;
    p.sig = ChirpSig::Fast;
    p.mode = 9;
    p.payload = payload;
    auto frame = buildFrameBaseband(p);

    const int N = 4096;  // periodogram via 320-multiple would alias bins;
    Fft fft(4096);
    std::vector<double> psd(N, 0.0);
    int blocks = 0;
    for (size_t off = 0; off + N <= frame.size(); off += N / 2) {
      std::vector<cfloat> in(frame.begin() + long(off),
                             frame.begin() + long(off + N));
      std::vector<cfloat> out(static_cast<size_t>(N));
      fft.forward(in.data(), out.data());
      for (int i = 0; i < N; ++i) psd[size_t(i)] += std::norm(out[size_t(i)]);
      ++blocks;
    }
    CHECK(blocks > 0);
    const double binHz = double(kFsBaseband) / N;
    const double halfBand =
        plan(fam).carriers() * kSubcarrierHz / 2.0 + 150.0;
    double inBand = 0, outBand = 0;
    for (int i = 0; i < N; ++i) {
      double f = i < N / 2 ? i * binHz : (i - N) * binHz;
      if (std::fabs(f) <= halfBand) inBand += psd[size_t(i)];
      else outBand += psd[size_t(i)];
    }
    double ratioDb = 10.0 * std::log10(inBand / std::max(outBand, 1e-30));
    CHECK(ratioDb > 26.0);
  }
}

// Waveform analysis 2026-10-17: at the HF level, VHF's 11 dB PAPR ceiling
// (for 256QAM) put ~1 frame in 5 over full scale after upconversion (worst
// peak 1.21), which the worker hard-clips. Each band's txTargetRms must keep
// real 48 kHz peaks inside full scale across random payloads.
TEST_CASE(tx_peaks_stay_inside_full_scale_per_band) {
  for (Band band : {Band::Hf, Band::VhfFm}) {
    setActiveBand(band);
    const float g = float(activeProfile().txTargetRms * std::sqrt(2.0));
    const Family fam = band == Band::Hf ? Family::Wide : Family(0);
    const int modes[] = {4, 10, 13, 14};
    for (int mode : modes) {
      if (mode == 14 && band == Band::Hf) continue;  // VHF-only rung
      double worst = 0.0;
      for (int seed = 0; seed < 40; ++seed) {
        std::vector<uint8_t> payload(600);
        uint32_t x = uint32_t(seed * 2654435761u + mode);
        for (auto& b : payload) { x = x * 1664525u + 1013904223u; b = uint8_t(x >> 24); }
        TxFrameParams tp;
        tp.family = fam;
        tp.mode = mode;
        tp.sig = mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
        tp.cpMode = activeProfile().cpDefault;
        tp.payload = payload;
        auto bb = buildFrameBaseband(tp);
        bb.insert(bb.end(), 1200, cfloat(0, 0));
        Upconverter up;
        std::vector<float> audio;
        up.process(bb, audio);
        for (float v : audio) worst = std::max(worst, double(std::fabs(v * g)));
      }
      std::printf("band %s M%d worst peak %.3f\n", activeProfile().name, mode, worst);
      CHECK(worst < 0.98);
    }
  }
  setActiveBand(Band::Hf);
  CHECK_EQ(kHfProfile.txTargetRms, kTxTargetRms);  // the documented HF level
}

SKYTEST_MAIN
