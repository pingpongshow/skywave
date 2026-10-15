// Shared ST2 world harness (TESTING.md): the REAL worker stack -
// ModemWorker (engine + streaming RX + converters + telemetry + command
// dispatch) behind REAL TCP data/control ports - with an in-process
// audio shuttle and the radio-chain emulator interposed. Used by
// test_st2 (scripted fault sessions) and soak_st2 (long impaired-channel
// endurance scenarios).
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "sim/radiochain.hpp"
#include "tcp/server.hpp"
#include "worker.hpp"

namespace st2w {

using namespace skw;

// HARNESS ISOLATION. The ST2 world must never read or write the
// operator's real channel-memory store. CMake sets SKW_CHANMEM_PATH=
// for registered ctest targets, but a harness run DIRECTLY from a shell
// (soak_st2, monkey_st2, repro_oneway) bypassed that entirely: each run
// both polluted ~/.skywave/chanmem.bin and INHERITED whatever the
// previous run had left there. Consecutive runs were therefore not
// independent, and a comparison chain built on them measured leftover
// adaptation state rather than the change under test - a churn soak
// swung 44 KB / 0 / 9 KB / 0 across runs of code that barely differed,
// which cost four wrong attributions before the cause was spotted.
// setenv(..., 0) does not overwrite, so an explicit path still wins.
struct ChanMemIsolation {
  ChanMemIsolation() { ::setenv("SKW_CHANMEM_PATH", "-", 0); }
};
inline const ChanMemIsolation kChanMemIsolation{};

constexpr size_t kRoundSamples =
    size_t(kFftAudio) + 192;  // worker chunk granule (fixed)
constexpr double kRoundSec = double(kRoundSamples) / kFsAudio;

// The harness's own raw client sockets must not SIGPIPE the binary
// (server may drop a client at any moment during fault runs).
struct SigpipeGuard {
  SigpipeGuard() { std::signal(SIGPIPE, SIG_IGN); }
};
inline SigpipeGuard g_sigpipeGuard;

inline int connectLoopback(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// STREAMING channel transport. The worker stages a whole burst into its
// TX ring in one write (the world's ring is sized so even a deep-tier
// frame fits), and the ring drains at real time - one round of samples
// per round - which is what the modem's keying and txq accounting see.
// Everything that APPEARS in the ring is run through the radio chain
// that same round, as one capture (the chain's filters are per-capture),
// and the chain output is released to the peer in step with the drain:
// sample n reaches the receiver on the round the sender airs it, exactly
// as over a real audio path.
//
// The previous shuttle accumulated a burst and forwarded it only after
// its trailing silence - one full burst-duration of latency per hop that
// no radio has. Measured: a 13 s ST2 round trip against ~3.5 s of
// airtime on a clean channel, and the ROADMAP's "ST2 roughly 2x harsher
// than real audio (ST3 handshake legs 4.5 s vs 8.6 s)". Every RTT-scaled
// timer in the link (ARQ, grant handoff, reclaim, turn quantum) was
// being sized to that artifact.
struct Shuttle {
  RadioChainParams chain;
  uint64_t seedCtr = 1;
  bool drop = false;         // fault injection: audio dropout
  uint64_t consumed = 0;     // ring samples drained (aired) so far
  uint64_t processed = 0;    // ring samples already through the chain
  std::deque<float> staged;  // chain output not yet released to the peer
  std::deque<float> outQ;    // released audio awaiting the peer's RX clock
  // Per-capture accounting. The chain returns MORE samples than it was
  // given (filter tail, delay-spread ringing), so releasing output "in
  // step with the drained count" left every burst's excess in `staged`;
  // under back-to-back bulk the ring never went idle to flush it and
  // each later burst reached the receiver later than the last (measured:
  // srtt climbing 11 -> 21 s over 300 s and zero bytes delivered). Each
  // capture's output is released as ITS OWN input drains, and its tail
  // is flushed the moment that input has fully aired - the ringing then
  // precedes the next burst, as it does on a real receiver.
  struct Capture {
    size_t inLeft;   // input samples of this capture still to drain
    size_t outLeft;  // chain-output samples of it still to release
  };
  std::deque<Capture> caps;

  // Channel-process everything that appeared in `ring` since last round.
  void processNew(const SampleRing& ring) {
    const size_t avail = ring.readAvailable();
    const uint64_t seen = consumed + avail;
    if (seen <= processed) return;
    const size_t n = size_t(seen - processed);
    std::vector<float> x(n);
    ring.peek(x, avail - n);
    bool signal = false;
    for (float s : x) {
      if (std::fabs(s) > 1e-5f) {
        signal = true;
        break;
      }
    }
    if (signal) {
      chain.channel.seed = chain.seed + (seedCtr++) * 7919;
      auto y = radioChain(chain, x);
      staged.insert(staged.end(), y.begin(), y.end());
      caps.push_back({n, y.size()});
    } else {
      staged.insert(staged.end(), x.begin(), x.end());  // lead/tail pad
      caps.push_back({n, n});
    }
    processed = seen;
  }
  void emit(size_t k) {
    for (size_t i = 0; i < k && !staged.empty(); ++i) {
      outQ.push_back(drop ? 0.0f : staged.front());
      staged.pop_front();
    }
  }
  // Release chain output for `drained` ring samples, capture by capture.
  void release(size_t drained) {
    while (drained > 0 && !caps.empty()) {
      Capture& c = caps.front();
      const size_t take = c.inLeft < drained ? c.inLeft : drained;
      c.inLeft -= take;
      drained -= take;
      const size_t out = c.outLeft < take ? c.outLeft : take;
      emit(out);
      c.outLeft -= out;
      if (c.inLeft == 0) {
        emit(c.outLeft);  // the capture has aired: its tail follows now
        c.outLeft = 0;
        caps.pop_front();
      }
    }
  }
};

struct Node {
  SampleRing rxRing{size_t(kFsAudio) * 8};
  // 64 s TX ring (the modem's own is 8 s): the streaming shuttle runs a
  // burst through the radio chain as ONE capture the round it is staged,
  // so even a deep-tier CSS frame (up to ~35 s) must fit whole. The
  // worker's txq accounting is pending+ring and does not depend on size.
  SampleRing txRing{size_t(kFsAudio) * 64};
  Config cfg;
  std::optional<ModemWorker> w;
  TcpServer dataSrv, ctrlSrv;
  LineBuffer ctrlLines;
  std::vector<uint8_t> dataOut;  // held while no data client attached
  int dataFd = -1, ctrlFd = -1;
  std::string ctrlAcc;                // everything the ctrl client saw
  std::vector<uint8_t> dataAcc;       // everything the data client saw
  // DETERMINISTIC TCP. The harness bridges the modem through real
  // loopback sockets, and WHEN the kernel hands bytes across used to
  // decide which simulated round saw them: under parallel CPU load the
  // same seed ran different timelines (churn seed 12: 55 KB in one run,
  // 131 KB in the next) and single-run soak A/B deltas were noise. Every
  // hand-off is now completed before the simulated clock moves: bytes
  // are counted at both ends and the harness waits (wall-clock bounded)
  // until each side has seen everything the other sent.
  uint64_t dataCliSent = 0, dataSrvRecv = 0, dataSrvSent = 0, dataCliRecv = 0;
  uint64_t ctrlCliSent = 0, ctrlSrvRecv = 0, ctrlSrvSent = 0, ctrlCliRecv = 0;

  Node(FakeClock& clk, const char* call) {
    cfg.callsign = call;
    cfg.listen = true;
    // SKW_ST2_FAMILY=wide|mid|narrow|ultra runs both modems on another
    // bandwidth family (default WIDE).
    if (const char* f = std::getenv("SKW_ST2_FAMILY")) {
      const std::string v(f);
      if (v == "mid") cfg.family = Family::Mid;
      else if (v == "narrow") cfg.family = Family::Narrow;
      else if (v == "ultra") cfg.family = Family::Ultra;
    }
    cfg.pttLeadMs = 20;
    cfg.pttTailMs = 10;
    w.emplace(cfg, clk, rxRing, txRing);
    dataSrv.open(0);
    ctrlSrv.open(0);
    reconnectData();
    reconnectCtrl();
  }
  ~Node() {
    if (dataFd >= 0) ::close(dataFd);
    if (ctrlFd >= 0) ::close(ctrlFd);
  }

  void reconnectData() {
    if (dataFd >= 0) ::close(dataFd);
    dataFd = connectLoopback(dataSrv.boundPort());
    dataCliSent = dataSrvRecv = dataSrvSent = dataCliRecv = 0;
  }
  void reconnectCtrl() {
    if (ctrlFd >= 0) ::close(ctrlFd);
    ctrlFd = connectLoopback(ctrlSrv.boundPort());
    ctrlCliSent = ctrlSrvRecv = ctrlSrvSent = ctrlCliRecv = 0;
  }

  // Client -> modem data, delivered whole (see DETERMINISTIC TCP).
  void sendData(const void* p, size_t n) {
    if (dataFd < 0) return;
    const auto* b = static_cast<const uint8_t*>(p);
    size_t off = 0;
    while (off < n) {
      const ssize_t k = ::send(dataFd, b + off, n - off, 0);
      if (k <= 0) break;
      off += size_t(k);
    }
    dataCliSent += off;
  }
  template <class C>
  void sendData(const C& c) {
    sendData(c.data(), c.size());
  }
  void sendCtrl(const std::string& line) {
    if (ctrlFd < 0) return;
    size_t off = 0;
    while (off < line.size()) {
      const ssize_t k = ::send(ctrlFd, line.data() + off, line.size() - off, 0);
      if (k <= 0) break;
      off += size_t(k);
    }
    ctrlCliSent += off;
  }

  static bool waitUntil(const std::function<bool()>& done) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!done()) {
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return true;
  }

  // One pass of the main-loop TCP bridging (mirrors src/main.cpp).
  void pumpTcp() {
    // Server side: everything the client has sent so far, whole.
    auto takeData = [&] {
      auto in = dataSrv.poll();
      dataSrvRecv += in.size();
      if (!in.empty()) w->submitAppData(in);
    };
    takeData();
    if (dataFd >= 0 && dataSrv.clientConnected()) {
      waitUntil([&] {
        if (dataSrvRecv >= dataCliSent) return true;
        takeData();
        return dataSrvRecv >= dataCliSent;
      });
    }
    auto out = w->takeAppData();
    dataOut.insert(dataOut.end(), out.begin(), out.end());
    if (!dataOut.empty() && dataSrv.clientConnected()) {
      dataSrvSent += dataOut.size();
      dataSrv.send(dataOut);
      dataOut.clear();
    }
    for (auto& rec : w->takeMonitorRecords()) {
      if (dataSrv.clientConnected()) {
        dataSrvSent += rec.size();
        dataSrv.send(rec);
      }
    }
    auto takeCtrl = [&] {
      auto in = ctrlSrv.poll();
      ctrlSrvRecv += in.size();
      ctrlLines.feed(in);
    };
    takeCtrl();
    if (ctrlFd >= 0 && ctrlSrv.clientConnected()) {
      waitUntil([&] {
        if (ctrlSrvRecv >= ctrlCliSent) return true;
        takeCtrl();
        return ctrlSrvRecv >= ctrlCliSent;
      });
    }
    auto ctrlSend = [&](const std::string& s) {
      if (!ctrlSrv.clientConnected()) return;
      ctrlSrvSent += s.size();
      ctrlSrv.send(s);
    };
    for (auto& line : ctrlLines.takeLines()) {
      ctrlSend(w->control(line) + "\n");
    }
    for (auto& n : w->takeNotifications()) ctrlSend(n + "\n");
    // Client side: everything the server has sent so far, whole.
    char buf[4096];
    auto readCtrl = [&] {
      ssize_t n;
      while (ctrlFd >= 0 &&
             (n = ::recv(ctrlFd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        ctrlAcc.append(buf, size_t(n));
        ctrlCliRecv += uint64_t(n);
      }
    };
    auto readData = [&] {
      ssize_t n;
      while (dataFd >= 0 &&
             (n = ::recv(dataFd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        dataAcc.insert(dataAcc.end(), buf, buf + n);
        dataCliRecv += uint64_t(n);
      }
    };
    readCtrl();
    readData();
    if (ctrlFd >= 0) {
      waitUntil([&] {
        readCtrl();
        if (ctrlCliRecv >= ctrlSrvSent) return true;
        if (ctrlSrv.txBacklog() > 0) takeCtrl();  // poll() flushes
        return false;
      });
    }
    if (dataFd >= 0) {
      waitUntil([&] {
        readData();
        if (dataCliRecv >= dataSrvSent) return true;
        if (dataSrv.txBacklog() > 0) takeData();  // poll() flushes
        return false;
      });
    }
  }

  bool saw(const std::string& needle) const {
    return ctrlAcc.find(needle) != std::string::npos;
  }
};

struct World {
  FakeClock clk{1000000};
  Node a, b;
  Shuttle ab, ba;
  bool starveB = false;  // fault injection: B's worker stops running
  uint64_t noiseRng = 0x243F6A8885A308D3ull;  // ambient-floor generator
  // Interference injection (monkey/immunity runs): added at the RX
  // point of each direction's path, heard only while unkeyed (a keyed
  // half-duplex radio is deaf to jammers too).
  double jamAmpAB = 0.0, jamToneAB = 0.0;   // into B's receiver
  double jamAmpBA = 0.0, jamToneBA = 0.0;   // into A's receiver
  double jamPhase = 0.0;
  // Ambient floor amplitude added at each receiver. 0 = EXACT digital
  // silence between bursts, which is what a virtual audio cable delivers
  // (ST3) and what an interface with a muted or dead source delivers.
  float ambientAmp = std::getenv("SKW_ST2_AMBIENT")
                         ? float(std::atof(std::getenv("SKW_ST2_AMBIENT")))
                         : 1e-3f;

  // HALF-DUPLEX IS THE DEFAULT WORLD: a single SSB transceiver, deaf to
  // the channel while its own PTT is keyed and for a TX/RX switching
  // interval after it unkeys. This is the physical reality scheduled
  // turn-taking exists to handle. SKW_FULL_DUPLEX=1 selects the old
  // split-TX/RX world, which never punishes keying over the peer - an
  // occupancy-gate scheduler looks fine there and one-at-a-time
  // discipline only costs parallelism - and is kept only as a variant.
  // (SKW_HALF_DUPLEX=1, the pre-default knob, remains accepted.)
  bool halfDuplex = std::getenv("SKW_FULL_DUPLEX") == nullptr;
  // TX/RX switching time (SKW_TXRX_SWITCH_MS, default 50): a rig's
  // receiver is blanked for tens of ms after unkey (relay/PIN switching,
  // AGC recovery). Modelled as continued deafness after keying drops.
  double switchSec =
      (std::getenv("SKW_TXRX_SWITCH_MS") ? std::atof(std::getenv("SKW_TXRX_SWITCH_MS"))
                                          : 50.0) / 1000.0;
  int deafRoundsA = 0, deafRoundsB = 0;  // post-unkey blanking countdown
  bool wasKeyedA = false, wasKeyedB = false;

  World(double snrDb, uint64_t seed) : a(clk, "W1AW"), b(clk, "K6XYZ") {
    for (Shuttle* s : {&ab, &ba}) {
      s->chain.channel.fading = false;
      s->chain.channel.snr3kDb = snrDb;
    }
    ab.chain.seed = seed;
    ba.chain.seed = seed + 5000;
  }

  void moveAudio(Node& from, Node& to, Shuttle& sh, bool toDeaf = false) {
    // 1) Channel-process what appeared in the ring this round (whole
    //    burst on the round it is staged - see Shuttle).
    sh.processNew(from.txRing);
    // 2) REAL-TIME TX drain: a real audio driver consumes exactly one
    //    round of samples per round of wall clock. Draining the whole
    //    ring here let a modem transmit ~100x faster than real time -
    //    under two-way load the shuttle queue grew unboundedly, ACKs
    //    arrived tens of seconds late, and every ARQ timer fired first
    //    (found by the bidirectional saturation soak).
    float buf[8192];
    static_assert(sizeof(buf) / sizeof(buf[0]) >= kRoundSamples);
    const size_t availNow = from.txRing.readAvailable();
    const size_t got =
        from.txRing.read({buf, std::min(kRoundSamples, availNow)});
    sh.consumed += got;
    // 3) Release chain output for what just aired, capture by capture
    //    (a capture's tail follows the instant its input has aired).
    sh.release(got);
    // Keep the receiver's audio clock running: exactly one round of
    // samples per round, silence when the channel is quiet.
    size_t want = std::min(kRoundSamples, to.rxRing.writeAvailable());
    std::vector<float> blk(want, 0.0f);
    for (size_t i = 0; i < want && !sh.outQ.empty(); ++i) {
      // Half-duplex deafness: while `to` is keyed it cannot hear the
      // channel. The samples are still CONSUMED from the queue (the
      // transmission physically passed while the radio was deaf - it is
      // lost, not buffered), but the receiver gets silence.
      const float s = sh.outQ.front();
      sh.outQ.pop_front();
      if (!toDeaf) blk[i] = s;
    }
    // NOTE full-duplex audio: this world does NOT model PTT-muted
    // receivers (a real single SSB transceiver is deaf while keyed).
    // A deaf-RX prototype showed the current protocol needs a true
    // turn-taking MAC to survive that - tracked as pre-OTA work in
    // ROADMAP. Current validated envelope: full-duplex audio paths
    // (virtual cables, split TX/RX).
    // Ambient noise floor (~-60 dBFS): real receivers never output
    // digital zero. Without it the worker's noise-floor tracker
    // acquires silence at -inf and the busy detector fires on ANY
    // nonzero sample - chansim noise tails read as a busy channel and
    // CCA over-deferred everything (observed: busy=1 pinned for
    // minutes on an idle link).
    for (auto& v : blk) {
      noiseRng ^= noiseRng >> 12;
      noiseRng ^= noiseRng << 25;
      noiseRng ^= noiseRng >> 27;
      v += ambientAmp * (float(uint32_t(noiseRng * 0x2545F4914F6CDD1Dull >> 40)) /
                        float(1u << 24) -
                    0.5f);
    }
    to.rxRing.write(blk);
  }

  void round() {
    a.w->runOnce();
    if (!starveB) b.w->runOnce();
    // Snapshot keying BEFORE either moveAudio drains a TX ring: a station
    // with audio queued to transmit is keyed this round, and a keyed
    // half-duplex radio is deaf to the incoming channel - and stays deaf
    // for the TX/RX switching interval after it unkeys.
    const bool keyedA = a.txRing.readAvailable() > 0;
    const bool keyedB = b.txRing.readAvailable() > 0;
    const int switchRounds = int(std::ceil(switchSec / kRoundSec));
    if (wasKeyedA && !keyedA) deafRoundsA = switchRounds;
    if (wasKeyedB && !keyedB) deafRoundsB = switchRounds;
    const bool deafA = halfDuplex && (keyedA || deafRoundsA > 0);
    const bool deafB = halfDuplex && (keyedB || deafRoundsB > 0);
    if (!keyedA && deafRoundsA > 0) --deafRoundsA;
    if (!keyedB && deafRoundsB > 0) --deafRoundsB;
    wasKeyedA = keyedA;
    wasKeyedB = keyedB;
    moveAudio(a, b, ab, deafB);  // B deaf while B keys / switches
    moveAudio(b, a, ba, deafA);  // A deaf while A keys / switches
    a.pumpTcp();
    b.pumpTcp();
    clk.advanceSec(kRoundSec);
  }

  std::string cmd(Node& n, const std::string& line) {
    const size_t mark = n.ctrlAcc.size();
    n.sendCtrl(line + "\n");
    // Control replies are synchronous: with the deterministic hand-off a
    // single pump answers without moving the simulated clock (a query no
    // longer costs a varying number of rounds - one source of the
    // run-to-run divergence). Rounds remain the fallback.
    for (int i = 0; i < 401; ++i) {
      if (i == 0) {
        n.pumpTcp();
      } else {
        round();
      }
      // First non-notification line after the mark is the response.
      size_t pos = mark;
      while (pos < n.ctrlAcc.size()) {
        size_t eol = n.ctrlAcc.find('\n', pos);
        if (eol == std::string::npos) break;
        std::string l = n.ctrlAcc.substr(pos, eol - pos);
        pos = eol + 1;
        if (l.rfind("EVT:", 0) == 0 || l.rfind("MON:", 0) == 0) continue;
        return l;
      }
    }
    return "";
  }

  bool waitState(Node& n, const std::string& want, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      round();
      if (i % 25 == 0) {
        if (cmd(n, "STATE") == "STATE " + want) return true;
      }
    }
    return cmd(n, "STATE") == "STATE " + want;
  }
};

}  // namespace st2w
