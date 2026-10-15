// CSS deep tier (ROADMAP 6.1): symbol round-trips and the gray/Hamming/
// interleave bit layer (the PHY header's transport). Channel FER, CFO and
// floors are full-frame tests in test_css_phy (the payload-only modem-
// layer tests went with that legacy API, 2026-10-14).
#include <cstdio>
#include <vector>

#include "dsp/css.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> noiseBytes(size_t n, uint64_t seed) {
  std::vector<uint8_t> v(n);
  uint64_t s = seed * 2654435761u + 1;
  for (auto& b : v) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    b = uint8_t(s >> 32);
  }
  return v;
}

}  // namespace

TEST_CASE(css_plan_geometry) {
  for (Family f :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    const CssPlan p = cssPlan(f);
    CHECK(p.samplesPerChip >= 2);
    // Band snapped so chips are an exact integer number of samples.
    CHECK_NEAR(p.bandHz * p.samplesPerChip, double(kFsBaseband), 1e-9);
    for (int sf = 5; sf <= 8; ++sf) {
      CHECK_EQ(cssSymbolSamples(f, sf),
               size_t(1u << sf) * size_t(p.samplesPerChip));
    }
  }
  // SF lives in kModes.spread: M0 deepest.
  CHECK_EQ(cssSf(0), 8);
  CHECK_EQ(cssSf(1), 7);
  CHECK_EQ(cssSf(2), 6);
  CHECK_EQ(cssSf(3), 5);
  CHECK_EQ(cssSf(4), 0);
}

// --- symbol layer ------------------------------------------------------------

TEST_CASE(css_symbol_roundtrip_all_sf_families) {
  for (Family f :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    for (int sf = 5; sf <= 8; ++sf) {
      const int N = 1 << sf;
      std::vector<uint16_t> syms = {0, 1, uint16_t(N / 2), uint16_t(N - 1)};
      for (int k = 0; k < 8; ++k) syms.push_back(uint16_t((k * 37 + 5) % N));
      const auto bb = cssModulate(f, sf, syms);
      const size_t symLen = cssSymbolSamples(f, sf);
      CHECK_EQ(bb.size(), syms.size() * symLen);
      for (size_t s = 0; s < syms.size(); ++s) {
        float ratio = 0.0f;
        const uint16_t got = cssDemodSymbol(
            f, sf, std::span<const cfloat>(bb).subspan(s * symLen, symLen),
            &ratio);
        CHECK_EQ(got, syms[s]);
        // Clean channel: one decisive matched-filter lag. The mean picks
        // up leakage from the N-1 near-zero lags, so the ratio scales
        // with N (~0.5*N empirically); N/3 is decisive with margin.
        CHECK(ratio > float(N) / 3.0f);
      }
    }
  }
}

// --- bit layer: gray + Hamming + interleave ----------------------------------

TEST_CASE(css_bitlayer_roundtrip) {
  for (int sf = 5; sf <= 8; ++sf) {
    const auto bytes = noiseBytes(19, uint64_t(77 + sf));
    const auto syms = cssEncode(bytes, sf);
    CHECK_EQ(syms.size(), cssSymbolsForBytes(bytes.size(), sf));
    for (uint16_t s : syms) CHECK(s < (1u << sf));
    CHECK(cssDecode(syms, sf, bytes.size()) == bytes);
  }
}

TEST_CASE(css_adjacent_bin_error_corrected) {
  // An off-by-one demod bin is one data bit (gray), and Hamming(7,4)
  // corrects one bit per codeword - at every SF, both bin neighbors.
  for (int sf = 5; sf <= 8; ++sf) {
    const int N = 1 << sf;
    const auto bytes = noiseBytes(16, uint64_t(5 + sf));
    const auto syms = cssEncode(bytes, sf);
    for (int dir : {+1, -1}) {
      auto bad = syms;
      bad[2] = uint16_t((int(bad[2]) + dir + N) % N);
      CHECK(cssDecode(bad, sf, bytes.size()) == bytes);
    }
  }
}

TEST_CASE(css_whole_symbol_loss_survives) {
  // The depth-7 interleaver spreads a symbol's sf consecutive bits across
  // distinct codewords (sf <= 7), so even a completely wrong symbol costs
  // each codeword at most one bit - which Hamming corrects.
  for (int sf = 5; sf <= 7; ++sf) {
    const int N = 1 << sf;
    const auto bytes = noiseBytes(16, uint64_t(31 + sf));
    const auto syms = cssEncode(bytes, sf);
    for (size_t victim : {size_t(0), size_t(3), syms.size() / 2}) {
      auto bad = syms;
      bad[victim] = uint16_t((int(bad[victim]) + N / 2 + 1) % N);
      CHECK(cssDecode(bad, sf, bytes.size()) == bytes);
    }
  }
}

SKYTEST_MAIN
