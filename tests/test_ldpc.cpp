#include <cmath>
#include <random>
#include <vector>

#include "fec/ldpc.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> randomBits(int k, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> bits(static_cast<size_t>(k));
  for (auto& b : bits) b = uint8_t(rng() & 1);
  return bits;
}

constexpr CodeRate kRates[] = {{1, 2}, {2, 3}, {3, 4}, {5, 6}};

// BPSK over AWGN at the given Eb/N0; returns frame error rate.
double awgnFer(const LdpcCode& code, double ebn0Db, int frames,
               uint32_t seed) {
  const double rate = double(code.k()) / code.n();
  const double ebn0 = std::pow(10.0, ebn0Db / 10.0);
  const double sigma = std::sqrt(1.0 / (2.0 * rate * ebn0));
  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, sigma);

  int errors = 0;
  std::vector<uint8_t> code_bits, decoded;
  std::vector<float> llr(static_cast<size_t>(code.n()));
  for (int f = 0; f < frames; ++f) {
    auto info = randomBits(code.k(), seed + uint32_t(f) * 7919u);
    code.encode(info, code_bits);
    for (int i = 0; i < code.n(); ++i) {
      double tx = code_bits[size_t(i)] ? -1.0 : 1.0;  // bit0 -> +1
      double rx = tx + noise(rng);
      llr[size_t(i)] = float(2.0 * rx / (sigma * sigma));
    }
    bool ok = code.decode(llr, decoded);
    if (!ok || decoded != info) ++errors;
  }
  return double(errors) / frames;
}

}  // namespace

TEST_CASE(ldpc_dimensions) {
  for (int n : {648, 1944}) {
    for (auto r : kRates) {
      const LdpcCode& c = LdpcCode::get(n, r);
      CHECK_EQ(c.n(), n);
      CHECK_EQ(c.k(), n * r.num / r.den);
    }
  }
}

TEST_CASE(ldpc_encoder_satisfies_parity) {
  // H*c == 0 for random info across every code - catches any table or
  // encoder-structure error.
  for (int n : {648, 1944}) {
    for (auto r : kRates) {
      const LdpcCode& c = LdpcCode::get(n, r);
      for (uint32_t s = 0; s < 5; ++s) {
        std::vector<uint8_t> cw;
        c.encode(randomBits(c.k(), 100 + s), cw);
        CHECK(c.checkCodeword(cw));
      }
    }
  }
}

TEST_CASE(ldpc_noiseless_roundtrip) {
  for (int n : {648, 1944}) {
    for (auto r : kRates) {
      const LdpcCode& c = LdpcCode::get(n, r);
      auto info = randomBits(c.k(), 42);
      std::vector<uint8_t> cw, out;
      c.encode(info, cw);
      std::vector<float> llr(static_cast<size_t>(c.n()));
      for (int i = 0; i < c.n(); ++i) llr[size_t(i)] = cw[size_t(i)] ? -8.0f : 8.0f;
      CHECK(c.decode(llr, out));
      CHECK(out == info);
    }
  }
}

TEST_CASE(ldpc_no_evidence_is_not_success) {
  // Review D: an all-zero (dropout) or all-NaN LLR vector must not
  // "decode" to the all-zero codeword. A strong all-zero codeword still
  // decodes, and a stray NaN is only an erasure.
  for (int n : {648, 1944}) {
    const LdpcCode& c = LdpcCode::get(n, kRates[0]);
    std::vector<uint8_t> out;
    std::vector<float> zero(size_t(c.n()), 0.0f);
    CHECK(!c.decode(zero, out));
    std::vector<float> nan(size_t(c.n()), std::nanf(""));
    CHECK(!c.decode(nan, out));
    std::vector<float> strong(size_t(c.n()), 8.0f);
    strong[3] = std::nanf("");
    CHECK(c.decode(strong, out));
    CHECK(out == std::vector<uint8_t>(size_t(c.k()), 0));
  }
}

TEST_CASE(ldpc_corrects_bit_errors) {
  // Flip a scattering of bits with strong-but-wrong LLRs; decoder must fix.
  const LdpcCode& c = LdpcCode::get(1944, {1, 2});
  auto info = randomBits(c.k(), 7);
  std::vector<uint8_t> cw, out;
  c.encode(info, cw);
  std::vector<float> llr(static_cast<size_t>(c.n()));
  for (int i = 0; i < c.n(); ++i) llr[size_t(i)] = cw[size_t(i)] ? -4.0f : 4.0f;
  std::mt19937 rng(11);
  for (int e = 0; e < 60; ++e) {
    size_t pos = rng() % size_t(c.n());
    llr[pos] = -llr[pos];
  }
  CHECK(c.decode(llr, out));
  CHECK(out == info);
}

TEST_CASE(ldpc_awgn_thresholds_1944) {
  // ROADMAP 1.5 gate: decode thresholds within 0.5 dB of reference curves.
  // Reference N=1944 thresholds (~1% FER) from the literature:
  //   r1/2 ~1.7 dB, r2/3 ~2.4 dB, r3/4 ~2.9 dB, r5/6 ~3.6 dB Eb/N0.
  // Assert: near-zero FER at threshold + 0.5 dB, and clear failure well
  // below threshold (validates both the code and the LLR scaling).
  struct Point { CodeRate r; double thrDb; };
  const Point pts[] = {
      {{1, 2}, 1.7}, {{2, 3}, 2.4}, {{3, 4}, 2.9}, {{5, 6}, 3.6}};
  for (const auto& p : pts) {
    const LdpcCode& c = LdpcCode::get(1944, p.r);
    double ferGood = awgnFer(c, p.thrDb + 0.5, 40, 555);
    double ferBad = awgnFer(c, p.thrDb - 1.5, 20, 777);
    CHECK(ferGood <= 0.10);
    CHECK(ferBad >= 0.80);
  }
}

TEST_CASE(ldpc_awgn_short_code) {
  // N=648 runs ~0.6 dB from the long code; sanity-check r1/2 only.
  const LdpcCode& c = LdpcCode::get(648, {1, 2});
  CHECK(awgnFer(c, 2.8, 40, 999) <= 0.10);
  CHECK(awgnFer(c, 0.0, 20, 888) >= 0.80);
}

SKYTEST_MAIN
