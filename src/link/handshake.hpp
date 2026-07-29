// Handshake policy logic (PROTOCOL.md "Handshake", LINK_ADAPTATION.md):
// body construction/evaluation for CALL / CALL_ACK / START / START_ACK-NAK,
// capability intersection, quality-seeded initial mode, ULTRA fallback
// schedule. Pure logic - the FSM drives WHEN, this decides WHAT.
#pragma once

#include <optional>
#include <string>
#include <variant>

#include "core/constants.hpp"
#include "link/frame.hpp"

namespace skw {

inline constexpr uint8_t kProtocolVersion = 1;

// Selectivity gate for proposing M12+ at handshake (CALL_ACK selectivity
// index units, 0..255; deep notches make 64QAM pointless at any SNR).
inline constexpr uint8_t kSelectivityGateM12 = 96;

// Initial proposals cap at M10: the M11+ high-mode gates (retry ratio,
// pilot stability, probes) need live traffic that does not exist yet.
inline constexpr uint8_t kInitialModeCap = 10;

struct HandshakePolicy {
  std::string ownCall;       // canonical
  Family family = Family::Wide;
  bool ultraEnable = false;
  bool familyFallback = true;  // unanswered ULTRA calls retry at WIDE
  uint16_t caps = 0;           // own advertised capability bitmap
  uint8_t maxMode = 13;        // config cap (AMC + proposal clamp)
  bool busy = false;           // session already active
  bool policyRefuse = false;   // operator/policy block hook
};

// One direction's quality measurement of a received handshake frame.
struct QualityMeasurement {
  double snrDb = 0.0;
  uint8_t selectivity = 0;
  double cfoHz = 0.0;
};

// Highest mode justified by a quality measurement: fading floor + the AMC
// promote margin (+2.5 dB), M12+ selectivity gate, kInitialModeCap, then
// clamped to [M4, maxMode]. kFadingNA rows are never eligible.
uint8_t initialModeFromQuality(Family f, double effectiveSnrDb,
                               uint8_t worstSelectivity, uint8_t maxMode);

// MAC burst profile id for a mode (PROTOCOL.md table rows 0/1/2).
uint8_t macProfileForMode(uint8_t mode);

// ---------------------------------------------------------------- initiator

CallBody makeCall(const HandshakePolicy& p, const std::string& dstCall);

// CALL_ACK received: build START. Effective SNR = min(peer's measurement
// of our CALL, our measurement of the CALL_ACK).
StartBody makeStart(const HandshakePolicy& p, const CallAckBody& peerAck,
                    const QualityMeasurement& ownMeas);

// Family to use for CALL attempt `attempt` (0-based) of `totalAttempts`:
// implements ULTRA fallback - first half (rounded up) at ULTRA, remaining
// attempts at WIDE when family_fallback is enabled.
Family callFamilyForAttempt(const HandshakePolicy& p, int attempt,
                            int totalAttempts);

// ---------------------------------------------------------------- responder

// CALL received. nullopt = stay silent (not addressed to us, family not
// permitted); otherwise the CALL_ACK to send. Silence is deliberate: there
// is no CALL_NAK on the air - negotiable rejections happen at START.
std::optional<CallAckBody> respondToCall(const HandshakePolicy& p,
                                         const CallBody& call,
                                         const QualityMeasurement& meas);

// START received: accept (START_ACK, final mode = min(proposal, own view))
// or reject with a reason. ownMeas is the responder's freshest quality
// view (its measurement of the START/CALL exchange).
std::variant<StartAckBody, StartNakBody> evaluateStart(
    const HandshakePolicy& p, const CallBody& call, const StartBody& start,
    const QualityMeasurement& ownMeas);

}  // namespace skw
