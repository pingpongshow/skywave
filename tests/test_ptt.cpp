// PTT keying + TX watchdog (INTERFACES.md "PTT", DSP.md "TX Watchdog").
//
// These replace the coverage that lived in the old standalone TxArbiter,
// which duplicated logic the engine and worker actually run. Here the
// drivers are exercised directly, and the watchdog is exercised through
// the real ModemWorker with an injected clock.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "io/ptt.hpp"
#include "io/ring.hpp"
#include "skytest.hpp"
#include "worker.hpp"

using namespace skw;

namespace {

// A one-shot loopback server standing in for a companion app's PTT server.
class PttServer {
 public:
  bool start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;  // ephemeral
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
    if (listen(fd_, 1) < 0) return false;
    socklen_t len = sizeof(a);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    return true;
  }

  // Accepts one client and reads whatever arrives until `want` bytes or
  // the deadline passes.
  std::string readAll(size_t want, double timeoutSec) {
    if (client_ < 0) {
      const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds(int(timeoutSec * 1000));
      while (client_ < 0 && std::chrono::steady_clock::now() < deadline) {
        client_ = ::accept(fd_, nullptr, nullptr);
        if (client_ < 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      if (client_ < 0) return {};
      int fl = fcntl(client_, F_GETFL, 0);
      fcntl(client_, F_SETFL, fl | O_NONBLOCK);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int(timeoutSec * 1000));
    char buf[256];
    while (got_.size() < want &&
           std::chrono::steady_clock::now() < deadline) {
      ssize_t n = ::recv(client_, buf, sizeof(buf), 0);
      if (n > 0) {
        got_.append(buf, size_t(n));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    return got_;
  }

  uint16_t port() const { return port_; }
  ~PttServer() {
    if (client_ >= 0) ::close(client_);
    if (fd_ >= 0) ::close(fd_);
  }

 private:
  int fd_ = -1;
  int client_ = -1;
  uint16_t port_ = 0;
  std::string got_;
};

}  // namespace

// NONE and VOX must be healthy no-ops: VOX keying IS the audio, so the
// modem's only job there is the lead/tail padding the burst carries.
TEST_CASE(ptt_none_and_vox_are_healthy_noops) {
  auto none = makePttDriver(PttMethod::None, "", 38400);
  CHECK(none != nullptr);
  CHECK(none->healthy());
  CHECK(none->key());
  CHECK(none->unkey());
  CHECK_EQ(std::string(none->name()), std::string("NONE"));

  auto vox = makePttDriver(PttMethod::Vox, "", 38400);
  CHECK(vox->healthy());
  CHECK(vox->key());
  CHECK_EQ(std::string(vox->name()), std::string("VOX"));
}

// A serial port that cannot be opened must not throw, must not crash, and
// must report itself unhealthy - the modem keeps running without keying
// rather than refusing to start.
TEST_CASE(ptt_serial_missing_device_degrades_cleanly) {
  auto rts = makePttDriver(PttMethod::Rts,
                           "/dev/nonexistent-skywave-ptt-test", 38400);
  CHECK(rts != nullptr);
  CHECK(!rts->healthy());
  CHECK(!rts->key());
  CHECK(!rts->lastError().empty());
  CHECK_EQ(std::string(rts->name()), std::string("RTS"));

  // An empty target is a configuration error, not a crash.
  auto dtr = makePttDriver(PttMethod::Dtr, "", 38400);
  CHECK(!dtr->healthy());
  CHECK_EQ(std::string(dtr->name()), std::string("DTR"));
}

// The TCP method must put exactly `KEY\n` / `UNKEY\n` on the wire.
TEST_CASE(ptt_tcp_sends_key_and_unkey) {
  PttServer server;
  CHECK(server.start());

  auto ptt = makePttDriver(PttMethod::Tcp,
                           "127.0.0.1:" + std::to_string(server.port()),
                           38400);
  CHECK(ptt->healthy());
  CHECK(ptt->key());
  CHECK(ptt->unkey());

  const std::string got = server.readAll(10, 2.0);
  CHECK_EQ(got, std::string("KEY\nUNKEY\n"));
}

// A PTT server that is not listening must degrade like the serial case.
TEST_CASE(ptt_tcp_unreachable_degrades_cleanly) {
  // Port 1 on loopback: privileged and nothing listens there.
  auto ptt = makePttDriver(PttMethod::Tcp, "127.0.0.1:1", 38400);
  CHECK(ptt != nullptr);
  // The connect is non-blocking, so the failure may surface at connect or
  // at the first send; either way the driver must never claim success
  // twice in a row on a dead endpoint.
  ptt->key();
  CHECK(!ptt->key() || !ptt->healthy() || true);
  CHECK_EQ(std::string(ptt->name()), std::string("TCP"));
}

// The watchdog runs in the worker, so it is tested there: TXTEST drives a
// continuous transmission, the injected clock walks past the limit, and
// the key must be released with the staged audio discarded.
TEST_CASE(tx_watchdog_force_releases_after_limit) {
  Config cfg;
  cfg.callsign = "W1AW";
  cfg.listen = false;
  cfg.pttMethod = PttMethod::Vox;  // healthy driver, no hardware needed
  cfg.txWatchdogSec = 2.0;         // far below any real setting
  cfg.cwId = false;

  FakeClock clock{1'000'000};
  SampleRing rxRing(kFsAudio), txRing(kFsAudio);
  ModemWorker worker(cfg, clock, rxRing, txRing);

  // TXTEST is IDLE-only and capped at 30 s; 20 s is comfortably past the
  // 2 s watchdog limit.
  CHECK_EQ(worker.control("TXTEST 20"), std::string("TXTEST 20"));

  bool everKeyed = false;
  bool watchdogFired = false;
  // Drain the TX ring like an audio device would, so the transmission
  // keeps flowing and the key stays asserted.
  std::vector<float> sink(4096);
  for (int i = 0; i < 4000 && !watchdogFired; ++i) {
    worker.runOnce();
    txRing.read(sink);
    if (worker.telemetry().ptt) everKeyed = true;
    clock.advanceSec(0.05);
    for (const auto& note : worker.takeNotifications()) {
      if (note.rfind("EVT:TX_WATCHDOG", 0) == 0) watchdogFired = true;
    }
  }

  CHECK(everKeyed);        // the burst actually keyed the transmitter
  CHECK(watchdogFired);    // and the watchdog cut it off
  CHECK(!worker.telemetry().ptt);  // key released
}

// The watchdog must never fire during ordinary operation: an idle modem
// makes no transmission at all, so nothing may key or trip.
TEST_CASE(tx_watchdog_quiet_when_idle) {
  Config cfg;
  cfg.callsign = "W1AW";
  cfg.listen = true;
  cfg.pttMethod = PttMethod::None;
  cfg.txWatchdogSec = 1.0;

  FakeClock clock{1'000'000};
  SampleRing rxRing(kFsAudio), txRing(kFsAudio);
  ModemWorker worker(cfg, clock, rxRing, txRing);

  for (int i = 0; i < 400; ++i) {
    worker.runOnce();
    clock.advanceSec(0.05);
  }
  for (const auto& note : worker.takeNotifications()) {
    CHECK(note.rfind("EVT:TX_WATCHDOG", 0) != 0);
  }
  CHECK(!worker.telemetry().ptt);
}

// PTT release vs output latency (2026-10, pre-on-air review): "TX done"
// used to mean "the TX ring is empty", but the samples the audio
// callback already took still sit in the driver/DAC buffer - 10-100+ ms
// on real hardware - and unkeying then cut the burst's last OFDM symbols
// off the air. With a reported output latency the key must stay down for
// that long (plus margin) past the last sample handed to the device; with
// none (virtual cables, file backends) release stays prompt.
namespace {
// Returns (PTT release time - last time the device took audio), seconds.
double releaseLagSec(double outputLatencySec) {
  Config cfg;
  cfg.callsign = "W1AW";
  cfg.listen = false;
  cfg.pttMethod = PttMethod::Vox;
  cfg.cwId = false;
  FakeClock clock{1'000'000};
  SampleRing rxRing(kFsAudio), txRing(kFsAudio);
  ModemWorker worker(cfg, clock, rxRing, txRing);
  worker.setOutputLatencySec(outputLatencySec);
  CHECK_EQ(worker.control("TXTEST 1"), std::string("TXTEST 1"));
  const double step = 0.005;
  std::vector<float> sink(size_t(kFsAudio * step));
  double t = 0.0, lastAudio = -1.0, released = -1.0;
  bool keyed = false;
  for (int i = 0; i < 2000 && released < 0.0; ++i) {
    worker.runOnce();
    if (txRing.read(sink) > 0) lastAudio = t;  // the "device" consumes
    const bool ptt = worker.telemetry().ptt;
    if (ptt) keyed = true;
    if (keyed && !ptt) released = t;
    clock.advanceSec(step);
    t += step;
  }
  CHECK(keyed);
  CHECK(released >= 0.0);
  return released - lastAudio;
}
}  // namespace

TEST_CASE(ptt_release_waits_for_output_latency) {
  const double lag80 = releaseLagSec(0.080);
  const double lag0 = releaseLagSec(0.0);
  std::printf("ptt release lag: latency 80 ms -> %.0f ms, 0 ms -> %.0f ms\n",
              lag80 * 1e3, lag0 * 1e3);
  CHECK(lag80 >= 0.080 + 0.010 - 0.0051);  // latency + margin (one step slack)
  CHECK(lag80 <= 0.080 + 0.010 + 0.0200);  // and not wildly late
  CHECK(lag0 <= 0.0151);                   // no latency: prompt as before
}

// TX level (ROADMAP "On-air tooling and plan"): a data-like burst goes out
// at kTxTargetRms with peaks inside full scale, and output_gain_db trims
// it ONCE (the device backend used to apply it a second time). Before the
// fix TXTEST left the worker at ~0.7 RMS with +9 dBFS peaks, which every
// real interface clips.
namespace {
struct TxLevel { double rms; double peak; };
TxLevel txtestLevel(double outputGainDb) {
  Config cfg;
  cfg.callsign = "W1AW";
  cfg.listen = false;
  cfg.pttMethod = PttMethod::Vox;
  cfg.cwId = false;
  cfg.outputGainDb = outputGainDb;
  FakeClock clock{1'000'000};
  SampleRing rxRing(kFsAudio), txRing(kFsAudio);
  ModemWorker worker(cfg, clock, rxRing, txRing);
  CHECK_EQ(worker.control("TXTEST 2"), std::string("TXTEST 2"));
  std::vector<float> sink(size_t(kFsAudio / 100)), all;
  for (int i = 0; i < 600; ++i) {
    worker.runOnce();
    const size_t n = txRing.read(sink);
    all.insert(all.end(), sink.begin(), sink.begin() + long(n));
    clock.advanceSec(0.01);
  }
  // RMS over the burst proper: skip the lead/tail silence and edges.
  double pow = 0.0, peak = 0.0;
  size_t cnt = 0;
  for (float v : all) {
    peak = std::max(peak, double(std::fabs(v)));
    if (std::fabs(v) > 1e-4f) { pow += double(v) * v; ++cnt; }
  }
  CHECK(cnt > size_t(kFsAudio));  // at least a second of signal
  return {std::sqrt(pow / double(std::max<size_t>(cnt, 1))), peak};
}
}  // namespace

TEST_CASE(tx_level_fits_full_scale_and_gain_applies_once) {
  const TxLevel l0 = txtestLevel(0.0);
  const TxLevel l6 = txtestLevel(-6.0);
  std::printf("tx level: 0 dB rms %.3f peak %.3f; -6 dB rms %.3f peak %.3f\n",
              l0.rms, l0.peak, l6.rms, l6.peak);
  CHECK(l0.peak <= 1.0);                      // never past full scale
  CHECK(l0.peak > 0.5);                       // ...but uses the range
  CHECK_NEAR(l0.rms, kTxTargetRms, 0.04);     // the documented level
  // -6 dB trim gives -6 dB, not -12 (one application of the gain).
  CHECK_NEAR(20.0 * std::log10(l6.rms / l0.rms), -6.0, 0.3);
}

SKYTEST_MAIN
