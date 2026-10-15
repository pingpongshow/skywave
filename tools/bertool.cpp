// bertool: FER/throughput conformance sweeps (BUILD_PLAN 1.13, TESTING.md).
//
// TX -> Watterson chansim -> receiveFrame, per (family, mode, SNR), CCIR
// Moderate by default. FER counts acquisition, header, and payload
// failures alike - the documented floors are end-to-end.
//
// Usage:
//   bertool [--trials N] [--offsets "0,3,10"] [--families wide,narrow]
//           [--modes 4,8,13] [--preset good|moderate|poor|awgn]
//           [--csv path] [--seed S] [--css-frames] [--cfo HZ]
//           [--drift HZ_PER_S] [--base awgn]
//
// --css-frames sweeps the CSS deep tier (M0-M3) through the full PHY
// frame path instead of the OFDM ladder. (The payload-only --css sweep of
// the legacy Hamming modem-layer API was removed 2026-10-14: frames carry
// an LDPC payload.)
//
// --cfo / --drift apply a static carrier offset and a linear drift (from
// the start of each trial's signal) in the channel; the table then also
// reports the receiver's mean and worst |CFO error| over decoded headers
// (GAP_CLOSURE_PLAN.md 8.0).
#include <algorithm>
#include <cmath>
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
  // --css-frames: run the FULL PHY frame path (chirp preamble, sync,
  // header copies, LDPC payload -> receiveFrame) for the CSS modes
  // M0-M3. --css-hdr N sets the
  // header copies on air (2 = v1 layout, 1 = connected-session short
  // layout) so the time-diversity trade-off can be measured.
  bool cssFrames = false;
  int cssHdrCopies = 2;
  // --payload N overrides the per-mode frame payload length (default
  // 60 B robust / 300 B fast): sweeps frame length against FER at the
  // fading floor, the input to any frame-length adaptation policy.
  int payloadLen = 0;
  Band band = Band::Hf;
  // --base awgn: offsets are relative to the AWGN floor whatever the
  // preset, and kFadingNA modes are swept too (floor-table measurement).
  bool baseAwgn = false;
  // --table moderate|poor: fading presets gate (and offset) against the
  // class tables instead of the designated kFadingFloors.
  std::string table = "designated";
  double cfo = 0.0;    // static channel CFO, Hz
  double drift = 0.0;  // linear CFO drift, Hz/s
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

double tableFloorDb(const std::string& table, Family f, int mode) {
  if (table == "moderate") return moderateFloorDb(f, mode);
  if (table == "poor") return poorFloorDb(f, mode);
  return fadingFloorDb(f, mode);
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
    else if (!strcmp(argv[i], "--cfo")) a.cfo = atof(need("--cfo").c_str());
    else if (!strcmp(argv[i], "--base")) a.baseAwgn = need("--base") == "awgn";
    else if (!strcmp(argv[i], "--table")) a.table = need("--table");
    else if (!strcmp(argv[i], "--drift")) a.drift = atof(need("--drift").c_str());
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
  if (a.cssFrames && a.modes == ofdmDefault) a.modes = {0, 1, 2, 3};

  if (csv) {
    fprintf(csv,
            "family,mode,snr3k,trials,noDetect,zcFail,hdrFail,payFail,fer,"
            "cfo,drift,cfoErrMean,cfoErrMaxAbs\n");
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
          a.preset == "awgn" || a.baseAwgn
              ? (cssMode ? cssFloorDb(fam, mode) : floorDb(fam, mode))
              : tableFloorDb(a.table, fam, mode);
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
        // Data-aided (post-decode EVM) SNR and the pilot-stability
        // indicator, over frames whose payload decoded: the estimator
        // comparison under fading, and the classifier's per-mode
        // calibration question.
        double evmSum = 0.0, pilotSum = 0.0;
        int evmN = 0;
        // Receiver CFO error (estimate - channel offset at t = 0) over
        // frames whose header decoded.
        double cfoErrSum = 0.0, cfoErrMax = 0.0;
        int cfoN = 0;
        for (int t = 0; t < a.trials; ++t) {
          ChansimParams cp = presetParams(a.preset);
          cp.snr3kDb = r.snrDb;
          cp.cfoHz = a.cfo;
          cp.cfoDriftHzPerSec = a.drift;
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
            const double cfoErr = res->cfoHz - a.cfo;
            cfoErrSum += cfoErr;
            cfoErrMax = std::max(cfoErrMax, std::abs(cfoErr));
            ++cfoN;
            estSum += res->snrDb;
            ++estN;
            if (res->payloadOk && res->evmSnrDb > -90.0) {
              evmSum += res->evmSnrDb;
              pilotSum += res->pilotStability;
              ++evmN;
            }
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
        const double evm = evmN > 0 ? evmSum / evmN : -99.0;
        const double pilot = evmN > 0 ? pilotSum / evmN : -1.0;
        // FRAME-SIZE ECONOMICS. `ovh` is the share of airtime that is
        // not payload symbols (preamble, ZC sync, header) - the cost a
        // longer frame amortises. `good` is the expected goodput of
        // this frame shape under ARQ: a frame that fails costs its whole
        // airtime and must be sent again, so the expected airtime per
        // delivered frame is air/(1-FER) and the goodput is
        // payload x (1-FER) / air. Frame length pays exactly while
        // `good` rises - the overhead saving against the fading-outage
        // penalty of a longer frame, measured rather than argued.
        const double ovhFrac = airSec > 0 ? 1.0 - paySec / airSec : 0.0;
        const double good =
            airSec > 0 ? double(payloadLen) * (1.0 - r.fer()) / airSec : 0.0;
        printf("%-7s M%-3d %7.1f %7.1f | %5d %5d %5d %5d %6.1f%% | %7.0f | est %6.1f evm %6.1f pilot %6.3f | %5zuB air %5.1f ovh %3.0f%% good %6.1f\n",
               plan(fam).name, mode, floor, r.snrDb, r.noDetect, r.zcFail,
               r.hdrFail, r.payFail, 100.0 * r.fer(), netEff, est, evm, pilot,
               payloadLen, airSec, 100.0 * ovhFrac, good);
        if (a.cfo != 0.0 || a.drift != 0.0) {
          printf("          cfo %+.1f Hz drift %.2f Hz/s | cfoErr mean %+.2f max|%.2f| Hz\n",
                 a.cfo, a.drift, cfoN ? cfoErrSum / cfoN : 0.0, cfoErrMax);
        }
        fflush(stdout);
        if (csv) {
          fprintf(csv, "%s,%d,%.1f,%d,%d,%d,%d,%d,%.4f,%.2f,%.3f,%.3f,%.3f\n",
                  plan(fam).name, mode, r.snrDb, r.trials, r.noDetect,
                  r.zcFail, r.hdrFail, r.payFail, r.fer(), a.cfo, a.drift,
                  cfoN ? cfoErrSum / cfoN : 0.0, cfoErrMax);
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
            : tableFloorDb(a.table, r.fam, r.mode);
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
