// Passive monitor (PROTOCOL.md "Monitoring", INTERFACES.md): every frame
// is self-describing, so a listener decodes anything it can hear. Emits
// control-port MON: lines and data-port binary records (tag 0xFB).
// Binary records are suppressed while the local modem has a CONNECTED
// session (interleaving tagged records with the session's raw payload
// stream would be ambiguous); MON: lines always flow.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "phy/receiver.hpp"

namespace skw {

struct MonitorEvent {
  std::string monLine;               // "MON:src:dst:type:family:Mx:hex"
  std::vector<uint8_t> binaryRecord; // empty when suppressed / no payload
  bool payloadOk = false;            // header-only decode when false
  double airtimeSec = 0.0;           // the frame's on-air span, from its header
};

class Monitor {
 public:
  explicit Monitor(Family f) : family_(f) {}

  void setLocalConnected(bool c) { localConnected_ = c; }

  // Feed a received capture; returns an event when a frame decoded.
  std::optional<MonitorEvent> rxBurst(std::span<const cfloat> samples);

  int decodedFrames() const { return decoded_; }

  // Callsign the last decoded frame was attributed to ("?" when unknown).
  const std::string& lastSource() const { return lastSrc_; }

  // Completed BCAST_DATA reassemblies (fragment chains joined), each
  // paired with the callsign it was attributed to. Chains are kept per
  // sender, so overlapping broadcasts no longer corrupt each other.
  std::vector<std::pair<std::string, std::vector<uint8_t>>>
  takeBroadcasts() {
    auto out = std::move(bcastOut_);
    bcastOut_.clear();
    return out;
  }
  size_t openBroadcastChains() const { return bcastChains_.size(); }

 private:
  Family family_;
  bool localConnected_ = false;
  // One reassembly chain PER SENDER, keyed on the 16-bit sender tag the
  // transmitter puts in the envelope's session-id field. Before this,
  // two stations broadcasting in the same period shared a single global
  // chain and silently corrupted each other's payloads.
  struct BcastChain {
    uint8_t count = 0;
    std::vector<std::vector<uint8_t>> frags;
    // Presence per fragment: a ZERO-length fragment is legal, so "empty"
    // cannot mean "missing" (such a chain never completed).
    std::vector<bool> have;
    std::string src;
    int lastFrame = 0;  // decoded_ when the chain last advanced
  };
  // A chain that has not advanced for this many decoded frames is
  // abandoned (its sender gave up or a fragment was lost): rxBurst has no
  // clock, and frame count is the honest proxy for "time on frequency".
  static constexpr int kBcastChainMaxFrames = 64;
  std::map<uint16_t, BcastChain> bcastChains_;
  // Completed payloads with the callsign they were attributed to.
  std::vector<std::pair<std::string, std::vector<uint8_t>>> bcastOut_;
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
