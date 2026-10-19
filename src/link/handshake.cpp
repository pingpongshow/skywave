#include "link/handshake.hpp"

#include <algorithm>

#include "link/amc.hpp"

namespace skw {

uint8_t initialModeFromQuality(Family f, double effectiveSnrDb,
                               uint8_t worstSelectivity, uint8_t maxMode,
                               bool cssTier) {
  const uint8_t cap =
      std::min<uint8_t>(std::min<uint8_t>(maxMode, kInitialModeCap),
                        uint8_t(kNumModes - 1));
  const uint8_t lowest = cssTier ? 0 : uint8_t(kFirstMode);
  uint8_t best = lowest;
  for (uint8_t m = lowest; m <= cap; ++m) {
    const double floor = fadingFloorDb(f, m);
    if (floor >= 90.0) continue;  // kFadingNA: never an initial proposal
    // AMC promote margin (amc.hpp): one operating point for the initial
    // proposal and the ladder, in the calibrated table convention.
    if (effectiveSnrDb < floor + AmcConfig{}.promoteMarginDb) continue;
    // The AMC's M12+ gate. (Inert while kInitialModeCap is M10; kept so
    // the proposal can never undercut the ladder's own rule.)
    if (m >= 12 && worstSelectivity > AmcConfig{}.selectivityMax) continue;
    best = m;
  }
  return best;
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
  s.initialMode = initialModeFromQuality(p.family, effSnr, worstSel,
                                         p.maxMode,
                                         (s.caps & kCapCssTier) != 0);
  if (p.chanMemMode) {
    s.initialMode = std::min(s.initialMode, *p.chanMemMode);
  }
  s.maxMode = std::min<uint8_t>(p.maxMode, uint8_t(kNumModes - 1));
  return s;
}

Family narrowerFamily(Family f) {
  switch (f) {
    case Family::Ultra: return Family::Wide;
    case Family::Wide: return Family::Mid;
    case Family::Mid: return Family::Narrow;
    case Family::Narrow: break;
  }
  return Family::Narrow;
}

Family callFamilyForAttempt(const HandshakePolicy& p, int attempt,
                            int totalAttempts) {
  if (!p.familyFallback) return p.family;
  // First half of the attempts on the configured family, then one step
  // narrower. This only lands because the RESPONDER also scans one family
  // below its own (LinkEngine::rxBurst) - without that a WIDE listener is
  // simply deaf to a MID call, which is why the schedule existed for
  // three phases without ever being wired in.
  const int ownAttempts = (totalAttempts + 1) / 2;
  return attempt < ownAttempts ? p.family : narrowerFamily(p.family);
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
  // The committed caps must be a subset of what BOTH sides advertised;
  // a peer committing something we never offered is a conflict.
  if (start.caps & uint16_t(~p.caps)) {
    return nak(StartNakReason::CapabilityConflict);
  }

  StartAckBody ack;
  // The session cap is the lower of the two stations' max_mode: the
  // responder's operator limit binds the initiator's AMC too.
  ack.maxMode = std::min(start.maxMode, p.maxMode);
  const bool cssTier = (start.caps & p.caps & kCapCssTier) != 0;
  uint8_t ownView =
      initialModeFromQuality(Family(call.family), ownMeas.snrDb,
                             ownMeas.selectivity, p.maxMode, cssTier);
  if (p.chanMemMode) ownView = std::min(ownView, *p.chanMemMode);
  ack.finalMode = std::min({start.initialMode, ownView, ack.maxMode});
  return ack;
}

}  // namespace skw
