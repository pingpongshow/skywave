#include "link/engine.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "dsp/morse.hpp"

namespace skw {

namespace {

FsmConfig fsmConfigFor(const HandshakePolicy& p) {
  // Airtime-derived handshake timings from the real PHY (PROTOCOL.md).
  // With the CSS tier enabled the timers must cover the SLOWER of the
  // two CALL transports - a deep-tier frame runs 10x the OFDM airtime.
  std::vector<uint8_t> callSized(48), startSized(24), discSized(16);
  auto air = [&](std::span<const uint8_t> body) {
    TxFrameParams tp;
    tp.family = p.family;
    tp.sig = ChirpSig::Calling;
    tp.mode = 4;
    tp.payload = body;
    double t = frameAirtimeSec(tp);
    if (p.cssEnable) {
      tp.sig = ChirpSig::Css;
      tp.mode = 0;
      t = std::max(t, frameAirtimeSec(tp));
    }
    return t;
  };
  FsmConfig c;
  c.timings.callSec = air(callSized);
  c.timings.callAckSec = air(callSized);
  c.timings.startSec = air(startSized);
  c.timings.startAckSec = air(startSized);
  c.timings.discSec = air(discSized);
  if (p.cssEnable) {
    // The CALL ladder alternates OFDM/CSS transports, halving the tries
    // each transport gets - and deep-tier frames near the fading floor
    // run ~50% one-shot FER by design (ARQ/retry territory). Double the
    // ladder so a deep connect keeps its odds.
    c.callAttempts = 8;
    c.startAttempts = 8;
  }
  c.jitterSeed = callsignHash24(p.ownCall);  // CALL-retry backoff jitter
  return c;
}

// Chirp signature for a transmit mode: CSS modes announce themselves
// with the Css pattern (sync arbitration keys off the preamble alone);
// OFDM splits robust (M4-M7) vs fast.
ChirpSig sigForMode(int mode) {
  if (kModes[size_t(mode)].waveform == Waveform::Css) return ChirpSig::Css;
  return mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
}

int macWindow(int mode) {
  // CSS modes run PURE PING-PONG (window 1): frame airtimes are tens
  // of seconds, and a second in-flight frame pushes the peer's ACK
  // past even a correctly-sized timeout (the transmitter is deaf while
  // keyed - the ACK cannot interleave).
  if (mode <= 3) return 1;
  if (mode <= 6) return 2;
  // M7-M9 carry TWO frames per burst container, so a window of 4 leaves
  // at most two frames of headroom - and with any in flight the
  // container ships short. MEASURED under a loaded queue (TURNSTAT,
  // real keyed airtime): at M8-M9 the container carried 1.0-1.5 of its
  // 2 frames and the turn was 27-52% keyed, against 65-76% and 2.5-3.2
  // of 4 frames at M10+ where the window is 16.
  //
  // Raising it to 8 was MEASURED and NOT adopted: churn seed 12
  // 63,344 -> 87,749 B (+39%) and seed 11 +0.5%, coupled identical -
  // but seed 13 goes from PASS (89,473 B) to a stall at t=741. Turning
  // a passing seed into a wedge disqualifies it as a default however
  // good the average looks; the deeper window keeps more unacknowledged
  // frames alive across a fade, which is exactly what that seed's tail
  // punishes. SKW_MID_WINDOW re-enables it for measurement.
  static const int kMidWindow = [] {
    const char* e = getenv("SKW_MID_WINDOW");  // diagnostic A/B
    return e ? atoi(e) : 4;
  }();
  if (mode <= 9) return kMidWindow;
  // HF window above M9: 16 (was 8). TURNSTAT at M10-M13 showed ~2.4 KB
  // per round trip against a 3-8 KB RTT x rate product - window-
  // limited. Measured at 16: one-way half-duplex 86,016 -> 106,496 B /
  // 300 s, coupled two-way churn 91.2 -> 196.7 KB, every gate green.
  // The old caution that a deeper window would time out its own
  // head-of-line burst predated the measured-RTT timeout plus the
  // TX-queue delay term and did not reproduce. SKW_HF_WINDOW is the A/B.
  static const int kHfWindow = [] {
    const char* e = getenv("SKW_HF_WINDOW");
    return e ? atoi(e) : 16;
  }();
  if (mode >= 10 && activeBand() != Band::VhfFm) return kHfWindow;
  // M13+ window 16 on VHF-FM only: the top rungs there need >= 2 full
  // containers in flight to hide the ACK round trip (PROTOCOL.md MAC
  // profiles). On HF the ARQ timeout budgets one container of airtime,
  // so a deeper window would time out its own head-of-line burst.
  if (mode >= 13 && activeBand() == Band::VhfFm) return 16;
  return 8;
}

// CPMODE AUTO thresholds: smallest CP covering the measured spread with
// margin for sync slack (CP lengths 2/4/6 ms at baseband).
CpMode cpForSpread(double spreadMs) {
  if (spreadMs < 1.2) return CpMode::Short;
  if (spreadMs < 3.2) return CpMode::Medium;
  return CpMode::Long;
}

int framesPerBurst(int mode) {
  // Robust tier frames per burst: diagnostic A/B (SKW_ROBUST_FPB).
  // TURNSTAT at M5/M6: one frame per burst, each with its own ACK wait,
  // 5 frames in a 60-80 s turn.
  static const int kRobustFpb = [] {
    const char* e = getenv("SKW_ROBUST_FPB");
    return e ? atoi(e) : 1;
  }();
  if (mode <= 6) return kRobustFpb;
  if (mode <= 9) return 2;
  return 4;
}

// Resume window (PROTOCOL.md "Session Resume").
constexpr double kResumeWindowSec = 600.0;
// Turn-taking minimum hold (PROTOCOL.md "Turn-Taking"), in measured
// round trips: once the peer has asked for the turn, the ISS keeps it at
// least this long so every turn moves at least a window's worth of data
// before the handover cost is paid again, then hands over at the next
// window boundary. Two round trips = one full send-and-acknowledge
// cycle. The previous scheme held a fixed max(45 s, 3 x ARQ timeout)
// "quantum" sized to the store-and-forward world's inflated RTT.
// Measured with TURNSTAT at two round trips: the ISS used 7-40% of its
// own turn (M6 22%, M8 23%, M10 15-19%, M13 7%) - one window, its ACK
// wait, then the handover, whose cost is itself ~1 RTT. Four round
// trips amortise it (SKW_TURN_HOLD_RTTS is the A/B).
constexpr double kTurnMinHoldRtts = 4.0;
// Absolute floor on the hold: at the fast tier a round trip is ~3 s,
// so four of them is one burst and its ACK (TURNSTAT at M13: 15 s
// turns carrying 4.6 frames). SKW_TURN_HOLD_MIN_SEC is the A/B.
constexpr double kTurnMinHoldSec = 6.0;
// Probe "clean" threshold relative to the candidate rung's floor, in the
// calibrated (3 kHz-noise) SNR convention. Was ref + 1.0 read against an
// estimate ~4 dB optimistic on WIDE (constants.hpp kOfdmSnrCalDb), i.e.
// ref - 3.0 in table terms; kept at that measured operating point.
constexpr double kProbeCleanMarginDb = -3.0;

}  // namespace

LinkEngine::LinkEngine(EngineConfig cfg)
    : cfg_(std::move(cfg)),
      fsm_(fsmConfigFor(cfg_.policy)),
      arqTx_(2, cfg_.arqMaxRetries),
      arqRx_(8) {
  deepIdleSec_ =
      std::max(120.0, 10.0 * fsmConfigFor(cfg_.policy).timings.callSec);
  // CCA backoff jitter: per-station seed so two saturated stations
  // don't lock into synchronized proceed times.
  ccaRng_ = 0x9e3779b97f4a7c15ull;
  for (char c : cfg_.policy.ownCall) ccaRng_ = ccaRng_ * 131 + uint64_t(c);
  configuredFamily_ = cfg_.policy.family;
  rxFamily_ = cfg_.policy.family;
}

// Run this session on `f`. Everything in the engine reads
// cfg_.policy.family, so a single assignment moves TX framing, RX
// demodulation, masks, voice-codec selection and the channel-memory key
// together - which is why it must only happen BEFORE a session's objects
// are built, and be undone when the session ends.
void LinkEngine::adoptSessionFamily(Family f, double now) {
  (void)now;
  if (f == cfg_.policy.family) return;
  cfg_.policy.family = f;
}

// Pre-TX CCA gate: true while DATA-class emission must hold. The busy
// flag is the worker's fused energy detector; without a worker feeding
// it (engine-level tests, bertool) the gate never engages.
bool LinkEngine::ccaDataGated(double now) {
  static const bool dbg = getenv("SKW_CCA_DEBUG") != nullptr;
  if (dbg) {
    fprintf(stderr, "%s cca t=%.1f busy=%d txq=%.2f wait=%.1f proceed=%.1f q=%zu inflight=%zu backlog=%zu\n",
            cfg_.policy.ownCall.c_str(), now, chanBusy_ ? 1 : 0, txQueueSec_,
            ccaWaitStart_, ccaProceedAt_, outQueue_.size(),
            arqTx_.inFlight(), protoTx_.backlogBytes());
  }
  // Half-duplex discipline: at most ONE data burst in the worker's TX
  // path. Stacking bursts into the 8 s ring kept the transmitter keyed
  // continuously, starved the receiver (a keyed radio is deaf), and
  // hid the channel from the busy detector.
  if (txQueueSec_ > 0.35) {
    txWasDraining_ = true;
    ccaReason_ = 1;
    return true;
  }
  if (turnSchedActive()) {
    // Scheduled turn-taking: the role, not the busy detector, arbitrates
    // DATA. Occupancy sensing is deaf inside fades and while keyed - the
    // measured failure mode of every fading soak - so it plays no part
    // here. What remains physical: one burst in the TX path (above) and
    // the post-burst listen window sized to the measured round trip.
    if (txWasDraining_) {
      txWasDraining_ = false;
      const bool batchDue =
          (activeBand() == Band::VhfFm && mode_ >= 13)
              ? int(arqTx_.inFlight()) >= 12
              : !arqTx_.canSend();
      // Only a burst that CARRIED DATA owes a listen turn: it is the
      // frames just put in flight that the peer must answer. A control
      // burst (MODE_CHANGE retry, MASK_UPDATE, standalone ACK) owes
      // nothing, yet used to re-arm a full-srtt data yield every time it
      // aired - and with the window shrunk under a stale in-flight set
      // (`!canSend()` permanently true) the initiator's MODE_CHANGE
      // retries kept the data path in a listen turn almost continuously
      // (measured: zero data retransmitted across a 100 s stall).
      if (batchDue && arqTx_.inFlight() > 0 && lastBurstHadData_) {
        yieldInFlight_ = arqTx_.inFlight();
        listenTurnStart_ = now;
        ccaProceedAt_ =
            now + (arqTx_.haveRtt() ? arqTx_.srttSec() : ackAirSec_ + 1.0);
      }
    }
    // HALF-DUPLEX PHYSICS, not arbitration: the ISS never keys over a
    // burst that is still airing. The busy detector arbitrates nothing
    // here, but "the peer is keyed, therefore deaf" is a physical fact
    // the schedule cannot repeal. Measured without this: the new ISS
    // keyed the instant it decoded TURN_GRANT while the REST of the
    // granting burst (the ACK and probe frames bundled behind it) was
    // still airing, so the peer never heard the confirming DATA; every
    // retransmit then landed the same way, the granter reclaimed, and
    // the link spent a minute with two senders. Frequent handovers
    // turned a rare collision into a systematic one. Same rule and same
    // runaway cap as the control-frame hold (30 x airScale_ outlasts
    // any legitimate peer burst, deep tier included); no post-clear
    // jitter - the ISS is the only sender, so it fires the instant the
    // channel clears.
    if (chanBusy_) {
      if (ccaWaitStart_ < 0.0) ccaWaitStart_ = now;
      if (now - ccaWaitStart_ < 30.0 * airScale_) {
        ccaReason_ = 6;
        return true;
      }
    } else {
      ccaWaitStart_ = -1.0;
    }
    if (turnRole_ != TurnRole::Iss) {
      ccaReason_ = 5;
      return true;
    }
    if (ccaProceedAt_ >= 0.0) {
      // Feedback ends the listen turn early (see the legacy path note).
      if ((yieldInFlight_ > 0 && arqTx_.inFlight() < yieldInFlight_) ||
          listenFeedbackArrived()) {
        yieldInFlight_ = 0;
        listenTurnStart_ = -1.0;
        ccaProceedAt_ = -1.0;
        ccaReason_ = 0;
        return false;
      }
      if (now < ccaProceedAt_) {
        ccaReason_ = 4;
        return true;
      }
      yieldInFlight_ = 0;
      ccaProceedAt_ = -1.0;
    }
    ccaReason_ = 0;
    return false;
  }
  if (txWasDraining_) {
    txWasDraining_ = false;
    double gap = 0.0;
    // Our burst just finished airing - the channel HANDOFF point of a
    // half-duplex link. Two reasons to stay quiet:
    // (a) Feedback is owed: a keyed transmitter is deaf, so keying the
    //     next burst over the peer's incoming ACK loses it and burns a
    //     full ARQ timeout per window. Threshold matches the peer's
    //     feedback cadence: per-burst on HF, per-12-frame batch on the
    //     VHF-FM deep pipe (mid-batch, no ACK is due - containers stay
    //     back-to-back and batch throughput is preserved).
    //     The trigger is a FULL WINDOW, not "one container sent". The
    //     window exists precisely to keep frames in flight across the
    //     round trip; yielding after every container threw that away
    //     and capped a turn at one container's worth of data while the
    //     round trip stayed the same.
    const bool batchDue =
        (activeBand() == Band::VhfFm && mode_ >= 13)
            ? int(arqTx_.inFlight()) >= 12
            : !arqTx_.canSend();
    if (batchDue && arqTx_.inFlight() > 0) {
      // Size the quiet turn to the peer's ACTUAL decode-and-reply
      // latency. The old `ackAirSec_ + 1.0` budgeted feedback AIRTIME
      // plus a fixed pad, but the measured one-way latency in the ST2
      // world was a median 12.4 s - dominated by the receiver's decode
      // ladder, not by airtime - so the sender keyed again long before
      // the peer could answer, and the ACK it was waiting for could
      // never be sent. The RTT estimate covers the whole turnaround;
      // the yield ends early the moment feedback actually arrives.
      gap = arqTx_.haveRtt() ? arqTx_.srttSec() : ackAirSec_ + 1.0;
      yieldInFlight_ = arqTx_.inFlight();
      listenTurnStart_ = now;
    }
    // (b) Bidirectional fairness: if the peer is actively sending data
    //     too, add a jittered yield so it can key up - without this the
    //     side with more traffic captures the channel and the reverse
    //     direction starves (observed: 0 B B->A over 900 s).
    if (now - lastDataRxAt_ < dataRecencySec_ && lastDataRxAt_ > 0.0) {
      ccaRng_ ^= ccaRng_ >> 12;
      ccaRng_ ^= ccaRng_ << 25;
      ccaRng_ ^= ccaRng_ >> 27;
      // A courtesy gap only has to be long enough for the peer's busy
      // detector to see the channel clear - fixed latency, not airtime.
      gap = std::max(gap, 0.25 + 0.5 * double(ccaRng_ % 1000) / 1000.0);
    }
    if (gap > 0.0) {
      ccaProceedAt_ = now + gap;
      ccaReason_ = 2;
      return true;
    }
  }
  if (chanBusy_) {
    ccaProceedAt_ = -1.0;
    if (ccaWaitStart_ < 0.0) ccaWaitStart_ = now;
    // CONNECTED sessions hold for a CLEAR channel: the busy source is
    // almost always our own peer, and "proceeding anyway" keys straight
    // over it - observed as simultaneous transmissions on a live shared
    // acoustic channel whenever a burst outlived the old 5 s cap. A
    // CONTINUOUS jammer (busy that never clears) is instead broken by
    // the 3x-timeout anti-starvation override in pumpData/emission -
    // data still moves, one forced burst per backstop period. The 5 s
    // courtesy cap stands only OUTSIDE a session.
    if (fsm_.state() != FsmState::Connected &&
        now - ccaWaitStart_ >= 5.0) {
      ccaWaitStart_ = -1.0;
      ccaReason_ = 0;
      return false;
    }
    ccaReason_ = 3;
    return true;
  }
  if (ccaWaitStart_ >= 0.0 && ccaProceedAt_ < 0.0) {
    // Channel just cleared after a defer: short randomized backoff so
    // the two ends of a saturated link interleave instead of keying
    // together the instant the other stops.
    ccaRng_ ^= ccaRng_ >> 12;
    ccaRng_ ^= ccaRng_ << 25;
    ccaRng_ ^= ccaRng_ >> 27;
    // Spread wider than busy-detection latency (see control jitter).
    ccaProceedAt_ = now + 0.15 + 0.55 * double(ccaRng_ % 1000) / 1000.0;
  }
  if (ccaProceedAt_ >= 0.0) {
    // FEEDBACK ENDS THE TURN. A yield sized to the round trip would
    // otherwise cost the full RTT even when the ACK came back promptly,
    // halving throughput on a healthy link. A drop in the in-flight
    // count means the peer reported and released frames; feedback that
    // arrived WITHOUT releasing anything (a NAK reporting a head-of-line
    // hole) equally means the channel is ours again and a retransmit is
    // due - either way the turn is over.
    if ((yieldInFlight_ > 0 && arqTx_.inFlight() < yieldInFlight_) ||
        listenFeedbackArrived()) {
      yieldInFlight_ = 0;
      listenTurnStart_ = -1.0;
      ccaProceedAt_ = -1.0;
      ccaWaitStart_ = -1.0;
      ccaReason_ = 0;
      return false;
    }
    if (now < ccaProceedAt_) {
      ccaReason_ = 4;
      return true;
    }
    yieldInFlight_ = 0;
    ccaWaitStart_ = -1.0;
    ccaProceedAt_ = -1.0;
  }
  ccaReason_ = 0;
  return false;
}

// ---------------------------------------------------------- turn-taking
// Scheduled ISS/IRS turn-taking (PROTOCOL.md "Turn-Taking"). Occupancy
// sensing cannot arbitrate a half-duplex link: each end is deaf while
// keyed and deaf again inside a fade, so a busy-gate wedges in exactly
// the conditions that need arbitration most (measured: every fading
// soak stalled with the gate permanently engaged and only the
// starvation backstop trickling data). Every proven HF ARQ system -
// PACTOR, VARA, ARDOP, STANAG 5066 - schedules the channel instead:
// one station holds the sending role, the other answers in its listen
// windows, and an explicit handover swaps the roles.

bool LinkEngine::turnDataWaiting() const {
  if (!protoTx_.idle() || fileTx_.active()) return true;
  if (arqTx_.inFlight() > 0) return true;  // unacked frames may need resend
  for (const auto& q : outQueue_) {
    if (q.env.type == LinkType::Data) return true;
  }
  return false;
}

uint8_t LinkEngine::ackRxFlags() const {
  uint8_t f = 0;
  if (peerModeRequest_ >= 0) {
    f = uint8_t(kRxFlagModeReq | (uint8_t(peerModeRequest_) & kRxFlagModeMask));
  }
  if (turnSchedActive() && turnRole_ == TurnRole::Irs && turnDataWaiting()) {
    f |= kRxFlagBreakReq;
  }
  if (rxFailPending_) f |= kRxFlagRxFail;
  return f;
}

// Proto seal cap (PROTOCOL.md "Payload Pipeline"). The deep tier always
// bounds a stream message to ~8 frames. OFDM modes coalesce to the full
// 4 KiB while the link is clean - a flat cap measurably cost the VHF and
// ramp throughput gates - and drop to the same 8-frame bound once the
// link is LOSSY: a 4 KiB message is ~30 frames at M4, and one stuck
// head-of-line fragment then delivers zero app bytes no matter how many
// later fragments land (measured as floor/churn "stalling" with frames
// still flowing). Called on every mode change and whenever the lossy
// verdict flips.
void LinkEngine::applySealCap() {
  static const bool kLossyCap = getenv("SKW_NO_LOSSY_CAP") == nullptr;  // A/B
  const size_t bounded = std::max<size_t>(8 * dataBudget_, 192);
  size_t cap =
      mode_ <= 3 || (kLossyCap && lossyCoalesce_) ? bounded : size_t(-1);
  // DELIVERY BOUND, mode-following. Proto assembly is head-of-line: the
  // app sees nothing until a whole coalesced message lands, so a
  // message's airtime IS the delivery interval, and a stall watchdog
  // (or a human) sees exactly that. The cap above follows the TIER -
  // deep or lossy - which left the robust tier coalescing the full
  // 4 KiB. MEASURED (SKW_DELIV_DEBUG, churn seed 11): median gap
  // between app-visible deliveries 131.8 s at M7 (max 223.7) and 89.3 s
  // at M6, against the soak's 300 s no-progress watchdog. Bounding a
  // message by frames-per-message instead scales with the mode: at the
  // fast tier the bound lands above coalesceMaxBytes and nothing
  // changes, while the robust tier seals smaller messages that actually
  // arrive. The cost is compression ratio and per-message overhead on
  // small blocks, which is why the target is measured, not assumed.
  static const double kDeliverTarget = [] {
    const char* e = getenv("SKW_DELIVER_TARGET_SEC");  // diagnostic A/B
    return e ? atof(e) : kDeliverTargetSec;
  }();
  // Bytes per second of airtime at the current shape. dataBurstBytes_ is
  // the payload of one TX unit (a burst container carries framesPerBurst
  // frames) and dataFrameAirSec_ is that same unit's airtime, so the two
  // must be paired: dividing container airtime into a single frame's
  // budget under-counts by framesPerBurst and made this bound bite four
  // times too hard at M10+ (measured: coupled churn 196.7 -> 120.7 KB,
  // a tier the bound is supposed to leave alone).
  if (dataFrameAirSec_ > 0.0 && kDeliverTarget > 0.0 &&
      dataBurstBytes_ > 0) {
    const double bytesPerSec = double(dataBurstBytes_) / dataFrameAirSec_;
    const size_t byAirtime =
        std::max<size_t>(size_t(kDeliverTarget * bytesPerSec), 192);
    cap = std::min(cap, byAirtime);
  }
  protoTx_.setSealCap(cap);
}

void LinkEngine::setTurnRole(TurnRole r, double now) {
  if (turnRole_ == r) return;
  static const bool dbg = getenv("SKW_TURN_DEBUG") != nullptr;
  if (dbg && turnRole_ == TurnRole::Iss) {
    // Utilisation of the turn just ended: what the handover cost buys.
    fprintf(stderr,
            "%s TURNSTAT t=%.1f dur=%.1f frames=%d bytes=%zu bursts=%d "
            "air=%.1f util=%.0f%% cfr=%d mode=%d srtt=%.1f\n",
            cfg_.policy.ownCall.c_str(), now, now - roleSince_, turnFrames_,
            turnBytes_, turnBursts_, turnAirSec_,
            now - roleSince_ > 0.0 ? 100.0 * turnAirSec_ / (now - roleSince_)
                                   : 0.0,
            turnContainerFrames_, mode_,
            arqTx_.haveRtt() ? arqTx_.srttSec() : -1.0);
  }
  turnFrames_ = 0;
  turnBytes_ = 0;
  turnBursts_ = 0;
  turnAirSec_ = 0.0;
  turnContainerFrames_ = 0;
  if (dbg) {
    fprintf(stderr, "%s TURN t=%.1f %s->%s dataWait=%d grantAwait=%d "
            "breakReq=%d lastRx=%.1f roleSince=%.1f\n",
            cfg_.policy.ownCall.c_str(), now,
            turnRole_ == TurnRole::Iss ? "ISS" : "IRS",
            r == TurnRole::Iss ? "ISS" : "IRS", turnDataWaiting() ? 1 : 0,
            grantAwaitingData_ ? 1 : 0, breakReqPending_ ? 1 : 0,
            lastDataRxAt_, roleSince_);
  }
  turnRole_ = r;
  roleSince_ = now;
  turnDrainSince_ = -1.0;   // a drain belongs to the turn that started it
  turnDrainRetx_ = false;
  dataQueuedSince_ = -1.0;  // the starvation clock re-arms fresh per role
  // A station without the sending role cannot retransmit, so its ACK
  // timers must not run (they would only burn retry budget and charge
  // the AMC with synthetic failures). Piggybacked ack_base from the new
  // sender's DATA keeps covering the in-flight frames meanwhile.
  arqTx_.setSuspended(turnSchedActive() && r == TurnRole::Irs, now);
}

void LinkEngine::queueTurnGrant() {
  // Grant transport mirrors ACK: at least as robust as forward traffic.
  const bool css = kModes[size_t(mode_)].waveform == Waveform::Css;
  const ChirpSig sig =
      css ? ChirpSig::Css : (mode_ <= 6 ? ChirpSig::Robust : ChirpSig::Fast);
  const int m = mode_ <= 3 ? mode_ : (mode_ <= 6 ? 4 : mode_);
  queueControl(LinkType::TurnGrant, TurnGrantBody{turnEpoch_}.pack(), sig, m);
}

// Grant/reclaim cadence: paced to the MEASURED round trip, not the ARQ
// retransmission timeout. The ARQ timeout carries the heavy-tail
// variance margin (srtt + 4*rttvar, up to 90 s) that keeps the sender
// from retransmitting into the decode tail - correct for data, but far
// too slow for a control handshake. A grant lost at a 59 s ARQ timeout
// left the link with two listeners for minutes (measured: churn stalled
// at t=621 with both ends reporting TURN=ISS after a grant series
// crossed under a mid-fade demotion).
double LinkEngine::turnHandoffSec() const {
  return std::max(6.0, arqTx_.haveRtt() ? 1.5 * arqTx_.srttSec() : 10.0);
}

void LinkEngine::grantTurn(double now) {
  ++turnEpoch_;
  queueTurnGrant();
  if (getenv("SKW_TURN_DEBUG")) {
    fprintf(stderr, "%s GRANT t=%.1f send epoch=%u inflight=%zu\n",
            cfg_.policy.ownCall.c_str(), now, unsigned(turnEpoch_),
            arqTx_.inFlight());
  }
  grantAwaitingData_ = true;
  grantTries_ = 1;
  grantRetryAt_ = now + turnHandoffSec();
  breakReqPending_ = false;
  setTurnRole(TurnRole::Irs, now);
}

// One handshake leg completed: record the round trip if it is
// plausible. Legs that needed an FSM retry are longer than the true
// turnaround, so the SMALLEST observed leg is the honest estimate.
void LinkEngine::noteHandshakeRtt(double now) {
  if (hsTxAt_ < 0.0) return;
  const double rtt = now - hsTxAt_;
  hsTxAt_ = -1.0;
  if (rtt <= 0.0 || rtt > 300.0) return;
  hsRttSec_ = hsRttSec_ > 0.0 ? std::min(hsRttSec_, rtt) : rtt;
}

bool LinkEngine::idleClean() const {
  return outQueue_.empty() && protoTx_.idle() && arqTx_.inFlight() == 0 &&
         !ackPending_;
}

void LinkEngine::connectTo(const std::string& call, double now) {
  remoteCall_ = call;
  isInitiator_ = true;
  callTxCount_ = 0;  // CALL transport ladder restarts (handshake-on-CSS)
  hsOnCss_ = false;
  fsm_.cmdConnect(now);
  drainFsmActions(now);
}

void LinkEngine::disconnect(double now) {
  fsm_.cmdDisconnect(now);
  drainFsmActions(now);
}

void LinkEngine::sendData(std::span<const uint8_t> bytes) {
  protoTx_.submit(bytes, lastNow_);
}

std::vector<uint8_t> LinkEngine::takeReceivedData() {
  auto out = protoRx_.takeBytes();
  // DELIVERY GRANULARITY (SKW_DELIV_DEBUG). Head-of-line assembly means
  // the app sees nothing until a whole coalesced message lands, so this
  // interval - not the frame rate - is what a stall watchdog observes.
  if (!out.empty()) {
    static const bool dbg = getenv("SKW_DELIV_DEBUG") != nullptr;
    if (dbg) {
      fprintf(stderr, "%s DELIV t=%.1f bytes=%zu gap=%.1f mode=%d\n",
              cfg_.policy.ownCall.c_str(), lastNow_, out.size(),
              lastDeliverAt_ < 0.0 ? -1.0 : lastNow_ - lastDeliverAt_, mode_);
    }
    lastDeliverAt_ = lastNow_;
  }
  return out;
}

bool LinkEngine::sendFile(const std::string& name,
                          std::vector<uint8_t> data, uint64_t mtime) {
  return fileTx_.start(nextFileId_++, name, std::move(data), mtime);
}

std::vector<std::string> LinkEngine::takeEvents() {
  auto out = std::move(events_);
  events_.clear();
  return out;
}

bool LinkEngine::resumeUsable(double now) const {
  return resumeCtx_.valid && (cfg_.policy.caps & kCapResume) &&
         resumeCtx_.remoteCall == remoteCall_ &&
         resumeCtx_.family == cfg_.policy.family &&
         (now - resumeCtx_.freshAt) < kResumeWindowSec;
}

std::array<uint8_t, 8> LinkEngine::mintToken(double now) {
  // Deterministic hash entropy (time + callsigns + counter): adequate
  // for v1 link identification, NOT a cryptographic secret - the
  // crypto layer (Phase 5) authenticates sessions properly.
  const uint64_t h = skwHash(
      uint64_t(now * 1e6) ^ (uint64_t(callsignHash24(cfg_.policy.ownCall))
                             << 40) ^
      (uint64_t(callsignHash24(remoteCall_)) << 16) ^ ++tokenCtr_);
  std::array<uint8_t, 8> t{};
  for (int i = 0; i < 8; ++i) t[size_t(i)] = uint8_t(h >> (8 * i));
  return t;
}

void LinkEngine::confirmDeliveredMessages() {
  static const bool dbg = getenv("SKW_REPLAY_DEBUG") != nullptr;
  if (dbg && lastNow_ - replayDbgAt_ > 5.0) {
    replayDbgAt_ = lastNow_;
    const auto o = arqTx_.oldestInFlight();
    fprintf(stderr,
            "%s replay t=%.0f ends=%zu unconf=%zu/%zuB oldest=%d "
            "inflight=%zu\n",
            cfg_.policy.ownCall.c_str(), lastNow_, msgEndSeq_.size(),
            protoTx_.unconfirmedMessages(), protoTx_.unconfirmedBytes(),
            o ? int(*o) : -1, arqTx_.inFlight());
  }
  if (msgEndSeq_.empty()) return;

  // Everything strictly below the oldest still-pending sequence has been
  // acknowledged contiguously, which is exactly the prefix the peer's
  // ARQ delivered in order to its proto layer. Nothing pending at all
  // means everything sent is acknowledged.
  const auto oldest = arqTx_.oldestInFlight();
  size_t n = 0;
  while (!msgEndSeq_.empty()) {
    if (oldest && int16_t(uint16_t(msgEndSeq_.front() - *oldest)) >= 0) break;
    msgEndSeq_.pop_front();
    ++n;
  }
  if (n) protoTx_.confirmMessages(n);
}

void LinkEngine::replayPipelineForFreshPeer() {
  // The peer restarted: it has no assembly buffer and no delivered
  // prefix beyond the messages it fully acknowledged before it died.
  // Rewind to that boundary instead of dropping the pipeline, which is
  // what used to happen and cost the application every byte it had
  // handed us but we had not yet got acknowledged.
  const size_t replayed = protoTx_.unconfirmedBytes();
  protoTx_.replayUnconfirmed();
  msgEndSeq_.clear();
  if (replayed) {
    // AT-LEAST-ONCE, and the application is told so. A message is
    // unconfirmed when we have not seen its last frame acknowledged,
    // which does NOT mean the peer failed to deliver it: a peer that
    // dies between handing the message to its application and getting
    // our way is indistinguishable, from here, from one that never
    // received it. Replaying is the only choice that cannot LOSE data,
    // so the bytes go again and the size of the uncertainty is
    // published rather than hidden. An application that cares can
    // discard this many bytes from the front of what follows.
    events_.push_back("EVT:STREAM_REPLAY " + std::to_string(replayed));
  }
  protoRx_.abortMessage();
  if (fileTx_.active()) {
    // File transfer has its own checkpoint/resume mechanism keyed to a
    // session the peer no longer has; stream replay does not cover it.
    events_.push_back("EVT:FILE_ERROR " + fileTx_.name() + " peer-restart");
    fileTx_.abort();
  }
  pausedForResume_ = false;
}

void LinkEngine::discardPausedPipeline() {
  protoTx_.reset();
  protoRx_.abortMessage();
  if (fileTx_.active()) {
    events_.push_back("EVT:FILE_ERROR " + fileTx_.name() +
                      " disconnected");
    fileTx_.abort();
  }
  pausedForResume_ = false;
}

void LinkEngine::sendCq() {
  CqBody c;
  c.callsign = cfg_.policy.ownCall;
  c.caps = cfg_.policy.caps;
  queueControl(LinkType::Cq, c.pack(), ChirpSig::Calling, 4);
}

void LinkEngine::sendBeacon(double snrFloorDb) {
  BeaconBody b;
  b.callsign = cfg_.policy.ownCall;
  b.snrFloorPlus20 =
      uint8_t(std::clamp(snrFloorDb + 20.0, 0.0, 255.0));
  b.family = uint8_t(cfg_.policy.family);
  queueControl(LinkType::Beacon, b.pack(), ChirpSig::Calling, 4);
}

void LinkEngine::abortSession(double now) {
  resumeCtx_.valid = false;  // ABORT is operator intent: no resume
  fsm_.cmdAbort(now);
  drainFsmActions(now);
}

bool LinkEngine::setCpPolicy(const std::string& p, double now) {
  if (p == "AUTO") {
    cfg_.cpAuto = true;
    return true;
  }
  CpMode want;
  if (p == "SHORT") {
    want = CpMode::Short;
  } else if (p == "MEDIUM") {
    want = CpMode::Medium;
  } else if (p == "LONG") {
    want = CpMode::Long;
  } else {
    return false;
  }
  cfg_.cpAuto = false;
  if (fsm_.state() == FsmState::Connected && want != cpMode_) {
    if (!isInitiator_) return false;  // v1: initiator owns mode control
    requestModeChange(mode_, want, now);
  } else {
    cpMode_ = want;
  }
  return true;
}

std::string LinkEngine::cpPolicy() const {
  if (cfg_.cpAuto) return "AUTO";
  switch (cpMode_) {
    case CpMode::Short: return "SHORT";
    case CpMode::Medium: return "MEDIUM";
    default: return "LONG";
  }
}

double LinkEngine::remoteSnrDb() const { return remoteSnrDb_; }

double LinkEngine::retryRatio() const {
  return amc_ ? amc_->retryRatio() : 0.0;
}

#ifdef SKW_HAVE_VOICE
bool LinkEngine::openVoice(std::optional<VoiceCodec> want, double now) {
  if (fsm_.state() != FsmState::Connected) return false;
  auto codec = want ? clampVoiceCodec(cfg_.policy.family, mode_, *want)
                    : autoVoiceCodec(cfg_.policy.family, mode_);
  if (!codec) return false;  // mode cannot carry voice
  voiceCodecAuto_ = !want.has_value();
  voiceEnc_.setCodec(*codec);
  voiceFsm_.cmdOpen(*codec, now);
  drainVoiceActions(now);
  return true;
}

void LinkEngine::closeVoice(double now) {
  voiceFsm_.cmdClose(now);
  drainVoiceActions(now);
  jitter_.reset();
  voiceDec_.reset();
  voiceEnc_.reset();
  haveVoiceRxSeq_ = false;
}

bool LinkEngine::setVoicePtt(bool on, double now) {
  if (voiceFsm_.state() == VoiceState::Closed) return false;
  if (on) {
    voiceFsm_.cmdPttOn(now);
  } else {
    voiceFsm_.cmdPttOff(now);
  }
  drainVoiceActions(now);
  return true;
}

void LinkEngine::pushVoicePcm(std::span<const int16_t> pcm) {
  voiceEnc_.pushPcm(pcm);
}

std::vector<int16_t> LinkEngine::takeVoicePcm(size_t samples) {
  return jitter_.take(samples);
}

size_t LinkEngine::voiceTxQueued() const {
  return voiceEnc_.pendingSamples();
}

void LinkEngine::drainVoiceActions(double now) {
  while (auto a = voiceFsm_.nextAction()) {
    // Voice control frames are small and time-critical: they ride the
    // session mode (voice needs an unspread mode anyway).
    const ChirpSig sig = sigForMode(mode_);
    switch (*a) {
      case VoiceAction::SendOpen:
        queueControl(LinkType::VoiceOpen,
                     VoiceOpenBody{voiceFsm_.codec()}.pack(), sig, mode_);
        break;
      case VoiceAction::SendOpenAck:
        queueControl(LinkType::VoiceOpenAck,
                     VoiceOpenBody{voiceFsm_.codec()}.pack(), sig, mode_);
        break;
      case VoiceAction::SendClose:
        queueControl(LinkType::VoiceClose, {}, sig, mode_);
        break;
      case VoiceAction::SendPttReq:
        queueControl(LinkType::VoicePttReq, {}, sig, mode_);
        break;
      case VoiceAction::SendPttGrant:
        queueControl(LinkType::VoicePttGrant, {}, sig, mode_);
        break;
      case VoiceAction::SendPttEnd:
        queueControl(LinkType::VoicePttEnd, {}, sig, mode_);
        break;
      case VoiceAction::IndicateOpen:
        events_.push_back("EVT:VOICE OPEN");
        break;
      case VoiceAction::IndicateClosed:
        events_.push_back("EVT:VOICE CLOSED");
        break;
      case VoiceAction::IndicateTalking:
        events_.push_back("EVT:VOICE TALKING");
        break;
      case VoiceAction::IndicateStopped:
        events_.push_back("EVT:VOICE STOPPED");
        break;
    }
  }
  (void)now;
}

void LinkEngine::pumpVoice(double now) {
  if (!voiceFsm_.talking()) return;
  // AMC demoted below what the codec needs: step down at the next frame
  // boundary rather than dropping voice (VOICE.md).
  if (voiceCodecAuto_) {
    if (auto best = autoVoiceCodec(cfg_.policy.family, mode_)) {
      if (*best != voiceFsm_.codec()) {
        voiceFsm_.setCodec(*best);
        voiceEnc_.setCodec(*best);
        events_.push_back("EVT:VOICE codec");
      }
    } else {
      closeVoice(now);  // below the family's voice floor entirely
      return;
    }
  }
  // Stream units back-to-back while the talkspurt holds the channel.
  // Voice preempts queued DATA but never control (queueControl pushes
  // control ahead of data already).
  while (auto unit = voiceEnc_.nextUnit(dataBudget_, false)) {
    OutFrame f;
    f.env.type = LinkType::VoiceData;
    f.env.sessionId = sessionId_;
    f.env.sequence = voiceTxSeq_++;  // independent voice counter
    f.env.ackBase = arqRx_.ackBase();
    f.body = unit->pack();
#ifdef SKW_HAVE_CRYPTO
    if (cryptoOn_) {
      f.env.flags |= kFlagEncrypted;
      sealPayload(f.env, f.body);
    }
#endif
    f.sig = sigForMode(mode_);
    f.mode = mode_;
    // Ahead of DATA, behind control.
    outQueue_.insert(std::find_if(outQueue_.begin(), outQueue_.end(),
                                  [](const OutFrame& q) {
                                    return q.env.type == LinkType::Data;
                                  }),
                     std::move(f));
  }
}
#endif  // SKW_HAVE_VOICE

#ifdef SKW_HAVE_CRYPTO
bool LinkEngine::cryptoAuth() const { return crypto_.established(); }
bool LinkEngine::cryptoActive() const { return cryptoOn_; }
uint16_t LinkEngine::cryptoEpoch() const {
  return cryptoOn_ ? crypto_.epoch() : 0;
}

KexContext LinkEngine::kexCtx() const {
  // Transcript inputs must be IDENTICAL on both sides, so name the roles
  // rather than "own/peer": the initiator is whoever sent CALL.
  KexContext c;
  c.initCall = isInitiator_ ? cfg_.policy.ownCall : lastCall_.srcCall;
  c.respCall = isInitiator_ ? remoteCall_ : cfg_.policy.ownCall;
  c.family = uint8_t(cfg_.policy.family);
  c.initCaps = isInitiator_ ? cfg_.policy.caps : lastCall_.caps;
  c.respCaps = isInitiator_ ? lastCallAck_.caps : cfg_.policy.caps;
  return c;
}

bool LinkEngine::rotateKeys(double now) {
  if (!cryptoOn_ || !isInitiator_ || fsm_.state() != FsmState::Connected) {
    return false;
  }
  if (pendingEpoch_ != 0) return false;  // one rotation in flight
  pendingEpoch_ = uint16_t(crypto_.epoch() + 1);
  // KEY_UPDATE rides the CURRENT epoch's keys (it is a normal control
  // frame); the local switch happens when the peer's frames arrive under
  // the new epoch - the grace window covers the crossover either way.
  queueControl(LinkType::KeyUpdate,
               KeyUpdateBody{uint8_t(pendingEpoch_ & 0xFF)}.pack(),
               sigForMode(mode_), std::min(mode_, 8));
  crypto_.setEpoch(pendingEpoch_);
  pendingEpoch_ = 0;
  epochRotateAt_ = now;
  return true;
}

// DATA payload sealing. AAD = the envelope's first 8 bytes, so any
// header tampering (type/flags/session/sequence) breaks authentication.
bool LinkEngine::sealPayload(const LinkEnvelope& env,
                             std::vector<uint8_t>& body) {
  if (!cryptoOn_) return true;
  std::vector<uint8_t> aad;
  aad.reserve(kEnvelopeBytes);
  aad.push_back(uint8_t(env.type));
  aad.push_back(env.flags);
  aad.push_back(uint8_t(env.sessionId));
  aad.push_back(uint8_t(env.sessionId >> 8));
  aad.push_back(uint8_t(env.sequence));
  aad.push_back(uint8_t(env.sequence >> 8));
  aad.push_back(uint8_t(env.ackBase));
  aad.push_back(uint8_t(env.ackBase >> 8));
  const uint8_t stream = usesVoiceSequence(env.type) ? 1 : 0;
  const uint64_t seq = CryptoSession::extendSeq(txSeqHi_, env.sequence);
  txSeqHi_ = std::max(txSeqHi_, seq);
  body = crypto_.seal(stream, seq, aad, body);
  return true;
}

bool LinkEngine::openPayload(const LinkEnvelope& env,
                            std::vector<uint8_t>& body) {
  if (!cryptoOn_) return true;
  std::vector<uint8_t> aad;
  aad.reserve(kEnvelopeBytes);
  aad.push_back(uint8_t(env.type));
  aad.push_back(env.flags);
  aad.push_back(uint8_t(env.sessionId));
  aad.push_back(uint8_t(env.sessionId >> 8));
  aad.push_back(uint8_t(env.sequence));
  aad.push_back(uint8_t(env.sequence >> 8));
  aad.push_back(uint8_t(env.ackBase));
  aad.push_back(uint8_t(env.ackBase >> 8));
  const uint8_t stream = usesVoiceSequence(env.type) ? 1 : 0;
  const uint64_t seq = CryptoSession::extendSeq(rxSeqHi_, env.sequence);
  auto plain = crypto_.open(stream, seq, aad, body);
  if (!plain) return false;  // tag/replay failure: drop, never deliver
  rxSeqHi_ = std::max(rxSeqHi_, seq);
  body = std::move(*plain);
  return true;
}
#endif  // SKW_HAVE_CRYPTO

bool LinkEngine::sendBroadcast(std::span<const uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > 16 * kBcastMaxPayload) return false;
  const size_t count = (bytes.size() + kBcastMaxPayload - 1) /
                       kBcastMaxPayload;
  for (size_t i = 0; i < count; ++i) {
    BcastDataBody b;
    b.fragIndex = uint8_t(i);
    b.fragCount = uint8_t(count);
    const size_t off = i * kBcastMaxPayload;
    const size_t n = std::min(kBcastMaxPayload, bytes.size() - off);
    b.payload.assign(bytes.begin() + long(off),
                     bytes.begin() + long(off + n));
    // Stamp the SENDER, not a session: two stations broadcasting in the
    // same period used to collapse into one reassembly chain and corrupt
    // each other. The field is unused for connectionless frames, so a
    // receiver that ignores it behaves exactly as before.
    queueControl(LinkType::BcastData, b.pack(), ChirpSig::Calling, 4,
                 int32_t(bcastSenderTag()));
  }
  return true;
}

// 16-bit sender tag for connectionless broadcasts: the low half of the
// callsign hash. Collisions merely merge two chains, which is the old
// behaviour, so a short tag is safe here.
uint16_t LinkEngine::bcastSenderTag() const {
  return uint16_t(callsignHash24(cfg_.policy.ownCall) & 0xFFFF);
}

// Feed the proto pipeline from an active file transfer, keeping only a
// bounded backlog sealed (flow control: the file must not displace
// interactive stream traffic wholesale, and teardown must not strand
// megabytes in the pipeline).
void LinkEngine::pumpFile() {
  if (!fileTx_.active() || fsm_.state() != FsmState::Connected) return;
  const size_t cap = 4 * dataBudget_;
  while (protoTx_.backlogBytes() < cap) {
    auto msg = fileTx_.nextMessage();
    if (!msg) {
      // Fully handed over; completion = pipeline drained + all ACKed.
      if (protoTx_.idle() && arqTx_.inFlight() == 0) {
        events_.push_back("EVT:TXCOMPLETE");
        fileTx_.finish();
      }
      return;
    }
    protoTx_.submitMessage(kSvcFile, *msg);
  }
}

void LinkEngine::drainFileRx() {
  for (auto& m : protoRx_.takeServiceMessages(kSvcFile)) {
    fileRx_.onMessage(m);
  }
  for (auto& e : fileRx_.takeEvents()) {
    switch (e.kind) {
      case FileRxEvent::Kind::Done:
        events_.push_back("EVT:FILE_RX " + e.name + " " +
                          std::to_string(e.offset) + " " +
                          (e.crcOk ? "1" : "0"));
        break;
      case FileRxEvent::Kind::Error:
        events_.push_back("EVT:FILE_ERROR " + e.name + " " + e.reason);
        break;
      case FileRxEvent::Kind::Started:
      case FileRxEvent::Kind::Checkpoint:
        break;  // internal (resume state); not control-port events
    }
  }
}

void LinkEngine::queueControl(LinkType t, std::vector<uint8_t> body,
                              ChirpSig sig, int mode,
                              int32_t sessionIdOverride) {
  OutFrame f;
  f.env.type = t;
  f.env.sessionId = sessionIdOverride >= 0
                        ? uint16_t(sessionIdOverride)
                        : sessionId_;
  f.env.sequence = 0;
  f.env.ackBase = arqRx_.ackBase();
  f.body = std::move(body);
  f.sig = sig;
  f.mode = mode;
  outQueue_.push_back(std::move(f));
}

void LinkEngine::drainFsmActions(double now) {
  while (auto a = fsm_.nextAction()) {
    switch (*a) {
      case FsmAction::SendCall: {
        // Alternate CALL transports when the CSS tier is enabled: OFDM
        // calling first, CSS M0 on the next attempt, and so on - a deep
        // link connects on the CSS attempts, a normal one on the first.
        hsOnCss_ = callOnCssForAttempt(cfg_.policy, callTxCount_);
        // Step DOWN the bandwidth ladder for the later attempts, from the
        // CONFIGURED family (not whatever the previous attempt adopted).
        // The responder scans one family below its own, so a narrower call
        // is still heard - the two halves only work together, which is why
        // this schedule sat unused since step 2.3.
        HandshakePolicy ladder = cfg_.policy;
        ladder.family = configuredFamily_;
        adoptSessionFamily(
            callFamilyForAttempt(ladder, callTxCount_,
                                 fsmConfigFor(cfg_.policy).callAttempts),
            now);
        ++callTxCount_;
        queueControl(LinkType::Call,
                     makeCall(cfg_.policy, remoteCall_).pack(), hsSig(),
                     hsMode());
        break;
      }
      case FsmAction::SendCallAck: {
        auto ack = respondToCall(cfg_.policy, lastCall_, lastRxQuality_);
        if (ack) {
          queueControl(LinkType::CallAck, ack->pack(), hsSig(), hsMode());
        }
        break;
      }
      case FsmAction::SendStart: {
        if (resumeUsable(now)) {
          // Reconnect within the window: RESUME_REQ rides the START
          // slot; capability set and pipeline continue.
          sentResumeReq_ = true;
          sessionCaps_ = resumeCtx_.caps;
          protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
          ResumeReqBody rr;
          rr.token = resumeCtx_.token;
          rr.lastRxSeq = arqRx_.ackBase();
          if (fileRx_.inProgress()) {
            rr.hasFileCkpt = true;
            rr.fileId = fileRx_.fileId();
            rr.fileOffset = fileRx_.checkpointOffset();
          }
          queueControl(LinkType::ResumeReq, rr.pack(), hsSig(), hsMode());
          break;
        }
        sentResumeReq_ = false;
        applyChanMemSeed(remoteCall_, now);
        StartBody s = makeStart(cfg_.policy, lastCallAck_, lastRxQuality_);
        sessionCaps_ = s.caps;
        protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
#ifdef SKW_HAVE_CRYPTO
        if (sessionCaps_ & kCapEncryption) {
          crypto_.zeroize();
          cryptoOn_ = false;
          if (crypto_.setPsk(cfg_.psk)) {
            if (auto blob = crypto_.makeKexInit(kexCtx())) {
              s.kexPublic = std::move(*blob);
            }
          }
          if (s.kexPublic.empty()) {
            // Encryption negotiated but unavailable locally: drop the
            // bit rather than proceed - never silent cleartext.
            sessionCaps_ &= uint16_t(~kCapEncryption);
            s.caps = sessionCaps_;
          }
        }
#endif
        queueControl(LinkType::Start, s.pack(), hsSig(), hsMode());
        break;
      }
      case FsmAction::SendStartAck: {
        if (sendResumeAckInstead_) {
          sendResumeAckInstead_ = false;
          ResumeAckBody ra;
          ra.lastRxSeq = arqRx_.ackBase();  // resume path: state kept
          if (fileRx_.inProgress()) {
            ra.hasFileCkpt = true;
            ra.fileId = fileRx_.fileId();
            ra.fileOffset = fileRx_.checkpointOffset();
          }
          queueControl(LinkType::ResumeAck, ra.pack(), hsSig(), hsMode());
          break;
        }
        // mode_ was fixed in onFrame when START was evaluated.
        StartAckBody sa;
        sa.finalMode = uint8_t(mode_);
#ifdef SKW_HAVE_CRYPTO
        sa.kexResponse = std::move(pendingKexResponse_);
        pendingKexResponse_.clear();
#endif
        if (sessionCaps_ & kCapResume) {
          // Mint the resume token here (PROTOCOL.md): both sides hold
          // it; either may initiate the resumed reconnect.
          sa.hasResumeToken = true;
          sa.resumeToken = mintToken(now);
          resumeCtx_.valid = true;
          resumeCtx_.token = sa.resumeToken;
          resumeCtx_.remoteCall = lastCall_.srcCall;
          resumeCtx_.family = cfg_.policy.family;
          resumeCtx_.caps = sessionCaps_;
          resumeCtx_.freshAt = now;
        }
        lastStartAckBody_ = sa.pack();  // kept for duplicate-START re-ack
        queueControl(LinkType::StartAck, lastStartAckBody_, hsSig(),
                     hsMode());
        break;
      }
      case FsmAction::SendStartNak:
        queueControl(LinkType::StartNak,
                     StartNakBody{pendingNakReason_}.pack(), hsSig(),
                     hsMode());
        break;
      case FsmAction::SendDisc:
        queueControl(LinkType::Disc,
                     DiscBody{faultDisc_ ? DiscReason::RetryExhausted
                                         : DiscReason::Normal}
                         .pack(),
                     mode_ <= 3 ? ChirpSig::Css : ChirpSig::Robust,
                     mode_ <= 3 ? 0 : 4);
        break;
      case FsmAction::SendDiscAck:
        queueControl(LinkType::DiscAck, {},
                     mode_ <= 3 ? ChirpSig::Css : ChirpSig::Robust,
                     mode_ <= 3 ? 0 : 4);
        break;
      case FsmAction::SendKeepalive:
        // Robust transport ALWAYS: the keepalive is the liveness probe
        // of last resort. Riding the fast session mode made a stranded
        // fast-mode session totally deaf when the channel collapsed
        // (nothing it sent OR needed to hear was decodable), forcing a
        // 120 s idle-death + reconnect instead of a demotion.
        queueControl(LinkType::Keepalive, KeepaliveBody{20}.pack(),
                     mode_ <= 3 ? ChirpSig::Css : ChirpSig::Robust,
                     mode_ <= 3 ? mode_ : 4);
        break;
      case FsmAction::IndicateConnected: {
        // TX-alignment bootstrap: connected fast-signature frames only
        // capture ~+-5 Hz, so the correction must land BEFORE data
        // traffic starts. The calling-signature handshake measured the
        // offset (+-50 Hz capture): the initiator applies the peer's
        // CALL_ACK report in full; the responder uses reciprocity (if I
        // hear you +X high, you hear me X low) on its own measurement.
        // The <=2 Hz slew applies to TRACKING adjustments thereafter.
        txShiftHz_ = std::clamp(
            isInitiator_ ? -double(lastCallAck_.cfoHalfHz) * 0.5
                         : lastRxQuality_.cfoHz,
            -30.0, 30.0);
        harq_.clear();
        probeGate_ = ProbeGate();
        maskDet_.emplace(cfg_.policy.family);
        txMask_ = CarrierMask();
        rxMask_ = CarrierMask();
        pendingMaskEpoch_ = -1;
        fsm_.setIdleDisconnectSec(
            kModes[size_t(mode_)].waveform == Waveform::Css ? deepIdleSec_
                                                            : 120.0);
        AmcConfig ac;
        ac.family = cfg_.policy.family;
        ac.maxMode = cfg_.policy.maxMode;
        // CAP_CSS_TIER extends the AMC ladder into M0-M3.
        ac.minMode = (sessionCaps_ & kCapCssTier) ? 0 : uint8_t(kFirstMode);
        amc_.emplace(ac, uint8_t(mode_), 0.0);
        // Seed with the handshake measurement the initial mode was chosen
        // from (a robust-header frame, so it converged the tracker): the
        // first connected frames are fast-tier ACKs, too short to
        // measure, and an AMC with no sample would otherwise sit at
        // "effective -99 dB" until the first long DATA frame.
        if (lastRxQuality_.snrTrusted) amc_->onLocalSnr(lastRxQuality_.snrDb);
        pendingMode_ = -1;
        lastCwAt_ = now;  // CW ID cadence restarts with the session
#ifdef SKW_HAVE_CRYPTO
        epochRotateAt_ = now;
        pendingEpoch_ = 0;
#endif
        if (resuming_) {
          // RESUME: sequence spaces continue - keep both ARQ engines,
          // drop what the peer confirmed, requeue the rest.
          arqTx_.resumeFrom(resumePeerNext_, now);
          updateArqTimeout();
          if (fileTx_.active()) {
            if (resumePeerHasCkpt_ &&
                resumePeerCkptId_ == fileTx_.fileId()) {
              // Replay from the peer's persisted checkpoint; the
              // receiver absorbs overlap with what it already holds.
              fileTx_.rewindTo(resumePeerCkptOff_);
            } else if (resumePeerHasCkpt_) {
              // Peer is mid-way through a DIFFERENT file: ours is dead.
              events_.push_back("EVT:FILE_ERROR " + fileTx_.name() +
                                " resume-mismatch");
              fileTx_.abort();
            }
          }
          resumed_ = true;
          resuming_ = false;
          pausedForResume_ = false;
        } else {
          if (pausedForResume_) {
            // Same peer, fresh session: it restarted (or its resume
            // token expired), so re-send from the last message boundary
            // it acknowledged. A DIFFERENT peer must never be handed
            // the old one's queued data.
            if (remoteCall_ == resumeCtx_.remoteCall) {
              replayPipelineForFreshPeer();
            } else {
              discardPausedPipeline();
            }
          }
          resumed_ = false;
          const int w = macWindow(mode_);
          // Deep-tier sessions live at ~50% per-frame FER by design:
          // retry exhaustion there is normal operation, not a fault.
          const int retries = mode_ <= 3
                                  ? std::max(cfg_.arqMaxRetries, 20)
                                  : cfg_.arqMaxRetries;
          arqTx_ = ArqTx(w, retries);
          arqRx_ = ArqRx(w);
          // Carry the handshake's measured round trip into the fresh
          // ARQ state. Without it the estimator cannot bootstrap: the
          // first data frames time out before any ACK can return, every
          // one becomes a retransmission, and Karn's algorithm refuses
          // to sample an ambiguous round trip - so the timeout stays at
          // the airtime-only floor forever.
          arqTx_.seedRtt(hsRttSec_);
          updateArqTimeout();
        }
        break;
      }
      case FsmAction::IndicateDisconnected: {
        // Drop stale DATA, but keep queued control frames: the teardown
        // handshake's own DISC_ACK is queued in this same drain and must
        // still transmit.
        outQueue_.erase(
            std::remove_if(outQueue_.begin(), outQueue_.end(),
                           [](const OutFrame& f) {
                             return f.env.type == LinkType::Data ||
                                    f.env.type == LinkType::Ack;
                           }),
            outQueue_.end());
        // A FAILURE-mode drop with a live resume token pauses the whole
        // pipeline (proto queues, ARQ state, file transfer) for a
        // resumed reconnect; a clean/intentional disconnect ends the
        // session for real. FileRx state is kept either way - it holds
        // the checkpoint a resumed session continues from.
        const auto cause = fsm_.lastDiscCause();
        const bool clean = !faultDisc_ &&
                           (cause == FsmDiscCause::LocalRequest ||
                            cause == FsmDiscCause::PeerRequest);
        faultDisc_ = false;
#ifdef SKW_HAVE_CRYPTO
        // Session keys are wiped at session end; the PSK lives in cfg_
        // and is re-installed at the next handshake. A crypto state
        // change also invalidates any resume context (SECURITY.md).
        if (cryptoOn_) {
          crypto_.zeroize();
          cryptoOn_ = false;
          resumeCtx_.valid = false;
        }
#endif
        if (clean) resumeCtx_.valid = false;
        if (cfg_.cwId) cwPending_ = true;  // identify after the session
        if (resumeCtx_.valid && (sessionCaps_ & kCapResume)) {
          pausedForResume_ = true;
          resumeCtx_.mode = mode_;
          resumeCtx_.cp = cpMode_;
          resumeCtx_.freshAt = now;  // window runs from the drop
        } else {
          discardPausedPipeline();
        }
        ackPending_ = false;
        break;
      }
    }
  }
}

void LinkEngine::pumpData(double now) {
  {
    static const bool pd = getenv("SKW_PUMP_DEBUG") != nullptr;
    if (pd && now - pumpDbgLast_ > 5.0) {
      pumpDbgLast_ = now;
      fprintf(stderr,
              "%s pumpENTRY t=%.0f state=%d gated=%d busy=%d txq=%.2f "
              "failed=%d inflight=%zu/%d backlog=%zu q=%zu ackp=%d "
              "toSec=%.1f why=%d rtt=%.1f\n",
              cfg_.policy.ownCall.c_str(), now, int(fsm_.state()),
              ccaGatedNow_ ? 1 : 0, chanBusy_ ? 1 : 0, txQueueSec_,
              arqTx_.failed() ? 1 : 0, arqTx_.inFlight(),
              macWindow(mode_), protoTx_.backlogBytes(), outQueue_.size(),
              ackPending_ ? 1 : 0, arqTx_.timeoutSec(), ccaReason_,
              arqTx_.srttSec());
    }
  }
  if (fsm_.state() != FsmState::Connected) return;
  // Turn-taking: only the ISS pulls DATA (new or retransmit). The IRS's
  // data waits for TURN_GRANT; its escape hatch is the break request
  // riding its ACKs, not the starvation backstop.
  if (turnSchedActive() && turnRole_ != TurnRole::Iss) return;
  // While the CCA gate holds, leave retransmits and new segments in the
  // ARQ/proto layers: frames pulled now would burn their ACK timers
  // sitting in the TX queue behind a busy channel. The anti-starvation
  // clock overrides here too - the emission-side override is useless
  // if the gate never lets a frame be PULLED in the first place
  // (observed: a station busy ACKing the peer's stream never once
  // pulled its own queued message).
  const bool forcePull =
      dataQueuedSince_ >= 0.0 &&
      now - dataQueuedSince_ > dataStarveTimeouts() * arqTx_.timeoutSec();
  if (ccaGatedNow_) {
    if (!forcePull) return;
    // Forced pull fetches exactly ONE airable burst: the emission-side
    // override fires it this same tick. Pulling more would park frames
    // behind the gate with their ACK timers running - each parked
    // frame then burned a retry per timeout cycle until a healthy
    // session tore itself down (observed as session death mid
    // transfer).
    for (const auto& q : outQueue_) {
      if (q.env.type == LinkType::Data) return;
    }
  }
  static const bool arqDbg = getenv("SKW_ARQ_DEBUG") != nullptr;
  // Retransmits first (TX-queue priority), then new segments.
  if (auto r = arqTx_.nextRetransmit(now)) {
    if (arqDbg) {
      fprintf(stderr, "%s pumpData retx seq=%u rv=%d\n",
              cfg_.policy.ownCall.c_str(), r->seq, int(r->rv));
    }
    // A copy of this sequence may already be queued (aggressive timeout
    // requeues while the TX queue is still draining): let the queued
    // copy fly instead of stacking duplicates. Charging the retry and
    // re-arming the deadline BEFORE this check is deliberate - a
    // peek-first variant that skipped the charge left the timer armed,
    // blocked pumpData head-of-line on every tick, and starved new
    // segments outright (one-way goodput went to ZERO). The retry
    // budget stays honest via progress-resets in onAck.
    for (const auto& q : outQueue_) {
      if (q.env.type == LinkType::Data && q.env.sequence == r->seq) {
        return;
      }
    }
    OutFrame f;
    f.env.type = LinkType::Data;
    f.env.sessionId = sessionId_;
    f.env.sequence = r->seq;
    f.env.ackBase = arqRx_.ackBase();
    // ARQ payloads are stored as [flags byte][wire segment] so the
    // retransmission reproduces the original envelope flags.
    f.env.flags = r->payload.empty() ? 0 : r->payload[0];
    f.body = DataBody{r->rv,
                      {r->payload.begin() + (r->payload.empty() ? 0 : 1),
                       r->payload.end()}}
                 .pack();
#ifdef SKW_HAVE_CRYPTO
    if (cryptoOn_) {
      // Retransmissions re-seal under the SAME sequence, so the nonce
      // and ciphertext are identical - HARQ soft-combining still works.
      f.env.flags |= kFlagEncrypted;
      sealPayload(f.env, f.body);
    }
#endif
    // HARQ AUTO: IR when both peers advertise it - the retransmission
    // steps down to the same-constellation r1/2 mode (M9->M8, M11->M10)
    // and the receiver seeds systematic priors from the failed attempt.
    // Modes without an r1/2 rung (and r1/2 modes) stay put = chase.
    f.mode = harqIr() ? irStepDownMode(mode_) : mode_;
    f.sig = sigForMode(f.mode);
    outQueue_.push_back(std::move(f));
    return;
  }
  static const bool pumpDbg = getenv("SKW_PUMP_DEBUG") != nullptr;
  // Handover drain (turn-taking): retransmits above still flow, but no
  // NEW segment is pulled - the window empties so the turn changes hands
  // with nothing outstanding.
  if (turnSchedActive() && turnDrainSince_ >= 0.0) return;
  for (int k = 0; k < framesPerBurst(mode_); ++k) {
    if (!arqTx_.canSend()) {
      if (pumpDbg) {
        fprintf(stderr, "%s pump t=%.0f BLOCKED canSend inflight=%zu\n",
                cfg_.policy.ownCall.c_str(), now, arqTx_.inFlight());
      }
      return;
    }
    auto seg = protoTx_.nextSegment(dataBudget_, now);
    if (!seg) {
      if (pumpDbg) {
        fprintf(stderr,
                "%s pump t=%.0f NOSEG budget=%zu backlog=%zu idle=%d\n",
                cfg_.policy.ownCall.c_str(), now, dataBudget_,
                protoTx_.backlogBytes(), protoTx_.idle() ? 1 : 0);
      }
      return;
    }
    std::vector<uint8_t> stored;
    stored.reserve(1 + seg->bytes.size());
    stored.push_back(seg->flags);
    stored.insert(stored.end(), seg->bytes.begin(), seg->bytes.end());
    auto seq = arqTx_.send(std::move(stored), now);
    if (!seq) return;
    // Last fragment of a message: remember where it ends so the replay
    // copy can be released once the peer has acknowledged that far.
    if (!(seg->flags & kFlagMoreFragments)) msgEndSeq_.push_back(*seq);
    static const bool protoDbg = getenv("SKW_PROTO_DEBUG") != nullptr;
      if (protoDbg) {
      fprintf(stderr, "%s txDATA seq=%u flags=%02x len=%zu mode=%d\n",
              cfg_.policy.ownCall.c_str(), *seq, seg->flags,
              seg->bytes.size(), mode_);
    }
    OutFrame f;
    f.env.type = LinkType::Data;
    f.env.flags = seg->flags;
    f.env.sessionId = sessionId_;
    f.env.sequence = *seq;
    f.env.ackBase = arqRx_.ackBase();
    f.body = DataBody{0, std::move(seg->bytes)}.pack();
#ifdef SKW_HAVE_CRYPTO
    if (cryptoOn_) {
      f.env.flags |= kFlagEncrypted;
      sealPayload(f.env, f.body);
    }
#endif
    f.sig = sigForMode(mode_);
    f.mode = mode_;
    outQueue_.push_back(std::move(f));
  }
}

std::vector<cfloat> LinkEngine::buildBurst(const OutFrame& f) {
  auto raw = packFrame(f.env, f.body);
  TxFrameParams tp;
  tp.family = cfg_.policy.family;
  tp.sig = f.sig;
  tp.mode = f.mode;
  // Waveform consistency guard: the mode decides the waveform, and the
  // preamble signature must match it - a mismatch produces a frame no
  // receiver can decode.
  if (kModes[size_t(tp.mode)].waveform == Waveform::Css) {
    tp.sig = ChirpSig::Css;
  } else if (tp.sig == ChirpSig::Css) {
    tp.sig = tp.mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
  }
  tp.frameType = uint8_t(f.env.type);
  tp.seqLow = uint8_t(f.env.sequence & 0xFF);
  tp.cpMode = cpMode_;
  tp.payload = raw;
  // Spread modes (M4-M7) skip masking: spreading already averages over
  // interference (LINK_ADAPTATION.md). Shared predicate with the receiver.
  if (modeUsesCarrierMask(f.mode) &&
      txMask_.maskedCount(cfg_.policy.family) > 0) {
    tp.mask = &txMask_;
  }
  auto burst = buildFrameBaseband(tp);
  applyTxShift(burst);
  return burst;
}

void LinkEngine::applyTxShift(std::vector<cfloat>& burst) const {
  if (std::abs(txShiftHz_) < 1e-3) return;
  for (size_t i = 0; i < burst.size(); ++i) {
    const double ph = 2.0 * M_PI * txShiftHz_ * double(i) / kFsBaseband;
    burst[i] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
  }
}

void LinkEngine::updateArqTimeout() {
  // ACK timeout from real airtimes AT THE CURRENT MODE. Computing this
  // only at connect was a live bug: after demoting to a robust mode the
  // round trip exceeded the fast-mode timeout, MODE_CHANGE retry cycles
  // outran their own MODE_ACKs, and the committed modes diverged until
  // the session died.
  // Segment budget first: exact codeword fill for the on-air shape a
  // full data burst takes at this mode (solo frame, or an F-entry burst
  // container when aggregation applies).
  arqTx_.setWindow(macWindow(mode_));  // profile follows the mode
  const int fpb = cfg_.burstAggregation ? framesPerBurst(mode_) : 1;
  // Codeword target: the top rungs (M13+) fill the 4095-byte PHY cap -
  // per-burst preamble/sync/header overhead is what caps VHF-FM
  // throughput, so bursts must be as long as the wire allows.
  // CSS frames carry 2 codewords (~66 B): the chirp preamble + sync +
  // duplicated header dominate a 1-codeword frame's airtime, and the
  // deep tier was delivering ~1 B/s of goodput on 25 B frames.
  // At the deep tier the chirp preamble + sync + duplicated header cost
  // tens of seconds REGARDLESS of payload, so goodput is decided almost
  // entirely by how many bytes ride behind one preamble. 2 codewords
  // measured 0.97 B/s at M1 - 7% of that mode's net rate.
  //
  // The reason it is not simply larger is fade exposure: a 4x longer
  // frame is 4x more likely to meet a fade it cannot survive, and a
  // deep-tier retransmission is enormously expensive. That trade depends
  // on the CHANNEL, which is exactly what the classifier measures - so
  // ask it instead of guessing a constant that must be wrong on one side
  // or the other.
  // The ROBUST TIER (M4-M7) faces the same trade for the same reason and
  // gets the same adaptive answer (constants.hpp kRobustCw*): its
  // preamble + ZC sync + header cost fixed seconds while one codeword
  // carries ~120 bytes, so a 1-codeword frame spends 49-85% of its
  // airtime on overhead and measured 17-24 B/s of goodput against 26-51
  // at 8 codewords. Grown on clean deliveries and halved on loss, so the
  // fade exposure of a longer frame is answered by the channel rather
  // than by a constant.
  const bool robustTier = mode_ >= kFirstMode && mode_ <= 7;
  int cwTarget = (mode_ >= 13 && activeBand() == Band::VhfFm)
                     ? 6 * fpb
                     : (mode_ <= 3 ? cssCwTarget_
                        : robustTier ? robustCwTarget_
                                     : 2) *
                           fpb;
  dataBudget_ = dataPayloadBudget(mode_, fpb, cwTarget);
  if (robustTier) {
    // Airtime bound: a frame is the unit of retransmission, of the ARQ
    // timer and of a turn's minimum hold, none of which the goodput
    // sweep can see. Shrink the target until the frame fits.
    auto frameAir = [&](size_t budget) {
      TxFrameParams probe;
      probe.family = cfg_.policy.family;
      probe.sig = sigForMode(mode_);
      probe.mode = mode_;
      probe.cpMode = cpMode_;
      std::vector<uint8_t> shape(static_cast<size_t>(fpb) *
                                 (budget + (fpb == 1 ? 13 : 15)));
      probe.payload = shape;
      return frameAirtimeSec(probe);
    };
    static const double kAirCap = [] {
      const char* e = getenv("SKW_ROBUST_AIR_CAP");  // diagnostic A/B
      return e ? atof(e) : kRobustFrameAirCapSec;
    }();
    while (cwTarget > fpb * kRobustCwMin && frameAir(dataBudget_) > kAirCap) {
      cwTarget = std::max(fpb * kRobustCwMin, cwTarget / 2);
      dataBudget_ = dataPayloadBudget(mode_, fpb, cwTarget);
    }
  }
  std::vector<uint8_t> seg(
      static_cast<size_t>(fpb) * (dataBudget_ + (fpb == 1 ? 13 : 15)));
  TxFrameParams tp;
  tp.family = cfg_.policy.family;
  tp.sig = sigForMode(mode_);
  tp.mode = mode_;
  tp.cpMode = cpMode_;
  tp.payload = seg;
  std::vector<uint8_t> ackSized(kAckBodyBytes + kFrameOverhead);
  TxFrameParams ta = tp;
  ta.payload = ackSized;
  // ACK transport mirrors pumpData/ackPending: session CSS mode below
  // M4, M4 for the OFDM robust tier, the session mode above.
  ta.mode = mode_ <= 3 ? mode_ : (mode_ <= 6 ? 4 : mode_);
  ta.sig = sigForMode(ta.mode);
  // Batched-ACK modes wait up to a 4-burst batch before feedback.
  const double batch =
      (activeBand() == Band::VhfFm && mode_ >= 13) ? 4.0 : 1.0;
  const double dataAir = frameAirtimeSec(tp);
  dataFrameAirSec_ = dataAir;
  dataBurstBytes_ = size_t(fpb) * dataBudget_;
  applySealCap();  // needs the frame airtime: see the delivery bound
  ackAirSec_ = frameAirtimeSec(ta);
  arqTx_.setTimeout(batch * dataAir + ackAirSec_ + 3.0);

  // --- ONE airtime scale for every half-duplex handoff constant -------
  //
  // The MAC's wall-clock constants (post-TX listen window, fairness
  // yield, post-clear backoffs, control busy-hold cap) were all tuned
  // against OFDM bursts of a couple of seconds. A deep-tier CSS frame
  // runs 35-55 s, so at M0-M3 every one of them is effectively zero:
  // a 30 s control hold EXPIRES INSIDE the peer's single burst and then
  // keys straight over it, the 0.25-0.75 s fairness yield is noise, and
  // the "peer sent data recently" window is shorter than one frame so
  // fairness never engages at all. Measured consequence: 0.83 B/s at M1
  // against 32 B/s theoretical net, the channel consumed by control
  // exchanges keying each side deaf across the other's replies.
  //
  // The reference is the SAME FAMILY's slowest OFDM mode (M4), computed
  // here rather than hard-coded, because families differ by an order of
  // magnitude in airtime. Every OFDM mode is at or faster than that
  // reference, so it clamps to 1.0 and HF behaviour is bit-for-bit
  // unchanged; only the CSS tier scales.
  TxFrameParams ref = tp;
  ref.mode = 4;
  ref.sig = sigForMode(4);
  const double refAir = frameAirtimeSec(ref);
  airScale_ = refAir > 0.0
                  ? std::clamp(dataAir / refAir, 1.0, kAirtimeScaleMax)
                  : 1.0;
  // Fairness must observe the peer over a window that can actually
  // CONTAIN its traffic: at deep tier one frame outlives the old fixed
  // 15 s window, so the peer always looked idle.
  dataRecencySec_ = std::max(15.0, 3.0 * dataAir);
  // The flush must NOT fire between back-to-back containers of a
  // healthy stream - only when the sender actually went quiet.
  ackFlushSec_ = std::max(0.4, 1.6 * dataAir);
}

void LinkEngine::applyMode(int mode, CpMode cp, double now) {
  if (modeReqAt_ >= 0.0 && mode != mode_) {
    static const bool mcDbg = getenv("SKW_MODE_DEBUG") != nullptr;
    if (mcDbg) {
      fprintf(stderr, "%s MCLAT t=%.1f M%d->M%d commit=%.1f s (%s)\n",
              cfg_.policy.ownCall.c_str(), now, modeReqFrom_, mode,
              now - modeReqAt_, mode > modeReqFrom_ ? "up" : "down");
    }
    modeReqAt_ = -1.0;
  }
  // Entering the deep tier restarts frame-length adaptation at the
  // fade-safe minimum. Without this a session that had grown to 16
  // codewords on a good channel, climbed out to OFDM, and later demoted
  // back would resume at 16 against a channel that is now, by
  // definition, worse - a large frame is exactly the wrong opening bid
  // there.
  const bool wasCss = mode_ <= 3;
  const bool nowCss = mode <= 3;
  if (nowCss && !wasCss) {
    cssCwTarget_ = kCssCwMin;
    cssCleanRun_ = 0;
  }
  // Entering the robust tier from anywhere else: a frame length learned
  // at another tier says nothing here, and a demotion into M4-M7 means
  // the channel just got worse - start short and re-earn the length.
  const bool wasRobust = mode_ >= kFirstMode && mode_ <= 7;
  const bool nowRobust = mode >= kFirstMode && mode <= 7;
  if (nowRobust && !wasRobust) {
    robustCwTarget_ = kRobustCwMin;
    robustCleanRun_ = 0;
  }
  mode_ = mode;
  cpMode_ = cp;
  // NOTE: queued DATA frames keep the mode they were built for. Re-
  // labelling them to the new mode was tried (2026-09-05) and regressed
  // the M3 fading ramp 0.84 -> 0.61: a frame's payload is sized to its
  // mode's exact codeword budget and burst geometry, and the ramp
  // changes mode constantly.
  // Deep-tier sessions survive routine multi-minute decode droughts
  // (only a DECODED frame refreshes the idle clock at ~50% frame FER);
  // OFDM sessions keep the tight 120 s hang detection.
  fsm_.setIdleDisconnectSec(
      kModes[size_t(mode_)].waveform == Waveform::Css ? deepIdleSec_
                                                      : 120.0);
  updateArqTimeout();
  if (amc_) amc_->onModeApplied(uint8_t(mode), now);
  // A responder's pin request is satisfied once the mode it asked for is
  // committed; leaving it set would re-ask on every ACK forever.
  if (!isInitiator_ && peerModeRequest_ >= 0 &&
      peerModeRequest_ != int(kRxModeAuto) && peerModeRequest_ == mode) {
    peerModeRequest_ = -1;
  }
}

void LinkEngine::onCfoReport(double reportedHz) {
  const double target = txShiftHz_ - reportedHz / 2.0;
  const double step = std::clamp(target - txShiftHz_, -2.0, 2.0);
  txShiftHz_ = std::clamp(txShiftHz_ + step, -30.0, 30.0);
}

void LinkEngine::requestModeChange(int target, CpMode cp, double now) {
  if (pendingMode_ >= 0 || (target == mode_ && cp == cpMode_)) return;
  // Commit latency instrumentation: a promotion is not worth anything
  // until the new mode is actually carrying data, and the ladder climbs
  // one rung per commit (LINK_ADAPTATION.md).
  modeReqAt_ = now;
  modeReqFrom_ = mode_;
  pendingMode_ = target;
  pendingCp_ = cp;
  ++modeEpoch_;
  modeChangeTries_ = 0;
  // DEMOTIONS ride the ROBUST transport (CSS mode for CSS-committed
  // sessions): riding min(target, mode_) deadlocked - a fast-mode
  // session whose channel collapsed below the fast floor could neither
  // send nor decode the very exchange that demotes it. PROMOTIONS ride
  // the current committed mode: the channel is healthy by definition,
  // and robust airtime for every upward step measurably drags ramp
  // tracking (M3 gate).
  const int txMode =
      mode_ <= 3 ? mode_ : (target < mode_ ? 4 : mode_);
  queueControl(LinkType::ModeChange,
               ModeChangeBody{uint8_t(target), uint8_t(cp), modeEpoch_}
                   .pack(),
               sigForMode(txMode), txMode);
  // APPLY AT ANNOUNCE. The old scheme applied here only on MODE_ACK
  // while the responder applied on receipt, so a lost MODE_ACK left the
  // two ends one rung apart for the whole retry window (measured as
  // A at M7 against B at M6 through ~100 s of fading, with the
  // responder's feedback riding the wrong transport the whole time).
  // Now both ends move at the same event: we at the announcement, the
  // responder at the announcement OR at the first DATA frame it decodes
  // at the new mode - its PHY header states the mode, and the only
  // other legitimate reason a session DATA frame rides a mode other
  // than the committed one is an HARQ-IR step-down, which is a known
  // function of the committed mode (see the DATA handler). MODE_ACK
  // becomes a confirmation, not a gate.
  modeChangeFrom_ = mode_;
  // DEMOTIONS ONLY. A promotion rides a healthy channel by definition
  // and its MODE_ACK arrives; applying it at announce made the initiator
  // run ahead of the responder for one announcement's propagation - the
  // responder's ARQ window and ACK batching still at the old mode while
  // M14 containers arrived - measured as a one-time ~1 s transient that
  // took the VHF sustained gate from 25.5 to 24.0 kbps. The split this
  // exists to fix is the demotion under fading whose MODE_ACK is lost.
  // (SKW_NO_APPLY_AT_ANNOUNCE: diagnostic A/B - always apply on ACK.)
  //
  // Upward at the HF robust tier this was tried and MEASURED, and it is
  // off by default (SKW_PROMOTE_AT_ANNOUNCE opts in). The commit latency
  // of a promotion is the ladder's climb rate, since it takes one commit
  // per rung, and MCLAT (SKW_MODE_DEBUG, M3 ramp) put it at a median of
  // 17-23 s from M4-M7 against 5-12 s from M8+ - the robust tier's own
  // round trip, spent at the rung the AMC has already judged too slow.
  // Applying at announce takes it to 0.0 s, as demotions already are.
  // But it ANTI-SYNERGISES with robust-tier frame growth
  // (constants.hpp kRobustCw*), churn seeds 11 / 12:
  //                          growth off        growth on
  //   promote on ACK       32,336 / 65,662  37,006 / 73,028  <- default
  //   promote at announce  37,255 / 67,942  33,361 / 65,662
  // Either lever alone pays ~+14%; together they cancel. Both buy the
  // same thing - more payload per unit of robust-tier airtime - by
  // opposite means, one by lengthening the frames while the link is
  // there, the other by leaving sooner, and leaving sooner denies the
  // frame growth the clean deliveries it needs. Frame growth wins the
  // pair (seed 12 by 7%), so the ladder keeps waiting for MODE_ACK.
  static const bool kApplyAtAnnounce =
      getenv("SKW_NO_APPLY_AT_ANNOUNCE") == nullptr;
  static const bool kPromoteAtAnnounce =
      getenv("SKW_PROMOTE_AT_ANNOUNCE") != nullptr;
  const bool robustPromote = kPromoteAtAnnounce && target > mode_ &&
                             mode_ >= kFirstMode && mode_ <= 7 &&
                             activeBand() != Band::VhfFm;
  if (getenv("SKW_MODE_DEBUG")) {
    fprintf(stderr, "%s ANNOUNCE t=%.1f M%d->M%d epoch=%u tx@M%d\n",
            cfg_.policy.ownCall.c_str(), now, mode_, target,
            unsigned(modeEpoch_), txMode);
  }
  if (kApplyAtAnnounce && (target < mode_ || robustPromote)) {
    applyMode(target, cp, now);
  }
  // Arm the confirmation deadline from the NEW mode's timeout: a
  // demotion into the CSS tier armed it from the old fast-mode timeout
  // (~10 s) and re-announced inside its own first 30 s burst - measured
  // as 6 MODE_CHANGE bursts for one pin, each a robust-M4 frame ahead
  // of data, and deep-tier throughput at 11% of net against a 15% gate.
  modeChangeDeadline_ = now + arqTx_.timeoutSec();
}

bool LinkEngine::pinMode(int mode, double now) {
  if (fsm_.state() != FsmState::Connected || !amc_) return false;
  const int lowest = (sessionCaps_ & kCapCssTier) ? 0 : kFirstMode;
  if (mode < lowest || mode >= kNumModes) return false;

  if (!isInitiator_) {
    // Ask, do not act: a responder that drove MODE_CHANGE itself would
    // be a second controller on the air. The request rides the next
    // ACK; amc_->pinned() turns true when the initiator commits the
    // MODE_CHANGE, so the operator sees the real state, not the wish.
    peerModeRequest_ = mode;
    // Feedback carries the request, so make sure some feedback is due -
    // on an otherwise idle link nothing would ever ask.
    ackPending_ = true;
    return true;
  }
  amc_->pin(uint8_t(mode), now);
  requestModeChange(mode, cpMode_, now);
  return true;
}

void LinkEngine::unpinMode() {
  if (!isInitiator_ && fsm_.state() == FsmState::Connected) {
    peerModeRequest_ = int(kRxModeAuto);   // ask the initiator to free it
    ackPending_ = true;
    return;
  }
  if (amc_) amc_->unpin();
}

bool LinkEngine::modePinned() const { return amc_ && amc_->pinned(); }

// --- channel memory (3.5) ----------------------------------------------------

std::optional<ChanMemKey> LinkEngine::chanMemKey(
    const std::string& peer) const {
  if (!cfg_.chanMem || peer.empty() || cfg_.policy.ownCall.empty()) {
    return std::nullopt;
  }
  ChanMemKey k;
  k.localHash = callsignHash24(cfg_.policy.ownCall);
  k.remoteHash = callsignHash24(peer);
  k.freqBucketKhz = cfg_.freqBucketKhz;
  k.family = uint8_t(cfg_.policy.family);
  return k;
}

// Install the stored recommendation into the handshake policy. Called
// immediately before a handshake decision so the store's verdict clamps
// the quality-derived proposal on whichever side is deciding.
void LinkEngine::applyChanMemSeed(const std::string& peer, double now) {
  cfg_.policy.chanMemMode.reset();
  auto key = chanMemKey(peer);
  if (!key) return;
  cfg_.policy.chanMemMode =
      cfg_.chanMem->recommend(*key, now, cfg_.policy.maxMode);
}

// Fold what this session actually achieved back into the store. Recorded
// on teardown rather than continuously: mid-session samples during a
// handshake or a collapse would bias the EMA toward transients.
void LinkEngine::recordChanMem(double now) {
  auto key = chanMemKey(remoteCall_);
  if (!key) return;
  ChanMemStats s;
  s.stableMode = uint8_t(std::clamp(mode_, 0, 255));
  s.snrDb = lastRxQuality_.snrDb;
  s.retryRatio = retryRatio();
  const uint64_t seen = framesOk_ + framesFailed_;
  s.decodeFailRatio = seen ? double(framesFailed_) / double(seen) : 0.0;
  s.selectivity = lastRxQuality_.selectivity;
  cfg_.chanMem->record(*key, s, now);
}

std::optional<std::vector<cfloat>> LinkEngine::tick(double now) {
  lastNow_ = now;
  // Session-end edge: the store learns from completed sessions only.
  // A call ladder that exhausted without connecting also hands the radio
  // back on the configured family.
  if (fsm_.state() == FsmState::Idle &&
      cfg_.policy.family != configuredFamily_) {
    cfg_.policy.family = configuredFamily_;
  }
  const bool connectedNow = fsm_.state() == FsmState::Connected;
  if (!wasConnected_ && connectedNow) {
    // Turn-taking initial roles (PROTOCOL.md): the connect initiator is
    // the first ISS - it placed the call, so it is the end presumed to
    // have traffic. On resume the same rule applies to the resumed
    // connect, which naturally hands the first turn to whichever end
    // had reason to re-establish the link.
    turnRole_ = isInitiator_ ? TurnRole::Iss : TurnRole::Irs;
    roleSince_ = now;
    turnEpoch_ = 0;
    peerGrantEpoch_ = -1;
    breakReqPending_ = false;
    breakSentAt_ = -1.0;
    grantAwaitingData_ = false;
    arqTx_.setSuspended(turnSchedActive() && turnRole_ == TurnRole::Irs,
                        now);
  }
  if (wasConnected_ && !connectedNow) {
    recordChanMem(now);
    // A session that adopted a narrower family by answering a narrower
    // CALL hands the radio back on the operator's configured family.
    cfg_.policy.family = configuredFamily_;
    // Turn-taking state must not outlive the session (a paused-for-
    // resume pipeline still needs working ARQ timers next connect).
    arqTx_.setSuspended(false, now);
    grantAwaitingData_ = false;
    breakReqPending_ = false;
  }
  wasConnected_ = connectedNow;
  ccaGatedNow_ = ccaDataGated(now);  // one state advance per tick
  // Anti-starvation clock: measures how long DATA has been waiting to
  // AIR, resetting only when a data frame is actually emitted. The old
  // version measured continuous gate-hold time and reset on every
  // momentary clear tick - which control traffic then consumed - so a
  // queued container once sat 234 s without the backstop ever firing.
  const bool dataWaiting =
      arqTx_.peekRetransmit().has_value() || !protoTx_.idle() ||
      std::any_of(outQueue_.begin(), outQueue_.end(),
                  [](const OutFrame& q) {
                    return q.env.type == LinkType::Data;
                  });
  if (dataWaiting) {
    if (dataQueuedSince_ < 0.0) dataQueuedSince_ = now;
  } else {
    dataQueuedSince_ = -1.0;
  }
  if (!chanBusy_ && ctrlWaitStart_ >= 0.0 &&
      now - ctrlWaitStart_ >= 0.0) {
    // Channel cleared while control was holding: the hold ends now;
    // the frame flies on this tick's emission pass.
    ctrlWaitStart_ = -1.0;
  }
  arqTx_.setExtraDelay(txQueueSec_);
  confirmDeliveredMessages();
  fsm_.tick(now);
  drainFsmActions(now);
  arqTx_.tick(now);
  // DEEP-TIER FRAME LENGTH. The chirp preamble + sync + duplicated
  // header cost tens of seconds regardless of payload, so goodput at the
  // CSS tier is decided almost entirely by how many bytes ride behind
  // one preamble - 2 codewords measured 0.97 B/s at M1, 7% of net. It is
  // not simply larger because a longer frame is proportionally more
  // likely to meet a fade it cannot survive, and a deep-tier
  // retransmission is enormously expensive.
  //
  // That trade is a property of the channel, so it is MEASURED rather
  // than modelled: grow after consecutive clean deliveries, halve on any
  // loss. The channel classifier cannot serve here - it needs a dozen
  // frames, which at deep-tier airtimes is many minutes.
  const int nto = arqTx_.takeTimeouts();
  if (mode_ <= 3 && nto > 0 && cssCwTarget_ > kCssCwMin) {
    cssCleanRun_ = 0;
    cssCwTarget_ = std::max(kCssCwMin, cssCwTarget_ / 2);
    updateArqTimeout();
  }
  if (mode_ >= kFirstMode && mode_ <= 7 && nto > 0 &&
      robustCwTarget_ > kRobustCwMin) {
    robustCleanRun_ = 0;
    robustCwTarget_ = std::max(kRobustCwMin, robustCwTarget_ / 2);
    updateArqTimeout();
  }
  if (nto > 0 && amc_) {
    // Unacknowledged bursts are failure evidence for adaptation: a mode
    // whose bursts never arrive must demote even though the receiver,
    // having heard nothing, reports nothing.
    //
    // But an ACK timeout does NOT prove the burst was lost. It equally
    // means the ACK was lost, or that the timeout was simply shorter
    // than the round trip. Charging up to EIGHT synthetic failures per
    // timeout swamped the AMC's 16-frame window - two timeouts filled
    // it outright and forced a demotion regardless of the channel.
    // That closed a doom loop: a stalled MAC looked like a collapsing
    // channel, AMC demoted, the deeper tier stalled harder. Measured in
    // the floor soak: a link whose peer was decoding fine (and had ACKed
    // 20 s earlier) demoted from the robust tier all the way into CSS,
    // where a window of 1 and 35-55 s frames made recovery impossible.
    //
    // So: the original weighting (2 x min(nto,4)) stands. HISTORY: a
    // progress-gated lighter charge was re-landed on the isolated
    // harness to break the floor doom loop (reverse-path ACK loss
    // charging the AMC into CSS), but it MEASURABLY regressed the M3
    // ramp gate (amc/genie ratio 0.74 vs 0.75, +1 drop) and gave no
    // floor benefit - floor is decode-latency-bound, not demotion-bound
    // (STATUS showed it sitting at CSS with the channel able to carry
    // M4, i.e. the frames were slow, not mis-moded). So the ROADMAP's
    // original caution holds: these charges are the only demotion
    // evidence a sender gets when the receiver, hearing nothing, reports
    // nothing, and weakening them costs AMC tracking. Reverted.
    for (int i = 0; i < std::min(nto, 4) * 2; ++i) {
      amc_->onFrameResult(false, false, now);
    }
  }
  if (arqTx_.failed() && fsm_.state() == FsmState::Connected) {
    faultDisc_ = true;  // dead link, not operator intent: resumable
    fsm_.cmdDisconnect(now);
    drainFsmActions(now);
  }
  // Mode-change retry / expiry, then AMC evaluation.
  if (fsm_.state() == FsmState::Connected) {
    if (pendingMode_ >= 0 && now >= modeChangeDeadline_) {
      // One re-announcement, then stop: with apply-at-announce and
      // DATA-header adoption at the responder, MODE_ACK is confirmation
      // only, and every retry is robust-tier airtime taken from data.
      if (++modeChangeTries_ >= 2) {
        // KNOWN BUG, deliberately left as-is for now: the responder
        // applies a MODE_CHANGE on receipt while the initiator applies
        // only on the MODE_ACK, so a lost MODE_ACK splits the two ends
        // and giving up here makes that split permanent (observed in
        // the floor soak as A on M4 against B on M1).
        //
        // CONVERGE rather than strand the link. The responder applies
        // on RECEIPT, so after three announcements it has almost
        // certainly moved; staying put here is what made the split
        // permanent. Adopting the mode we announced restores a single
        // agreed mode in that case. If the responder heard NONE of the
        // three, the forward path is dead and the session is already
        // failing on its own terms - the ARQ retry budget tears it down
        // and RESUME re-establishes a fresh agreed mode.
        //
        // Verified by m2_mode_change_survives_lost_mode_ack, which
        // reproduces the split deterministically (a=10 against b=5).
        // Earlier attempts here were judged against churn soak
        // throughput, whose readings turned out to be confounded by
        // persistent channel memory - hence the dedicated test.
        applyMode(pendingMode_, pendingCp_, now);
        pendingMode_ = -1;
      } else {
        // Re-announce on the transport the ORIGINAL announcement used:
        // mode_ is already the new mode (applied at announce), so the
        // demotion/promotion test must run against the mode we left.
        const int txMode =
            modeChangeFrom_ <= 3
                ? modeChangeFrom_
                : (pendingMode_ < modeChangeFrom_ ? 4 : modeChangeFrom_);
        queueControl(LinkType::ModeChange,
                     ModeChangeBody{uint8_t(pendingMode_),
                                    uint8_t(pendingCp_), modeEpoch_}
                         .pack(),
                     sigForMode(txMode), txMode);
        modeChangeDeadline_ = now + arqTx_.timeoutSec();
      }
    }
#ifdef SKW_HAVE_CRYPTO
    // Automatic rotation (SECURITY.md): every 2^20 frames or 4 hours.
    if (cryptoOn_ && isInitiator_ &&
        (now - epochRotateAt_ > 4.0 * 3600.0 ||
         crypto_.framesSealed() >= (1u << 20))) {
      rotateKeys(now);
    }
#endif
    probeGate_.tick(now);
    // Adaptive proto seal cap: bound message size once the retry ratio
    // says the link is lossy; restore full coalescing once it is clean
    // again. Hysteresis (enter > 0.15, leave < 0.05) so a single retry
    // does not flap the policy.
    {
      const double rr = retryRatio();
      const bool was = lossyCoalesce_;
      if (!lossyCoalesce_ && rr > 0.15) {
        lossyCoalesce_ = true;
      } else if (lossyCoalesce_ && rr < 0.05) {
        lossyCoalesce_ = false;
      }
      if (was != lossyCoalesce_) applySealCap();
    }
    // Carrier-mask exchange (per direction; either side may originate).
    if (maskDet_ && cfg_.maskingEnabled && (sessionCaps_ & kCapMasking) &&
        mode_ >= 8) {
      if (pendingMaskEpoch_ >= 0 && now >= maskRetryAt_) {
        if (++maskTries_ >= 4) {
          pendingMaskEpoch_ = -1;  // abandon this mask generation
          // Cooldown before proposing again: an abandoned exchange
          // means the reverse channel is in bad shape - immediately
          // re-proposing produced an endless MASK_UPDATE storm that
          // occupied the channel and starved data AND teardown
          // (observed under half-duplex loss).
          maskCooldownUntil_ = now + 60.0;
        } else {
          MaskUpdateBody mu;
          mu.epoch = uint8_t(pendingMaskEpoch_);
          mu.family = uint8_t(cfg_.policy.family);
          std::copy(pendingRxMask_.bits.begin(),
                    pendingRxMask_.bits.begin() + 9, mu.mask.begin());
          queueControl(LinkType::MaskUpdate, mu.pack(),
                       sigForMode(mode_),
                       std::min(mode_, 8));
          // Airtime-derived like every other timer: a fixed short
          // interval abandoned the exchange before the MASK_ACK could
          // even ride back behind a data container.
          maskRetryAt_ = now + arqTx_.timeoutSec();
        }
      }
      if (pendingMaskEpoch_ < 0 && now >= maskCooldownUntil_) {
        CarrierMask want = maskDet_->proposal(now);
        if (!(want == rxMask_) &&
            want.maskedCount(cfg_.policy.family) > 0) {
          advisor_.onMaskChange(now);  // churn evidence (mask domain)
          pendingRxMask_ = want;
          pendingMaskEpoch_ = (maskEpoch_ + 1) & 0x0F;
          maskEpoch_ = pendingMaskEpoch_;
          maskTries_ = 0;
          MaskUpdateBody mu;
          mu.epoch = uint8_t(pendingMaskEpoch_);
          mu.family = uint8_t(cfg_.policy.family);
          std::copy(pendingRxMask_.bits.begin(),
                    pendingRxMask_.bits.begin() + 9, mu.mask.begin());
          queueControl(LinkType::MaskUpdate, mu.pack(),
                       sigForMode(mode_),
                       std::min(mode_, 8));
          // Airtime-derived like every other timer: a fixed short
          // interval abandoned the exchange before the MASK_ACK could
          // even ride back behind a data container.
          maskRetryAt_ = now + arqTx_.timeoutSec();
        }
      }
    }
    if (amc_ && isInitiator_ && pendingMode_ < 0 && cfg_.cpAuto &&
        haveSpread_ && cpWant_ != cpMode_ && cpVerdictStable(now) &&
        now - lastCpChange_ > 30.0) {
      // CP correction outranks ladder moves: a CP too short for the
      // channel is a safety problem, and AMC churn (probe retries)
      // must not starve it.
      lastCpChange_ = now;
      requestModeChange(mode_, cpWant_, now);
    } else if (amc_ && isInitiator_ && pendingMode_ < 0) {
      // Advisor (LINK_ADAPTATION.md, 6.3): bounded + deterministic.
      // Shadow computes and logs only; assist applies single-step
      // influence when confidence clears the per-domain floor. Never
      // touches handshake mode selection or voice.
      if (advisor_.mode() != AdvisorMode::Off) {
        advisor_.onSnr(amc_->effectiveSnrDb(now), now);
        const auto amcAdv = advisor_.adviseAmc(amc_->retryRatio());
        const auto cfoAdv = advisor_.adviseCfo(now);
        const auto maskAdv = advisor_.adviseMask(now);
        SKW_DEBUG(
            "advisor t=%.0f amc{bias=%d conf=%.2f} cfo{drift=%.2f "
            "conf=%.2f} mask{d=%.1f conf=%.2f}%s",
            now, amcAdv.stepBias, amcAdv.confidence, cfoAdv.driftHzPerMin,
            cfoAdv.confidence, maskAdv.thresholdDeltaDb, maskAdv.confidence,
            advisor_.mode() == AdvisorMode::Assist ? " (assist)" : "");
        if (advisor_.assistAmc(amcAdv) && amcAdv.stepBias != 0) {
          amc_->applyAdvisorBias(amcAdv.stepBias);
        }
        if (advisor_.assistCfo(cfoAdv) && lastCfoAssistAt_ > 0.0) {
          // Counteract measured oscillator drift between reports: one
          // bounded step per evaluation cadence, inside the existing
          // global TX-shift clamp.
          const double dtMin = (now - lastCfoAssistAt_) / 60.0;
          const double step =
              std::clamp(cfoAdv.driftHzPerMin * dtMin * 0.5, -0.5, 0.5);
          txShiftHz_ = std::clamp(txShiftHz_ - step, -30.0, 30.0);
        }
        lastCfoAssistAt_ = now;
        if (advisor_.assistMask(maskAdv) && maskDet_) {
          maskDet_->setThresholdBiasDb(maskAdv.thresholdDeltaDb);
        }
      }
      auto d = amc_->evaluate(now);
      static const bool amcDbg = getenv("SKW_AMC_DEBUG") != nullptr;
      if (amcDbg) {
        fprintf(stderr,
                "%s amc t=%.0f mode=%d eff=%.1f retry=%.2f pilot=%.3f "
                "sel=%d class=%d fill=%d span=%.1f decision=%d->%d\n",
                cfg_.policy.ownCall.c_str(), now, mode_,
                amc_->effectiveSnrDb(now), amc_->retryRatio(),
                amc_->pilotStability(), int(amc_->selectivity()),
                int(amc_->channelClass()), amc_->windowFill(),
                amc_->windowSpanSec(now), int(d.kind), int(d.targetMode));
      }
      // Probe requirement: fragile high modes (gate config) and every
      // kFadingNA row (near-static-only - including CSS M2/M3 on the
      // wide-chip families).
      auto needsProbe = [&](int m) {
        return probeGate_.required(m) ||
               fadingFloorDb(cfg_.policy.family, m) >= 90.0;
      };
      auto launchProbe = [&](int target) {
        // The PROBE rides the CANDIDATE mode; its probeId also travels
        // in the PHY header seqLow so the peer can report a payload
        // decode failure.
        if (auto req = probeGate_.begin(target, now)) {
          for (int i = 0; i < req->count; ++i) {
            OutFrame f;
            f.env.type = LinkType::Probe;
            f.env.sessionId = sessionId_;
            f.env.sequence = req->probeId;
            f.env.ackBase = arqRx_.ackBase();
            f.body = ProbeBody{req->probeId, uint8_t(req->mode)}.pack();
            f.sig = sigForMode(req->mode);
            f.mode = req->mode;
            outQueue_.push_back(std::move(f));
          }
        }
      };
      if (d.kind == AmcDecision::Kind::Promote &&
          needsProbe(d.targetMode) && !probeGate_.cleared(d.targetMode)) {
        launchProbe(d.targetMode);
      } else if (d.kind != AmcDecision::Kind::None) {
        if (d.kind == AmcDecision::Kind::Promote &&
            needsProbe(d.targetMode)) {
          probeGate_.consumeClearance();
        }
        requestModeChange(d.targetMode,
                          cfg_.cpAuto && cpVerdictStable(now) ? cpWant_
                                                              : cpMode_,
                          now);
      } else if (auto pw = amc_->probeWanted(now)) {
        // AMC is blocked ONLY on the clean-probe requirement (kFadingNA
        // rung): run the probe exchange it needs.
        if (!probeGate_.cleared(*pw) && !probeGate_.pending() &&
            !probeGate_.inCooldown()) {
          launchProbe(*pw);
        }
      }
    }
  }
  // --- Turn-taking scheduler (PROTOCOL.md "Turn-Taking") ----------------
  if (turnSchedActive()) {
    if (turnRole_ == TurnRole::Iss) {
      breakSentAt_ = -1.0;
      // Hand the turn over when the peer wants it: at once if we are out
      // of data, otherwise at the first window boundary after a short
      // round-trip-scaled minimum hold - continuous two-way load must
      // ALTERNATE, and every turn must pay for its handover with at
      // least one window of data.
      if (breakReqPending_ && !grantAwaitingData_) {
        // HANDOVER AT THE NEXT WINDOW BOUNDARY, after a short minimum
        // hold - PACTOR's changeover rule (finish the outstanding
        // exchange, then honour the break), not a fixed quantum. The
        // previous scheme held max(45 s, 3 x ARQ timeout) and, worse,
        // its "drained" test required the proto BACKLOG to be empty, so
        // under any continuous load the window-clear condition could
        // never be met and every handover waited out the full drain cap
        // (2 x ARQ timeout, 30-60 s). Measured on the corrected world as
        // turn-taking losing to legacy CCA 2.7:1 on a static-start
        // churn - handover overhead, not collisions, was the cost.
        static const double kHoldRtts = [] {
          const char* e = getenv("SKW_TURN_HOLD_RTTS");  // diagnostic A/B
          return e ? atof(e) : kTurnMinHoldRtts;
        }();
        static const double kHoldMinSec = [] {
          const char* e = getenv("SKW_TURN_HOLD_MIN_SEC");  // diagnostic A/B
          return e ? atof(e) : kTurnMinHoldSec;
        }();
        const double minHoldSec = std::max(
            kHoldMinSec,
            kHoldRtts * (arqTx_.haveRtt() ? arqTx_.srttSec() : 10.0));
        const bool queuedData =
            std::any_of(outQueue_.begin(), outQueue_.end(),
                        [](const OutFrame& q) {
                          return q.env.type == LinkType::Data;
                        });
        const bool haveBacklog =
            !protoTx_.idle() || fileTx_.active() || queuedData;
        if (turnDrainSince_ < 0.0 &&
            (!haveBacklog || now - roleSince_ >= minHoldSec)) {
          turnDrainSince_ = now;  // stop pulling new segments; let it clear
        }
        if (turnDrainSince_ >= 0.0) {
          // The window is clear when nothing is in flight and nothing
          // already pulled is still waiting to air. Granting with
          // unacknowledged frames in flight strands them for the peer's
          // whole turn (the IRS cannot retransmit).
          const bool windowClear = arqTx_.inFlight() == 0 && !queuedData;
          // A window that will not clear is a LOST ACK (or a lost
          // frame): during a drain nothing new is pulled, so the ISS
          // would otherwise sit silent until that frame's ARQ timer -
          // 90 s at the robust tier. Measured: 110 s of dead air with
          // one frame in flight, gate open, backlog waiting, then the
          // drain cap at 2 x 90 s. Fast-retransmit the head-of-line
          // frame once after one handoff interval, RTT-paced.
          if (!windowClear && !turnDrainRetx_ &&
              now - turnDrainSince_ > turnHandoffSec()) {
            turnDrainRetx_ = true;
            if (auto s = arqTx_.oldestInFlight()) arqTx_.nakAt(*s, now);
          }
          // The cap is the deaf-peer backstop, paced to the measured
          // round trip like every other turn timer, not to the ARQ
          // timeout: stranding one frame for the peer's turn costs a
          // turn cycle, holding the turn for 180 s cost the whole
          // schedule. Two handoff intervals (>= 12 s). Three was tried
          // so the fast-retransmit's ACK could return first: it halved
          // the coupled churn (85.3 -> 41.8 KB, both retry rules) -
          // the extra interval of held turn costs more than the
          // occasional stranded frame. SKW_DRAIN_CAP_MULT is the A/B.
          static const double kDrainCapMult = [] {
            const char* e = getenv("SKW_DRAIN_CAP_MULT");  // diagnostic A/B
            return e ? atof(e) : 2.0;
          }();
          const bool capped =
              now - turnDrainSince_ >
              std::max(6.0 * kDrainCapMult, kDrainCapMult * turnHandoffSec());
          if (windowClear || capped) grantTurn(now);
        }
      } else {
        turnDrainSince_ = -1.0;  // request withdrawn / not pending
        turnDrainRetx_ = false;
      }
    } else if (turnDataWaiting()) {
      // IRS with data: the break request rides every ACK
      // (kRxFlagBreakReq); this arms a standalone one when the ISS has
      // gone idle and no ACK is otherwise due.
      if (breakSentAt_ < 0.0 || now - breakSentAt_ > turnHandoffSec()) {
        breakSentAt_ = now;
        ackPending_ = true;
      }
    }
    // Grant reliability: retry an unanswered TURN_GRANT at the RTT-paced
    // handoff cadence, then reclaim the sending role - a link must never
    // be left with two listeners because one short control frame faded.
    if (grantAwaitingData_ && now >= grantRetryAt_) {
      static const bool gdbg = getenv("SKW_TURN_DEBUG") != nullptr;
      if (grantTries_ < 3) {
        queueTurnGrant();
        ++grantTries_;
        grantRetryAt_ = now + turnHandoffSec();
        if (gdbg) {
          fprintf(stderr, "%s GRANT t=%.1f retry #%d epoch=%u\n",
                  cfg_.policy.ownCall.c_str(), now, grantTries_,
                  unsigned(turnEpoch_));
        }
      } else {
        if (gdbg) {
          fprintf(stderr, "%s GRANT t=%.1f RECLAIM after %d tries\n",
                  cfg_.policy.ownCall.c_str(), now, grantTries_);
        }
        grantAwaitingData_ = false;
        setTurnRole(TurnRole::Iss, now);
      }
    }
    // ROLE RECONCILIATION - the divergence backstop, and the hard
    // guarantee the whole scheme rests on. A handover rides ONE control
    // frame (TURN_GRANT / the break-request bit); under a fade both can
    // be lost, stranding the link with two listeners (measured in churn:
    // both ends IRS for ~100 s, past the progress watchdog). The
    // guarantee is the PACTOR-style master rule: the connect INITIATOR
    // is the default ISS, and it reclaims the channel FAST whenever it
    // holds data and is hearing nothing from the peer - bounding any
    // two-listener window to a couple of round trips. Crossing that with
    // the responder-yields-on-DATA rule (in the DATA handler) means the
    // only reachable divergence is brief two-sender contention, which
    // self-heals in one exchange; long two-listener deadlock cannot
    // persist. This reclaim fires EVEN with a grant of our own pending:
    // a lost grant is exactly when the slow grant-retry path would
    // otherwise strand us.
    const double reclaimSec = std::max(12.0, 2.0 * turnHandoffSec());
    if (turnDataWaiting()) {
      if (turnRole_ == TurnRole::Irs && isInitiator_ &&
          (lastDataRxAt_ <= 0.0 || now - lastDataRxAt_ > reclaimSec) &&
          now - roleSince_ > reclaimSec) {
        grantAwaitingData_ = false;
        setTurnRole(TurnRole::Iss, now);
      } else if (turnRole_ == TurnRole::Iss && !isInitiator_ &&
                 !grantAwaitingData_ &&
                 (arqTx_.lastProgressAt() < 0.0 ||
                  now - arqTx_.lastProgressAt() > reclaimSec) &&
                 now - roleSince_ > reclaimSec) {
        // Responder holding the sending role but delivering nothing:
        // relinquish so the initiator (master) can reclaim cleanly.
        setTurnRole(TurnRole::Irs, now);
      }
    }
  }
  if (!ackPending_ && dataSinceAck_ > 0 &&
      fsm_.state() == FsmState::Connected &&
      now - lastDataRxAt_ > ackFlushSec_) {
    ackPending_ = true;  // tail flush: batch closed by silence
    dataSinceAck_ = 0;
  }
  // PIGGYBACK FIRST on a bidirectional link. Standalone ACKs are control
  // frames and outrank DATA, so on a two-way link the side that is
  // BEHIND owes an ACK for every burst it receives and spends every
  // transmit opportunity acknowledging - while the side that is ahead
  // has little to acknowledge and keeps sending. That is positive
  // feedback, and it ends with one direction at zero: measured in the
  // churn soak as B->A 8500 bytes against A->B 0, both ends healthy at
  // M10 with RETRY 0.1 and FFAIL 0.
  //
  // Every frame's envelope already carries ack_base (PROTOCOL.md), and
  // receivers now consume it, so a DATA frame acknowledges by itself.
  // When we have data to send and no HOLES to report, let the data carry
  // the floor and skip the standalone ACK entirely. Gaps still demand a
  // real ACK - only the AckBody has the bitmap.
  static const bool kPiggyOff = getenv("SKW_NO_ACK_PIGGYBACK") != nullptr;
  // Under turn-taking an IRS may hold data it is not allowed to SEND, so
  // deferring its ACK onto that data would starve the peer of feedback
  // (and of the break request the ACK carries) for the whole turn.
  const bool piggyEligible =
      !turnSchedActive() || turnRole_ == TurnRole::Iss;
  if (!kPiggyOff && piggyEligible && ackPending_ &&
      fsm_.state() == FsmState::Connected &&
      arqRx_.gapBitmap() == 0 &&
      (!protoTx_.idle() || arqTx_.peekRetransmit().has_value() ||
       std::any_of(outQueue_.begin(), outQueue_.end(),
                   [](const OutFrame& f) {
                     return f.env.type == LinkType::Data;
                   }))) {
    ackPending_ = false;   // the outgoing DATA envelope carries it
    if (ackDeferredAt_ < 0.0) ackDeferredAt_ = now;
  }
  // SAFETY on the above: the piggyback only actually happens if our data
  // AIRS. If it is stuck behind the CCA gate or a busy channel, the peer
  // would sit without feedback until its ARQ timeout - so a deferral
  // that has not been redeemed within the flush window re-arms a real
  // ACK. `ackDeferredAt_` clears whenever a DATA frame is emitted.
  if (ackDeferredAt_ >= 0.0 &&
      now - ackDeferredAt_ > ackFlushSec_ &&
      fsm_.state() == FsmState::Connected) {
    ackDeferredAt_ = -1.0;
    ackPending_ = true;
  }
  if (ackPending_ && fsm_.state() == FsmState::Connected) {
    ackPending_ = false;
    // At most one ACK in the queue, ahead of bulk DATA but behind other
    // control frames. Feedback priority matters: an ACK stuck behind a
    // retransmit backlog reports stale state and drives the peer into
    // dup storms (observed under two-way bulk). Absolute front is wrong
    // too: with data arriving every burst, a fresh front ACK per tick
    // starved MASK_UPDATE forever. The body is refreshed at transmit
    // time below, so an already-queued ACK just keeps its slot.
    const bool queued =
        std::any_of(outQueue_.begin(), outQueue_.end(),
                    [](const OutFrame& f) {
                      return f.env.type == LinkType::Ack;
                    });
    if (!queued) {
      auto ack = arqRx_.makeAck(
          int8_t(std::clamp(reportSnrDb() * 2.0, -127.0, 127.0)),
          ackRxFlags(),
          int8_t(std::clamp(lastRxQuality_.cfoHz / 0.5, -127.0, 127.0)));
      OutFrame f;
      f.env.type = LinkType::Ack;
      f.env.sessionId = sessionId_;
      f.env.sequence = 0;
      f.env.ackBase = arqRx_.ackBase();
      f.body = ack.pack();
      // Feedback at least as robust as forward traffic (M4 for M4-M6;
      // CSS sessions ACK at the session's own CSS mode).
      f.sig = kModes[size_t(mode_)].waveform == Waveform::Css
                  ? ChirpSig::Css
                  : (mode_ <= 6 ? ChirpSig::Robust : ChirpSig::Fast);
      f.mode = mode_ <= 3 ? mode_ : (mode_ <= 6 ? 4 : mode_);
      outQueue_.insert(std::find_if(outQueue_.begin(), outQueue_.end(),
                                    [](const OutFrame& q) {
                                      return q.env.type == LinkType::Data;
                                    }),
                       std::move(f));
    }
  }
#ifdef SKW_HAVE_VOICE
  voiceFsm_.tick(now);
  drainVoiceActions(now);
  pumpVoice(now);
#endif
  pumpFile();
  pumpData(now);
  // CW ID (PROTOCOL.md): pure audio-layer identification - after
  // disconnect, and every 10 min during long connected sessions. It is
  // the lowest-priority "burst": only an otherwise-idle transmitter
  // sends it.
  if (cfg_.cwId && fsm_.state() == FsmState::Connected &&
      now - lastCwAt_ > 600.0) {
    cwPending_ = true;
    lastCwAt_ = now;
  }
  if (outQueue_.empty()) {
    if (cwPending_) {
      cwPending_ = false;
      return renderMorseBaseband(cfg_.policy.ownCall, cfg_.cwWpm);
    }
    return std::nullopt;
  }
  // BOUNDED KEYING: never stage a new burst behind more than ~1 s of
  // un-aired audio (voice exempt). Control stacked freely here and a
  // DISC was observed queueing behind 7 s of staged control - every
  // frame in that pile keeps the transmitter keyed (deaf) and extends
  // the airtime the peer must wait out. One second admits a short
  // control tail behind a data burst; a robust-tier control frame
  // (3.5-7 s air) can never pile onto another.
  if (txQueueSec_ > 1.0 &&
      !usesVoiceSequence(outQueue_.front().env.type)) {
    return std::nullopt;
  }
  static const bool kEmitDbg = getenv("SKW_EMIT_DEBUG") != nullptr;
  if (kEmitDbg && !outQueue_.empty()) {
    fprintf(stderr,
            "%s EMIT t=%.1f front=%d q=%zu busy=%d gated=%d txq=%.2f "
            "ctrlWait=%.1f role=%s\n",
            cfg_.policy.ownCall.c_str(), now, int(outQueue_.front().env.type),
            outQueue_.size(), chanBusy_ ? 1 : 0, ccaGatedNow_ ? 1 : 0,
            txQueueSec_, ctrlWaitStart_,
            turnRole_ == TurnRole::Iss ? "ISS" : "IRS");
  }
  auto pick = outQueue_.begin();
  // Backstop: DATA waiting longer than 3 ARQ timeouts airs NOW -
  // overriding the CCA gate, control precedence, everything except
  // voice and the keying bound above. Without a hard override, a busy
  // channel plus a steady control stream can starve data indefinitely
  // while the ARQ concludes the peer is dead.
  bool forceData =
      (!turnSchedActive() || turnRole_ == TurnRole::Iss) &&
      dataQueuedSince_ >= 0.0 &&
      lastNow_ - dataQueuedSince_ > dataStarveTimeouts() * arqTx_.timeoutSec();
  if (forceData) {
    auto d = std::find_if(outQueue_.begin(), outQueue_.end(),
                          [](const OutFrame& q) {
                            return q.env.type == LinkType::Data;
                          });
    if (d != outQueue_.end()) {
      pick = d;
    } else {
      forceData = false;
    }
  }
  // Fair alternation: control frames insert AHEAD of data by design
  // (feedback priority), and emission is one burst per tick - so on a
  // busy shared channel the rare clear ticks were consumed ENTIRELY by
  // the control stream and a queued DATA frame starved for minutes
  // (observed: 200 s from queue to air for a 50-byte message). After
  // two consecutive control bursts with data waiting, data goes next.
  if (!forceData && ctrlRun_ >= 2 && !ccaGatedNow_) {
    auto d = std::find_if(outQueue_.begin(), outQueue_.end(),
                          [](const OutFrame& q) {
                            return q.env.type == LinkType::Data;
                          });
    if (d != outQueue_.end()) pick = d;
  }
  if (!forceData && pick->env.type == LinkType::Data && ccaGatedNow_) {
    // Data holds for the gate, but a control frame queued behind it
    // must not suffer priority inversion: emit the first non-DATA
    // frame instead, or nothing this tick.
    pick = std::find_if(outQueue_.begin(), outQueue_.end(),
                        [](const OutFrame& q) {
                          return q.env.type != LinkType::Data;
                        });
    if (pick == outQueue_.end()) return std::nullopt;
  }
  // Control feedback must not key over a burst still airing: the peer
  // is DEAF while transmitting, so an ACK sent into its transmission
  // is simply lost and the whole window burns an ARQ timeout. Hold
  // control while the channel is busy - short cap so feedback is never
  // starved, and NO jitter: feedback should fly the instant the
  // channel clears. Voice bypasses everything (real-time).
  if (pick->env.type != LinkType::Data && !usesVoiceSequence(pick->env.type) &&
      chanBusy_) {
    if (ctrlWaitStart_ < 0.0) ctrlWaitStart_ = now;
    // While connected the busy source is our own peer's burst: keying
    // over it is ALWAYS wasted (the peer is deaf while keyed) and
    // doubles on a shared channel. Hold for CLEAR with only a runaway
    // guard; outside a session keep the short courtesy cap.
    // The cap is a RUNAWAY GUARD, so it must outlast a legitimate peer
    // burst. At 30 s flat it expired INSIDE a 35-55 s deep-tier frame
    // and keyed over it every time - the same "proceed anyway" failure
    // that was fixed for OFDM, still live at the deep tier.
    const double cap = fsm_.state() == FsmState::Connected
                           ? 30.0 * airScale_
                           : 2.0 * airScale_;
    if (now - ctrlWaitStart_ < cap) return std::nullopt;
    ctrlJitterUntil_ = 0.0;  // cap expired: fire now, no jitter on top
  } else if (pick->env.type != LinkType::Data &&
             !usesVoiceSequence(pick->env.type) && ctrlWaitStart_ >= 0.0) {
    // Channel just cleared after a hold: fire after a SMALL random
    // jitter. Both ends often have feedback queued at a burst boundary
    // and firing "the instant the channel clears" made their control
    // frames race head-on (simultaneous TX on a shared channel).
    if (ctrlJitterUntil_ <= 0.0) {
      ccaRng_ ^= ccaRng_ >> 12;
      ccaRng_ ^= ccaRng_ << 25;
      ccaRng_ ^= ccaRng_ >> 27;
      // Jitter spread must EXCEED the busy detector's latency
      // (~100-300 ms energy blocks + audio path), or both ends key
      // inside the window where neither can yet hear the other.
      ctrlJitterUntil_ =
          now + 0.10 + 0.50 * double(ccaRng_ % 1000) / 1000.0;
    }
    if (now < ctrlJitterUntil_) return std::nullopt;
    // Jitter elapsed and the channel is STILL clear: transmit.
  }
  // Disarm on emission AND whenever the channel is clear - a stale
  // armed timestamp would make the cap look already-expired and let
  // the next control frame key over a live transmission.
  ctrlWaitStart_ = -1.0;
  ctrlJitterUntil_ = 0.0;
  if (pick->env.type == LinkType::Data) {
    ackDeferredAt_ = -1.0;  // this frame's envelope carries the ack floor
    ctrlRun_ = 0;
    dataQueuedSince_ = -1.0;  // the anti-starvation clock rearms fresh
  } else if (!usesVoiceSequence(pick->env.type)) {
    ++ctrlRun_;
  }
  // A burst's data-ness is latched from its FIRST frame: while the TX
  // path is draining, further picks are blocked by the keying bound, so
  // this is what the post-drain listen-turn decision sees.
  if (txQueueSec_ <= 0.35) {
    lastBurstHadData_ = pick->env.type == LinkType::Data;
    if (pick->env.type == LinkType::Data) {
      ++turnBursts_;
    }
  } else if (pick->env.type == LinkType::Data) {
    lastBurstHadData_ = true;
  }
  if (pick->env.type == LinkType::Data) {
    ++turnFrames_;
    turnBytes_ += pick->body.size();
  }
  OutFrame f = std::move(*pick);
  outQueue_.erase(pick);
  // Handshake round-trip stopwatch: the four-frame connect exchange is
  // the only unambiguous latency measurement the link ever gets (no
  // retransmission ambiguity), and it measures exactly what the ARQ
  // timeout must cover - the peer's decode-and-reply turnaround.
  if (f.env.type == LinkType::Call || f.env.type == LinkType::CallAck ||
      f.env.type == LinkType::Start || f.env.type == LinkType::StartAck) {
    hsTxAt_ = now;
  }
  {
    static const bool frDbg = getenv("SKW_FRAME_DEBUG") != nullptr;
    if (frDbg) {
      fprintf(stderr, "%s TXFRM t=%.1f type=%d seq=%u busy=%d txq=%.2f\n",
              cfg_.policy.ownCall.c_str(), now, int(f.env.type),
              unsigned(f.env.sequence), chanBusy_ ? 1 : 0, txQueueSec_);
    }
  }
  if (f.env.type == LinkType::Ack && fsm_.state() == FsmState::Connected) {
    // Refresh ACK feedback at transmit time - see the ackPending_ note.
    f.env.ackBase = arqRx_.ackBase();
    const uint8_t rxFlags = ackRxFlags();
    rxFailPending_ = false;  // the NAK rides THIS ACK; never repeat it
    f.body = arqRx_.makeAck(
                 int8_t(std::clamp(reportSnrDb() * 2.0, -127.0, 127.0)),
                 rxFlags,
                 int8_t(std::clamp(lastRxQuality_.cfoHz / 0.5, -127.0,
                                   127.0)))
                 .pack();
  }
  // MAC burst container: pack consecutive DATA frames of the same
  // mode/signature into one PHY frame (one preamble/sync/header for the
  // whole burst). Control frames always travel solo.
  const int quota = framesPerBurst(f.mode);
  if (cfg_.burstAggregation && f.env.type == LinkType::Data && quota > 1 &&
      !outQueue_.empty()) {
    std::vector<OutFrame> group;
    group.push_back(std::move(f));
    while (int(group.size()) < quota && !outQueue_.empty() &&
           outQueue_.front().env.type == LinkType::Data &&
           outQueue_.front().mode == group[0].mode &&
           outQueue_.front().sig == group[0].sig) {
      // Frames 2..N of a container never pass the pick site, so they
      // must be counted here or a container's turn looks like one
      // frame's worth of bytes (which is how the "~89 B/s per turn"
      // figure came about - it undercounted by the container fill).
      turnBytes_ += outQueue_.front().body.size();
      ++turnFrames_;
      group.push_back(std::move(outQueue_.front()));
      outQueue_.pop_front();
    }
    if (getenv("SKW_PROTO_DEBUG")) {
      fprintf(stderr, "%s emit group=%zu queue=%zu\n",
              cfg_.policy.ownCall.c_str(), group.size(), outQueue_.size());
    }
    if (group.size() > 1) {
      std::vector<uint8_t> payload;
      for (auto& g : group) {
        auto raw = packFrame(g.env, g.body);
        payload.push_back(uint8_t(raw.size() & 0xFF));
        payload.push_back(uint8_t(raw.size() >> 8));
        payload.insert(payload.end(), raw.begin(), raw.end());
      }
      TxFrameParams tp;
      tp.family = cfg_.policy.family;
      tp.sig = group[0].sig;
      tp.mode = group[0].mode;
      tp.frameType = kBurstContainerFrameType;
      tp.seqLow = uint8_t(group[0].env.sequence & 0xFF);
      tp.cpMode = cpMode_;
      tp.payload = payload;
      if (modeUsesCarrierMask(group[0].mode) &&
          txMask_.maskedCount(cfg_.policy.family) > 0) {
        tp.mask = &txMask_;
      }
      auto burst = buildFrameBaseband(tp);
      applyTxShift(burst);
      // Real keyed airtime of the burst actually sent, and the frames
      // it actually carried - a container is often short of
      // framesPerBurst when the backlog cannot fill it.
      turnAirSec_ += double(burst.size()) / kFsBaseband;
      turnContainerFrames_ += int(group.size());
      return burst;
    }
    f = std::move(group[0]);
  }
  auto solo = buildBurst(f);
  if (f.env.type == LinkType::Data) {
    turnAirSec_ += double(solo.size()) / kFsBaseband;
    turnContainerFrames_ += 1;
  }
  return solo;
}

bool LinkEngine::rxBurst(std::span<const cfloat> samples, double now,
                         bool finalAttempt) {
  lastNow_ = now;
  RxStage stage;
  auto res = receiveFrame(cfg_.policy.family, samples, &stage,
                          finalAttempt ? &harq_ : nullptr, &rxMask_,
                          wantConstellation_);

  // RECEIVER-SIDE FAMILY SCAN. A listener that only ever demodulates its
  // own family is DEAF to a caller that stepped down the bandwidth ladder
  // looking for margin - which made the handshake's family-fallback
  // schedule unlandable for three phases. While no session exists, a
  // failed decode is retried one family narrower.
  //
  // Cost is bounded deliberately: only when idle/listening/calling (a
  // connected session's family is fixed for its lifetime by design), only
  // when the primary family found nothing at all, and never with the HARQ
  // store or carrier mask attached - both are keyed to the primary
  // family's geometry.
  Family rxFamily = cfg_.policy.family;
  if (!res && cfg_.policy.familyFallback &&
      fsm_.state() != FsmState::Connected &&
      fsm_.state() != FsmState::Disconnecting) {
    const Family alt = narrowerFamily(cfg_.policy.family);
    if (alt != cfg_.policy.family) {
      RxStage altStage;
      auto altRes = receiveFrame(alt, samples, &altStage, nullptr, nullptr);
      if (altRes) {
        res = std::move(altRes);
        stage = altStage;
        rxFamily = alt;
      }
    }
  }
  rxFamily_ = rxFamily;
  // Header-implied airtime for the worker's burst-end finalization. Reset
  // per call so a stale value from an earlier capture can never be
  // attributed to this one.
  lastRxAirtimeSec_ = 0.0;
  if (res && res->header.mode < kNumModes) {
    TxFrameParams tp;
    tp.family = rxFamily;
    tp.sig = res->sig;
    tp.mode = res->header.mode;
    tp.cpMode = res->header.cpMode;
    tp.frameType = res->header.frameType;
    tp.seqLow = res->header.seqLow;
    std::vector<uint8_t> shape(res->header.length);  // geometry needs size only
    tp.payload = shape;
    // The peer masks toward us exactly when it would mask its own TX:
    // same predicate, or the estimate is wrong for spread-mode frames.
    tp.mask = fsm_.state() == FsmState::Connected &&
                      modeUsesCarrierMask(int(res->header.mode))
                  ? &rxMask_
                  : nullptr;
    lastRxAirtimeSec_ = frameAirtimeSec(tp);
  }
  if (res && !res->constellation.empty()) {
    lastConstellation_ = res->constellation;
    ++constellationSeq_;
  }
  if (res) lastRxWasCss_ = res->sig == ChirpSig::Css;
  if (res && (res->payloadOk || finalAttempt) && maskDet_ &&
      fsm_.state() == FsmState::Connected && mode_ >= 8 &&
      !res->carrierSnrDb.empty()) {
    maskDet_->onSnapshot(res->carrierSnrDb, now);
  }
  // Only a DECODED frame's spread estimate feeds the CP verdict: on a
  // frame that failed, the tracker's tap state is whatever the noise
  // left it with, and it read 4.4-4.6 ms on a 1 ms CCIR-Moderate
  // channel (churn trace, M10/M12 frames marked ok=0). The peak-hold
  // then kept that reading and forced a LONG CP - a capacity loss on a
  // channel that needed none.
  if (res && res->payloadOk && fsm_.state() == FsmState::Connected) {
    // Peak-hold with slow decay: the CP must cover the WORST spread the
    // channel presents, and under Rayleigh fading the instantaneous tap
    // extent collapses whenever the far tap fades - a plain EMA (or any
    // averaged estimate) flickers and can never sustain a CP verdict.
    if (!haveSpread_) {
      spreadEmaMs_ = res->delaySpreadMs;
      haveSpread_ = true;
    } else {
      spreadEmaMs_ =
          std::max(double(res->delaySpreadMs), spreadEmaMs_ * 0.99);
    }
    // CP stability gate: a CP-only change needs the same verdict for a
    // sustained run of frames (spread estimates are noisy; churn starves
    // the link with MODE_CHANGE traffic).
    const CpMode want = cpForSpread(spreadEmaMs_);
    if (want != cpWant_ || cpWantSince_ < 0.0) cpWantSince_ = now;
    cpWantStreak_ = want == cpWant_ ? cpWantStreak_ + 1 : 1;
    cpWant_ = want;
    static const bool cpDbg = getenv("SKW_CP_DEBUG") != nullptr;
    if (cpDbg) {
      fprintf(stderr,
              "%s cp t=%.1f spread=%.2f ema=%.2f want=%d cur=%d streak=%d "
              "ok=%d mode=%d\n",
              cfg_.policy.ownCall.c_str(), now, res->delaySpreadMs,
              spreadEmaMs_, int(want), int(cpMode_), cpWantStreak_,
              res->payloadOk ? 1 : 0, int(res->header.mode));
    }
  }
  if (res) {
    if (res->payloadOk) {
      ++framesOk_;
    } else if (finalAttempt) {
      ++framesFailed_;
    }
  }
  // Channel-character evidence. Only FINAL attempts count as failures -
  // a truncated ladder try is not a channel verdict (the same rule the
  // AMC and mask detector use).
  if (res && fsm_.state() == FsmState::Connected &&
      (res->payloadOk || finalAttempt)) {
    chanClass_.onFrame(res->snrDb, res->delaySpreadMs, res->pilotStability,
                       res->payloadOk);
    static const bool clsDbg = getenv("SKW_CHANCLASS_DEBUG") != nullptr;
    if (clsDbg) {
      // Per-frame classifier input: what the Moderate statistic has to
      // be designed from (pilot excess per decoded frame, by mode).
      fprintf(stderr,
              "%s cls t=%.1f mode=%d snr=%.1f evm=%.1f spread=%.2f "
              "pilot=%.4f ok=%d verdict=%d\n",
              cfg_.policy.ownCall.c_str(), now, int(res->header.mode),
              res->snrDb, res->evmSnrDb, res->delaySpreadMs,
              res->pilotStability, res->payloadOk ? 1 : 0,
              int(chanClass_.verdict()));
    }
    if (amc_) {
      amc_->setChannelClass(chanClass_.verdict());
      // The M11+ pilot-stability and M12+ selectivity gates have existed
      // since step 3.1 but nothing ever fed them - they were evaluating
      // their default 0 and could never fire.
      amc_->onQualityIndicators(res->selectivity, res->pilotStability);
    }
  }
  if (!res || !res->payloadOk) {
    if (amc_ && stage == RxStage::Ok && finalAttempt) {
      amc_->onFrameResult(false, false, now);
    }
    // EXPLICIT NAK (PACTOR's CS-NAK). A DATA-class burst whose PHY
    // header decoded but whose payload failed its FINAL attempt is a
    // burst the peer will otherwise only learn about from its own
    // retransmission timer. Answer it immediately: the next ACK carries
    // kRxFlagRxFail and our ack_base, and the sender resends that frame.
    // Header-less failures are not attributed (could be noise, could be
    // a third party) and probes have their own verdict below.
    static const bool kNak = getenv("SKW_NO_NAK") == nullptr;  // diagnostic A/B
    if (kNak && res && !res->payloadOk && finalAttempt &&
        fsm_.state() == FsmState::Connected &&
        (res->header.frameType == uint8_t(LinkType::Data) ||
         res->header.frameType == kBurstContainerFrameType)) {
      rxFailPending_ = true;
      ackPending_ = true;
    }
    // A PROBE whose header decoded but payload failed still gets its
    // verdict: the probeId rides the PHY header's seqLow.
    if (res && !res->payloadOk && finalAttempt &&
        res->header.frameType == uint8_t(LinkType::Probe) &&
        fsm_.state() == FsmState::Connected) {
      queueControl(LinkType::ProbeAck,
                   ProbeAckBody{res->header.seqLow, 0, kProbeFailed}.pack(),
                   sigForMode(mode_),
                   std::min(mode_, 8));
    }
    return false;
  }
  if (res->header.frameType == kBurstContainerFrameType) {
    // Greedy container parse: [u16le len][link frame] entries; each
    // inner frame carries its own CRC and dispatches independently.
    QualityMeasurement q{res->snrDb, 0, res->cfoHz};
    size_t off = 0;
    const auto& pl = res->payload;
    while (off + 2 <= pl.size()) {
      const size_t len = size_t(pl[off]) | (size_t(pl[off + 1]) << 8);
      off += 2;
      if (len == 0 || off + len > pl.size()) break;
      auto inner = parseFrame({pl.data() + off, len});
      off += len;
      if (inner) onFrame(*inner, q, now);
    }
    return true;
  }
  auto parsed = parseFrame(res->payload);
  if (!parsed) return true;  // frame decoded; envelope just isn't ours
  QualityMeasurement q;
  q.snrDb = res->snrDb;
  q.selectivity = 0;
  q.cfoHz = res->cfoHz;
  // A frame too short to converge the tracker reports its seed, not a
  // measurement (RxFrameResult::snrConverged). Keyed on symbols walked,
  // not payload bytes: at M4 (spread 16) even a 20-byte frame spans
  // dozens of symbols, while a fast-tier ACK spans ~5.
  q.snrTrusted = res->snrConverged;
  onFrame(*parsed, q, now);
  return true;
}

void LinkEngine::onFrame(const ParsedFrame& f, const QualityMeasurement& q,
                         double now) {
  static const bool frDbg = getenv("SKW_FRAME_DEBUG") != nullptr;
  if (frDbg) {
    fprintf(stderr, "%s RXFRM t=%.1f type=%d seq=%u\n",
            cfg_.policy.ownCall.c_str(), now, int(f.env.type),
            unsigned(f.env.sequence));
  }
  // The peer's SNR report (ACK snrHalfDb) and the AMC read this: keep
  // the last MEASURED figure, not a short frame's seed guess.
  if (q.snrTrusted || !haveTrustedRxQuality_) {
    lastRxQuality_ = q;
    haveTrustedRxQuality_ = haveTrustedRxQuality_ || q.snrTrusted;
  }
  const size_t inFlightBefore = arqTx_.inFlight();
  // Idle-disconnect liveness: refreshed by session traffic, and NOT by
  // pre-session CALL frames - a dead peer's reconnect attempts used to
  // keep the stale session alive forever (half of the reconnect
  // deadlock handled in the Call case below). Everything except CALL
  // refreshes: gating on exact sessionId regressed one-way transfer
  // outright (ACKs ride containers whose envelope id can differ
  // legitimately across resume epochs).
  if (f.env.type != LinkType::Call) fsm_.rxTraffic(now);

  // Piggybacked ack_base (PROTOCOL.md link envelope, bytes 6-7). Every
  // session frame carries the peer's cumulative receive floor; consuming
  // it releases delivered frames without waiting for a dedicated ACK,
  // which is the whole point of putting it in the envelope. It was
  // written on TX (and covered by the crypto AAD) but never read - so
  // every acknowledgement cost a separate frame.
  //
  // Only frames from THIS session may move our window: pre-session
  // handshake frames and connectionless broadcasts carry ack_base 0,
  // which would otherwise read as "everything below 0" - harmless by
  // luck today, but a trap the moment sequence numbers wrap.
  switch (f.env.type) {
    case LinkType::Call:
    case LinkType::CallAck:
    case LinkType::Start:
    case LinkType::StartAck:
    case LinkType::StartNak:
    case LinkType::Cq:
    case LinkType::Beacon:
    case LinkType::BcastData:
      break;
    default:
      if (fsm_.state() == FsmState::Connected &&
          f.env.sessionId == sessionId_) {
        arqTx_.onPiggybackAck(f.env.ackBase, now);
      }
      break;
  }

  probeGate_.onFrameObserved();
  if (amc_ && fsm_.state() == FsmState::Connected) {
    if (q.snrTrusted) amc_->onLocalSnr(q.snrDb);
    advisor_.onCfo(q.cfoHz, now);
  }
  switch (f.env.type) {
    case LinkType::Call: {
      auto call = CallBody::parse(f.body);
      if (!call || call->dstCall != cfg_.policy.ownCall) return;
      if ((fsm_.state() == FsmState::Connected ||
           fsm_.state() == FsmState::Disconnecting) &&
          call->srcCall == remoteCall_) {
        // The peer is calling FRESH while we still hold a session with
        // it: it lost that session (died at marginal SNR, rebooted,
        // operator abort). This is the session-level reconnect dedup
        // the FSM's rxCall comment always promised but nothing
        // implemented - without it the stale side IGNORED every
        // reconnect CALL while those same CALLs refreshed its idle
        // timer, keeping the dead session immortal and the link down
        // for good. Tear down as a FAULT drop (resumable: the peer may
        // present the token in its START) and answer as a fresh
        // responder.
        faultDisc_ = true;
        fsm_.cmdAbort(now);
        drainFsmActions(now);
      }
      if (fsm_.state() == FsmState::Calling) {
        // Simultaneous-connect collision: both stations are calling each
        // other. Deterministic tie-break - the lexically GREATER callsign
        // yields and answers as responder; the other ignores the CALL
        // and keeps its initiator ladder running.
        if (call->srcCall != remoteCall_ ||
            cfg_.policy.ownCall <= call->srcCall) {
          return;
        }
      }
      lastCall_ = *call;
      remoteCall_ = call->srcCall;
      isInitiator_ = false;
      hsOnCss_ = lastRxWasCss_;  // answer on the transport that reached us
      // Answer on the FAMILY that reached us too. The caller stepped down
      // the ladder because the wider family would not close; replying on
      // our own configured family would put the answer right back into
      // the channel that just failed. Restored on teardown.
      adoptSessionFamily(rxFamily_, now);
      fsm_.rxCall(now);
      break;
    }
    case LinkType::CallAck: {
      auto ack = CallAckBody::parse(f.body);
      if (!ack) return;
      noteHandshakeRtt(now);
      lastCallAck_ = *ack;
      onCfoReport(ack->cfoHalfHz * 0.5);
      fsm_.rxCallAck(now);
      break;
    }
    case LinkType::Start: {
      noteHandshakeRtt(now);  // responder: its CALL_ACK -> this START
      auto start = StartBody::parse(f.body);
      if (!start) return;
      if (fsm_.state() == FsmState::Connected && !isInitiator_ &&
          !lastStartAckBody_.empty()) {
        // Duplicate START: our START_ACK was lost on the way back (near
        // the deep-tier floor every leg runs ~50%). Repeat the SAME ack
        // - never re-run session setup or the KEX.
        queueControl(LinkType::StartAck, lastStartAckBody_, hsSig(),
                     hsMode());
        return;
      }
      if (fsm_.state() != FsmState::CallAnswered) return;
      // The responder consults its own memory of this peer/channel too:
      // evaluateStart() takes the LOWER of the proposal and its own view.
      applyChanMemSeed(lastCall_.srcCall, now);
      auto r = evaluateStart(cfg_.policy, lastCall_, *start, q);
      bool ok = std::holds_alternative<StartAckBody>(r);
      pendingNakReason_ = ok ? StartNakReason::Busy
                             : std::get<StartNakBody>(r).reason;
#ifdef SKW_HAVE_CRYPTO
      cryptoOn_ = false;
      pendingKexResponse_.clear();
      if (ok && (start->caps & kCapEncryption)) {
        // Encryption was negotiated: the KEX must succeed or the session
        // is refused (SECURITY.md - no silent cleartext fallback).
        crypto_.zeroize();
        bool kexOk = crypto_.setPsk(cfg_.psk) && !start->kexPublic.empty();
        if (kexOk) {
          isInitiator_ = false;
          if (auto blob = crypto_.acceptKexInit(kexCtx(), start->kexPublic)) {
            pendingKexResponse_ = std::move(*blob);
          } else {
            kexOk = false;  // transcript auth failed
          }
        }
        if (!kexOk) {
          ok = false;
        } else {
          cryptoOn_ = true;
          txSeqHi_ = rxSeqHi_ = 0;
        }
      }
#endif
      if (ok) {
        mode_ = std::get<StartAckBody>(r).finalMode;
        sessionCaps_ = start->caps;
        protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
      }
      fsm_.rxStart(now, ok);
      break;
    }
    case LinkType::StartAck: {
      auto ack = StartAckBody::parse(f.body);
      if (!ack) return;
      noteHandshakeRtt(now);
#ifdef SKW_HAVE_CRYPTO
      if (sessionCaps_ & kCapEncryption) {
        if (ack->kexResponse.empty() ||
            !crypto_.acceptKexResponse(kexCtx(), ack->kexResponse)) {
          // Peer could not authenticate: tear down rather than continue
          // in cleartext.
          crypto_.zeroize();
          cryptoOn_ = false;
          fsm_.rxStartNak(now);
          return;
        }
        cryptoOn_ = true;
        txSeqHi_ = rxSeqHi_ = 0;
      }
#endif
      mode_ = ack->finalMode;
      if (ack->hasResumeToken && (sessionCaps_ & kCapResume)) {
        resumeCtx_.valid = true;
        resumeCtx_.token = ack->resumeToken;
        resumeCtx_.remoteCall = remoteCall_;
        resumeCtx_.family = cfg_.policy.family;
        resumeCtx_.caps = sessionCaps_;
        resumeCtx_.freshAt = now;
      }
      fsm_.rxStartAck(now);
      break;
    }
    case LinkType::StartNak: {
      auto nak = StartNakBody::parse(f.body);
      // ResumeInvalid is NEVER fatal (frame.hpp, PROTOCOL.md): the peer
      // kept nothing to continue from, so fall back to a clean START on
      // this same call. This used to be gated on sentResumeReq_, which
      // the START RETRY PATH CLEARS - so when the NAK for a RESUME_REQ
      // arrived after a retry had already moved on (and it does: a
      // control frame can sit 10-20 s in the capture ladder), the guard
      // was false, the NAK fell through to rxStartNak, and the caller
      // tore the attempt down as a hard refusal. That is the whole
      // reconnect-after-peer-death failure: a station that restarts
      // cannot honour the token, correctly says so, and the caller
      // treats "your token is stale" as "go away". Acting on the reason
      // alone is also the safer rule - an unsolicited ResumeInvalid
      // costs one extra START, which the responder already de-duplicates
      // (lastStartAckBody_ re-ack).
      if (nak && nak->reason == StartNakReason::ResumeInvalid) {
        // Peer rejected the token: fall back to a clean START in place
        // (never a session failure). The paused pipeline is dead - the
        // peer kept nothing to continue from.
        sentResumeReq_ = false;
        resumeCtx_.valid = false;
        replayPipelineForFreshPeer();
        applyChanMemSeed(remoteCall_, now);
        StartBody s = makeStart(cfg_.policy, lastCallAck_, lastRxQuality_);
        sessionCaps_ = s.caps;
        protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
        queueControl(LinkType::Start, s.pack(), hsSig(), hsMode());
        return;  // FSM stays in WaitStartAck for the real START_ACK
      }
      // A refused connect is one of the few failures an operator can
      // actually act on (wrong family, wrong band plan, a peer that
      // will not talk to us), so name it rather than leaving the
      // station to fall silently back to LISTENING.
      if (nak) {
        const char* why = "unknown";
        switch (nak->reason) {
          case StartNakReason::FamilyMismatch: why = "family mismatch"; break;
          case StartNakReason::VersionMismatch: why = "version mismatch"; break;
          case StartNakReason::CapabilityConflict:
            why = "capability conflict";
            break;
          case StartNakReason::Busy: why = "peer busy"; break;
          case StartNakReason::PolicyRefusal: why = "policy refusal"; break;
          case StartNakReason::ResumeInvalid: why = "resume token invalid"; break;
        }
        SKW_WARN("peer refused our START: %s (reason %u)", why,
                 unsigned(nak->reason));
      }
      fsm_.rxStartNak(now);
      break;
    }
    case LinkType::ResumeReq: {
      auto rr = ResumeReqBody::parse(f.body);
      if (!rr || fsm_.state() != FsmState::CallAnswered) return;
      const bool ok = resumeCtx_.valid &&
                      rr->token == resumeCtx_.token &&
                      lastCall_.srcCall == resumeCtx_.remoteCall &&
                      cfg_.policy.family == resumeCtx_.family &&
                      (now - resumeCtx_.freshAt) < kResumeWindowSec;
      if (!ok) {
        // Reject the token but stay ready: the peer falls back to a
        // clean START on this same call (PROTOCOL.md - never fatal).
        queueControl(LinkType::StartNak,
                     StartNakBody{StartNakReason::ResumeInvalid}.pack(),
                     hsSig(), hsMode());
        return;
      }
      resuming_ = true;
      sendResumeAckInstead_ = true;
      resumePeerNext_ = rr->lastRxSeq;
      resumePeerHasCkpt_ = rr->hasFileCkpt;
      resumePeerCkptId_ = rr->fileId;
      resumePeerCkptOff_ = rr->fileOffset;
      sessionCaps_ = resumeCtx_.caps;
      protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
      // Resume must NOT restore a mode the CURRENT channel cannot
      // carry: sessions that die because conditions collapsed would
      // resume at the stale fast mode, decode nothing, die, and loop
      // forever (observed at floor SNR: 3 resume cycles, 0 bytes in
      // 20 min). Clamp by the quality measured on this very exchange -
      // the AMC re-coordinates upward later if conditions allow.
      mode_ = std::min<int>(resumeCtx_.mode,
                            initialModeFromQuality(
                                cfg_.policy.family, q.snrDb, q.selectivity,
                                cfg_.policy.maxMode,
                                (sessionCaps_ & kCapCssTier) != 0));
      cpMode_ = resumeCtx_.cp;
      fsm_.rxStart(now, true);  // emits ResumeAck via the StartAck slot
      break;
    }
    case LinkType::ResumeAck: {
      auto ra = ResumeAckBody::parse(f.body);
      if (!ra || !sentResumeReq_ ||
          fsm_.state() != FsmState::WaitStartAck) {
        return;
      }
      sentResumeReq_ = false;
      resuming_ = true;
      resumePeerNext_ = ra->lastRxSeq;
      resumePeerHasCkpt_ = ra->hasFileCkpt;
      resumePeerCkptId_ = ra->fileId;
      resumePeerCkptOff_ = ra->fileOffset;
      // Same stale-mode clamp as the responder side (see ResumeReq):
      // transient asymmetry is fine - frames self-describe their mode
      // and MODE_CHANGE re-coordinates.
      mode_ = std::min<int>(resumeCtx_.mode,
                            initialModeFromQuality(
                                cfg_.policy.family, q.snrDb, q.selectivity,
                                cfg_.policy.maxMode,
                                (sessionCaps_ & kCapCssTier) != 0));
      cpMode_ = resumeCtx_.cp;
      fsm_.rxStartAck(now);
      break;
    }
    case LinkType::Data: {
#ifdef SKW_HAVE_CRYPTO
      std::vector<uint8_t> opened;
      const uint8_t* bodyPtr = f.body.data();
      size_t bodyLen = f.body.size();
      if (cryptoOn_) {
        if (!(f.env.flags & kFlagEncrypted)) return;  // downgrade attempt
        opened = f.body;
        if (!openPayload(f.env, opened)) {
          ++framesFailed_;  // tag or replay failure
          return;
        }
        bodyPtr = opened.data();
        bodyLen = opened.size();
      } else if (f.env.flags & kFlagEncrypted) {
        return;  // encrypted frame with no session crypto: ignore
      }
      auto d = DataBody::parse({bodyPtr, bodyLen});
#else
      auto d = DataBody::parse(f.body);
#endif
      if (!d || fsm_.state() != FsmState::Connected) return;
      // (Responder mode adoption from the DATA header was tried here
      // 2026-09-05 and removed: across a 900 s half-duplex churn soak with
      // ten mode changes it never fired once - apply-at-announce plus the
      // MODE_CHANGE-on-receipt path keep both ends in step - while it cost
      // the deep-tier throughput gate 19% -> 11% of net.)
      if (turnSchedActive()) {
        // A DATA frame proves the peer holds (or just took) the sending
        // role: an outstanding grant is confirmed, and a station that
        // STILL believes it is the ISS has diverged (a lost grant
        // series). Deterministic tie-break: the responder yields, the
        // initiator holds - the link converges to one sender either way.
        if (grantAwaitingData_ && getenv("SKW_TURN_DEBUG")) {
          fprintf(stderr, "%s GRANT t=%.1f CONFIRMED by peer DATA\n",
                  cfg_.policy.ownCall.c_str(), now);
        }
        grantAwaitingData_ = false;
        if (turnRole_ == TurnRole::Iss && !isInitiator_) {
          setTurnRole(TurnRole::Irs, now);
        }
      }
      // (AMC retry accounting happens after the ARQ accept below: a
      // retransmission that arrives as a DUPLICATE proves the original
      // got through, so it was the MAC's doing, not the channel's.)
      // Stash the proto flags with the payload so they survive the
      // out-of-order window and reach the proto layer in order.
      std::vector<uint8_t> stored;
      stored.reserve(1 + d->payload.size());
      stored.push_back(f.env.flags &
                       (kFlagCompressed | kFlagMoreFragments));
      stored.insert(stored.end(), d->payload.begin(), d->payload.end());
      const auto acc = arqRx_.onData(f.env.sequence, std::move(stored));
      // A retransmission counts as channel evidence only when it is the
      // FIRST copy to arrive (the earlier copies were lost). A duplicate
      // means the original was received and the retransmit was
      // spurious - a frame stranded at a turn handover, or the drain's
      // fast-retransmit racing a late ACK. Turn-taking produces those
      // structurally; counted as retries they held the ratio at 0.2-0.5
      // and pinned the ladder at M6 for 300 s at a measured 10-12 dB,
      // above the 0.15 promotion gate (churn seed 11).
      if (amc_) {
        static const bool kCountDupRetries =
            getenv("SKW_DUP_RETRY") != nullptr;  // diagnostic A/B: old rule
        const bool duplicate = acc == decltype(acc)::Duplicate;
        amc_->onFrameResult(d->rv > 0 && (kCountDupRetries || !duplicate),
                            true, now);
      }
      static const bool protoDbg = getenv("SKW_PROTO_DEBUG") != nullptr;
      if (protoDbg) {
        fprintf(stderr, "%s rxDATA seq=%u flags=%02x len=%zu acc=%d\n",
                cfg_.policy.ownCall.c_str(), f.env.sequence, f.env.flags,
                d->payload.size(), int(acc));
      }
      for (auto& p : arqRx_.takeDelivered()) {
        if (p.empty()) continue;
        protoRx_.onSegment(
            p[0], std::span<const uint8_t>(p.data() + 1, p.size() - 1));
      }
      drainFileRx();
      // ACK batching (M13+): one cumulative ACK per 4 data bursts keeps
      // the reverse channel from eating ~25% of the airtime at the
      // VHF-FM top rungs. Lower modes ACK per burst as before; the idle
      // flush in tick() covers message tails shorter than a batch.
      ++dataSinceAck_;
      lastDataRxAt_ = now;
      // Batch = 3 containers (12 frames): the ACK rides back while the
      // sender still holds one container of window - no stall. Batched
      // feedback is a VHF-FM sustained-bulk optimization ONLY: on HF
      // M13 the traffic is interactive/small and a lost tail-flush ACK
      // stalls the sender for the batch-scaled timeout (observed as a
      // 50 s data freeze in the real-device soak).
      if (activeBand() != Band::VhfFm || mode_ < 13 ||
          dataSinceAck_ >= 12) {
        ackPending_ = true;
        dataSinceAck_ = 0;
      }
      break;
    }
    case LinkType::Ack: {
      auto a = AckBody::parse(f.body);
      if (!a) return;
      static const bool protoDbg = getenv("SKW_PROTO_DEBUG") != nullptr;
      if (protoDbg) {
        fprintf(stderr, "%s rxACK base=%u bitmap=%04x inflight=%zu\n",
                cfg_.policy.ownCall.c_str(), a->ackBase, a->gapBitmap,
                arqTx_.inFlight());
      }
      // Responder-side pin request (rx_flags). Only the initiator acts
      // on it, so there is still exactly one mode controller.
      if (isInitiator_ && amc_ && (a->rxFlags & kRxFlagModeReq)) {
        const uint8_t want = a->rxFlags & kRxFlagModeMask;
        if (want == kRxModeAuto) {
          if (amc_->pinned()) amc_->unpin();
        } else {
          const int lowest = (sessionCaps_ & kCapCssTier) ? 0 : kFirstMode;
          if (int(want) >= lowest && int(want) < kNumModes &&
              (!amc_->pinned() || amc_->mode() != want)) {
            amc_->pin(want, now);
            requestModeChange(int(want), cpMode_, now);
          }
        }
      }
      if (amc_) amc_->onRemoteSnr(a->snrHalfDb * 0.5, now);
      remoteSnrDb_ = a->snrHalfDb * 0.5;
      onCfoReport(a->cfoHalfHz * 0.5);
      lastFeedbackAt_ = now;  // ends the post-burst listen turn
      // Explicit NAK: the peer heard a DATA burst it could not decode.
      // Its ack_base is the frame it needs; resend it NOW rather than
      // after the retransmission timer (50-90 s under fading - the
      // per-frame cost that made the floor scenario deliver nothing).
      if (a->rxFlags & kRxFlagRxFail) {
        static const bool nakDbg = getenv("SKW_ARQ_DEBUG") != nullptr;
        const bool q = arqTx_.nakAt(a->ackBase, now);
        if (nakDbg) {
          fprintf(stderr, "%s rxNAK base=%u requeued=%d\n",
                  cfg_.policy.ownCall.c_str(), a->ackBase, q ? 1 : 0);
        }
        // A NAK IS demotion evidence. The sender's only failure signal
        // for its own bursts used to be the ACK timeout (charged to the
        // AMC in tick); a NAK arrives before the timer and re-arms it,
        // so without this the AMC never heard of a single lost burst -
        // measured on the static M3 ramp as one frame NAKed 22 times at
        // an unchanged mode until the retry budget tore the session
        // down. ONE charge per NAK: a NAK is one lost burst. The timeout
        // path charges two per timeout EVENT however many bursts were in
        // flight; a NAK arrives per burst, and on the 16-deep VHF-FM pipe
        // several bursts fail inside one RTO - two per NAK drew several
        // times the old demotion pressure for the same loss and knocked
        // M14 down mid-run (VHF sustained gate 24.0 vs 25.0 kbps). The
        // static M3 case that needed charging at all had 22 NAKs.
        if (amc_) amc_->onFrameResult(false, false, now);
      }
      // Turn-taking: mirror the peer's break request. Tracking the
      // LATEST ACK (set or clear) means a withdrawn request cannot
      // leave a stale grant pending.
      if (turnSchedActive()) {
        breakReqPending_ = (a->rxFlags & kRxFlagBreakReq) != 0;
      }
      arqTx_.onAck(*a, now);
      break;
    }
    case LinkType::ModeChange: {
      auto mc = ModeChangeBody::parse(f.body);
      if (!mc || fsm_.state() != FsmState::Connected) return;
      if (isInitiator_) return;  // only the initiator drives mode control
      // Demote-echo on the ROBUST transport; promote-echo rides the
      // (lower of target/current) committed mode as before.
      {
        const int ackMode =
            mode_ <= 3 ? mode_
                       : (int(mc->targetMode) < mode_
                              ? 4
                              : std::min(int(mc->targetMode), mode_));
        queueControl(LinkType::ModeAck, ModeAckBody{mc->epoch}.pack(),
                     sigForMode(ackMode), ackMode);
      }
      applyMode(mc->targetMode, CpMode(mc->cpMode), now);
      break;
    }
    case LinkType::ModeAck: {
      auto ma = ModeAckBody::parse(f.body);
      if (!ma || pendingMode_ < 0 || ma->epoch != modeEpoch_) return;
      applyMode(pendingMode_, pendingCp_, now);
      pendingMode_ = -1;
      break;
    }
    case LinkType::Disc: {
      auto d = DiscBody::parse(f.body);
      if (d && (d->reason == DiscReason::RetryExhausted ||
                d->reason == DiscReason::IdleTimeout ||
                d->reason == DiscReason::Fault)) {
        faultDisc_ = true;  // peer dropped us for cause: resumable
      }
      fsm_.rxDisc(now);
      break;
    }
    case LinkType::DiscAck:
      fsm_.rxDiscAck(now);
      break;
    case LinkType::Probe: {
      auto p = ProbeBody::parse(f.body);
      if (!p || fsm_.state() != FsmState::Connected) return;
      // Decoded at the candidate mode: clean when the margin holds,
      // marginal when it decoded right at the edge.
      const double floor = fadingFloorDb(cfg_.policy.family, p->probeMode);
      const double ref = floor < 90.0
                             ? floor
                             : floorDb(cfg_.policy.family, p->probeMode);
      // Same operating point as the AMC's promote margin (amc.hpp): the
      // probe is the AMC's admission test for a rung, so "clean" means
      // the rung is promotable, in the calibrated table convention.
      const uint8_t quality = q.snrDb >= ref + kProbeCleanMarginDb
                                  ? kProbeClean
                                  : kProbeMarginal;
      queueControl(LinkType::ProbeAck,
                   ProbeAckBody{p->probeId,
                                int8_t(std::clamp(q.snrDb * 2.0, -127.0,
                                                  127.0)),
                                quality}
                       .pack(),
                   sigForMode(mode_),
                   std::min(mode_, 8));
      break;
    }
    case LinkType::ProbeAck: {
      auto pa = ProbeAckBody::parse(f.body);
      if (!pa || !isInitiator_) return;
      const bool clearedNow =
          probeGate_.onProbeAck(pa->probeId, pa->decodedOk);
      if (amc_) amc_->onProbeResult(clearedNow, now);
      break;
    }
    case LinkType::MaskUpdate: {
      auto mu = MaskUpdateBody::parse(f.body);
      if (!mu || fsm_.state() != FsmState::Connected) return;
      // The peer's RX sees interference on these carriers: zero them in
      // OUR transmissions from now on, and confirm.
      txMask_ = CarrierMask();  // wire mask covers the HF 72-carrier max
      std::copy(mu->mask.begin(), mu->mask.end(), txMask_.bits.begin());
      static const bool maskDbg = getenv("SKW_MASK_DEBUG") != nullptr;
      if (maskDbg) {
        fprintf(stderr, "%s rxMASKUPD epoch=%d masked=%d\n",
                cfg_.policy.ownCall.c_str(), mu->epoch,
                txMask_.maskedCount(cfg_.policy.family));
      }
      queueControl(LinkType::MaskAck, MaskAckBody{mu->epoch}.pack(),
                   sigForMode(mode_),
                   std::min(mode_, 8));
      break;
    }
    case LinkType::MaskAck: {
      auto ma = MaskAckBody::parse(f.body);
      static const bool maskDbg = getenv("SKW_MASK_DEBUG") != nullptr;
      if (maskDbg) {
        fprintf(stderr, "%s rxMASKACK epoch=%d pending=%d\n",
                cfg_.policy.ownCall.c_str(), ma ? ma->epoch : -1,
                pendingMaskEpoch_);
      }
      if (!ma || pendingMaskEpoch_ < 0 ||
          ma->epoch != uint8_t(pendingMaskEpoch_)) {
        return;
      }
      rxMask_ = pendingRxMask_;  // peer now zeroes these toward us
      pendingMaskEpoch_ = -1;
      maskCooldownUntil_ = lastNow_ + 20.0;  // space successive epochs
      break;
    }
    case LinkType::KeyUpdate: {
#ifdef SKW_HAVE_CRYPTO
      auto ku = KeyUpdateBody::parse(f.body);
      if (!ku || !cryptoOn_ || isInitiator_) return;
      // Epoch skew tolerance is 1 (grace window only): the low byte on
      // the wire resolves against the current epoch.
      const uint16_t want = uint16_t((crypto_.epoch() & 0xFF00) | ku->newEpoch);
      if (want != uint16_t(crypto_.epoch() + 1)) return;
      crypto_.setEpoch(want);
#endif
      break;
    }
#ifdef SKW_HAVE_VOICE
    case LinkType::VoiceOpen: {
      auto vo = VoiceOpenBody::parse(f.body);
      if (!vo || fsm_.state() != FsmState::Connected) return;
      // Accept at what our transport can carry (never above the peer's
      // proposal): the responder may only lower it.
      auto codec = clampVoiceCodec(cfg_.policy.family, mode_, vo->codec);
      if (!codec) return;  // cannot carry voice here: stay silent
      voiceEnc_.setCodec(*codec);
      voiceFsm_.rxOpen(*codec, now);
      drainVoiceActions(now);
      break;
    }
    case LinkType::VoiceOpenAck: {
      auto vo = VoiceOpenBody::parse(f.body);
      if (!vo) return;
      voiceEnc_.setCodec(vo->codec);
      voiceFsm_.rxOpenAck(vo->codec, now);
      drainVoiceActions(now);
      break;
    }
    case LinkType::VoiceClose:
      voiceFsm_.rxClose(now);
      drainVoiceActions(now);
      jitter_.reset();
      voiceDec_.reset();
      haveVoiceRxSeq_ = false;
      break;
    case LinkType::VoicePttReq:
      // Tie-break by callsign order so a simultaneous request never
      // leaves both stations believing they hold the channel.
      voiceFsm_.rxPttReq(now, cfg_.policy.ownCall < remoteCall_);
      drainVoiceActions(now);
      break;
    case LinkType::VoicePttGrant:
      voiceFsm_.rxPttGrant(now);
      drainVoiceActions(now);
      break;
    case LinkType::VoicePttEnd:
      voiceFsm_.rxPttEnd(now);
      drainVoiceActions(now);
      break;
    case LinkType::VoiceData: {
      std::vector<uint8_t> body = f.body;
#ifdef SKW_HAVE_CRYPTO
      if (cryptoOn_) {
        if (!(f.env.flags & kFlagEncrypted)) return;
        if (!openPayload(f.env, body)) return;
      }
#endif
      auto vd = VoiceDataBody::parse(body);
      if (!vd) return;
      // Gaps in the VOICE sequence space conceal; they are never NAKed.
      if (haveVoiceRxSeq_) {
        const int16_t gap = seqDiff(f.env.sequence, voiceRxSeq_);
        if (gap > 1) {
          voiceDec_.onGap((gap - 1) * vd->frameCount);
        } else if (gap <= 0) {
          return;  // duplicate or reordered: voice has no retransmission
        }
      }
      voiceRxSeq_ = f.env.sequence;
      haveVoiceRxSeq_ = true;
      voiceDec_.onVoiceData(*vd);
      jitter_.push(voiceDec_.takePcm());
      break;
    }
#endif
    case LinkType::Keepalive:
      break;  // rxTraffic already refreshed the idle timer
    case LinkType::TurnGrant: {
      auto tg = TurnGrantBody::parse(f.body);
      if (!tg || !turnSchedActive()) return;
      if (int(tg->epoch) == peerGrantEpoch_) {
        break;  // retried grant already accepted; our DATA will confirm
      }
      peerGrantEpoch_ = tg->epoch;
      if (getenv("SKW_TURN_DEBUG")) {
        fprintf(stderr, "%s GRANT t=%.1f RECEIVED epoch=%u (was %s)\n",
                cfg_.policy.ownCall.c_str(), now, unsigned(tg->epoch),
                turnRole_ == TurnRole::Iss ? "ISS" : "IRS");
      }
      // The granting peer clearly has nothing to send right now; a
      // stale request from before its grant must not bounce the turn
      // straight back. An outstanding grant of our own is cancelled -
      // crossing grants (both ends yielding at once) must not keep
      // re-announcing a handover neither side wants.
      breakReqPending_ = false;
      grantAwaitingData_ = false;
      setTurnRole(TurnRole::Iss, now);
      break;
    }
    default:
      break;  // unknown-to-this-phase types are ignored, never fatal
  }

  // DEEP-TIER FRAME LENGTH, growth half. Measured across the WHOLE frame
  // rather than inside the ACK case: the envelope's piggybacked ack_base
  // releases frames before any dedicated ACK is parsed, so an ACK-only
  // hook saw no progress and never grew (observed: the target stayed at
  // 2 codewords and goodput stayed at 7% of net).
  if (mode_ >= kFirstMode && mode_ <= 7 &&
      arqTx_.inFlight() < inFlightBefore) {
    if (++robustCleanRun_ >= kCssGrowRun && robustCwTarget_ < kRobustCwMax) {
      robustCleanRun_ = 0;
      robustCwTarget_ = std::min(kRobustCwMax, robustCwTarget_ * 2);
      updateArqTimeout();
      if (getenv("SKW_CSS_DEBUG")) {
        fprintf(stderr, "robust grow -> cw=%d budget=%zu\n", robustCwTarget_,
                dataBudget_);
      }
    }
  }
  if (mode_ <= 3 && arqTx_.inFlight() < inFlightBefore) {
    if (++cssCleanRun_ >= kCssGrowRun && cssCwTarget_ < kCssCwMax) {
      cssCleanRun_ = 0;
      cssCwTarget_ = std::min(kCssCwMax, cssCwTarget_ * 2);
      updateArqTimeout();
      if (getenv("SKW_CSS_DEBUG")) {
        fprintf(stderr, "css grow -> cw=%d budget=%zu\n", cssCwTarget_,
                dataBudget_);
      }
    }
  }
  drainFsmActions(now);
}

}  // namespace skw
