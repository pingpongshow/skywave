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
  w.a.sendData(msgA);
  w.b.sendData(msgB);
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
  w.a.sendData(blob);
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
  w.a.sendData(more);
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

  // Deterministic incompressible stream. The window bounds UNDELIVERED
  // bytes and proto delivery is head-of-line, so it caps throughput at
  // window / delivery-gap regardless of the link: at 4 KiB this gate
  // measured the harness (the two-modem soak, same pattern, ran 98-100%
  // with the feed window shut and the modem's TX buffer empty).
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
      w.a.sendData(c);
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

// Reconnect after the PEER DIES, in the simulated world - the same
// scenario as the ST3 kill/resume gate, without the audio devices.
// Destroying and re-emplacing B's ModemWorker reproduces a SIGKILL and
// relaunch faithfully: all engine state is gone, while the audio rings
// keep whatever signal was in flight.
//
// SCOPE - READ THIS BEFORE TRUSTING IT. This is a SMOKE test for the
// reconnect path. It does NOT reproduce either of the two real-device
// defects that the ST3 kill/resume gate found, and it was CHECKED
// against both: it still passes with the capture-backlog cap disabled
// (a large pending-capture cap) and with the ResumeInvalid fallback
// restored to its old buggy guard. Both bugs need the real device
// path's decode latency - a control frame sitting 10-20 s in the
// capture ladder - which this world is far too fast to produce. Nothing
// here would have caught them, and a passing test that cannot fail on
// the bug it names is worse than no test, so it is labelled rather than
// advertised. The real guards are st3_hardware and st3_killresume.
TEST_CASE(st2_peer_death_then_reconnect) {
  World w(24.0, 7);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 3000));
  CHECK(w.waitState(w.b, "CONNECTED", 500));

  // Get traffic moving so the peer dies mid-transfer, which is what
  // leaves the survivor retransmitting into a station that cannot
  // decode it.
  // Big enough that the kill lands MID-transfer: at 4096 B the whole
  // payload arrived before the kill and the continuity assertion below
  // was vacuous.
  // NON-PERIODIC on purpose. The obvious (i*131+7)%251 pattern repeats
  // every 251 bytes, so the divergence locator below matched the wrong
  // offset and reported a replay restart at 0 that never happened.
  std::vector<uint8_t> payload(32768);
  {
    uint64_t st = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < payload.size(); ++i) {
      st ^= st << 13;
      st ^= st >> 7;
      st ^= st << 17;
      payload[i] = uint8_t(st >> 32);
    }
  }
  w.a.sendData(payload);
  for (int i = 0; i < 8000 && w.b.dataAcc.size() < 4096; ++i) w.round();
  CHECK(w.b.dataAcc.size() >= 4096);
  CHECK(w.b.dataAcc.size() < payload.size());  // the kill must interrupt
  const size_t deliveredBeforeKill = w.b.dataAcc.size();

  // Kill and relaunch B. dataAcc is deliberately NOT cleared: the whole
  // point is that what arrives across BOTH lifetimes concatenates to
  // the original stream, with nothing lost and nothing repeated.
  w.b.w.reset();
  w.b.w.emplace(w.b.cfg, w.clk, w.b.rxRing, w.b.txRing);
  w.b.ctrlAcc.clear();
  CHECK(w.cmd(w.b, "LISTEN ON") == "LISTEN ON");

  // The survivor must notice the dead peer by itself and stand down.
  CHECK(w.waitState(w.a, "LISTENING", 12000));

  // And the link must come back. BOTH ends must agree it is up: the
  // failure this guards against is not only "no reconnect" but the
  // half-open state, where one end completes the handshake and the
  // other has already given up.
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  const bool aUp = w.waitState(w.a, "CONNECTED", 8000);
  const bool bUp = w.waitState(w.b, "CONNECTED", 1000);
  std::printf("peer-death reconnect: A %s, B %s\n",
              aUp ? "CONNECTED" : "NOT CONNECTED",
              bUp ? "CONNECTED" : "NOT CONNECTED");
  CHECK(aUp);
  CHECK(bUp);

  // DATA CONTINUITY. The application handed the modem 32768 bytes once.
  // A peer that restarts keeps nothing, so the sender re-sends from the
  // last MESSAGE boundary that peer acknowledged - not dropping the
  // pipeline (which loses every byte not yet acknowledged) and not
  // resuming mid-message (which would hand the fresh peer a fragment
  // chain with no head).
  //
  // The contract is AT-LEAST-ONCE, and it cannot be better. A peer that
  // dies between delivering a message to its application and getting
  // its acknowledgement to us is, from here, indistinguishable from one
  // that never received it; replaying is the only choice that cannot
  // LOSE data. So: NO byte may be lost, and any repetition is bounded
  // by one message and reported in EVT:STREAM_REPLAY.
  // Run until the payload's own TAIL has arrived. Counting bytes is not
  // enough: a replayed message means more bytes cross the link than the
  // application submitted, so a length test stops short of the end.
  auto tailArrived = [&] {
    return w.b.dataAcc.size() >= 64 &&
           std::memcmp(w.b.dataAcc.data() + w.b.dataAcc.size() - 64,
                       payload.data() + payload.size() - 64, 64) == 0;
  };
  for (int i = 0; i < 30000 && !tailArrived(); ++i) w.round();

  // Everything before the reconnect must be an exact prefix.
  const size_t pre = std::min(deliveredBeforeKill, w.b.dataAcc.size());
  CHECK(std::memcmp(w.b.dataAcc.data(), payload.data(), pre) == 0);

  // Find where the replayed stream resumed, by matching a 64-byte run
  // (the payload is non-periodic, so this is unambiguous).
  size_t resume = payload.size();
  if (deliveredBeforeKill + 64 <= w.b.dataAcc.size()) {
    for (size_t j = 0; j + 64 <= payload.size(); ++j) {
      if (std::memcmp(w.b.dataAcc.data() + deliveredBeforeKill,
                      payload.data() + j, 64) == 0) {
        resume = j;
        break;
      }
    }
  }
  const size_t repeated =
      resume <= deliveredBeforeKill ? deliveredBeforeKill - resume : 0;
  std::printf("peer-death continuity: %zu B in, resumed at %zu (%zu "
              "delivered before the kill) -> %zu B repeated\n",
              w.b.dataAcc.size(), resume, deliveredBeforeKill, repeated);

  // NO LOSS: the replay must not skip past what was delivered.
  CHECK(resume <= deliveredBeforeKill);
  // BOUNDED REPETITION: at most one coalesced message. HF caps
  // coalescing at kCoalesceMaxBytes = 4096; 8192 leaves headroom for
  // the marker framing. (VHF-FM coalesces to 16384 - this world is HF.)
  CHECK(repeated <= 8192);
  // And the whole payload must actually arrive: prefix + the replayed
  // remainder reconstructs it byte for byte.
  const size_t tail = w.b.dataAcc.size() - deliveredBeforeKill;
  CHECK_EQ(resume + tail, payload.size());
  CHECK(std::memcmp(w.b.dataAcc.data() + deliveredBeforeKill,
                    payload.data() + resume, tail) == 0);
}


// Peer death over a DIGITALLY SILENT channel (2026-10, ST3 kill/resume
// stall). The ST2 world's -60 dBFS ambient floor hid this: on the real
// virtual-cable path an idle or dead peer is exact zeros, the chirp
// detector's noise floor collapsed, and it emitted a phantom detection
// every ~0.65 s. Each phantom opened a capture that climbed the 90 s
// ladder; on real audio the worker fell behind real time, the RX ring
// overran and handshake frames were lost. FakeClock hides the CPU cost,
// so this test pins the CAUSE instead: captures opened while the peer is
// dead must track real bursts, not the silence (measured before the fix:
// hundreds; after: a few dozen at most).
TEST_CASE(st2_peer_death_digital_silence) {
  World w(24.0, 7);
  w.ambientAmp = 0.0f;
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 3000));
  std::vector<uint8_t> payload(32768);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 3);
  w.a.sendData(payload);
  for (int i = 0; i < 8000 && w.b.dataAcc.size() < 4096; ++i) w.round();
  w.b.w.reset();
  w.b.w.emplace(w.b.cfg, w.clk, w.b.rxRing, w.b.txRing);
  w.b.ctrlAcc.clear();
  CHECK(w.cmd(w.b, "LISTEN ON") == "LISTEN ON");
  const auto a0 = w.a.w->telemetry();
  CHECK(w.waitState(w.a, "LISTENING", 12000));
  const auto a1 = w.a.w->telemetry();
  const auto b1 = w.b.w->telemetry();
  const uint64_t aOpened = a1.rxCapturesOpened - a0.rxCapturesOpened;
  std::printf("digital-silence peer death: A opened %llu captures "
              "(%llu evicted), restarted B opened %llu (%llu evicted)\n",
              (unsigned long long)aOpened,
              (unsigned long long)(a1.rxCapturesDropped - a0.rxCapturesDropped),
              (unsigned long long)b1.rxCapturesOpened,
              (unsigned long long)b1.rxCapturesDropped);
  // The dead peer transmits NOTHING: the survivor has nothing to capture.
  CHECK(aOpened <= 4);
  CHECK_EQ(a1.rxCapturesDropped - a0.rxCapturesDropped, 0u);
  CHECK_EQ(b1.rxCapturesDropped, 0u);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 8000));
  CHECK(w.waitState(w.b, "CONNECTED", 1000));
}

// Keepalives were flag-gated off and nothing enabled them: an idle
// session with nothing else on the air died at the 120 s idle timeout.
// An unpinned session hid it (AMC probes are traffic); a pinned one (an
// operator's MODE, the voice tests) dropped every time. Both ends now
// keep it alive.
TEST_CASE(st2_idle_pinned_session_stays_up) {
  World w(24.0, 33);
  CHECK(w.cmd(w.a, "CONNECT K6XYZ") == "CONNECT K6XYZ");
  CHECK(w.waitState(w.a, "CONNECTED", 6000));
  CHECK(w.waitState(w.b, "CONNECTED", 1000));
  CHECK(w.cmd(w.a, "MODE M10") == "MODE M10");
  for (int i = 0; i < int(300.0 / kRoundSec); ++i) w.round();  // 5 idle minutes
  CHECK(w.cmd(w.a, "STATE") == "STATE CONNECTED");
  CHECK(w.cmd(w.b, "STATE") == "STATE CONNECTED");
  CHECK(!w.a.saw("EVT:DISCONNECTED"));
}

SKYTEST_MAIN
