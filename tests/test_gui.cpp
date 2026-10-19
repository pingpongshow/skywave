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

#include <cmath>

#include "core/clock.hpp"
#include "io/ring.hpp"
#include "worker.hpp"
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

TEST_CASE(gui_log_never_shows_the_psk) {
  // C10: the command log is on screen; ENCRYPTKEY's argument is the PSK.
  Fixture f;
  const std::string psk =
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  f.gui.submit("ENCRYPTKEY " + psk);
  f.gui.submit("encryptkey " + psk);
  for (const auto& l : f.gui.log()) {
    CHECK(l.cmd.find(psk) == std::string::npos);
    CHECK(l.cmd.find("********") != std::string::npos);
  }
  CHECK(Gui::redactForLog("STATUS") == "STATUS");
  CHECK(Gui::redactForLog("ENCRYPTKEY") == "ENCRYPTKEY");
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


// The waterfall and constellation need real DSP taps through the
// telemetry seqlock - the gap that deferred them since step 5.7. This
// drives the whole path: audio in -> worker spectrum -> snapshot.
TEST_CASE(instrument_taps_reach_telemetry) {
  Config cfg;
  cfg.callsign = "W1AW";
  cfg.listen = true;
  cfg.guiEnabled = true;   // enables the constellation tap
  cfg.pttMethod = PttMethod::None;

  FakeClock clock{1000000};
  SampleRing rxRing(kFsAudio), txRing(kFsAudio);
  ModemWorker worker(cfg, clock, rxRing, txRing);

  // Feed a steady tone so the spectrum has something to show.
  std::vector<float> tone(size_t(kFsAudio / 10));
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.2f * std::sin(2.0f * float(M_PI) * 1500.0f *
                              float(i) / float(kFsAudio));
  }

  uint32_t seq = 0;
  for (int i = 0; i < 60 && seq == 0; ++i) {
    rxRing.write(tone);
    worker.runOnce();
    clock.advanceSec(0.1);
    seq = worker.telemetry().spectrumSeq;
  }
  const Telemetry t = worker.telemetry();
  CHECK(t.spectrumSeq > 0);            // a spectrum was published

  // The tone must actually appear: at least one bin well above the
  // quietest. A flat spectrum would mean the tap is wired to nothing.
  float lo = 0.0f, hi = -200.0f;
  for (int i = 0; i < kSpectrumBins; ++i) {
    lo = std::min(lo, t.spectrumDbfs[i]);
    hi = std::max(hi, t.spectrumDbfs[i]);
  }
  std::printf("gui spectrum: %d bins, %.1f .. %.1f dBFS (seq %u)\n",
              kSpectrumBins, double(lo), double(hi), unsigned(t.spectrumSeq));
  CHECK(hi > lo + 10.0f);

  // The mask overlay must describe the active family, not a stale width.
  CHECK_EQ(int(t.maskCarriers), plan(cfg.family).carriers());
  // Nothing masked on a fresh session: every carrier bit set.
  CHECK(t.maskBits[0] == 0xFF);
}

SKYTEST_MAIN

// Every widget action, driven exactly as a click drives it (the async
// executor path), must change the modem - and a refused one must put its
// widget back and say why. Before this the widgets assumed success: FAMILY
// ULTRA was refused (ultra_enable off) while the combo went on showing
// ULTRA, with the reason only in the log.
TEST_CASE(gui_widget_actions_reach_modem) {
  Fixture f;
  Gui& g = f.gui;
  g.syncFromModem();
  auto& c = g.controls();
  auto settle = [&] { CHECK(g.waitIdle(5000)); };
  auto noticeIs = [&](bool error) {
    if (g.notice().error != error) std::printf("  notice: %s\n", g.notice().text.c_str());
    return g.notice().error == error;
  };
  CHECK_EQ(c.family, 0);  // WIDE
  CHECK(!c.ultra);

  // ULTRA from the family list: one choice enables the gate and switches.
  int prev = c.family;
  c.family = 3;
  g.actFamily(prev);
  settle();
  CHECK(f.worker.family() == "ULTRA");
  CHECK(c.ultra);
  CHECK(noticeIs(false));
  // The gate cannot be closed under the family in use: refused, reverted.
  c.ultra = false;
  g.actUltra();
  settle();
  CHECK(c.ultra);
  CHECK(noticeIs(true));
  CHECK(f.worker.config().ultraEnable);
  prev = c.family;
  c.family = 1;
  g.actFamily(prev);
  settle();
  CHECK(f.worker.family() == "MID");
  c.ultra = false;
  g.actUltra();
  settle();
  CHECK(!f.worker.config().ultraEnable);

  // Mode / CP / highest mode.
  prev = c.mode;
  c.mode = 5;  // M8
  g.actMode(prev);
  prev = c.cp;
  c.cp = 3;  // LONG
  g.actCp(prev);
  prev = c.maxMode;
  c.maxMode = 6;  // M10
  g.actMaxMode(prev);
  settle();
  CHECK_EQ(f.worker.config().mode, 8);
  CHECK_EQ(f.worker.config().cpMode, 2);
  CHECK_EQ(f.worker.maxMode(), 10);
  prev = c.mode;
  c.mode = 0;
  g.actMode(prev);
  settle();
  CHECK_EQ(f.worker.config().mode, -1);

  // Station toggles.
  std::snprintf(c.call, sizeof(c.call), "K6ABC");
  g.actCall();
  c.listen = false;
  g.actListen();
  c.monitor = true;
  g.actMonitor();
  c.cwid = true;
  g.actCwid();
  settle();
  CHECK(f.worker.myCall() == "K6ABC");
  CHECK(!f.worker.listening());
  CHECK(f.worker.monitoring());
  CHECK(f.worker.cwId());
  // A refused callsign goes back to the modem's.
  f.worker.runOnce();
  g.refreshStatus();
  std::snprintf(c.call, sizeof(c.call), "!!");
  g.actCall();
  settle();
  CHECK(std::string(c.call) == "K6ABC");
  CHECK(noticeIs(true));

  // PTT.
  prev = c.pttMethod;
  c.pttMethod = 1;  // VOX
  g.actPttMethod(prev);
  c.pttLead = 120;
  c.pttTail = 40;
  g.actPttTimes();
  std::snprintf(c.pttPort, sizeof(c.pttPort), "127.0.0.1:4532");
  g.actPttPort();
  settle();
  CHECK(f.worker.pttGet("METHOD") == "vox" || f.worker.pttGet("METHOD") == "VOX");
  CHECK(f.worker.pttGet("LEAD") == "120");
  CHECK(f.worker.pttGet("TAIL") == "40");
  CHECK(f.worker.pttGet("PORT") == "127.0.0.1:4532");
  c.pttLead = 9999;  // out of range: refused, back to the accepted value
  g.actPttTimes();
  settle();
  CHECK_EQ(c.pttLead, 120);
  CHECK(noticeIs(true));

  // Session buttons: an idle modem calls, CQs and beacons; DISCONNECT
  // with nothing to disconnect is explained, not silently logged.
  g.actTxTest(true);
  g.actTxTest(false);
  g.actCq();
  g.actAbort();
  g.actBeacon();
  g.actAbort();
  settle();
  CHECK(noticeIs(false));
  g.actDisconnect();
  settle();
  CHECK(noticeIs(true));
  std::snprintf(c.connect, sizeof(c.connect), "W1XYZ");
  g.actConnect();
  settle();
  CHECK(noticeIs(false));
  g.actAbort();
  settle();

  // Security: a non-crypto build answers UNSUPPORTED and the GUI says so.
  g.actSecurityRefresh();
  settle();
#ifndef SKW_HAVE_CRYPTO
  c.encrypt = true;
  g.actEncryption();
  settle();
  CHECK(!c.encrypt);
  CHECK(noticeIs(true));
#else
  c.encrypt = true;  // no key yet: refused, reverted
  g.actEncryption();
  settle();
  CHECK(!c.encrypt);
  std::snprintf(c.psk, sizeof(c.psk), "%s",
                "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  g.actInstallPsk();
  settle();
  CHECK(c.psk[0] == '\0');  // wiped from the widget
  c.encrypt = true;
  g.actEncryption();
  settle();
  CHECK(c.encrypt);
  CHECK(f.worker.config().cryptoEnabled);
#endif

  // Changes made elsewhere (control port, companion app) move the widgets.
  f.gui.submit("FAMILY NARROW");
  f.gui.submit("MODE M9");
  for (int i = 0; i < 4; ++i) {
    std::vector<float> quiet(size_t(kFsAudio / 10), 0.0f);
    f.rx.write(quiet);
    f.worker.runOnce();
    f.clk.advanceSec(0.3);
  }
  g.refreshStatus();
  CHECK_EQ(c.family, 2);
  CHECK_EQ(c.mode, 6);  // M9

  // The placeholder callsign: CQ is explained and not sent (no station
  // answers N0CALL).
  f.gui.submit("MYCALL N0CALL");
  for (int i = 0; i < 2; ++i) {
    std::vector<float> quiet(size_t(kFsAudio / 10), 0.0f);
    f.rx.write(quiet);
    f.worker.runOnce();
    f.clk.advanceSec(0.3);
  }
  g.refreshStatus();
  const size_t logBefore = g.log().size();
  g.actCq();
  settle();
  CHECK(noticeIs(true));
  CHECK_EQ(int(g.log().size()), int(logBefore));  // nothing went to the modem
}
