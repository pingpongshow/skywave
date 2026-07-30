#include "link/monitor.hpp"

#include <cstdio>

#include "core/bytepack.hpp"
#include "link/frame.hpp"
#include "tcp/control.hpp"

namespace skw {

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
        default:
          break;
      }
    }
  }
  if (lastSrc_ != "?") srcHash = callsignHash24(lastSrc_);
  if (lastDst_ != "*") dstHash = callsignHash24(lastDst_);

  // Encrypted or undecodable payloads surface as metadata only.
  std::string hex;
  if (res->payloadOk) {
    const size_t cap = std::min<size_t>(res->payload.size(), 64);
    hex.reserve(cap * 2);
    char b[4];
    for (size_t i = 0; i < cap; ++i) {
      snprintf(b, sizeof(b), "%02X", res->payload[i]);
      hex += b;
    }
  }

  MonitorEvent ev;
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
