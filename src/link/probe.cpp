#include "link/probe.hpp"

namespace skw {

std::optional<ProbeGate::Request> ProbeGate::begin(int targetMode,
                                                   double now) {
  if (pendingMode_ >= 0 || cooldown_ > 0) return std::nullopt;
  if (cleared(targetMode)) return std::nullopt;  // already usable
  pendingMode_ = targetMode;
  ++probeId_;
  acksNeeded_ = targetMode >= 12 ? 2 : 1;
  cleanAcks_ = 0;
  deadline_ = now + cfg_.timeoutSec;
  return Request{probeId_, targetMode, acksNeeded_};
}

void ProbeGate::fail() {
  pendingMode_ = -1;
  cleanAcks_ = 0;
  cooldown_ = cfg_.cooldownFrames;
}

bool ProbeGate::onProbeAck(uint8_t probeId, uint8_t quality) {
  if (pendingMode_ < 0 || probeId != probeId_) return false;
  if (quality != kProbeClean) {
    fail();
    return false;
  }
  if (++cleanAcks_ < acksNeeded_) return false;
  clearedMode_ = pendingMode_;
  pendingMode_ = -1;
  return true;
}

void ProbeGate::onFrameObserved() {
  if (cooldown_ > 0) --cooldown_;
}

void ProbeGate::tick(double now) {
  if (pendingMode_ >= 0 && now >= deadline_) fail();
}

}  // namespace skw
