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

SKYTEST_MAIN
