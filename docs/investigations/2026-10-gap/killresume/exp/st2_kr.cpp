// In-process kill/resume reproducer: ST2 world, peer death, with the
// ambient floor set by SKW_ST2_AMBIENT (0 = exact digital silence, as a
// virtual audio cable delivers). Reports worker CPU per simulated second
// during the dead-peer period (>1.0 = a real device would overrun its RX
// ring), and whether/when the link reconnects.
#include <chrono>
#include "st2_world.hpp"
using namespace st2w;
int main() {
  World w(24.0, 7);
  auto sim = [&] { return w.clk.nowUs() / 1e6; };
  if (w.cmd(w.a, "CONNECT K6XYZ") != "CONNECT K6XYZ") return 2;
  if (!w.waitState(w.a, "CONNECTED", 3000)) { puts("initial connect FAIL"); return 1; }
  std::vector<uint8_t> payload(32768);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 131 + 7);
  ::send(w.a.dataFd, payload.data(), payload.size(), 0);
  for (int i = 0; i < 8000 && w.b.dataAcc.size() < 4096; ++i) w.round();
  w.b.w.reset();
  w.b.w.emplace(w.b.cfg, w.clk, w.b.rxRing, w.b.txRing);
  w.cmd(w.b, "LISTEN ON");
  const double t0 = sim();
  auto c0 = std::chrono::steady_clock::now();
  bool dropped = w.waitState(w.a, "LISTENING", 12000);
  double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
  printf("dead-peer period: %.0f sim-s, %.1f cpu-s -> real-time factor %.2f (A noticed: %d)\n",
         sim() - t0, cpu, cpu / (sim() - t0), int(dropped));
  const double t1 = sim();
  w.cmd(w.a, "CONNECT K6XYZ");
  c0 = std::chrono::steady_clock::now();
  bool aUp = w.waitState(w.a, "CONNECTED", 8000);
  bool bUp = w.waitState(w.b, "CONNECTED", 1000);
  cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
  printf("reconnect: A %d B %d after %.0f sim-s (real-time factor %.2f)\n", int(aUp), int(bUp), sim() - t1, cpu / (sim() - t1));
  return aUp && bUp ? 0 : 1;
}
