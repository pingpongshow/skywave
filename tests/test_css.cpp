// CSS deep tier (ROADMAP 6.1): symbol round-trips, gray/Hamming/interleave
// properties, CFO tolerance of the non-coherent demod, and the FER-vs-SNR
// gate down to the tier design floor (WAVEFORM.md: floors -9 -> -14 dB).
#include <cstdio>
#include <vector>

#include "dsp/css.hpp"
#include "fec/crc.hpp"
#include "sim/chansim.hpp"
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

// One CSS frame trial: payload + CRC-32 through modulate -> channel ->
// demodulate; success = CRC clean.
bool cssTrial(Family fam, int mode, double snr3kDb, uint64_t seed,
              size_t payloadBytes = 24, double cfoHz = 0.0) {
  auto payload = noiseBytes(payloadBytes, seed);
  const uint32_t crc = crc32(payload);
  for (int i = 0; i < 4; ++i) payload.push_back(uint8_t(crc >> (8 * i)));
  auto tx = cssModulatePayload(fam, mode, payload);
  ChansimParams cp;
  cp.fading = false;
  cp.snr3kDb = snr3kDb;
  cp.cfoHz = cfoHz;
  cp.seed = seed * 31 + 7;
  auto rx = Chansim::run(cp, tx);
  rx.resize(tx.size());  // symbols are aligned from sample 0
  auto got = cssDemodPayload(fam, mode, rx, payload.size());
  if (got.size() != payload.size()) return false;
  uint32_t wantCrc = 0;
  for (int i = 0; i < 4; ++i) {
    wantCrc |= uint32_t(got[got.size() - 4 + size_t(i)]) << (8 * i);
  }
  got.resize(got.size() - 4);
  return crc32(got) == wantCrc;
}

}  // namespace

// --- geometry ----------------------------------------------------------------

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

// --- payload layer over the channel ------------------------------------------

TEST_CASE(css_payload_clean_channel_all_modes_families) {
  for (Family f :
       {Family::Wide, Family::Mid, Family::Narrow, Family::Ultra}) {
    for (int mode = 0; mode <= 3; ++mode) {
      CHECK(cssTrial(f, mode, 40.0, uint64_t(900 + mode)));
    }
  }
}

TEST_CASE(css_cfo_tolerance) {
  // Non-coherent dechirp: a CFO shifts the post-FFT tone by cfo/binHz
  // bins. WIDE SF8 bins are ~9.4 Hz apart; the connected-preamble capture
  // spec (kCfoCaptureConnHz = 5 Hz) must decode after gray+Hamming absorb
  // the occasional adjacent-bin hit. Test at moderate SNR, both signs.
  for (double cfo : {-kCfoCaptureConnHz / 2.0, 2.0, kCfoCaptureConnHz / 2.0}) {
    CHECK(cssTrial(Family::Wide, 0, 0.0, 4242, 24, cfo));
  }
  // SF5 bins are 8x wider; full capture-range CFO is harmless.
  CHECK(cssTrial(Family::Wide, 3, 5.0, 777, 24, kCfoCaptureConnHz));
}

// --- ROADMAP 6.1 gate: FER vs SNR down to -14 dB -----------------------------

TEST_CASE(css_fer_gate_floors) {
  // Every (family, mode) at its design floor: FER <= ~15% by definition
  // of the floor, gated at 32% for 25-trial binomial + seed slack. The
  // full curves are bertool --css; this pins the floor table.
  const int kTrials = 25;
  const Family fams[] = {Family::Wide, Family::Mid, Family::Narrow,
                         Family::Ultra};
  std::printf("  CSS FER at design floors (AWGN, %d trials)\n", kTrials);
  for (Family f : fams) {
    for (int mode = 0; mode <= 3; ++mode) {
      int fails = 0;
      for (int t = 0; t < kTrials; ++t) {
        if (!cssTrial(f, mode, cssFloorDb(f, mode),
                      uint64_t(f) * 77777 + uint64_t(mode) * 1000 +
                          uint64_t(t) + 1)) {
          ++fails;
        }
      }
      const double fer = double(fails) / kTrials;
      std::printf("    %-6s M%d SF%d %+6.1f dB : FER %3.0f%%\n",
                  plan(f).name, mode, cssSf(mode), cssFloorDb(f, mode),
                  100.0 * fer);
      CHECK(fer <= 0.32);
    }
  }
  // Curve shape above the floor (WIDE): +3 dB must be nearly clean.
  for (int mode = 0; mode <= 3; ++mode) {
    int fails = 0;
    for (int t = 0; t < kTrials; ++t) {
      if (!cssTrial(Family::Wide, mode, cssFloorDb(Family::Wide, mode) + 3.0,
                    uint64_t(9000 + mode * 100 + t))) {
        ++fails;
      }
    }
    CHECK(double(fails) / kTrials <= 0.12);
  }
  // ROADMAP 6.1 gate: usable FER down to -14 dB. MID and NARROW M0
  // floors sit below -14, so -14 itself must be nearly clean there.
  for (Family f : {Family::Mid, Family::Narrow}) {
    int fails = 0;
    for (int t = 0; t < 20; ++t) {
      if (!cssTrial(f, 0, -14.0, uint64_t(f) * 331 + uint64_t(t) + 40)) {
        ++fails;
      }
    }
    std::printf("    %-6s M0 SF8  -14.0 dB : FER %3.0f%%\n", plan(f).name,
                5.0 * fails);
    CHECK(fails <= 2);
  }
}

SKYTEST_MAIN
