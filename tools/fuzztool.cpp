// fuzztool: deterministic adversarial-input stress driver (TESTING.md).
//
// The conformance suites prove the modem on well-formed input; this tool
// proves it never crashes, corrupts memory, or wedges on HOSTILE input.
// Run it under ASan/UBSan builds - the pass criterion is "sanitizer
// silence + no assert" over millions of iterations, not any decode
// outcome.
//
// Modes:
//   fuzztool parsers [--iters N] [--seed S]
//       Random bytes, truncation sweeps, and valid-CRC random frames
//       into parseFrame + every link body parser + parsePhyHeader +
//       FileMeta + validateCallsign (+ VoiceDataBody when built).
//   fuzztool proto [--iters N] [--seed S]
//       ProtoTx segment streams mutated in flight (bitflip, truncate,
//       drop, duplicate, flag-randomize, garbage zstd) into ProtoRx;
//       random + mutated kSvcFile messages into FileRx.
//   fuzztool engine [--iters N] [--seed S]
//       Two live LinkEngines; adversarial burst delivery (bitflipped,
//       truncated, replayed, noise) interleaved with random API calls
//       (connect/disconnect/sendData/sendFile/pin/broadcast).
//   fuzztool rx [--iters N] [--seed S]
//       receiveFrame on corrupted basebands: truncated, spliced,
//       clipped, noise-swamped, zero-stretched, plus pure noise/tones.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/constants.hpp"
#include "link/engine.hpp"
#include "link/frame.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "proto/filesvc.hpp"
#include "proto/pipeline.hpp"
#ifdef SKW_HAVE_VOICE
#include "voice/codec.hpp"
#endif

using namespace skw;

namespace {

// Deterministic xorshift64* - reproducible across runs and platforms.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545f4914f6cdd1dull;
  }
  uint32_t u32(uint32_t bound) { return bound ? uint32_t(next() % bound) : 0; }
  double uniform() { return double(next() >> 11) * 0x1.0p-53; }
  std::vector<uint8_t> bytes(size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = uint8_t(next());
    return v;
  }
};

// One random mutation of a byte buffer (never leaves it untouched).
void mutate(Rng& rng, std::vector<uint8_t>& v) {
  switch (rng.u32(5)) {
    case 0:  // bitflip 1..8 bits
      if (!v.empty()) {
        for (uint32_t i = 0, n = 1 + rng.u32(8); i < n; ++i) {
          v[rng.u32(uint32_t(v.size()))] ^= uint8_t(1u << rng.u32(8));
        }
        break;
      }
      [[fallthrough]];
    case 1:  // truncate
      v.resize(v.size() ? rng.u32(uint32_t(v.size())) : 0);
      break;
    case 2:  // extend with garbage
      for (uint32_t i = 0, n = 1 + rng.u32(32); i < n; ++i) {
        v.push_back(uint8_t(rng.next()));
      }
      break;
    case 3:  // overwrite a run
      if (!v.empty()) {
        size_t at = rng.u32(uint32_t(v.size()));
        size_t n = std::min(v.size() - at, size_t(1 + rng.u32(16)));
        for (size_t i = 0; i < n; ++i) v[at + i] = uint8_t(rng.next());
        break;
      }
      [[fallthrough]];
    default:  // replace wholesale
      v = rng.bytes(rng.u32(96));
      break;
  }
}

// Every stateless parser, one buffer. The return values are irrelevant;
// only sanitizer silence matters.
void hitParsers(std::span<const uint8_t> b) {
  parseFrame(b);
  parsePhyHeader(b);
  CallBody::parse(b);
  CallAckBody::parse(b);
  StartBody::parse(b);
  StartAckBody::parse(b);
  StartNakBody::parse(b);
  DataBody::parse(b);
  AckBody::parse(b);
  ModeChangeBody::parse(b);
  ModeAckBody::parse(b);
  ProbeBody::parse(b);
  ProbeAckBody::parse(b);
  MaskUpdateBody::parse(b);
  MaskAckBody::parse(b);
  KeepaliveBody::parse(b);
  ResumeReqBody::parse(b);
  ResumeAckBody::parse(b);
  DiscBody::parse(b);
  BeaconBody::parse(b);
  CqBody::parse(b);
  BcastDataBody::parse(b);
  VoiceOpenBody::parse(b);
  KeyUpdateBody::parse(b);
  FileMeta::parse(b);
#ifdef SKW_HAVE_VOICE
  VoiceDataBody::parse(b);
#endif
}

int runParsers(Rng& rng, long iters) {
  for (long i = 0; i < iters; ++i) {
    // 1) random buffer, biased small (parsers are length-gated first)
    auto buf = rng.bytes(rng.u32(2) ? rng.u32(48) : rng.u32(600));
    hitParsers(buf);
    std::string s(buf.begin(), buf.begin() + long(std::min<size_t>(buf.size(), 16)));
    validateCallsign(s);
    // 2) valid-CRC frame with random envelope/body: gets past the CRC
    //    gate so type/flag/body validation runs on hostile content
    LinkEnvelope env;
    env.type = LinkType(uint8_t(rng.next()));
    env.flags = uint8_t(rng.next());
    env.sessionId = uint16_t(rng.next());
    env.sequence = uint16_t(rng.next());
    env.ackBase = uint16_t(rng.next());
    auto body = rng.bytes(rng.u32(300));
    auto framed = packFrame(env, body);
    parseFrame(framed);
    // 3) every truncation of that valid frame
    for (size_t cut = 0; cut < framed.size(); cut += 1 + rng.u32(7)) {
      parseFrame(std::span(framed.data(), cut));
    }
    // 4) mutated valid frame (CRC usually breaks; sometimes survives)
    mutate(rng, framed);
    hitParsers(framed);
  }
  return 0;
}

int runProto(Rng& rng, long iters) {
  for (long i = 0; i < iters; ++i) {
    ProtoTx tx;
    ProtoRx rx;
    FileRx frx;
    tx.setZstd(rng.u32(2));
    double now = 0.0;
    // Build a legitimate segment stream from random submissions...
    std::deque<ProtoSegment> segs;
    for (uint32_t m = 0, n = 1 + rng.u32(6); m < n; ++m) {
      if (rng.u32(2)) {
        auto msg = rng.bytes(1 + rng.u32(4000));
        if (rng.u32(2)) {  // compressible payload exercises zstd deep
          std::fill(msg.begin() + long(msg.size() / 2), msg.end(),
                    uint8_t(0x55));
        }
        tx.submitMessage(rng.u32(2) ? kSvcFile : kSvcStream, msg);
      } else {
        auto b = rng.bytes(1 + rng.u32(900));
        tx.submit(b, now);
      }
      now += 3.0;  // past any coalesce deadline
      while (auto seg = tx.nextSegment(30 + rng.u32(400), now)) {
        segs.push_back(std::move(*seg));
      }
    }
    // ...then deliver it through an adversarial channel.
    std::vector<ProtoSegment> replayPool;
    while (!segs.empty()) {
      ProtoSegment seg = std::move(segs.front());
      segs.pop_front();
      switch (rng.u32(8)) {
        case 0:  // drop
          continue;
        case 1:  // duplicate
          segs.push_front(seg);
          break;
        case 2:  // corrupt bytes
          mutate(rng, seg.bytes);
          break;
        case 3:  // randomize flags
          seg.flags = uint8_t(rng.next());
          break;
        case 4:  // replay something old
          if (!replayPool.empty()) {
            const auto& old = replayPool[rng.u32(uint32_t(replayPool.size()))];
            rx.onSegment(old.flags, old.bytes);
          }
          break;
        default:
          break;
      }
      rx.onSegment(seg.flags, seg.bytes);
      if (replayPool.size() < 32) replayPool.push_back(seg);
      for (auto& m : rx.takeServiceMessages(kSvcFile)) frx.onMessage(m);
      rx.takeBytes();
      frx.takeEvents();
      frx.takeCompleted();
    }
    // Raw garbage segments straight into a fresh ProtoRx / FileRx.
    for (uint32_t g = 0, n = 4 + rng.u32(12); g < n; ++g) {
      rx.onSegment(uint8_t(rng.next()), rng.bytes(rng.u32(700)));
      frx.onMessage(rng.bytes(rng.u32(200)));
    }
    rx.takeServiceMessages(kSvcFile);
    rx.takeBytes();
    frx.takeEvents();
  }
  return 0;
}

// Corrupt a baseband capture in place.
void corruptSamples(Rng& rng, std::vector<cfloat>& s) {
  if (s.empty()) return;
  switch (rng.u32(6)) {
    case 0:  // truncate
      s.resize(rng.u32(uint32_t(s.size())));
      break;
    case 1: {  // zero a stretch (dropout)
      size_t at = rng.u32(uint32_t(s.size()));
      size_t n = std::min(s.size() - at, size_t(1 + rng.u32(4000)));
      std::fill(s.begin() + long(at), s.begin() + long(at + n), cfloat{});
      break;
    }
    case 2: {  // hard clip
      for (auto& x : s) {
        x = {std::clamp(x.real(), -0.2f, 0.2f),
             std::clamp(x.imag(), -0.2f, 0.2f)};
      }
      break;
    }
    case 3: {  // heavy noise
      float a = float(0.2 + 2.0 * rng.uniform());
      for (auto& x : s) {
        x += cfloat(a * float(rng.uniform() - 0.5),
                    a * float(rng.uniform() - 0.5));
      }
      break;
    }
    case 4: {  // splice: overwrite tail with a shifted copy of the head
      size_t n = s.size() / 2;
      size_t off = rng.u32(uint32_t(n));
      for (size_t i = 0; i + off < n; ++i) s[n + i] = s[i + off];
      break;
    }
    default: {  // random sample flips
      for (uint32_t i = 0, n = 1 + rng.u32(64); i < n; ++i) {
        s[rng.u32(uint32_t(s.size()))] =
            cfloat(float(rng.uniform() * 4 - 2), float(rng.uniform() * 4 - 2));
      }
      break;
    }
  }
}

int runRx(Rng& rng, long iters) {
  HarqChase harq;
  for (long i = 0; i < iters; ++i) {
    const Family fam = Family(rng.u32(4));
    std::vector<cfloat> cap;
    if (rng.u32(4) == 0) {
      // Pure noise / tone captures - must never crash or wedge.
      cap.resize(4000 + rng.u32(40000));
      if (rng.u32(2)) {
        for (auto& x : cap) {
          x = cfloat(float(rng.uniform() - 0.5), float(rng.uniform() - 0.5));
        }
      } else {
        double w = 0.05 + 0.4 * rng.uniform();
        for (size_t k = 0; k < cap.size(); ++k) {
          cap[k] = cfloat(float(0.5 * std::cos(w * double(k))),
                          float(0.5 * std::sin(w * double(k))));
        }
      }
    } else {
      TxFrameParams tp;
      tp.family = fam;
      tp.mode = 4 + int(rng.u32(10));  // M4..M13
      tp.sig = tp.mode <= 6 ? ChirpSig::Robust : ChirpSig::Fast;
      tp.cpMode = CpMode(rng.u32(3));
      tp.frameType = uint8_t(rng.next());
      tp.seqLow = uint8_t(rng.next());
      auto payload = rng.bytes(1 + rng.u32(1200));
      tp.payload = payload;
      cap = buildFrameBaseband(tp);
      // Padding the receiver expects around a capture window.
      cap.insert(cap.begin(), 500 + rng.u32(2000), cfloat{});
      cap.resize(cap.size() + 500 + rng.u32(2000));
      corruptSamples(rng, cap);
      if (rng.u32(3) == 0) corruptSamples(rng, cap);
    }
    RxStage stage;
    receiveFrame(fam, cap, &stage, rng.u32(2) ? &harq : nullptr, nullptr);
  }
  return 0;
}

int runEngine(Rng& rng, long iters) {
  for (long round = 0; round < iters; ++round) {
    EngineConfig ca, cb;
    ca.policy.ownCall = "W1AW";
    cb.policy.ownCall = "K6XYZ";
    ca.policy.family = cb.policy.family = Family(rng.u32(4));
    ca.policy.cssEnable = cb.policy.cssEnable = (rng.u32(2) == 0);
#ifdef SKW_HAVE_CRYPTO
    // Crypto-adversarial leg (campaign #55): most rounds run encrypted
    // sessions so every corrupted/replayed/reflected burst also
    // exercises AEAD tag rejection and replay windows; some rounds
    // mismatch the PSKs - the sessions must refuse cleanly, never
    // fall back to cleartext, never crash.
    if (rng.u32(4) != 0) {
      ca.encryptionRequested = cb.encryptionRequested = true;
      ca.psk.assign(32, uint8_t(0xA5));
      cb.psk.assign(32, uint8_t(0xA5));
      if (rng.u32(5) == 0) cb.psk[7] ^= 0x01;  // wrong-PSK attempts
      ca.policy.caps |= kCapEncryption;
      cb.policy.caps |= kCapEncryption;
    }
#endif
    LinkEngine a(ca), b(cb);
    double now = 1000.0 * double(round + 1);
    std::vector<std::vector<cfloat>> replay;
    a.connectTo("K6XYZ", now);
    const int steps = 300 + int(rng.u32(400));
    for (int s = 0; s < steps; ++s) {
      now += 0.02 + 1.5 * rng.uniform();
      LinkEngine* src = rng.u32(2) ? &a : &b;
      LinkEngine* dst = src == &a ? &b : &a;
      if (auto burst = src->tick(now)) {
        std::vector<cfloat> wire = std::move(*burst);
        if (replay.size() < 24 && rng.u32(3) == 0) replay.push_back(wire);
        switch (rng.u32(10)) {
          case 0:
            break;  // lost
          case 1:
            corruptSamples(rng, wire);
            dst->rxBurst(wire, now);
            break;
          case 2:  // delivered twice
            dst->rxBurst(wire, now);
            dst->rxBurst(wire, now + 0.5);
            break;
          case 3:  // reflected back at the sender
            src->rxBurst(wire, now);
            break;
          default:
            dst->rxBurst(wire, now);
            break;
        }
      }
      // Replay an old capture at a random party.
      if (!replay.empty() && rng.u32(12) == 0) {
        (rng.u32(2) ? a : b)
            .rxBurst(replay[rng.u32(uint32_t(replay.size()))], now);
      }
      // Random API abuse while the machine runs.
      switch (rng.u32(24)) {
        case 0: {
          auto d = rng.bytes(1 + rng.u32(300));
          a.sendData(d);
          break;
        }
        case 1: {
          auto d = rng.bytes(1 + rng.u32(300));
          b.sendData(d);
          break;
        }
        case 2:
          a.sendFile("f.bin", rng.bytes(1 + rng.u32(5000)),
                     uint64_t(rng.next()));
          break;
        case 3:
          b.pinMode(int(rng.u32(16)), now);
          break;
        case 4:
          a.unpinMode();
          break;
        case 5:
          a.sendCq();
          break;
        case 6:
          b.sendBeacon(-20.0 + 40.0 * rng.uniform());
          break;
        case 7:
          a.sendBroadcast(rng.bytes(1 + rng.u32(1000)));
          break;
        case 8:
          if (rng.u32(8) == 0) a.disconnect(now);
          break;
        case 9:
          if (rng.u32(8) == 0) b.abortSession(now);
          break;
        case 10:
          a.connectTo("K6XYZ", now);  // mid-anything re-connect
          break;
        case 11:
          b.connectTo("W1AW", now);  // call collision path
          break;
        case 12:
          a.setCpPolicy(rng.u32(2) ? "SHORT" : "AUTO", now);
          break;
        default:
          break;
      }
      a.takeReceivedData();
      b.takeReceivedData();
      a.takeEvents();
      b.takeEvents();
      a.takeReceivedFile();
      b.takeReceivedFile();
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "";
  long iters = 1000;
  uint64_t seed = 1;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--iters")) iters = std::atol(argv[i + 1]);
    if (!std::strcmp(argv[i], "--seed")) seed = uint64_t(std::atoll(argv[i + 1]));
  }
  Rng rng(seed);
  int rc = 1;
  if (mode == "parsers") rc = runParsers(rng, iters);
  else if (mode == "proto") rc = runProto(rng, iters);
  else if (mode == "rx") rc = runRx(rng, iters);
  else if (mode == "engine") rc = runEngine(rng, iters);
  else {
    std::fprintf(stderr,
                 "usage: fuzztool parsers|proto|rx|engine [--iters N] "
                 "[--seed S]\n");
    return 2;
  }
  std::printf("fuzztool %s: %ld iterations clean (seed %llu)\n", mode.c_str(),
              iters, (unsigned long long)seed);
  return rc;
}
