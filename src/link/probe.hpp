// Probe-before-promote (LINK_ADAPTATION.md): before committing promotion
// into a fragile mode (M11+), verify it live with PROBE frame(s)
// transmitted at the CANDIDATE mode; the peer answers PROBE_ACK at the
// current mode with decode quality. Clean -> promote; marginal/failed ->
// stay, with a 10-observed-frame cooldown. Pure logic; engine executes.
#pragma once

#include <cstdint>
#include <optional>

namespace skw {

// PROBE_ACK decode-quality values (ProbeAckBody::decodedOk).
inline constexpr uint8_t kProbeFailed = 0;    // header only / nothing
inline constexpr uint8_t kProbeMarginal = 1;  // decoded, poor margin
inline constexpr uint8_t kProbeClean = 2;

struct ProbeGateConfig {
  int fromMode = 11;        // probes required for promotions into >= this
  int cooldownFrames = 10;  // observed frames after a failed probe
  double timeoutSec = 20.0; // unanswered probe counts as failed
};

class ProbeGate {
 public:
  explicit ProbeGate(ProbeGateConfig cfg = {}) : cfg_(cfg) {}

  bool required(int targetMode) const { return targetMode >= cfg_.fromMode; }
  bool cleared(int targetMode) const {
    return clearedMode_ == targetMode;
  }
  bool inCooldown() const { return cooldown_ > 0; }
  bool pending() const { return pendingMode_ >= 0; }

  // Start a probe exchange toward targetMode. Returns the request to
  // transmit (probeId + how many PROBE frames: 1, or 2 for M12+), or
  // nullopt when one is already pending / cooling down.
  struct Request {
    uint8_t probeId;
    int mode;
    int count;
  };
  std::optional<Request> begin(int targetMode, double now);

  // Peer verdict for one PROBE (id must match). All expected acks must
  // be CLEAN to clear; any marginal/failed verdict ends the exchange and
  // starts the cooldown. Returns true when the gate just cleared.
  bool onProbeAck(uint8_t probeId, uint8_t quality);

  // Every observed frame ticks the cooldown; the timeout fails an
  // unanswered exchange.
  void onFrameObserved();
  void tick(double now);

  // Promotion consumed the clearance (mode change applied).
  void consumeClearance() { clearedMode_ = -1; }

 private:
  void fail();

  ProbeGateConfig cfg_;
  int pendingMode_ = -1;
  uint8_t probeId_ = 0;
  int acksNeeded_ = 0;
  int cleanAcks_ = 0;
  double deadline_ = 0.0;
  int cooldown_ = 0;
  int clearedMode_ = -1;
};

}  // namespace skw
