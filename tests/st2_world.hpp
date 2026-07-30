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

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "sim/radiochain.hpp"
#include "tcp/server.hpp"
#include "worker.hpp"

namespace st2w {

using namespace skw;

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

// Burst-accumulating channel: silence passes straight through; an active
// burst is collected until its trailing silence, then run through the
// radio chain in one piece (the chain's filters are per-capture).
struct Shuttle {
  RadioChainParams chain;
  std::vector<float> acc;
  int quiet = 0;
  std::deque<float> outQ;
  uint64_t seedCtr = 1;
  bool drop = false;  // fault injection: audio dropout
  int idleRounds = 0;  // rounds with no TX samples at all

  void push(std::span<const float> in) {
    for (float s : in) {
      if (drop) s = 0.0f;
      if (std::fabs(s) > 1e-5f) {
        acc.push_back(s);
        quiet = 0;
        // Safety cap: sustained continuous TX must not accumulate an
        // unbounded capture (the chain processes per capture).
        if (acc.size() > size_t(12 * kFsAudio)) flush();
      } else if (!acc.empty()) {
        acc.push_back(s);
        if (++quiet > int(0.12 * kFsAudio)) flush();
      } else {
        outQ.push_back(0.0f);
      }
    }
  }
  void flush() {
    chain.channel.seed = chain.seed + (seedCtr++) * 7919;
    auto y = radioChain(chain, acc);
    for (float v : y) outQ.push_back(v);
    acc.clear();
    quiet = 0;
  }
};

struct Node {
  SampleRing rxRing{size_t(kFsAudio) * 8};
  SampleRing txRing{size_t(kFsAudio) * 8};
  Config cfg;
  std::optional<ModemWorker> w;
  TcpServer dataSrv, ctrlSrv;
  LineBuffer ctrlLines;
  std::vector<uint8_t> dataOut;  // held while no data client attached
  int dataFd = -1, ctrlFd = -1;
  std::string ctrlAcc;                // everything the ctrl client saw
  std::vector<uint8_t> dataAcc;       // everything the data client saw

  Node(FakeClock& clk, const char* call) {
    cfg.callsign = call;
    cfg.listen = true;
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
  }
  void reconnectCtrl() {
    if (ctrlFd >= 0) ::close(ctrlFd);
    ctrlFd = connectLoopback(ctrlSrv.boundPort());
  }

  // One pass of the main-loop TCP bridging (mirrors src/main.cpp).
  void pumpTcp() {
    auto in = dataSrv.poll();
    if (!in.empty()) w->submitAppData(in);
    auto out = w->takeAppData();
    dataOut.insert(dataOut.end(), out.begin(), out.end());
    if (!dataOut.empty() && dataSrv.clientConnected()) {
      dataSrv.send(dataOut);
      dataOut.clear();
    }
    for (auto& rec : w->takeMonitorRecords()) {
      if (dataSrv.clientConnected()) dataSrv.send(rec);
    }
    ctrlLines.feed(ctrlSrv.poll());
    for (auto& line : ctrlLines.takeLines()) {
      ctrlSrv.send(w->control(line) + "\n");
    }
    for (auto& n : w->takeNotifications()) {
      if (ctrlSrv.clientConnected()) ctrlSrv.send(n + "\n");
    }
    // Client-side reads (nonblocking).
    char buf[4096];
    ssize_t n;
    while (ctrlFd >= 0 &&
           (n = ::recv(ctrlFd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
      ctrlAcc.append(buf, size_t(n));
    }
    while (dataFd >= 0 &&
           (n = ::recv(dataFd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
      dataAcc.insert(dataAcc.end(), buf, buf + n);
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

  World(double snrDb, uint64_t seed) : a(clk, "W1AW"), b(clk, "K6XYZ") {
    for (Shuttle* s : {&ab, &ba}) {
      s->chain.channel.fading = false;
      s->chain.channel.snr3kDb = snrDb;
    }
    ab.chain.seed = seed;
    ba.chain.seed = seed + 5000;
  }

  void moveAudio(Node& from, Node& to, Shuttle& sh) {
    // REAL-TIME TX drain: a real audio driver consumes exactly one
    // round of samples per round of wall clock. Draining the whole
    // ring here let a modem transmit ~100x faster than real time -
    // under two-way load the shuttle queue grew unboundedly, ACKs
    // arrived tens of seconds late, and every ARQ timer fired first
    // (found by the bidirectional saturation soak).
    float buf[8192];
    static_assert(sizeof(buf) / sizeof(buf[0]) >= kRoundSamples);
    const size_t got = from.txRing.read(
        {buf, std::min(kRoundSamples, from.txRing.readAvailable())});
    const size_t total = got;
    if (got > 0) sh.push({buf, got});
    // The worker writes only bursts to its TX ring (a real audio driver
    // emits silence on its own): an empty ring for a few rounds marks
    // the burst end, so flush it through the radio chain.
    if (total == 0) {
      if (!sh.acc.empty() && ++sh.idleRounds >= 3) sh.flush();
    } else {
      sh.idleRounds = 0;
    }
    // Keep the receiver's audio clock running: exactly one round of
    // samples per round, silence when the channel is quiet.
    size_t want = std::min(kRoundSamples, to.rxRing.writeAvailable());
    std::vector<float> blk(want, 0.0f);
    for (size_t i = 0; i < want && !sh.outQ.empty(); ++i) {
      blk[i] = sh.outQ.front();
      sh.outQ.pop_front();
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
      v += 1e-3f * (float(uint32_t(noiseRng * 0x2545F4914F6CDD1Dull >> 40)) /
                        float(1u << 24) -
                    0.5f);
    }
    to.rxRing.write(blk);
  }

  void round() {
    a.w->runOnce();
    if (!starveB) b.w->runOnce();
    moveAudio(a, b, ab);
    moveAudio(b, a, ba);
    a.pumpTcp();
    b.pumpTcp();
    clk.advanceSec(kRoundSec);
  }

  std::string cmd(Node& n, const std::string& line) {
    const std::string out = line + "\n";
    ::send(n.ctrlFd, out.data(), out.size(), 0);
    const size_t mark = n.ctrlAcc.size();
    for (int i = 0; i < 400; ++i) {
      round();
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
