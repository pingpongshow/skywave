// soak_st2: long impaired-channel endurance scenarios over the ST2 world
// (TESTING.md). Simulated time - hours of link life run in minutes of
// wall clock, through the full worker stack and the radio-chain
// emulator. The pass criterion is byte-exact delivery + no stall/wedge
// while the channel is usable, with session loss and reconnect allowed
// (and exercised) when it is not.
//
// Scenarios (env SKW_SOAK_SCENARIO):
//   churn    - SNR staircases 30 -> -14 -> 30 dB with random jumps while
//              the channel preset rotates static/good/moderate; AMC must
//              ride the whole ladder, sessions may die at the bottom but
//              must recover, and every delivered byte must be exact.
//   floor    - connect clean, then hold SKW_SOAK_SNR (default +6 dB)
//              on CCIR Moderate, one-way load: retransmission
//              machinery under continuous marginal-SNR grind.
//   blackout - good channel, but periodic 10-60 s total dead air in both
//              directions mid-transfer: ARQ retry budgets, keepalives,
//              reconnect, and stream continuity.
// Env: SKW_SOAK_SIMSEC (default 900), SKW_SOAK_SEED, SKW_SOAK_SNR.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "st2_world.hpp"

using namespace skw;
using namespace st2w;

namespace {

struct Gen {  // deterministic per-direction stream
  uint64_t s;
  std::vector<uint8_t> sent;
  explicit Gen(uint64_t seed) : s(seed) {}
  std::vector<uint8_t> chunk(size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      b = uint8_t(s >> 32);
    }
    sent.insert(sent.end(), v.begin(), v.end());
    return v;
  }
};

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545f4914f6cdd1dull;
  }
  uint32_t u32(uint32_t bound) { return bound ? uint32_t(next() % bound) : 0; }
};

int fail(const char* what, double simSec) {
  std::printf("SOAK FAIL at t=%.0fs: %s\n", simSec, what);
  return 1;
}

void applyPreset(World& w, int preset, double snrDb) {
  for (Shuttle* sh : {&w.ab, &w.ba}) {
    ChansimParams& c = sh->chain.channel;
    const uint64_t seed = sh->chain.seed;
    switch (preset) {
      case 0:
        c = ChansimParams{};
        c.fading = false;
        break;
      case 1:
        c = ccirGood();
        break;
      default:
        c = ccirModerate();
        break;
    }
    c.snr3kDb = snrDb;
    sh->chain.seed = seed;
  }
}

const char* presetName(int p) {
  return p == 0 ? "static" : p == 1 ? "good" : "moderate";
}

}  // namespace

int main() {
  const char* e;
  std::string scen = (e = std::getenv("SKW_SOAK_SCENARIO")) ? e : "churn";
  const double simSecTarget =
      (e = std::getenv("SKW_SOAK_SIMSEC")) ? std::atof(e) : 900.0;
  const uint64_t seed =
      (e = std::getenv("SKW_SOAK_SEED")) ? uint64_t(std::atoll(e)) : 11;
  // Default +9 dB set-point: the ST2 world's radio-chain bandpass and
  // fading dips put the modem's MEASURED eff ~6 dB below the set
  // point, so this lands just above the WIDE robust-tier floor. The
  // scenario's purpose is sustained marginal-SNR retransmission grind;
  // bertool owns exact floor calibration at clean baseband.
  const double floorSnr =
      (e = std::getenv("SKW_SOAK_SNR")) ? std::atof(e) : 9.0;

  Rng rng(seed * 40503 + 7);
  double snr = scen == "churn" ? 30.0 : scen == "floor" ? floorSnr : 20.0;
  int preset = scen == "floor" ? 2 : scen == "blackout" ? 1 : 0;

  World w(snr, seed);
  // The floor scenario tests the RETRANSMISSION machinery at the tier
  // floor, not handshake acquisition: connect on a clean channel first
  // (links degrade after establishment in reality too), then drop to
  // the floor for the whole soak.
  const bool floorScen = scen == "floor";
  // Blackout tests DEAD-AIR RECOVERY (ARQ retries, keepalives,
  // reconnect, resume), not two-way saturation - simultaneous two-way
  // bulk is a documented open limitation and pumping both directions
  // made this scenario measure that instead of what it names.
  const bool oneWay = floorScen || scen == "blackout";
  applyPreset(w, floorScen ? 0 : preset, floorScen ? 15.0 : snr);
  Gen gA(seed * 7 + 1), gB(seed * 7 + 2);

  // Floor-SNR handshakes legitimately need several CALL/START retry
  // ladders; give three full attempts before declaring failure.
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; ++attempt) {
    if (w.cmd(w.a, "CONNECT K6XYZ").rfind("CONNECT", 0) != 0 &&
        attempt == 0) {
      return fail("CONNECT refused", 0);
    }
    connected = w.waitState(w.a, "CONNECTED", 12000);
  }
  if (!connected) return fail("initial connect", 0);
  if (floorScen) applyPreset(w, preset, snr);

  const double t0 = double(w.clk.nowUs()) * 1e-6;
  // Churn staircase state.
  double dwellLeft = 60.0;
  int snrDir = -1;
  // Blackout scheduler state.
  double boPhase = 90.0;
  bool boActive = false;
  int boIdx = 0;
  const double boLens[4] = {10.0, 25.0, 45.0, 60.0};
  // Progress watchdog.
  size_t lastDeliveredSum = 0;
  double lastProgressAt = 0.0, lastReportAt = 0.0, lastPollAt = 0.0;
  double usableSince = -1.0;  // sim time snr has been >= 5 dB
  size_t verifiedA = 0, verifiedB = 0;
  int reconnects = 0;
  double callingSince = -1.0;

  auto simNow = [&] { return double(w.clk.nowUs()) * 1e-6 - t0; };

  while (simNow() < simSecTarget) {
    w.round();
    const double now = simNow();

    // --- keep the directions loaded (<= 4 KiB in flight each). The
    // floor scenario is ONE-WAY: the documented tier floors are one-way
    // FER gates, and sustained simultaneous two-way bulk at the floor
    // is a known-limitation regime (ROADMAP).
    if (!boActive) {
      if (gA.sent.size() < w.b.dataAcc.size() + 4096) {
        auto c = gA.chunk(256 + rng.u32(768));
        ::send(w.a.dataFd, c.data(), c.size(), 0);
      }
      if (!oneWay && gB.sent.size() < w.a.dataAcc.size() + 4096) {
        auto c = gB.chunk(256 + rng.u32(768));
        ::send(w.b.dataFd, c.data(), c.size(), 0);
      }
    }

    // --- verify newly delivered bytes (incremental, byte-exact) --------
    if (w.b.dataAcc.size() > verifiedA) {
      if (w.b.dataAcc.size() > gA.sent.size()) {
        return fail("B received more than A sent", now);
      }
      if (std::memcmp(w.b.dataAcc.data() + verifiedA,
                      gA.sent.data() + verifiedA,
                      w.b.dataAcc.size() - verifiedA) != 0) {
        return fail("A->B payload mismatch", now);
      }
      verifiedA = w.b.dataAcc.size();
    }
    if (w.a.dataAcc.size() > verifiedB) {
      if (w.a.dataAcc.size() > gB.sent.size()) {
        return fail("A received more than B sent", now);
      }
      if (std::memcmp(w.a.dataAcc.data() + verifiedB,
                      gB.sent.data() + verifiedB,
                      w.a.dataAcc.size() - verifiedB) != 0) {
        return fail("B->A payload mismatch", now);
      }
      verifiedB = w.a.dataAcc.size();
    }

    // --- scenario schedule ---------------------------------------------
    if (scen == "churn") {
      dwellLeft -= kRoundSec;
      if (dwellLeft <= 0.0) {
        dwellLeft = 45.0 + double(rng.u32(105));
        if (rng.u32(5) == 0) {
          snr += double(rng.u32(9)) - 4.0;  // random jump
        } else {
          snr += snrDir * (2.0 + double(rng.u32(3)));
        }
        if (snr <= -14.0) {
          snr = -14.0;
          snrDir = 1;
        }
        if (snr >= 30.0) {
          snr = 30.0;
          snrDir = -1;
        }
        if (rng.u32(3) == 0) preset = int(rng.u32(3));
        applyPreset(w, preset, snr);
      }
    } else if (scen == "blackout") {
      boPhase -= kRoundSec;
      if (boPhase <= 0.0) {
        boActive = !boActive;
        boPhase = boActive ? boLens[boIdx++ % 4] : 90.0;
        w.ab.drop = w.ba.drop = boActive;
      }
    }

    // --- reconnect + watchdog (poll state sparingly: cmd() runs rounds)
    if (now - lastPollAt > 20.0) {
      lastPollAt = now;
      const std::string sa = w.cmd(w.a, "STATE");
      if (sa == "STATE LISTENING" || sa == "STATE IDLE") {
        w.cmd(w.a, "CONNECT K6XYZ");
        ++reconnects;
        callingSince = -1.0;
      } else if (sa == "STATE CALLING" || sa == "STATE CONNECTING") {
        // A call ladder is running. It must CONVERGE: deep-tier legs
        // are legitimately minutes long, but a ladder that outlives
        // its own worst case means the session is neither connected
        // nor recovering - the state the modem must never occupy.
        if (callingSince < 0.0) callingSince = now;
        if (now - callingSince > 600.0) {
          return fail("call ladder never converged (600 s in CALLING)", now);
        }
      } else {
        callingSince = -1.0;
      }
      const size_t sum = w.a.dataAcc.size() + w.b.dataAcc.size();
      if (sum > lastDeliveredSum) {
        lastDeliveredSum = sum;
        lastProgressAt = now;
      }
      const bool usable = snr >= 5.0 && !boActive;
      if (usable && usableSince < 0.0) usableSince = now;
      if (!usable) usableSince = -1.0;
      // Stall: a usable channel and zero delivered bytes. Deep-tier
      // links (floor scenario) deliver the 4 KiB coalesce lump in one
      // piece over minutes at ~260 bps - give them the deep timescale.
      const double stallSec = floorScen ? 900.0 : 300.0;
      if (usableSince >= 0.0 && now - usableSince > stallSec &&
          now - lastProgressAt > stallSec) {
        return fail("stall: usable channel but no progress", now);
      }
    }

    if (now - lastReportAt > 120.0) {
      lastReportAt = now;
      std::printf(
          "SOAK t=%5.0fs snr=%5.1f %-8s bo=%d A->B=%zu B->A=%zu recon=%d\n",
          now, snr, presetName(preset), boActive ? 1 : 0, w.b.dataAcc.size(),
          w.a.dataAcc.size(), reconnects);
      if (std::getenv("SKW_SOAK_DEBUG")) {
        std::printf("  A %s\n  B %s\n", w.cmd(w.a, "STATUS").c_str(),
                    w.cmd(w.b, "STATUS").c_str());
      }
      std::fflush(stdout);
    }
  }

  // Final: at least SOME traffic must have flowed, byte-exact.
  if (w.b.dataAcc.size() < 4096 ||
      (!oneWay && w.a.dataAcc.size() < 4096)) {
    return fail("too little delivered over the whole run", simNow());
  }
  std::printf(
      "SOAK PASS %s: %.0f sim-s, A->B %zu B, B->A %zu B, %d reconnects\n",
      scen.c_str(), simSecTarget, w.b.dataAcc.size(), w.a.dataAcc.size(),
      reconnects);
  return 0;
}
