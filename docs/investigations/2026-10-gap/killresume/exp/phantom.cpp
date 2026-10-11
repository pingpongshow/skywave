#include "dsp/chirp.hpp"
#include "core/constants.hpp"
#include <cstdio>
#include <random>
using namespace skw;
int main(int argc, char** argv) {
  double amp = argc > 1 ? atof(argv[1]) : 0.0;
  double secs = 60;
  ChirpDetector det(Family::Wide);
  std::mt19937 rng(1);
  std::normal_distribution<float> g(0, 1);
  std::vector<ChirpDetection> out;
  size_t total = size_t(secs * kFsBaseband), blk = 512; int n = 0;
  // 3 s of preamble-free noise at amp, then silence/amp
  for (size_t i = 0; i < total; i += blk) {
    std::vector<cfloat> x(blk);
    for (auto& v : x) v = cfloat(amp * g(rng), amp * g(rng));
    out.clear();
    det.process(x, out);
    for (auto& d : out) { if (n < 5) printf("det start=%.2fs q=%.3f sig=%d\n", d.startSample / kFsBaseband, d.quality, int(d.sig)); ++n; }
  }
  printf("amp=%g: %d detections in %.0f s (noiseNcc=%g)\n", amp, n, secs, det.noiseFloorNcc());
}
