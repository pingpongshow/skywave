// cfo_st2: frequency-offset scenarios over the ST2 two-modem world
// (GAP_CLOSURE_PLAN.md 8.0 / 8.2 / 8.7). One-way bulk A -> B while B's
// local oscillator is offset, stepped, or drifting relative to A.
//
// Env (all optional):
//   CFO0     initial LO offset of B relative to A, Hz         (default 0)
//   STEP     additional step of B's LO, Hz                    (default 0)
//   STEP_AT  sim-s after CONNECTED at which STEP applies       (default 120)
//   DRIFT    linear LO drift of B from CONNECTED, Hz/s        (default 0)
//   DUR      sim-s to run after CONNECTED                     (default 600)
//   SNR      dB, 3 kHz reference                              (default 20)
//   FADE     1 = CCIR Moderate instead of AWGN                (default 0)
//   SEED     world seed                                       (default 11)
//   MIN_BYTES  pass threshold on bytes delivered to B         (default 0)
//   REV_MAX  pass threshold on the REVERSE path's residual at the end:
//            |CFO| A measures on B's frames, Hz           (default off)
//
// Exit status: 0 when the session never dropped and B received at least
// MIN_BYTES; 1 otherwise. Prints a "RESULT" line for scripts.
#include <cmath>
#include <cstdio>
#include <string>
#include <cstdlib>

#include "st2_world.hpp"

using namespace skw;
using namespace st2w;

static double env(const char* k, double d) {
  const char* v = std::getenv(k);
  return v ? std::atof(v) : d;
}

int main() {
  const double cfo0 = env("CFO0", 0), step = env("STEP", 0),
               stepAt = env("STEP_AT", 120), drift = env("DRIFT", 0),
               dur = env("DUR", 600), snr = env("SNR", 20);
  const size_t minBytes = size_t(env("MIN_BYTES", 0));
  const double revMax = env("REV_MAX", -1);
  World w(snr, uint64_t(env("SEED", 11)));
  // B's LO offset `lo`: A -> B sees -lo, B -> A sees +lo.
  auto setLo = [&](double lo) {
    w.ab.chain.channel.cfoHz = -lo;
    w.ba.chain.channel.cfoHz = lo;
  };
  for (Shuttle* sh : {&w.ab, &w.ba}) {
    const bool fade = env("FADE", 0) > 0;
    sh->chain.channel = fade ? ccirModerate() : ChansimParams{};
    if (!fade) sh->chain.channel.fading = false;
    sh->chain.channel.snr3kDb = snr;
  }
  setLo(cfo0);
  w.cmd(w.a, "CONNECT K6XYZ");
  if (!w.waitState(w.a, "CONNECTED", 20000)) {
    std::printf("RESULT noconnect\n");
    return 1;
  }
  std::printf("connected; A %s | B %s\n", w.cmd(w.a, "FREQINFO").c_str(),
              w.cmd(w.b, "FREQINFO").c_str());
  uint64_t s = 0x9E3779B97F4A7C15ull;
  size_t sent = 0;
  const double t0 = double(w.clk.nowUs()) * 1e-6;
  double nextLog = 0, lastProgress = 0;
  size_t lastB = 0;
  bool dropped = false;
  while (true) {
    const double t = double(w.clk.nowUs()) * 1e-6 - t0;
    if (t > dur) break;
    setLo(cfo0 + drift * t + (t >= stepAt ? step : 0.0));
    if (sent < w.b.dataAcc.size() + 4096) {
      std::vector<uint8_t> c(512);
      for (auto& b : c) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        b = uint8_t(s >> 32);
      }
      sent += c.size();
      ::send(w.a.dataFd, c.data(), c.size(), 0);
    }
    w.round();
    if (w.b.dataAcc.size() > lastB) {
      lastB = w.b.dataAcc.size();
      lastProgress = t;
    }
    if (t >= nextLog) {
      nextLog += 20;
      std::string st = w.cmd(w.a, "STATE");
      if (st != "STATE CONNECTED") dropped = true;
      std::printf("t=%6.0f lo=%6.1f B=%7zu %s | A[%s] B[%s] %s\n", t,
                  cfo0 + drift * t + (t >= stepAt ? step : 0.0),
                  w.b.dataAcc.size(), st.c_str(),
                  w.cmd(w.a, "FREQINFO").c_str(),
                  w.cmd(w.b, "FREQINFO").c_str(), w.cmd(w.a, "MODE").c_str());
      std::fflush(stdout);
    }
  }
  // Reverse path: what A hears of B. With reports riding ACKs only, a
  // one-way sender never reported it and B's signal stayed off by the
  // whole step (ROADMAP open-item review); A's standalone CFO ACK fixes it.
  const std::string fa = w.cmd(w.a, "FREQINFO");
  const size_t at = fa.find("CFO=");
  const double rev = at == std::string::npos ? 1e9 : std::atof(fa.c_str() + at + 4);
  std::printf("RESULT bytes=%zu dropped=%d lastProgress=%.0f rev=%.1f\n",
              w.b.dataAcc.size(), int(dropped), lastProgress, rev);
  const bool revOk = revMax < 0 || std::abs(rev) <= revMax;
  return (!dropped && w.b.dataAcc.size() >= minBytes && revOk) ? 0 : 1;
}
