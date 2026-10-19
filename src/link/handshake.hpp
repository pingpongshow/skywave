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

// Initial proposals cap at M10: the M11+ high-mode gates (retry ratio,
// pilot stability, probes) need live traffic that does not exist yet.
inline constexpr uint8_t kInitialModeCap = 10;

struct HandshakePolicy {
  std::string ownCall;       // canonical
  Family family = Family::Wide;
  bool ultraEnable = false;
  bool familyFallback = true;  // CALL retries / listening one family narrower
  uint16_t caps = 0;           // own advertised capability bitmap
  uint8_t maxMode = 13;        // config cap (AMC + proposal clamp)
  // CSS deep tier (6.2): advertise CAP_CSS_TIER and alternate CALL
  // attempts onto CSS M0 so a connect can land below the OFDM robust
  // handshake floor.
  bool cssEnable = false;
  // Channel-memory recommendation for this peer/frequency/family
  // (LINK_ADAPTATION.md). When present it CLAMPS the quality-derived
  // proposal - the LOWER of the two wins, on both sides of the
  // handshake, so a channel that historically misbehaved is not
  // re-attacked at a mode one clean measurement happened to justify.
  std::optional<uint8_t> chanMemMode;
};

// One direction's quality measurement of a received handshake frame.
struct QualityMeasurement {
  double snrDb = 0.0;
  uint8_t selectivity = 0;
  double cfoHz = 0.0;
  // False when the frame was too short to converge the tracker's noise
  // estimate (OFDM header-piggyback-only frames: ACKs, MODE_ACK, grants).
  // Their SNR figure is the seed guess, not a measurement; feeding it to
  // the AMC's EMA alongside converged DATA measurements swung the
  // effective SNR 14 -> 8.5 -> 7 dB within seconds on a STATIC channel
  // and flapped the ladder (churn seed 12, t=418-479).
  bool snrTrusted = true;
};

// Highest mode justified by a quality measurement: fading floor + the AMC
// promote margin (AmcConfig::promoteMarginDb), the AMC's M12+ selectivity
// gate (AmcConfig::selectivityMax), kInitialModeCap, then
// clamped to [M4, maxMode] - or [M0, maxMode] when the session committed
// CAP_CSS_TIER (the deep tier both extends the ladder downward and is
// the only floor that exists below the OFDM handshake threshold).
// kFadingNA rows are never eligible.
uint8_t initialModeFromQuality(Family f, double effectiveSnrDb,
                               uint8_t worstSelectivity, uint8_t maxMode,
                               bool cssTier = false);

// One step DOWN the bandwidth ladder, or the same family when there is
// nowhere narrower to go. ULTRA -> WIDE -> MID -> NARROW: each step trades
// rate for a smaller noise bandwidth, which is what buys margin when a
// handshake will not close (measured MID 3% CALL FER at +3 dB through the
// radio chain where WIDE is 50% - docs/conformance/handshake_chain.txt).
Family narrowerFamily(Family f);

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

// START received: accept (START_ACK, final mode = min(proposal, own view),
// session max mode = min(both caps)) or reject with a reason. ownMeas is the responder's freshest quality
// view (its measurement of the START/CALL exchange).
std::variant<StartAckBody, StartNakBody> evaluateStart(
    const HandshakePolicy& p, const CallBody& call, const StartBody& start,
    const QualityMeasurement& ownMeas);

}  // namespace skw
