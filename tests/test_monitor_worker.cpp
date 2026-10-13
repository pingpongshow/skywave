// Passive monitoring through the worker's capture ladder (INTERFACES.md
// "MONITOR ON"). A station that is NOT listening never runs the engine on
// a capture, so the burst's airtime is never learned from its header and
// the capture used to climb the whole retry ladder: the monitor's single
// look came at the last rung, ~90 s after the frame. Found on the bench
// (2026-10-12): a monitor-only modem decoding a Web-888 live printed no
// MON: line inside an 85 s run although every frame decoded offline.
#include <cstdio>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "io/ring.hpp"
#include "skytest.hpp"
#include "worker.hpp"

using namespace skw;

namespace {

struct MonResult {
  double latencySec = -1.0;  // last MON line after A's last unkey
  int monLines = 0;
  int monWithPayload = 0;
  int broadcasts = 0;
};

// A transmits a BEACON and a BCAST (each a multi-second robust frame); B,
// with MONITOR ON and listening as given, must report both with their
// payloads and reassemble the broadcast.
MonResult runMonitor(bool listen) {
  Config ca;
  ca.callsign = "W1AW";
  ca.listen = false;
  ca.pttMethod = PttMethod::Vox;
  ca.cwId = false;
  Config cb = ca;
  cb.callsign = "K6XYZ";
  cb.listen = listen;

  FakeClock clock{1'000'000};
  SampleRing aRx(kFsAudio), aTx(kFsAudio * 4);
  SampleRing bRx(kFsAudio * 8), bTx(kFsAudio);
  ModemWorker a(ca, clock, aRx, aTx);
  ModemWorker b(cb, clock, bRx, bTx);
  CHECK_EQ(b.control("MONITOR ON"), std::string("MONITOR ON"));
  CHECK_EQ(a.control("BEACON"), std::string("BEACON"));
  // "HELLO MONITOR" as hex: one BCAST_DATA fragment.
  CHECK(a.control("BCAST 48454C4C4F204D4F4E49544F52").rfind("BCAST", 0) == 0);

  const double step = 0.02;
  std::vector<float> chunk(size_t(kFsAudio * step));
  uint32_t rng = 12345;  // ambient floor: real receivers never give zero
  double t = 0.0, lastUnkey = -1.0, lastMon = -1.0;
  bool wasKeyed = false;
  MonResult r;
  for (int i = 0; i < int(150.0 / step); ++i) {
    a.runOnce();
    b.runOnce();
    const size_t n = aTx.read(chunk);
    for (size_t k = n; k < chunk.size(); ++k) chunk[k] = 0.0f;
    for (auto& v : chunk) {
      rng = rng * 1664525u + 1013904223u;
      v += 1e-3f * (float(rng >> 8) / float(1u << 24) - 0.5f);
    }
    bRx.write(chunk);
    bTx.read(chunk);
    const bool keyed = a.telemetry().ptt;
    if (wasKeyed && !keyed) lastUnkey = t;
    wasKeyed = keyed;
    for (const auto& note : b.takeNotifications()) {
      if (note.rfind("MON:", 0) == 0) {
        ++r.monLines;
        if (note.back() != ':') ++r.monWithPayload;
        lastMon = t;
        std::printf("  listen %s +%.1fs %s\n", listen ? "on " : "off", t,
                    note.substr(0, 48).c_str());
      } else if (note.rfind("EVT:BROADCAST", 0) == 0) {
        ++r.broadcasts;
      }
    }
    if (r.monLines >= 2 && r.broadcasts >= 1 && t > lastUnkey + 1.0) break;
    clock.advanceSec(step);
    t += step;
  }
  if (lastMon >= 0.0 && lastUnkey >= 0.0) r.latencySec = lastMon - lastUnkey;
  return r;
}

}  // namespace

TEST_CASE(monitor_reports_promptly_when_not_listening) {
  for (bool listen : {false, true}) {
    const MonResult r = runMonitor(listen);
    std::printf("listen %s: %d MON (%d with payload), %d broadcast(s), "
                "last MON %.1f s after the last unkey\n",
                listen ? "on" : "off", r.monLines, r.monWithPayload,
                r.broadcasts, r.latencySec);
    CHECK_EQ(r.monLines, 2);         // beacon + bcast, each reported once
    CHECK_EQ(r.monWithPayload, 2);   // with payloads, not header-only
    CHECK_EQ(r.broadcasts, 1);       // chain reassembled
    // Was ~90 s listening off (the last ladder rung); a header-only
    // report on a partial window was the intermediate failure.
    CHECK(r.latencySec >= 0.0 && r.latencySec < 5.0);
  }
}

SKYTEST_MAIN
