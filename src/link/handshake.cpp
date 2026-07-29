#include "link/handshake.hpp"

#include <algorithm>

namespace skw {

uint8_t initialModeFromQuality(Family f, double effectiveSnrDb,
                               uint8_t worstSelectivity, uint8_t maxMode) {
  const uint8_t cap =
      std::min<uint8_t>(std::min<uint8_t>(maxMode, kInitialModeCap),
                        uint8_t(kNumModes - 1));
  uint8_t best = uint8_t(kFirstMode);
  for (uint8_t m = uint8_t(kFirstMode); m <= cap; ++m) {
    const double floor = fadingFloorDb(f, m);
    if (floor >= 90.0) continue;  // kFadingNA: never an initial proposal
    if (effectiveSnrDb < floor + 2.5) continue;  // AMC promote margin
    if (m >= 12 && worstSelectivity > kSelectivityGateM12) continue;
    best = m;
  }
  return best;
}

uint8_t macProfileForMode(uint8_t mode) {
  if (mode <= 6) return 0;
  if (mode <= 9) return 1;
  return 2;
}

CallBody makeCall(const HandshakePolicy& p, const std::string& dstCall) {
  CallBody c;
  c.srcCall = p.ownCall;
  c.dstCall = dstCall;
  c.version = kProtocolVersion;
  c.family = uint8_t(p.family);
  c.caps = p.caps;
  return c;
}

StartBody makeStart(const HandshakePolicy& p, const CallAckBody& peerAck,
                    const QualityMeasurement& ownMeas) {
  StartBody s;
  s.caps = uint16_t(p.caps & peerAck.caps);  // committed intersection
  const double peerSnr = peerAck.snrHalfDb * 0.5;
  const double effSnr = std::min(peerSnr, ownMeas.snrDb);
  const uint8_t worstSel =
      std::max(peerAck.selectivity, ownMeas.selectivity);
  s.initialMode =
      initialModeFromQuality(p.family, effSnr, worstSel, p.maxMode);
  s.cpMode = uint8_t(kCpDefault);
  s.maxMode = p.maxMode;
  s.macProfile = macProfileForMode(s.initialMode);
  return s;
}

Family callFamilyForAttempt(const HandshakePolicy& p, int attempt,
                            int totalAttempts) {
  if (p.family != Family::Ultra || !p.familyFallback) return p.family;
  const int ultraAttempts = (totalAttempts + 1) / 2;
  return attempt < ultraAttempts ? Family::Ultra : Family::Wide;
}

std::optional<CallAckBody> respondToCall(const HandshakePolicy& p,
                                         const CallBody& call,
                                         const QualityMeasurement& meas) {
  // Not addressed to us: monitor silently.
  if (call.dstCall != p.ownCall) return std::nullopt;
  // Placeholder callsigns never answer (TX paths are blocked anyway).
  if (p.ownCall == kPlaceholderCall || call.srcCall == kPlaceholderCall) {
    return std::nullopt;
  }
  // A family we do not operate cannot even be detected in practice
  // (per-family chirp spans); an ULTRA CALL reaching a non-ULTRA config
  // via a shared detector still stays silent.
  if (Family(call.family) == Family::Ultra && !p.ultraEnable) {
    return std::nullopt;
  }
  CallAckBody ack;
  const double snr = std::clamp(meas.snrDb, -63.5, 63.5);
  ack.snrHalfDb = int8_t(std::lround(snr * 2.0));
  ack.selectivity = meas.selectivity;
  const double cfoHalf = std::clamp(meas.cfoHz / 0.5, -127.0, 127.0);
  ack.cfoHalfHz = int8_t(std::lround(cfoHalf));
  ack.caps = p.caps;
  return ack;
}

std::variant<StartAckBody, StartNakBody> evaluateStart(
    const HandshakePolicy& p, const CallBody& call, const StartBody& start,
    const QualityMeasurement& ownMeas) {
  auto nak = [](StartNakReason r) {
    return std::variant<StartAckBody, StartNakBody>(StartNakBody{r});
  };
  if (call.version != kProtocolVersion) {
    return nak(StartNakReason::VersionMismatch);
  }
  if (Family(call.family) != p.family &&
      !(Family(call.family) == Family::Ultra && p.ultraEnable) &&
      !(p.family == Family::Ultra &&
        Family(call.family) == Family::Wide)) {
    // The family rode in on the CALL; a responder configured for a
    // different family answers only its own (ULTRA configs also accept
    // WIDE - the fallback path must be able to land).
    return nak(StartNakReason::FamilyMismatch);
  }
  if (p.busy) return nak(StartNakReason::Busy);
  if (p.policyRefuse) return nak(StartNakReason::PolicyRefusal);
  // The committed caps must be a subset of what BOTH sides advertised;
  // a peer committing something we never offered is a conflict.
  if (start.caps & uint16_t(~p.caps)) {
    return nak(StartNakReason::CapabilityConflict);
  }

  StartAckBody ack;
  const uint8_t ownView = initialModeFromQuality(
      Family(call.family), ownMeas.snrDb, ownMeas.selectivity, p.maxMode);
  ack.finalMode = std::min(start.initialMode, ownView);  // only lower
  return ack;
}

}  // namespace skw
