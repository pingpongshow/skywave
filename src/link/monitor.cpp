#include "link/monitor.hpp"

#include <cstdio>

#include "core/bytepack.hpp"
#include "link/frame.hpp"
#include "tcp/control.hpp"

namespace skw {

namespace {
// Concurrent senders tracked on one frequency. Well past anything a
// real channel sustains; the cap only stops unbounded growth.
constexpr size_t kMaxBcastChains = 8;
}  // namespace

std::vector<uint8_t> makeMonitorRecord(uint8_t frameType, Family family,
                                       int mode, uint32_t srcHash,
                                       uint32_t dstHash,
                                       std::span<const uint8_t> payload) {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(0xFB);
  w.u8(frameType);
  w.u8(uint8_t(family));
  w.u8(uint8_t(mode));
  w.u24be(srcHash);
  w.u24be(dstHash);
  w.u16be(uint16_t(payload.size()));
  w.bytes(payload);
  return out;
}

std::optional<MonitorEvent> Monitor::rxBurst(
    std::span<const cfloat> samples) {
  auto res = receiveFrame(family_, samples);
  if (!res) return std::nullopt;
  ++decoded_;

  // Learn callsign context from frames that carry full calls.
  uint32_t srcHash = 0xFFFFFF, dstHash = 0xFFFFFF;
  if (res->payloadOk) {
    if (auto f = parseFrame(res->payload)) {
      switch (f->env.type) {
        case LinkType::Call:
          if (auto c = CallBody::parse(f->body)) {
            lastSrc_ = c->srcCall;
            lastDst_ = c->dstCall;
          }
          break;
        case LinkType::Cq:
          if (auto c = CqBody::parse(f->body)) {
            lastSrc_ = c->callsign;
            lastDst_ = "*";
          }
          break;
        case LinkType::Beacon:
          if (auto b = BeaconBody::parse(f->body)) {
            lastSrc_ = b->callsign;
            lastDst_ = "*";
          }
          break;
        case LinkType::BcastData:
          if (auto b = BcastDataBody::parse(f->body)) {
            // Reassemble the 4-bit fragment chain, one chain PER SENDER.
            // The sender's 16-bit tag rides the envelope session-id
            // field, which is meaningless for a connectionless frame.
            const uint16_t tag = f->env.sessionId;
            // Retire chains that stopped advancing, so a lost fragment
            // cannot hold a sender's next broadcast hostage.
            for (auto it = bcastChains_.begin(); it != bcastChains_.end();) {
              if (decoded_ - it->second.lastFrame > kBcastChainMaxFrames) {
                it = bcastChains_.erase(it);
              } else {
                ++it;
              }
            }
            BcastChain& ch = bcastChains_[tag];
            // A new message from the same sender: different geometry, or
            // a fragment index we already hold (the old chain lost a
            // fragment and the sender has moved on). Start over rather
            // than splice two messages together.
            const bool repeat = b->fragIndex < ch.have.size() &&
                                ch.have[b->fragIndex];
            if (b->fragCount != ch.count || repeat) {
              ch.count = b->fragCount;
              ch.frags.assign(size_t(b->fragCount), {});
              ch.have.assign(size_t(b->fragCount), false);
            }
            ch.lastFrame = decoded_;
            // Attribute to whatever callsign context we have; a sender
            // that announced itself (CQ/BEACON/CALL) is named exactly.
            if (lastSrc_ != "?") ch.src = lastSrc_;
            if (b->fragIndex < b->fragCount) {
              ch.frags[b->fragIndex] = std::move(b->payload);
              ch.have[b->fragIndex] = true;
              bool all = true;
              for (bool h : ch.have) {
                if (!h) all = false;
              }
              if (all) {
                std::vector<uint8_t> whole;
                for (auto& fr : ch.frags) {
                  whole.insert(whole.end(), fr.begin(), fr.end());
                }
                bcastOut_.emplace_back(ch.src, std::move(whole));
                bcastChains_.erase(tag);
              }
            }
            // Bound the map: a busy frequency must not accumulate
            // half-finished chains from stations that went away.
            if (bcastChains_.size() > kMaxBcastChains) {
              bcastChains_.erase(bcastChains_.begin());
            }
          }
          break;
        default:
          break;
      }
    }
  }
  if (lastSrc_ != "?") srcHash = callsignHash24(lastSrc_);
  if (lastDst_ != "*") dstHash = callsignHash24(lastDst_);

  // Encrypted or undecodable payloads surface as metadata only.
  //
  // The WHOLE link frame is emitted. An earlier 64-byte cap made every
  // longer frame unverifiable (the CRC-32 lives at the end) and made
  // broadcast reassembly impossible for clients, while saving nothing
  // that matters on a localhost socket.
  std::string hex;
  if (res->payloadOk) {
    hex.reserve(res->payload.size() * 2);
    char b[4];
    for (uint8_t v : res->payload) {
      snprintf(b, sizeof(b), "%02X", v);
      hex += b;
    }
  }

  MonitorEvent ev;
  ev.payloadOk = res->payloadOk;
  {
    // The header states the burst's exact span: a caller holding a
    // capture shorter than this can wait for the rest of the frame.
    std::vector<uint8_t> len(res->header.length);
    TxFrameParams tp;
    tp.family = family_;
    tp.sig = res->sig;
    tp.mode = res->header.mode;
    tp.cpMode = res->header.cpMode;
    tp.cssHeaderCopies = res->header.cssHeaderCopies;
    tp.payload = len;
    ev.airtimeSec = double(computeGeometry(tp).totalSamples) / kFsBaseband;
  }
  ev.monLine = monLine(lastSrc_, lastDst_ == "*" ? "" : lastDst_,
                       res->header.frameType, plan(family_).name,
                       res->header.mode, hex);
  if (!localConnected_ && res->payloadOk) {
    ev.binaryRecord =
        makeMonitorRecord(res->header.frameType, family_, res->header.mode,
                          srcHash, dstHash, res->payload);
  }
  return ev;
}

}  // namespace skw
