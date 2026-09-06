// bertool: FER/throughput conformance sweeps (BUILD_PLAN 1.13, TESTING.md).
//
// TX -> Watterson chansim -> receiveFrame, per (family, mode, SNR), CCIR
// Moderate by default. FER counts acquisition, header, and payload
// failures alike - the documented floors are end-to-end.
//
// Usage:
//   bertool [--trials N] [--offsets "0,3,10"] [--families wide,narrow]
//           [--modes 4,8,13] [--preset good|moderate|poor|awgn]
//           [--csv path] [--seed S] [--css]
//
// --css sweeps the CSS deep tier (M0-M3) instead of the OFDM ladder:
// aligned modem-level FER (no PHY sync - that is ROADMAP 6.2), offsets
// relative to the per-family cssFloorDb table.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/constants.hpp"
#include "dsp/css.hpp"
#include "fec/crc.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"

using namespace skw;

namespace {

struct Args {
  int trials = 25;
  // WAVEFORM conformance points: floor+1 (<=10% AWGN), +3, +10.
  std::vector<double> offsets = {1.0, 3.0, 10.0};
  std::vector<Family> families = {Family::Wide, Family::Mid, Family::Narrow,
                                  Family::Ultra};
  std::vector<int> modes = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  std::string preset = "moderate";
  std::string csv;
  uint64_t seed = 1000;
  bool css = false;
  // --css-frames: run the FULL PHY frame path (chirp preamble, sync,
  // header copies, LDPC payload -> receiveFrame) for the CSS modes
  // M0-M3, unlike --css which measures payload-only FER through the
  // modem-layer API and cannot see header loss. --css-hdr N sets the
  // header copies on air (2 = v1 layout, 1 = connected-session short
  // layout) so the time-diversity trade-off can be measured.
  bool cssFrames = false;
  int cssHdrCopies = 2;
  // --payload N overrides the per-mode frame payload length (default
  // 60 B robust / 300 B fast): sweeps frame length against FER at the
  // fading floor, the input to any frame-length adaptation policy.
  int payloadLen = 0;
  Band band = Band::Hf;
};

std::vector<std::string> split(const std::string& s, char d) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find(d, p);
    if (q == std::string::npos) q = s.size();
    out.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

ChansimParams presetParams(const std::string& name) {
  if (name == "good") return ccirGood();
  if (name == "poor") return ccirPoor();
  if (name == "vhf") return vhfFlat();
  if (name == "awgn") {
    ChansimParams p;
    p.fading = false;
    return p;
  }
  return ccirModerate();
}

struct ConfigResult {
  Family fam;
  int mode;
  double snrDb;
  int trials;
  int noDetect = 0;
  int zcFail = 0;
  int hdrFail = 0;
  int payFail = 0;
  double fer() const {
    return double(noDetect + zcFail + hdrFail + payFail) / trials;
  }
};

// One aligned CSS frame trial (24-byte payload + CRC-32), success = CRC
// clean after modulate -> chansim -> matched-filter demod.
bool cssFrameTrial(Family fam, int mode, const ChansimParams& cp) {
  std::vector<uint8_t> payload(24);
  uint64_t s = cp.seed * 2654435761u + 1;
  for (auto& b : payload) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    b = uint8_t(s >> 32);
  }
  const uint32_t crc = crc32(payload);
  for (int i = 0; i < 4; ++i) payload.push_back(uint8_t(crc >> (8 * i)));
  auto tx = cssModulatePayload(fam, mode, payload);
  auto rx = Chansim::run(cp, tx);
  rx.resize(tx.size());
  auto got = cssDemodPayload(fam, mode, rx, payload.size());
  if (got.size() != payload.size()) return false;
  uint32_t want = 0;
  for (int i = 0; i < 4; ++i) {
    want |= uint32_t(got[got.size() - 4 + size_t(i)]) << (8 * i);
  }
  got.resize(got.size() - 4);
  return crc32(got) == want;
}

// CSS deep-tier sweep (ROADMAP 6.1 gate): FER vs SNR for M0-M3 at
// offsets around the per-family design floors.
int cssSweep(const Args& a, FILE* csv) {
  if (csv) fprintf(csv, "family,mode,sf,snr3k,trials,fails,fer\n");
  printf("%-7s %-4s %-3s %7s %7s | %5s %7s\n", "family", "mode", "sf",
         "floor", "snr", "fails", "FER");
  int gateFails = 0;
  for (Family fam : a.families) {
    for (int mode : a.modes) {
      if (cssSf(mode) == 0) continue;  // CSS modes only
      const double floor = cssFloorDb(fam, mode);
      for (double off : a.offsets) {
        ChansimParams base = presetParams(a.preset);
        int fails = 0;
        for (int t = 0; t < a.trials; ++t) {
          ChansimParams cp = base;
          cp.snr3kDb = floor + off;
          cp.seed = a.seed + uint64_t(t) * 7919 + uint64_t(mode) * 131 +
                    uint64_t(fam) * 100003 +
                    uint64_t(std::llround(off * 2.0 + 64.0)) * 15485863ull;
          if (!cssFrameTrial(fam, mode, cp)) ++fails;
        }
        const double fer = double(fails) / a.trials;
        printf("%-7s M%-3d %-3d %7.1f %7.1f | %5d %6.1f%%\n",
               plan(fam).name, mode, cssSf(mode), floor, floor + off, fails,
               100.0 * fer);
        fflush(stdout);
        if (csv) {
          fprintf(csv, "%s,%d,%d,%.1f,%d,%d,%.4f\n", plan(fam).name, mode,
                  cssSf(mode), floor + off, a.trials, fails, fer);
          fflush(csv);
        }
        // Floors are AWGN-referenced; only gate the AWGN preset.
        if (a.preset == "awgn") {
          auto gate = [&](double p) {
            return p + 1.3 * std::sqrt(p * (1.0 - p) / a.trials);
          };
          const bool bad =
              (off < 0.5 && off > -0.5 && fer > gate(0.15)) ||
              (off >= 2.5 && fer > gate(0.04 + 1.0 / a.trials));
          if (bad) {
            printf("FAIL: %s M%d @ %.1f dB FER=%.1f%%\n", plan(fam).name,
                   mode, floor + off, 100 * fer);
            ++gateFails;
          }
        }
      }
    }
  }
  printf(gateFails ? "== %d CSS conformance failures ==\n"
                   : "== all CSS points pass ==\n",
         gateFails);
  return gateFails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing arg for %s\n", opt);
        exit(2);
      }
      return argv[++i];
    };
    if (!strcmp(argv[i], "--trials")) a.trials = atoi(need("--trials").c_str());
    else if (!strcmp(argv[i], "--offsets")) {
      a.offsets.clear();
      for (auto& t : split(need("--offsets"), ',')) {
        a.offsets.push_back(atof(t.c_str()));
      }
    } else if (!strcmp(argv[i], "--families")) {
      a.families.clear();
      for (auto& t : split(need("--families"), ',')) {
        if (auto f = familyFromString(t)) a.families.push_back(*f);
      }
    } else if (!strcmp(argv[i], "--modes")) {
      a.modes.clear();
      for (auto& t : split(need("--modes"), ',')) {
        a.modes.push_back(atoi(t.c_str()));
      }
    } else if (!strcmp(argv[i], "--preset")) a.preset = need("--preset");
    else if (!strcmp(argv[i], "--csv")) a.csv = need("--csv");
    else if (!strcmp(argv[i], "--seed")) a.seed = strtoull(need("--seed").c_str(), nullptr, 10);
    else if (!strcmp(argv[i], "--css")) a.css = true;
    else if (!strcmp(argv[i], "--css-frames")) a.cssFrames = true;
    else if (!strcmp(argv[i], "--css-hdr")) a.cssHdrCopies = atoi(need("--css-hdr").c_str());
    else if (!strcmp(argv[i], "--payload")) a.payloadLen = atoi(need("--payload").c_str());
    else if (!strcmp(argv[i], "--band")) {
      auto bb = bandFromString(need("--band"));
      if (!bb) { fprintf(stderr, "bad band\n"); return 2; }
      a.band = *bb;
    }
    else {
      fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  setActiveBand(a.band);
  if (a.band == Band::VhfFm) {
    // One family, no CSS tier; the flat channel is its own preset.
    a.families = {Family::Wide};
    if (a.preset == "moderate") a.preset = "vhf";
  }

  FILE* csv = a.csv.empty() ? nullptr : fopen(a.csv.c_str(), "w");

  // The OFDM mode list default does not apply to the CSS tier.
  const std::vector<int> ofdmDefault = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  if ((a.css || a.cssFrames) && a.modes == ofdmDefault) a.modes = {0, 1, 2, 3};
  if (a.css) {
    const int rc = cssSweep(a, csv);
    if (csv) fclose(csv);
    return rc;
  }

  if (csv) {
    fprintf(csv,
            "family,mode,snr3k,trials,noDetect,zcFail,hdrFail,payFail,fer\n");
  }

  printf("%-7s %-4s %7s %7s | %5s %5s %5s %5s %7s | %8s\n", "family",
         "mode", "floor", "snr", "noDet", "zcF", "hdrF", "payF", "FER",
         "netEff");
  std::vector<ConfigResult> all;

  for (Family fam : a.families) {
    for (int mode : a.modes) {
      const ModeDef& md = kModes[size_t(mode)];
      const bool cssMode = md.waveform == Waveform::Css;
      if (!md.active) continue;
      if (cssMode ? !a.cssFrames : a.cssFrames) continue;
      // Offsets are relative to the floor that matches the channel: the
      // AWGN reference for --preset awgn, the measured fading floor for
      // the fading presets (WAVEFORM.md fading conformance annex).
      const double floor =
          a.preset == "awgn"
              ? (cssMode ? cssFloorDb(fam, mode) : floorDb(fam, mode))
              : fadingFloorDb(fam, mode);
      if (floor >= 90.0) continue;  // kFadingNA: no fading conformance
      const bool robust = mode <= 7;
      const size_t payloadLen =
          a.payloadLen > 0 ? size_t(std::min(a.payloadLen, 4095))
                           : (robust ? 60 : 300);

      // Build the frame once per config; the channel varies per trial.
      std::vector<uint8_t> payload(payloadLen);
      for (size_t i = 0; i < payloadLen; ++i) payload[i] = uint8_t(i * 41 + 3);
      TxFrameParams tp;
      tp.family = fam;
      tp.sig = cssMode ? ChirpSig::Css
                       : (robust ? ChirpSig::Robust : ChirpSig::Fast);
      tp.mode = mode;
      tp.cssHeaderCopies = uint8_t(a.cssHdrCopies == 1 ? 1 : 2);
      tp.payload = payload;
      auto frame = buildFrameBaseband(tp);
      auto geo = computeGeometry(tp);
      const double paySec =
          double(geo.payloadSymbols) *
          double(kFftBaseband +
                 cpSamplesAudio(tp.cpMode) / kDecimation) /
          double(kFsBaseband);
      // CSS: report GROSS efficiency over the whole frame (preamble +
      // sync + header copies + payload) - the overhead IS the question.
      const double airSec = double(geo.totalSamples) / double(kFsBaseband);
      const double netEff =
          cssMode ? (airSec > 0 ? payloadLen * 8.0 / airSec : 0.0)
                  : (paySec > 0 ? (payloadLen - 30) * 8.0 / paySec : 0.0);
      if (cssMode) {
        printf("# %s M%d: %zu B frame, %.1f s on air, header copies %d\n",
               plan(fam).name, mode, payloadLen, airSec, int(tp.cssHeaderCopies));
      }

      std::vector<cfloat> x(2000, cfloat(0, 0));
      x.insert(x.end(), frame.begin(), frame.end());
      x.insert(x.end(), 2500, cfloat(0, 0));

      for (double off : a.offsets) {
        ConfigResult r;
        r.fam = fam;
        r.mode = mode;
        r.snrDb = floor + off;
        r.trials = a.trials;
        // Receiver-estimated SNR over the frames whose header decoded:
        // the AMC steers on this number, so its bias against the set
        // point IS the "floor calibration" question (ROADMAP: set 9 dB
        // reads as -6 dB in the soak).
        double estSum = 0.0;
        int estN = 0;
        for (int t = 0; t < a.trials; ++t) {
          ChansimParams cp = presetParams(a.preset);
          cp.snr3kDb = r.snrDb;
          // Seed must vary with the SNR point too: reusing realizations
          // across offsets pins any unlucky deep-fade-over-preamble seed
          // as a false FER floor across the whole ladder.
          cp.seed = a.seed + uint64_t(t) * 7919 +
                    uint64_t(mode) * 131 + uint64_t(fam) * 100003 +
                    uint64_t(std::llround(off * 2.0 + 64.0)) * 15485863ull;
          auto y = Chansim::run(cp, x);
          RxStage st = RxStage::NoDetect;
          auto res = receiveFrame(fam, y, &st);
          if (res) {
            estSum += res->snrDb;
            ++estN;
          }
          if (!res) {
            if (st == RxStage::NoDetect) ++r.noDetect;
            else if (st == RxStage::ZcFail) ++r.zcFail;
            else ++r.hdrFail;
          } else if (res->header.mode != mode ||
                     res->header.length != payloadLen) {
            ++r.hdrFail;
          } else if (!res->payloadOk || res->payload != payload) {
            ++r.payFail;
          }
        }
        const double est = estN > 0 ? estSum / estN : -99.0;
        printf("%-7s M%-3d %7.1f %7.1f | %5d %5d %5d %5d %6.1f%% | %7.0f | est %6.1f\n",
               plan(fam).name, mode, floor, r.snrDb, r.noDetect, r.zcFail,
               r.hdrFail, r.payFail, 100.0 * r.fer(), netEff, est);
        fflush(stdout);
        if (csv) {
          fprintf(csv, "%s,%d,%.1f,%d,%d,%d,%d,%d,%.4f\n", plan(fam).name,
                  mode, r.snrDb, r.trials, r.noDetect, r.zcFail, r.hdrFail,
                  r.payFail, r.fer());
          fflush(csv);
        }
        all.push_back(r);
      }
    }
  }
  if (csv) fclose(csv);

  // Summary: floor-point pass/fail (FER <= 10% at offset 0, <= 1%-ish at
  // +3 given trial count) against the same floor family as the offsets.
  int fail = 0;
  for (const auto& r : all) {
    const bool cssR = kModes[size_t(r.mode)].waveform == Waveform::Css;
    const double base =
        a.preset == "awgn"
            ? (cssR ? cssFloorDb(r.fam, r.mode) : floorDb(r.fam, r.mode))
            : fadingFloorDb(r.fam, r.mode);
    double off = r.snrDb - base;
    // AWGN decays as a coding waterfall (steep); fading FER is outage-
    // limited and falls off far more gently above the floor. Gates carry
    // one-sided binomial slack (~90%): a config whose true FER equals
    // the spec must not fail the sweep half the time.
    const double p3 = (a.preset == "awgn" ? 0.04 : 0.10) + 1.0 / r.trials;
    // One-sided acceptance: AWGN waterfalls are steep (1.3 sigma);
    // fading FER is outage-limited with high per-run variance and slow
    // tails - a config whose true FER equals the spec must not flip
    // coin-flips across 40-trial runs (2 sigma).
    const double slack = a.preset == "awgn" ? 1.3 : 2.0;
    auto gate = [&](double p) {
      return p + slack * std::sqrt(p * (1.0 - p) / r.trials);
    };
    // Fading floors are defined at one-shot FER <= ~10-15% (outage-
    // limited tails; ARQ is the design response) - the floor-point base
    // is 0.15 there. AWGN keeps the tight waterfall gates.
    const double p0 = a.preset == "awgn" ? 0.10 : 0.15;
    bool bad = (off < 1.5 && r.fer() > gate(p0)) ||
               (off >= 2.5 && off < 5.0 && r.fer() > gate(p3));
    if (bad) {
      printf("FAIL: %s M%d @ %.1f dB FER=%.1f%%\n", plan(r.fam).name, r.mode,
             r.snrDb, 100 * r.fer());
      ++fail;
    }
  }
  printf(fail ? "== %d conformance failures ==\n" : "== all points pass ==\n",
         fail);
  return fail ? 1 : 0;
}
