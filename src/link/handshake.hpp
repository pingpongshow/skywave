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
  // CSS deep tier (6.2): advertise CAP_CSS_TIER and alternate CALL
  // attempts onto CSS M0 so a connect can land below the OFDM robust
  // handshake floor.
  bool cssEnable = false;
};

// One direction's quality measurement of a received handshake frame.
struct QualityMeasurement {
  double snrDb = 0.0;
  uint8_t selectivity = 0;
  double cfoHz = 0.0;
};

// Highest mode justified by a quality measurement: fading floor + the AMC
// promote margin (+2.5 dB), M12+ selectivity gate, kInitialModeCap, then
// clamped to [M4, maxMode] - or [M0, maxMode] when the session committed
// CAP_CSS_TIER (the deep tier both extends the ladder downward and is
// the only floor that exists below the OFDM handshake threshold).
// kFadingNA rows are never eligible.
uint8_t initialModeFromQuality(Family f, double effectiveSnrDb,
                               uint8_t worstSelectivity, uint8_t maxMode,
                               bool cssTier = false);

// CALL transport for 0-based attempt N: OFDM robust first (fast
// connect at workable SNR), then STRICT ALTERNATION with CSS.
//
// The original 2-CSS-per-OFDM split assumed CSS dominates once an OFDM
// call goes unanswered. Measuring CALL-frame FER through the FULL
// radio chain (docs/conformance/handshake_chain.txt - the measurement
// conformance never made, since it only covered clean baseband) shows
// that is only true BELOW ~0 dB:
//
//   WIDE, CCIR Moderate:  +9 dB  OFDM  0%  / CSS 13%
//                         +3 dB  OFDM 50%  / CSS 27%
//                          0 dB  OFDM 87%  / CSS 50%
//
// CSS also carries an SNR-INDEPENDENT ~10-13% fading floor, so
// spending two thirds of the ladder on it made reconnection at
// marginal SNR markedly WORSE than alternating: unanswered calls
// happen at every SNR (the peer may simply be transmitting), and the
// old split answered every one of them by mostly abandoning the
// transport that actually works above 0 dB.
inline bool callOnCssForAttempt(const HandshakePolicy& p, int attempt) {
  return p.cssEnable && (attempt % 2) != 0;
}

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
