// repro_oneway: minimal one-way bulk over the ST2 world on a CLEAN
// static channel. Isolates "does sustained one-way bulk make progress"
// from every scenario impairment in soak_st2.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "st2_world.hpp"

using namespace skw;
using namespace st2w;

int main() {
  const char* e;
  const double snrDb = (e = std::getenv("REPRO_SNR")) ? std::atof(e) : 30.0;
  const double simSec = (e = std::getenv("REPRO_SEC")) ? std::atof(e) : 300.0;
  const size_t window = (e = std::getenv("REPRO_WIN")) ? size_t(std::atoll(e)) : 4096;

  World w(snrDb, 11);
  for (Shuttle* sh : {&w.ab, &w.ba}) {
    sh->chain.channel = ChansimParams{};
    sh->chain.channel.fading = false;
    sh->chain.channel.snr3kDb = snrDb;
  }
  if (w.cmd(w.a, "CONNECT K6XYZ").rfind("CONNECT", 0) != 0) {
    std::printf("REPRO FAIL: connect refused\n");
    return 1;
  }
  if (!w.waitState(w.a, "CONNECTED", 12000)) {
    std::printf("REPRO FAIL: no connect\n");
    return 1;
  }
  const double t0 = double(w.clk.nowUs()) * 1e-6;
  auto now = [&] { return double(w.clk.nowUs()) * 1e-6 - t0; };

  uint64_t s = 12345;
  std::vector<uint8_t> sent;
  double lastReport = -1e9;
  while (now() < simSec) {
    w.round();
    if (sent.size() < w.b.dataAcc.size() + window) {
      std::vector<uint8_t> c(512);
      for (auto& b : c) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        b = uint8_t(s >> 32);
      }
      sent.insert(sent.end(), c.begin(), c.end());
      ::send(w.a.dataFd, c.data(), c.size(), 0);
    }
    if (now() - lastReport > 30.0) {
      lastReport = now();
      std::printf("t=%6.0f  sent=%7zu  delivered=%7zu\n   A %s\n   B %s\n",
                  now(), sent.size(), w.b.dataAcc.size(),
                  w.cmd(w.a, "STATUS").c_str(),
                  w.cmd(w.b, "STATUS").c_str());
      std::fflush(stdout);
    }
  }
  std::printf("REPRO DONE sent=%zu delivered=%zu\n", sent.size(),
              w.b.dataAcc.size());
  return w.b.dataAcc.size() >= 4096 ? 0 : 1;
}
