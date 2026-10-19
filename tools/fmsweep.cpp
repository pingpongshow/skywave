// fmsweep: VHF-FM profile FER vs carrier-to-noise ratio through the FM
// radio emulator (sim/fmradio.hpp), per radio preset and mode.
//
// TX frame -> 48 kHz upconverter at the worker's TX level -> FM radio
// (modulator, flutter, RF offset, AWGN in the IF, IF filter,
// discriminator, audio filters) -> downconverter -> receiveFrame. FER
// counts acquisition, header and payload failures alike.
//
// Usage:
//   fmsweep [--preset NAME] [--modes 4,8,14] [--trials N]
//           [--cnr-from DB] [--cnr-to DB] [--cnr-step DB] [--seed S]
//           [--payload BYTES] [--csv path]
//
// Presets: flat (port 300-7000 Hz, the profile's assumption), lowedge
// (port flat down to 20 Hz, as a DC-coupled 9600 port), narrow (12.5 kHz
// channel), walk (0.7 Hz flutter),
// mobile (15 Hz), uhf (50 Hz), offset (700 Hz RF offset + 100 ppm
// soundcard), hot (+6 dB drive into a 3.5 kHz deviation limiter), quiet
// (-10 dB drive), voice (rig left on its 1200/mic audio path).
//
// For each mode the CNR rises in steps until two consecutive points have
// FER <= 5%; the report gives the lowest CNR with FER <= 10% ("floor"),
// and the receiver's SNR estimate there (what the AMC steers on).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/constants.hpp"
#include "dsp/updown.hpp"
#include "phy/framer.hpp"
#include "phy/layout.hpp"
#include "phy/receiver.hpp"
#include "sim/fmradio.hpp"

using namespace skw;

namespace {

bool presetParams(const std::string& name, FmRadioParams& p) {
  p = fmDataPort();
  if (name == "flat") return true;
  if (name == "narrow") { p = fmDataPortNarrow(); return true; }
  if (name == "lowedge") { p.txLowHz = p.rxLowHz = 20.0; return true; }
  if (name == "walk") { p.flutterHz = 0.7; return true; }
  if (name == "mobile") { p.flutterHz = 15.0; return true; }
  if (name == "uhf") { p.flutterHz = 50.0; return true; }
  if (name == "offset") { p.rfOffsetHz = 700.0; p.soundcardPpm = 100.0; return true; }
  if (name == "hot") { p.driveLevelDb = 6.0; p.devLimitHz = 3500.0; return true; }
  if (name == "quiet") { p.driveLevelDb = -10.0; return true; }
  if (name == "voice") { p = fmVoicePort(); return true; }
  return false;
}

std::vector<int> parseList(const char* s) {
  std::vector<int> v;
  for (const char* q = s; *q;) {
    v.push_back(std::atoi(q));
    while (*q && *q != ',') ++q;
    if (*q == ',') ++q;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  std::string preset = "flat", csvPath;
  std::vector<int> modes = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
  int trials = 20, payloadArg = 0;
  double cnrFrom = 0.0, cnrTo = 50.0, cnrStep = 2.0;
  uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--preset")) preset = need("--preset");
    else if (!std::strcmp(argv[i], "--modes")) modes = parseList(need("--modes"));
    else if (!std::strcmp(argv[i], "--trials")) trials = std::atoi(need("--trials"));
    else if (!std::strcmp(argv[i], "--cnr-from")) cnrFrom = std::atof(need("--cnr-from"));
    else if (!std::strcmp(argv[i], "--cnr-to")) cnrTo = std::atof(need("--cnr-to"));
    else if (!std::strcmp(argv[i], "--cnr-step")) cnrStep = std::atof(need("--cnr-step"));
    else if (!std::strcmp(argv[i], "--seed")) seed = uint64_t(std::atoll(need("--seed")));
    else if (!std::strcmp(argv[i], "--payload")) payloadArg = std::atoi(need("--payload"));
    else if (!std::strcmp(argv[i], "--csv")) csvPath = need("--csv");
    else { std::fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
  }
  FmRadioParams radio;
  if (!presetParams(preset, radio)) { std::fprintf(stderr, "bad preset\n"); return 2; }

  setActiveBand(Band::VhfFm);
  const Family fam = Family::Wide;  // every index resolves to the VHF plan
  const float txGain = float(activeProfile().txTargetRms * std::sqrt(2.0));
  FILE* csv = csvPath.empty() ? nullptr : std::fopen(csvPath.c_str(), "w");
  if (csv) std::fprintf(csv, "preset,mode,cnr,trials,ok,fer,estSnrMean\n");
  std::printf("preset %s: %-4s %7s %8s  (FER by CNR)\n", preset.c_str(), "mode",
              "floor", "estSNR");

  for (int mode : modes) {
    if (mode < 4 || mode >= kNumModes) continue;
    const bool robust = mode <= 7;
    const size_t len = payloadArg > 0 ? size_t(payloadArg) : (robust ? 60 : 300);
    double floorCnr = -1.0, floorEst = 0.0;
    int cleanRun = 0;
    std::string trace;
    for (double cnr = cnrFrom; cnr <= cnrTo + 1e-9; cnr += cnrStep) {
      int ok = 0;
      double estSum = 0.0;
      int estN = 0;
      for (int t = 0; t < trials; ++t) {
        uint64_t s = skwHash(seed * 1000003u + uint64_t(mode) * 7919u + uint64_t(t));
        std::vector<uint8_t> payload(len);
        for (auto& b : payload) { s = skwHash(s); b = uint8_t(s >> 40); }
        TxFrameParams tp;
        tp.family = fam;
        tp.mode = mode;
        tp.sig = robust ? ChirpSig::Robust : ChirpSig::Fast;
        tp.cpMode = activeProfile().cpDefault;
        tp.payload = payload;
        auto bb = buildFrameBaseband(tp);
        std::vector<cfloat> padded(1600, cfloat(0, 0));  // bertool-like lead; the FM path adds ~19 ms
        padded.insert(padded.end(), bb.begin(), bb.end());
        padded.insert(padded.end(), 2500, cfloat(0, 0));
        Upconverter up;
        std::vector<float> audio;
        up.process(padded, audio);
        for (auto& v : audio) v = std::clamp(v * txGain, -1.0f, 1.0f);
        FmRadioParams p = radio;
        p.cnrDb = cnr;
        p.seed = s ^ 0x5eedull;
        auto rx = fmRadio(p, audio);
        Downconverter dn;
        std::vector<cfloat> rx12;
        dn.process(rx, rx12);
        auto res = receiveFrame(fam, rx12);
        if (res) {  /* a result means the header decoded */ estSum += res->snrDb; ++estN; }
        if (res && res->payloadOk && res->payload == payload) ++ok;
      }
      const double fer = 1.0 - double(ok) / trials;
      const double est = estN ? estSum / estN : -99.0;
      if (csv) std::fprintf(csv, "%s,%d,%.1f,%d,%d,%.3f,%.1f\n", preset.c_str(), mode,
                            cnr, trials, ok, fer, est);
      char buf[32];
      std::snprintf(buf, sizeof buf, " %.0f:%.2f", cnr, fer);
      trace += buf;
      if (fer <= 0.10 && floorCnr < 0) { floorCnr = cnr; floorEst = est; }
      if (fer > 0.10) floorCnr = -1.0;  // a later failure moves the floor up
      cleanRun = fer <= 0.05 ? cleanRun + 1 : 0;
      if (cleanRun >= 2 && floorCnr >= 0) break;
    }
    if (floorCnr >= 0) {
      std::printf("  M%-3d %6.1f dB %6.1f dB |%s\n", mode, floorCnr, floorEst, trace.c_str());
    } else {
      std::printf("  M%-3d   none          |%s\n", mode, trace.c_str());
    }
    std::fflush(stdout);
  }
  if (csv) std::fclose(csv);
  return 0;
}
