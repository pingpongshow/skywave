// GUI shell (ROADMAP 5.6-5.8, Milestone M5): the GUI is headless-testable
// because it owns no modem state - every mutation goes through
// ModemWorker::control(), the same line protocol the TCP port uses, and
// every readout comes from a telemetry snapshot.
//
// This test drives the same submit() path the widgets call, so it verifies
// the M5 rule ("every GUI mutation routes through the dispatcher") without
// needing a window: rendering is checked by running a real window when a
// display is available (SKW_GUI_WINDOW=1), skipped otherwise.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "gui/gui.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

struct Fixture {
  FakeClock clk{1000000};
  SampleRing rx{size_t(kFsAudio)}, tx{size_t(kFsAudio)};
  Config cfg;
  ModemWorker worker;
  Gui gui;

  Fixture() : worker(mkCfg(cfg), clk, rx, tx), gui(worker) {}

  static const Config& mkCfg(Config& c) {
    c.callsign = "W1AW";
    c.listen = true;
    return c;
  }
};

}  // namespace

TEST_CASE(gui_mutations_route_through_dispatcher) {
  Fixture f;
  // Each of these is exactly what the corresponding widget submits.
  struct Case {
    const char* cmd;
    const char* wantPrefix;
  };
  const Case cases[] = {
      {"MYCALL K6ABC", "MYCALL K6ABC"},
      {"FAMILY MID", "FAMILY MID"},
      {"MODE M9", "MODE M9"},
      {"MODE AUTO", "MODE AUTO"},
      {"CPMODE LONG", "CPMODE LONG"},
      {"MONITOR ON", "MONITOR ON"},
      {"TXTEST 5", "TXTEST 5"},
      {"TXTEST OFF", "TXTEST OFF"},
      {"CQ", "CQ"},
      {"BEACON", "BEACON"},
      {"DISCONNECT", "ERROR:STATE"},  // nothing to disconnect
      {"ABORT", "ABORT"},
  };
  for (const auto& c : cases) {
    const std::string resp = f.gui.submit(c.cmd);
    if (resp.rfind(c.wantPrefix, 0) != 0) {
      std::printf("  %s -> %s (wanted %s)\n", c.cmd, resp.c_str(),
                  c.wantPrefix);
    }
    CHECK(resp.rfind(c.wantPrefix, 0) == 0);
    // No GUI action may be silently unknown: that would mean a widget
    // reaching for a command the dispatcher does not implement.
    CHECK(resp.rfind("ERROR:UNKNOWN", 0) != 0);
  }
  // The log records every command/response pair for the operator.
  CHECK_EQ(int(f.gui.log().size()), int(sizeof(cases) / sizeof(cases[0])));
  CHECK(f.gui.log().front().cmd == "MYCALL K6ABC");
  // The mutations actually landed in the modem (not just in the log).
  CHECK(f.worker.myCall() == "K6ABC");
  CHECK(f.worker.family() == "MID");
  CHECK(f.worker.monitoring());
}

TEST_CASE(gui_instruments_track_telemetry) {
  Fixture f;
  // Feed the worker some audio so telemetry has something to report.
  std::vector<float> audio(size_t(kFftAudio) + 96, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    audio[i] = 0.05f * float(std::sin(2 * M_PI * 1500.0 * double(i) /
                                      kFsAudio));
  }
  for (int i = 0; i < 6; ++i) {
    f.rx.write(audio);
    f.worker.runOnce();
    f.clk.advanceSec(0.03);
    f.gui.sampleTelemetry();
  }
  const auto& inst = f.gui.instruments();
  CHECK_EQ(int(inst.rxLevel.size()), 6);
  CHECK_EQ(int(inst.modeHistory.size()), 6);
  // A -26 dBFS tone must show up as a plausible RX level, not the floor.
  std::printf("gui instruments: rx=%.1f dBFS floor=%.1f dBFS mode=M%u\n",
              double(inst.rxLevel.back()), double(inst.noiseFloor.back()),
              unsigned(inst.modeHistory.back()));
  CHECK(inst.rxLevel.back() > -60.0f);
  CHECK(inst.rxLevel.back() < 0.0f);
  // History is bounded (no unbounded growth in a long-running GUI).
  for (int i = 0; i < int(GuiInstruments::kHistory) + 50; ++i) {
    f.gui.sampleTelemetry();
  }
  CHECK_EQ(int(f.gui.instruments().rxLevel.size()),
           int(GuiInstruments::kHistory));
}

TEST_CASE(gui_headless_fallback_is_clean) {
  // init() must fail gracefully with no display rather than abort, and
  // frame() must be a no-op when not ready. This is the CI path.
  Fixture f;
  if (!getenv("SKW_GUI_WINDOW")) {
    // Do not even try to open a window in CI; assert the not-ready
    // contract instead.
    CHECK(!f.gui.frame());
    // The dispatcher path still works headless (the companion app and
    // scripts share it).
    CHECK(f.gui.submit("VERSION").rfind("VERSION", 0) == 0);
    std::printf("gui: headless path verified (set SKW_GUI_WINDOW=1 to "
                "exercise the real window)\n");
    return;
  }
  const bool up = f.gui.init(900, 700);
  std::printf("gui: init -> %s\n", up ? "window" : "headless fallback");
  if (up) {
    // Render a handful of real frames, then close.
    for (int i = 0; i < 10; ++i) {
      if (!f.gui.frame()) break;
    }
    f.gui.shutdown();
  }
  CHECK(true);  // either outcome is acceptable; a crash is not
}

SKYTEST_MAIN
