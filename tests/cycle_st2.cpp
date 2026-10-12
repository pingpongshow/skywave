// cycle_st2: the ST3 soak cycle loop - send 512 B A->B, wait for
// delivery, repeat - over the ST2 world in SIMULATED time, so cycle-rate
// comparisons between builds are deterministic (real-device soaks swing
// +-10-20% run to run; ROADMAP 8.10 cycle-rate investigation).
// Env: DUR (sim-s, 900), SNR (dB, 40), SEED (11). Not a registered test.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "st2_world.hpp"

using namespace skw;
using namespace st2w;

int main() {
  const double dur = getenv("DUR") ? atof(getenv("DUR")) : 900.0;
  const double snr = getenv("SNR") ? atof(getenv("SNR")) : 40.0;
  World w(snr, getenv("SEED") ? strtoull(getenv("SEED"), nullptr, 10) : 11);
  for (Shuttle* sh : {&w.ab, &w.ba}) {
    sh->chain.channel = ChansimParams{};
    sh->chain.channel.fading = false;
    sh->chain.channel.snr3kDb = snr;
  }
  w.cmd(w.a, "CONNECT K6XYZ");
  if (!w.waitState(w.a, "CONNECTED", 20000)) {
    std::printf("RESULT noconnect\n");
    return 1;
  }
  const double t0 = double(w.clk.nowUs()) * 1e-6;
  int cycles = 0;
  std::vector<double> lat;
  size_t want = 0;
  while (double(w.clk.nowUs()) * 1e-6 - t0 < dur) {
    std::vector<uint8_t> c(512);
    for (size_t i = 0; i < c.size(); ++i) c[i] = uint8_t((cycles + i) % 251);
    want += c.size();
    ::send(w.a.dataFd, c.data(), c.size(), 0);
    const double ts = double(w.clk.nowUs()) * 1e-6;
    while (w.b.dataAcc.size() < want &&
           double(w.clk.nowUs()) * 1e-6 - t0 < dur) {
      w.round();
    }
    if (w.b.dataAcc.size() < want) break;
    lat.push_back(double(w.clk.nowUs()) * 1e-6 - ts);
    ++cycles;
  }
  std::sort(lat.begin(), lat.end());
  auto q = [&](double p) { return lat.empty() ? 0.0 : lat[size_t(p * (lat.size() - 1))]; };
  std::printf("RESULT cycles=%d median=%.2f p10=%.2f p90=%.2f mode=%s\n",
              cycles, q(0.5), q(0.1), q(0.9), w.cmd(w.a, "MODE").c_str());
  return 0;
}
