// System test layer ST2 (TESTING.md): process-level loopback. The REAL
// worker stack - ModemWorker (engine + streaming RX + converters +
// telemetry + command dispatch) behind REAL TCP data/control ports - with
// an in-process audio shuttle and the radio-chain emulator interposed.
// Scripted the way an application would: control-port commands, STATUS
// JSON scraping, data through the data socket, EVT: assertions, fault
// injection (audio dropout, ring starvation, ABORT, client reconnect,
// simultaneous TX).
#include "skytest.hpp"
#include "st2_world.hpp"

using namespace skw;
using namespace st2w;
TEST_CASE(st2_scripted_session_over_tcp) {
  World w(24.0, 42);
  // Application-style bring-up: identity, status scrape, connect.
  CHECK(w.cmd(w.a, "MYCALL") == "MYCALL W1AW");
  CHECK(w.cmd(w.b, "STATE") == "STATE LISTENING");
  auto js = w.cmd(w.a, "STATUS JSON");
  CHECK(js.find("\"state\":\"LISTENING\"") != std::string::npos);
  CHECK(js.find("\"call\":\"W1AW\"") != std::string::npos);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 3000));
  CHECK(w.waitState(w.b, "CONNECTED", 500));
  CHECK(w.a.saw("EVT:CONNECTED K6XYZ"));
  CHECK(w.b.saw("EVT:CONNECTED W1AW"));
  CHECK(w.a.saw("EVT:STATE CONNECTED"));

  // Data through the data port, both directions.
  const std::string msgA = "ST2: the quick brown fox jumps over the lazy dog";
  const std::string msgB = "ST2: reply from K6XYZ";
  ::send(w.a.dataFd, msgA.data(), msgA.size(), 0);
  ::send(w.b.dataFd, msgB.data(), msgB.size(), 0);
  for (int i = 0;
       i < 4000 && (w.b.dataAcc.size() < msgA.size() ||
                    w.a.dataAcc.size() < msgB.size());
       ++i) {
    w.round();
  }
  CHECK(std::string(w.b.dataAcc.begin(), w.b.dataAcc.end()) == msgA);
  CHECK(std::string(w.a.dataAcc.begin(), w.a.dataAcc.end()) == msgB);

  // Observability while connected.
  auto st = w.cmd(w.a, "STATUS");
  CHECK(st.find("STATE=CONNECTED") != std::string::npos);
  CHECK(st.find("REMOTE=K6XYZ") != std::string::npos);
  CHECK(w.cmd(w.a, "NOISEFLOOR").rfind("NOISEFLOOR ", 0) == 0);
  CHECK(w.cmd(w.a, "AUDIOLEVEL").rfind("AUDIOLEVEL RX=", 0) == 0);
  CHECK(w.cmd(w.a, "FREQINFO").rfind("FREQINFO CENTER=1500", 0) == 0);

  // Orderly teardown with events. Budget reflects HALF-DUPLEX timing:
  // every frame (including DISC/DISC_ACK) waits for a clear channel
  // plus anti-collision jitter, and robust-tier bursts run seconds
  // each - the old 1500-round budget encoded the pre-LBT overlap.
  CHECK(w.cmd(w.a, "DISCONNECT") == "DISCONNECT");
  CHECK(w.waitState(w.a, "LISTENING", 3500));
  CHECK(w.a.saw("EVT:DISCONNECTED NORMAL"));
  std::printf("st2 session: %zu B ctrl trace at A\n", w.a.ctrlAcc.size());
}

TEST_CASE(st2_faults_dropout_starvation_reconnect_abort) {
  World w(24.0, 77);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 3000));

  // Long transfer with a mid-air audio dropout AND a receiver stall
  // (ring starvation): ARQ + retransmission must carry it.
  std::vector<uint8_t> blob(3 * 1024);
  uint64_t s = 99;
  for (auto& v : blob) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    v = uint8_t(s >> 32);
  }
  ::send(w.a.dataFd, blob.data(), blob.size(), 0);
  // 26000 rounds: half-duplex LBT serializes what previously
  // overlapped, the dropout/starvation recovery adds ARQ cycles on
  // top, and the 3 KiB message is delivered as ONE unit at the end
  // (probe-measured completion ~18-24k rounds on this seed).
  for (int i = 0; i < 26000 && w.b.dataAcc.size() < blob.size(); ++i) {
    w.ab.drop = (i >= 300 && i < 500);      // ~5.7 s of dead air
    w.starveB = (i >= 900 && i < 1000);     // B's worker stalls ~2.9 s
    w.round();
  }
  w.ab.drop = false;
  w.starveB = false;
  CHECK(w.b.dataAcc == blob);

  // Control client drops and reconnects mid-session: commands keep
  // working on the new connection. Close first, let the server's poll
  // notice the drop, then reconnect (one-client-per-port policy).
  ::close(w.a.ctrlFd);
  w.a.ctrlFd = -1;
  for (int i = 0; i < 5; ++i) w.round();
  w.a.reconnectCtrl();
  for (int i = 0; i < 10; ++i) w.round();
  CHECK(w.cmd(w.a, "STATE") == "STATE CONNECTED");
  // Data client reconnect: subsequently delivered bytes still arrive.
  ::close(w.b.dataFd);
  w.b.dataFd = -1;
  for (int i = 0; i < 5; ++i) w.round();
  w.b.reconnectData();
  for (int i = 0; i < 10; ++i) w.round();
  const std::string more = "after reconnect";
  const size_t base = w.b.dataAcc.size();
  ::send(w.a.dataFd, more.data(), more.size(), 0);
  for (int i = 0; i < 6000 && w.b.dataAcc.size() < base + more.size(); ++i) {
    w.round();
  }
  CHECK(std::string(w.b.dataAcc.begin() + long(base), w.b.dataAcc.end()) ==
        more);

  // ABORT: immediate local teardown, no DISC exchange.
  CHECK(w.cmd(w.a, "ABORT") == "ABORT");
  CHECK(w.cmd(w.a, "STATE") == "STATE LISTENING");
  // The peer is left hanging and must idle-timeout on its own.
  for (int i = 0; i < 200; ++i) {
    w.clk.advanceSec(2.0);
    w.round();
    if (w.cmd(w.b, "STATE") == "STATE LISTENING") break;
  }
  CHECK(w.cmd(w.b, "STATE") == "STATE LISTENING");
}

TEST_CASE(st2_simultaneous_tx_collision) {
  // Both stations call each other in the same instant: the collision
  // must never crash or wedge either stack - within the budget both
  // settle into valid states (connected pair or clean idle).
  World w(24.0, 123);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.cmd(w.b, "CONNECT W1AW") == "CONNECT W1AW");
  bool settled = false;
  for (int i = 0; i < 6000 && !settled; ++i) {
    w.round();
    if (i % 100 == 0) {
      const std::string sa = w.cmd(w.a, "STATE");
      const std::string sb = w.cmd(w.b, "STATE");
      const bool aQuiet = sa == "STATE LISTENING" || sa == "STATE IDLE";
      const bool bQuiet = sb == "STATE LISTENING" || sb == "STATE IDLE";
      settled = (sa == "STATE CONNECTED" && sb == "STATE CONNECTED") ||
                (aQuiet && bQuiet);
    }
  }
  std::printf("st2 collision: settled=%d a=%s b=%s\n", settled ? 1 : 0,
              w.cmd(w.a, "STATE").c_str(), w.cmd(w.b, "STATE").c_str());
  CHECK(settled);
}

// THROUGHPUT GATE (ROADMAP "Decode-latency lever"): sustained one-way
// bulk on a clean 30 dB channel must deliver a floor number of bytes,
// byte-exact, in 300 sim-s. The byte-exact and ramp gates never caught
// a throughput regression - turn-taking's full-duplex cost went
// unnoticed until a soak was read by hand. Reference on landing:
// 40,960 B (ten 4 KiB coalesced messages); the floor is 70% of that so
// the half-duplex variant and channel-seed jitter pass while a real
// regression (a lost window, a stuck head-of-line frame) fails.
TEST_CASE(st2_oneway_throughput_floor) {
  World w(30.0, 11);
  for (Shuttle* sh : {&w.ab, &w.ba}) {
    sh->chain.channel = ChansimParams{};
    sh->chain.channel.fading = false;
    sh->chain.channel.snr3kDb = 30.0;
  }
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 12000));

  // Deterministic incompressible stream, <= 4 KiB in flight.
  uint64_t s = 0x9E3779B97F4A7C15ull;
  std::vector<uint8_t> sent;
  const double t0 = double(w.clk.nowUs()) * 1e-6;
  size_t verified = 0;
  while (double(w.clk.nowUs()) * 1e-6 - t0 < 300.0) {
    if (sent.size() < w.b.dataAcc.size() + 4096) {
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
    w.round();
    if (w.b.dataAcc.size() > verified) {
      CHECK(w.b.dataAcc.size() <= sent.size());
      CHECK(std::memcmp(w.b.dataAcc.data() + verified, sent.data() + verified,
                        w.b.dataAcc.size() - verified) == 0);
      verified = w.b.dataAcc.size();
    }
  }
  std::printf("oneway throughput: %zu B in 300 sim-s (floor 28672)\n",
              w.b.dataAcc.size());
  CHECK(w.b.dataAcc.size() >= 28672);
}

SKYTEST_MAIN
