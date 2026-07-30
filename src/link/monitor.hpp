// Passive monitor (PROTOCOL.md "Monitoring", INTERFACES.md): every frame
// is self-describing, so a listener decodes anything it can hear. Emits
// control-port MON: lines and data-port binary records (tag 0xFB).
// Binary records are suppressed while the local modem has a CONNECTED
// session (interleaving tagged records with the session's raw payload
// stream would be ambiguous); MON: lines always flow.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "phy/receiver.hpp"

namespace skw {

struct MonitorEvent {
  std::string monLine;               // "MON:src:dst:type:family:Mx:hex"
  std::vector<uint8_t> binaryRecord; // empty when suppressed / no payload
};

class Monitor {
 public:
  explicit Monitor(Family f) : family_(f) {}

  void setLocalConnected(bool c) { localConnected_ = c; }

  // Feed a received capture; returns an event when a frame decoded.
  std::optional<MonitorEvent> rxBurst(std::span<const cfloat> samples);

  int decodedFrames() const { return decoded_; }

  // Completed BCAST_DATA reassemblies (fragment chains joined).
  std::vector<std::vector<uint8_t>> takeBroadcasts() {
    auto out = std::move(bcastOut_);
    bcastOut_.clear();
    return out;
  }

 private:
  Family family_;
  bool localConnected_ = false;
  int bcastCount_ = 0;  // fragment count of the chain being collected
  std::vector<std::vector<uint8_t>> bcastFrags_;
  std::vector<std::vector<uint8_t>> bcastOut_;
  // Callsign context: full calls appear only in handshake/broadcast
  // frames; session frames are attributed to the last observed exchange.
  std::string lastSrc_ = "?";
  std::string lastDst_ = "*";
  int decoded_ = 0;
};

// INTERFACES.md data-port binary record.
std::vector<uint8_t> makeMonitorRecord(uint8_t frameType, Family family,
                                       int mode, uint32_t srcHash,
                                       uint32_t dstHash,
                                       std::span<const uint8_t> payload);

}  // namespace skw
