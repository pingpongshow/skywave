// LinkEngine: the glue that runs a complete modem link - SessionFsm +
// handshake policy + ARQ + link frames over the real PHY (frame builder /
// receiver). Burst-oriented and time-injected: tick() may produce one TX
// burst (baseband samples); rxBurst() consumes a received capture.
// The worker (and the two-modem harness) own audio and scheduling.
//
// Notes:
//  - control frames ride sequence 0 outside ARQ (FSM retries cover them);
//    DATA frames carry ARQ sequence numbers
//  - app bytes flow through the proto pipeline (proto/pipeline.hpp):
//    coalesce -> zstd (negotiated) -> exact codeword-budget segments;
//    each segment's flags byte is stored WITH the ARQ payload so
//    retransmissions reproduce the original envelope flags
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "link/advisor.hpp"
#include "link/amc.hpp"
#include "link/arq.hpp"
#include "link/mask.hpp"
#include "link/monitor.hpp"
#include "link/probe.hpp"
#include "link/chanclass.hpp"
#include "link/chanmem.hpp"
#include "link/frame.hpp"
#include "link/fsm.hpp"
#include "link/handshake.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "proto/pipeline.hpp"
#ifdef SKW_HAVE_CRYPTO
#include "crypto/session.hpp"
#endif
#ifdef SKW_HAVE_VOICE
#include "voice/codec.hpp"
#include "voice/session.hpp"
#endif

namespace skw {

#ifdef SKW_HAVE_VOICE
// How voice rides a mode (VOICE.md "Codec Set and Mode Mapping"). Every
// VOICE_DATA unit is its own PHY frame - preamble, sync and header
// included - so a codec that merely fits the mode's NET rate can still
// fall behind real time: WIDE M10 at 2400 spent 160% of the speech
// duration on the air and the talkspurt lagged further every second.
// AUTO picks the highest codec, and the smallest unit (1-4 codewords,
// at most kVoiceUnitMaxSec of speech), whose airtime stays within
// kVoiceRealTimeFrac of the speech it carries. A manual codec the plan
// cannot carry in real time is still honoured (the NET-rate rule, 4
// codeword units) and reported as not real-time.
struct VoicePlan {
  VoiceCodec codec = VoiceCodec::C700;
  size_t unitBytes = 0;    // VOICE_DATA body budget (6-byte header incl.)
  double unitSpeechSec = 0;
  double airFrac = 0;      // unit airtime / speech it carries
  bool realTime = false;
};
inline constexpr double kVoiceRealTimeFrac = 0.85;
inline constexpr double kVoiceUnitMaxSec = 3.0;
std::optional<VoicePlan> planVoiceStream(Family f, int mode, CpMode cp,
                                         std::optional<VoiceCodec> want);
#endif

struct EngineConfig {
  HandshakePolicy policy;
  int arqMaxRetries = 8;
  bool maskingEnabled = true;  // negotiated via kCapMasking as well
  bool cpAuto = true;            // delay-spread-driven CPMODE
  bool cwId = false;             // CW ID after disconnect / 10-min
  int cwWpm = 20;
  std::vector<uint8_t> psk;      // 32 bytes when installed
  // Channel memory (LINK_ADAPTATION.md 3.5). Owned by the caller (the
  // worker persists it across restarts); null disables the feature
  // entirely, which is what `CHANMEM OFF` does.
  ChannelMemory* chanMem = nullptr;
  int32_t freqBucketKhz = -1;    // dial/1 kHz, -1 = unknown
};

class LinkEngine {
 public:
  explicit LinkEngine(EngineConfig cfg);

  // --- application API -------------------------------------------------
  void connectTo(const std::string& call, double now);
  void disconnect(double now);
  void sendData(std::span<const uint8_t> bytes);  // buffered until sent
  std::vector<uint8_t> takeReceivedData();

  // Control-port notification lines ("EVT:..."), drained by the caller.
  std::vector<std::string> takeEvents();

  // --- session resume (4.4) --------------------------------------------
  // True while the current connection was established via RESUME
  // (tests and diagnostics).
  bool sessionResumed() const { return resumed_; }
  bool hasResumeContext() const { return resumeCtx_.valid; }
  // Why the peer refused our last START (nullopt: it did not).
  std::optional<StartNakReason> lastRefusal() const { return lastRefusal_; }

  // --- crypto (5.3) ------------------------------------------------------
  // SECURITY reporting: AUTH is identity assurance (HMAC verified),
  // distinct from "encryption active".
  bool cryptoAuth() const;
  bool cryptoActive() const;
  uint16_t cryptoEpoch() const;
  // Manual KEYROTATE: bumps the epoch on a live encrypted session.
  bool rotateKeys(double now);

#ifdef SKW_HAVE_VOICE
  // --- voice (5.5) -------------------------------------------------------
  // VOICESTART/VOICESTOP/VOICEPTT. openVoice picks the codec from the
  // session transport (AUTO) or clamps a manual choice; false when the
  // current mode cannot carry voice at all.
  // Session voice: needs a connected session whose peer advertised
  // kCapVoice.
  bool openVoice(std::optional<VoiceCodec> want, double now);
  // Broadcast voice (VOICE.md): one-way talkspurts without a session, at
  // a fixed mode (M8+ that carries a codec). closeVoice / setVoicePtt /
  // pushVoicePcm drive it like session voice. Any listening station that
  // is not in a session plays a broadcast out (takeVoicePcm), locked to
  // one sender until it has been silent for kBcastVoiceIdleSec.
  bool openBroadcastVoice(std::optional<VoiceCodec> want, int mode, double now);
  bool broadcastVoiceOpen() const { return bvOpen_; }
  int broadcastVoiceMode() const { return bvMode_; }
  bool broadcastVoiceRx() const { return bvRxActive_; }
  void closeVoice(double now);
  bool setVoicePtt(bool on, double now);
  VoiceState voiceState() const {
    if (bvOpen_) return bvTalking_ ? VoiceState::Talking : VoiceState::Open;
    return voiceFsm_.state();
  }
  VoiceCodec voiceCodec() const {
    return bvOpen_ ? voiceEnc_.codec() : voiceFsm_.codec();
  }
  // The plan the open voice service runs on (VOICESTATUS RT=, UNIT=).
  const VoicePlan& voicePlanActive() const {
    return bvOpen_ ? bvPlan_ : voicePlan_;
  }
  // Microphone PCM in (8 kHz mono), speaker PCM out (jitter-buffered).
  void pushVoicePcm(std::span<const int16_t> pcm);
  std::vector<int16_t> takeVoicePcm(size_t samples);
  size_t voiceTxQueued() const;
  int voiceRxDepthMs() const { return jitter_.depthMs(); }
  uint32_t voiceConcealed() const { return voiceDec_.concealedFrames(); }
#endif

  // --- broadcast (4.5) ---------------------------------------------------
  // Connectionless frames: calling signature + robust header + M4.
  // Usable while idle (and alongside a session; the TX queue interleaves).
  void sendCq();
  void sendBeacon(double snrFloorDb);
  // Fragments into <= 16 BCAST_DATA frames (4-bit index/count); false
  // if the payload exceeds 16 * 256 bytes or is empty.
  bool sendBroadcast(std::span<const uint8_t> bytes);

  FsmState state() const { return fsm_.state(); }
  FsmDiscCause lastDiscCause() const { return fsm_.lastDiscCause(); }
  int sessionMode() const { return mode_; }
  // Session mode cap: the lower of the two stations' max_mode (START /
  // START_ACK). Neither the AMC nor a pin goes above it.
  int sessionMaxMode() const { return sessionMaxMode_; }
  size_t txBacklog() const { return protoTx_.backlogBytes(); }
  bool idleClean() const;  // nothing pending anywhere

  // --- mode control (3.2) ---------------------------------------------
  // Manual pin/unpin (MODE Mx / MODE AUTO). Pinning coordinates a
  // MODE_CHANGE to the pinned mode; fast-demote safety stays active.
  // Pin the runtime mode. On the INITIATOR this coordinates a
  // MODE_CHANGE directly. On the RESPONDER it raises a request that
  // rides the next ACK's rx_flags; the initiator remains the only mode
  // controller on the air, so the two cannot collide.
  bool pinMode(int mode, double now);
  void unpinMode();
  bool modePinned() const;
  double txShiftHz() const { return txShiftHz_; }
  const Advisor& advisor() const { return advisor_; }
  // This session's channel-character verdict (link/chanclass.hpp).
  ChannelClass channelClass() const { return chanClass_.verdict(); }
  CpMode cpMode() const { return cpMode_; }
  double lastRxCfoHz() const { return lastRxQuality_.cfoHz; }

  // --- observability / control surface (4.6) ---------------------------
  void abortSession(double now);       // immediate, no DISC exchange
  bool setCpPolicy(const std::string& p, double now);  // AUTO/SHORT/...
  std::string cpPolicy() const;
  const std::string& remoteCall() const { return remoteCall_; }
  uint16_t sessionId() const { return sessionId_; }
  uint16_t sessionCaps() const { return sessionCaps_; }
  double lastRxSnrDb() const { return lastRxQuality_.snrDb; }
  // Turn-taking (PROTOCOL.md "Turn-Taking") governs every connected
  // session: true while connected, and whether this station currently
  // holds the data-sender (ISS) role. The IRS answers only the burst
  // whose PHY header carries POLL (PROTOCOL.md "Poll").
  bool turnSchedActive() const {
    return fsm_.state() == FsmState::Connected;
  }
  bool turnIss() const { return turnRole_ == TurnRole::Iss; }
  // On-air duration implied by the PHY header of the capture the LAST
  // rxBurst() call decoded a header for (0 when it decoded none). The
  // header survives payload failure, so the worker's decode ladder can
  // tell when a failing burst is fully in hand and go straight to the
  // final attempt instead of climbing rungs of trailing noise.
  double lastRxBurstAirtimeSec() const { return lastRxAirtimeSec_; }
  // How long a burst from the peer is expected to stay on the air when
  // its own header has not said so yet: in session the ISS hears
  // feedback and the IRS hears DATA bursts; before a session exists, the
  // last decoded burst's airtime.
  double expectedPeerBurstSec() const {
    if (fsm_.state() != FsmState::Connected) return lastRxAirtimeSec_;
    return turnIss() ? ackAirSec_ : dataFrameAirSec_;
  }

  // --- GUI instrument taps -----------------------------------------------
  // Equalized payload symbols from the most recent decode. Collected only
  // while enabled, which the worker ties to the GUI being up.
  void setWantConstellation(bool on) { wantConstellation_ = on; }
  const std::vector<cfloat>& lastConstellation() const {
    return lastConstellation_;
  }
  uint32_t constellationSeq() const { return constellationSeq_; }
  // The carriers this session has excised, for the waterfall overlay.
  const CarrierMask& rxMask() const { return rxMask_; }

  // --- channel memory (3.5) ---------------------------------------------
  // The dial frequency keys the store's buckets; the companion app pushes
  // it with `FREQINFO <hz>` (INTERFACES.md). Changing it mid-session is
  // harmless: the key is only read at handshake and teardown.
  void setFreqBucketKhz(int32_t khz) { cfg_.freqBucketKhz = khz; }
  int32_t freqBucketKhz() const { return cfg_.freqBucketKhz; }
  double remoteSnrDb() const;   // from ACK reports; -99 when unknown
  double retryRatio() const;
  uint64_t framesOk() const { return framesOk_; }
  uint64_t framesFailed() const { return framesFailed_; }
  double netBps() const {
    return netRate(cfg_.policy.family, mode_, cpMode_);
  }

  // --- scheduling ------------------------------------------------------
  // Busy detector (DSP.md): the worker feeds the fused busy-detector
  // verdict each chunk. It arbitrates nothing in session (turn-taking
  // does); it only keeps any burst - DATA included - from keying over a
  // peer burst still airing, with a runaway cap (ccaDataGated, and the
  // control hold in tick). Voice bypasses.
  void setChannelBusy(bool busy) { chanBusy_ = busy; }
  // Seconds of already-emitted audio still waiting in the worker's TX
  // path (staging + ring). Feeds ARQ deadline arming (a frame sent now
  // airs only after this delay) - see ArqTx::setExtraDelay.
  void setTxQueueSec(double s) { txQueueSec_ = s < 0.0 ? 0.0 : s; }
  // Advance timers and, if a burst is due, return its baseband samples.
  std::optional<std::vector<cfloat>> tick(double now);
  // Feed a received capture (burst + padding); engine decodes and
  // reacts. Returns true when a frame decoded payload-clean (the
  // worker's windowed retry stops there). finalAttempt marks the LAST
  // ladder try for this capture: only then do failures count (FFAIL,
  // AMC, mask stats, HARQ accumulation, probe verdicts) - earlier
  // windows may simply have TRUNCATED the burst, and treating those as
  // real failures fed ~50% fake FER per container into every adaptive
  // layer (observed as mode/mask churn strangling two-way traffic on a
  // clean +25 dB channel).
  // `windowStartSec` (absolute time of samples[0], -1 = unknown) lets a
  // burst decoded from two overlapping captures be dispatched once.
  bool rxBurst(std::span<const cfloat> samples, double now,
               bool finalAttempt = true, double windowStartSec = -1.0);

 private:
  // Incremental preamble search per capture (receiver.hpp DetectCache),
  // keyed by window start and family: several captures are pending at
  // once on a noisy band, each climbing its own retry ladder.
  struct DetCacheSlot {
    DetectCache cache;
    uint64_t lastUse = 0;
  };
  static constexpr size_t kDetCacheSlots = 12;
  std::vector<DetCacheSlot> detCaches_;
  uint64_t detCacheClock_ = 0;
  DetectCache* detCacheFor(uint64_t key, Family family);
  struct OutFrame {
    LinkEnvelope env;
    std::vector<uint8_t> body;
    ChirpSig sig;
    int mode;
    bool retx = false;  // ARQ retransmission (diagnostics)
  };
  // sessionIdOverride: connectionless BCAST_DATA carries the SENDER's
  // callsign hash here instead of a session id (which is meaningless
  // outside a session), so a monitor can demultiplex overlapping
  // broadcasts. -1 keeps the current session id.
  void queueControl(LinkType t, std::vector<uint8_t> body, ChirpSig sig,
                    int mode, int32_t sessionIdOverride = -1);

  uint16_t bcastSenderTag() const;
  void drainFsmActions(double now);
  void pumpData(double now);
  std::vector<cfloat> buildBurst(const OutFrame& f, bool poll = false);
  // One PHY burst: a single link frame or a burst container (frameType
  // kBurstContainerFrameType). Mask, CP and TX shift applied.
  std::vector<cfloat> buildPhyBurst(std::span<const uint8_t> payload,
                                    ChirpSig sig, int mode, uint8_t frameType,
                                    uint8_t seqLow, bool poll);
  // Queue the clean START (with the KEX blob when encryption was
  // negotiated): the FSM's SendStart and the ResumeInvalid fallback.
  void queueStart(double now);
  // The committed capability set and everything that follows from it.
  void commitSessionCaps(uint16_t caps);
  // RESUME_REQ / RESUME_ACK accepted: continue the sequence spaces at the
  // stored mode, clamped by what this exchange's quality supports.
  void adoptResume(uint16_t peerNextSeq, const QualityMeasurement& q);
  void queueMaskUpdate(double now);
  void queueKeepalive();
  // Feedback (ACK, TURN_GRANT) transport: at least as robust as the
  // forward traffic - the session's CSS mode below M4, M4 for M4-M6.
  int feedbackMode() const;
  ChirpSig feedbackSig() const;
  OutFrame makeAckFrame();
  AckBody makeAckBody() const;
  // Transport of a MODE_CHANGE / MODE_ACK leaving `from` toward
  // `target`: demotions ride the robust tier, promotions the target.
  static int modeChangeTxMode(int from, int target);
  // Per-station xorshift jitter in [0, 1).
  double nextJitter();
  bool hasQueuedData() const;
  std::deque<OutFrame>::iterator firstQueuedData();
  // True when the DATA burst being built is the last before this station
  // stops and listens (PhyHeader::poll).
  bool burstEndsWindow() const;
  void applyTxShift(std::vector<cfloat>& burst) const;
  void onFrame(const ParsedFrame& f, const QualityMeasurement& q,
               double now);

  void applyMode(int mode, CpMode cp, double now);
  void updateArqTimeout();
  void requestModeChange(int target, CpMode cp, double now);

  EngineConfig cfg_;
  SessionFsm fsm_;
  ArqTx arqTx_;
  ArqRx arqRx_;
  std::optional<Amc> amc_;
  Advisor advisor_{AdvisorConfig::fromEnv()};
  bool advisorDue_ = false;  // a new SNR sample awaits one evaluation
  double lastCfoAssistAt_ = 0.0;
  HarqChase harq_;  // chase soft-combining across retransmissions
  ProbeGate probeGate_;
  // Carrier masking (per direction): txMask_ = what the PEER asked us to
  // zero when transmitting; rxMask_ = what we asked the peer to zero
  // (applies to our RX once the peer ACKs). Spread modes skip masking.
  std::optional<MaskDetector> maskDet_;
  CarrierMask txMask_;
  CarrierMask rxMask_;
  CarrierMask pendingRxMask_;
  int maskEpoch_ = 0;
  int pendingMaskEpoch_ = -1;
  double maskRetryAt_ = 0.0;
  int maskTries_ = 0;

 public:
  int txMaskedCount() const {
    return txMask_.maskedCount(cfg_.policy.family);
  }
  int rxMaskedCount() const {
    return rxMask_.maskedCount(cfg_.policy.family);
  }

 private:
  // Mode-control ownership: the session INITIATOR drives AMC decisions
  // and MODE_CHANGE; the responder's channel view reaches it via ACK and
  // keepalive SNR reports. Two independent controllers would collide
  // mid-air (observed: mode oscillation and pin override). A responder
  // pin is a REQUEST riding its ACKs (pinMode).
  bool isInitiator_ = false;
  std::string remoteCall_;
  int mode_ = 4;                  // committed session mode
  int sessionMaxMode_ = 13;       // min of both stations' max_mode
  CpMode cpMode_ = activeProfile().cpDefault;  // committed session CP
  // TX frequency alignment (PROTOCOL.md): tracks -reported/2 per report
  // (loop gain 0.5), clamped +-kTxShiftMaxHz.
  double txShiftHz_ = 0.0;
  // TX alignment clamp: the detector's +-55 Hz capture, less margin.
  // EVT:CFO_WARN fires from 40 Hz (worker), inside this clamp.
  static constexpr double kTxShiftMaxHz = 50.0;
  // Reverse-path alignment ACK: sent when the peer's frames arrive at
  // least this far off and no ACK has reported it, at most this often.
  static constexpr double kCfoAckMinHz = 2.0;
  static constexpr double kCfoAckIntervalSec = 15.0;
  double callRxCfoHz_ = 0.0;  // responder: CFO measured on the peer's CALL
  void onCfoReport(double reportedHz);
  double spreadEmaMs_ = 0.0;      // delay-spread EMA for CPMODE AUTO
  bool haveSpread_ = false;
  int cpWantStreak_ = 0;          // consecutive frames agreeing on a CP
  CpMode cpWant_ = kCpDefault;
  double cpWantSince_ = -1.0;     // when the current CP verdict began
  // A CP verdict is STABLE after 25 agreeing frames, or after a shorter
  // run (8) held for a full minute. The frame count alone was slowest
  // exactly when it mattered: ISI forces the robust tier, where an
  // exchange takes ~20 s, so 25 frames meant ~8 minutes at a CP too
  // short for the channel (measured: streak 21 after 670 s, transfer
  // stalled at 0 bytes). The peak-hold spread estimate is already
  // stable; the streak only filters estimator noise.
  bool cpVerdictStable(double now) const {
    return cpWantStreak_ >= 25 ||
           (cpWantStreak_ >= 8 && cpWantSince_ >= 0.0 &&
            now - cpWantSince_ >= 60.0);
  }
  double lastCpChange_ = 0.0;
  uint16_t sessionCaps_ = 0;      // committed capability intersection
  bool harqIr() const { return (sessionCaps_ & kCapHarqIr) != 0; }
  // Handshake-on-CSS (6.2): the initiator alternates CALL attempts onto
  // CSS M0 when enabled; the responder mirrors the transport the CALL
  // arrived on. All handshake control follows hsSig()/hsMode().
  bool hsOnCss_ = false;
  int callTxCount_ = 0;
  std::optional<StartNakReason> lastRefusal_;
  bool lastRxWasCss_ = false;
  std::vector<uint8_t> lastStartAckBody_;  // duplicate-START re-ack
  // Reason computed for the pending refusal (evaluateStart, or the KEX
  // failing), sent by SendStartNak: a refused connect must be
  // diagnosable from the calling end.
  StartNakReason pendingNakReason_ = StartNakReason::CapabilityConflict;
  // ARQ sequence of the LAST fragment of each proto message handed to
  // the ARQ, oldest first. When the ARQ's contiguous acknowledged point
  // passes one, that whole message reached the peer and ProtoTx can
  // drop its replay copy.
  std::deque<uint16_t> msgEndSeq_;
  double replayDbgAt_ = 0.0;
  // Confirm delivered messages and release their replay copies.
  void confirmDeliveredMessages();
  // A peer that RESTARTED kept nothing: re-send from the last message
  // boundary it is known to have taken, rather than discarding the
  // pipeline (which loses every byte the app had already handed us).
  void replayPipelineForFreshPeer();
  double deepIdleSec_ = 120.0;  // idle window for CSS-mode sessions
  int dataSinceAck_ = 0;        // ACK batching (M13+)
  double lastDataRxAt_ = 0.0;
  double ackFlushSec_ = 0.5;
  ChirpSig hsSig() const {
    return hsOnCss_ ? ChirpSig::Css : ChirpSig::Calling;
  }
  int hsMode() const { return hsOnCss_ ? 0 : 4; }
  // In-flight MODE_CHANGE we initiated (awaiting MODE_ACK). The new mode
  // is APPLIED at announce time (see requestModeChange); pendingMode_
  // now only drives the announcement retries and the MODE_ACK match.
  int pendingMode_ = -1;
  int modeChangeFrom_ = 4;  // committed mode the announcement left from
  CpMode pendingCp_ = kCpDefault;
  uint8_t modeEpoch_ = 0;
  double modeChangeDeadline_ = 0.0;
  int modeChangeTries_ = 0;
  uint16_t sessionId_ = 0;
  bool ackPending_ = false;       // send an ACK on the next tick
  // Pre-TX gate state (setChannelBusy, ccaDataGated).
  bool chanBusy_ = false;
  double ccaWaitStart_ = -1.0;   // DATA held for a busy channel since
  double ccaProceedAt_ = -1.0;   // post-burst listen turn ends at
  uint64_t ccaRng_ = 0;          // per-station jitter (desyncs peers)
  bool ccaGatedNow_ = false;     // per-tick cached verdict
  double txQueueSec_ = 0.0;      // worker TX backlog (setTxQueueSec)
  bool txWasDraining_ = false;   // edge detector: own burst just aired
  bool lastBurstHadData_ = false;  // the burst that just drained carried DATA
  bool lastBurstModeChange_ = false;  // ... or announced a MODE_CHANGE
  double modeReplyUntil_ = -1.0;      // hold DATA for the MODE_ACK until
  double ctrlWaitStart_ = -1.0;  // control busy-hold start
  double ctrlJitterUntil_ = 0.0; // post-clear anti-race jitter deadline
  int ctrlRun_ = 0;              // consecutive control bursts (fairness)
  double ackAirSec_ = 0.5;       // expected feedback airtime (listen window)
  // Half-duplex handoff scale: 1.0 for every OFDM mode (constants below
  // are OFDM-tuned), > 1 for the CSS deep tier whose frames are an order
  // of magnitude longer. Recomputed in updateArqTimeout().
  double airScale_ = 1.0;
  double dataQueuedSince_ = -1.0;  // data waiting to AIR (backstop clock)
  double maskCooldownUntil_ = 0.0;  // mask-epoch spacing / abandon backoff
  bool ccaDataGated(double now);
  QualityMeasurement lastRxQuality_{};
  bool haveTrustedRxQuality_ = false;  // a converged (long-frame) SNR seen
  // Reverse-path alignment (PROTOCOL.md "TX Frequency Alignment"): the
  // CFO of the latest decoded peer frame, whatever its length (the SNR
  // trust gate above would hold a one-way sender's figure at whatever
  // long frame it last heard), and when our last ACK - the only frame
  // that carries a CFO report - went out.
  double peerCfoHz_ = 0.0;
  double peerCfoAt_ = -1.0;
  double lastAckTxAt_ = -1.0;
  // CFO figure an ACK reports: the latest peer frame's while connected.
  double reportCfoHz() const {
    return peerCfoAt_ >= 0.0 ? peerCfoHz_ : lastRxQuality_.cfoHz;
  }
  // SNR figure reported to the peer in ACKs: the AMC's smoothed local
  // EMA once it has one (the number this end actually steers on), else
  // the last measurement. A per-frame figure held a fade-edge reading
  // for up to the remote-freshness window once only converged frames
  // refreshed it (measured: a -5 dB report on a 14 dB channel drove a
  // three-rung cascade in 27 s).
  double reportSnrDb() const {
    return amc_ && amc_->haveLocalSnr() ? amc_->localSnrDb()
                                        : lastRxQuality_.snrDb;
  }
  // Family the last capture actually decoded on. Differs from the
  // configured family only while the receiver-side scan is in play.
  Family rxFamily_ = Family::Wide;
  bool wantConstellation_ = false;
  double lastDeliverAt_ = -1.0;  // app-visible delivery (SKW_DELIV_DEBUG)
  double dataFrameAirSec_ = 0.0;  // current DATA TX-unit airtime
  // MODE_CHANGE commit latency (instrumentation, SKW_MODE_DEBUG MCLAT):
  // when the exchange was requested and from which rung.
  double modeReqAt_ = -1.0;
  int modeReqFrom_ = 0;
  // Adaptive frame length, in LDPC codewords per frame, for the deep
  // (CSS M0-M3) and the OFDM robust (M4-M7) tiers: grown after a run of
  // clean deliveries, halved on loss (see tick / onFrame).
  struct CwTarget {
    int min, max, target, cleanRun = 0;
    void restart() { target = min; cleanRun = 0; }
    bool shrink() {  // a loss; true when the target changed
      if (target <= min) return false;
      cleanRun = 0;
      target = std::max(min, target / 2);
      return true;
    }
    bool grow() {  // a clean delivery; true when the target changed
      if (++cleanRun < kCssGrowRun || target >= max) return false;
      cleanRun = 0;
      target = std::min(max, target * 2);
      return true;
    }
  };
  CwTarget cssCw_{kCssCwMin, kCssCwMax, 2};
  CwTarget robustCw_{kRobustCwMin, kRobustCwMax, kRobustCwMin};
  // When a standalone ACK was skipped in favour of piggybacking, so the
  // skip can be undone if our data never airs. -1 = nothing deferred.
  double ackDeferredAt_ = -1.0;
  // --- Turn-taking (PROTOCOL.md "Turn-Taking") --------------------------
  // Scheduled ISS/IRS roles replace occupancy sensing for in-session
  // DATA arbitration in every session: exactly one
  // station (the ISS) sends data; the other (IRS) sends feedback and
  // requests the turn via kRxFlagBreakReq; TURN_GRANT hands it over.
  enum class TurnRole : uint8_t { Iss, Irs };
  TurnRole turnRole_ = TurnRole::Iss;
  double roleSince_ = 0.0;
  uint8_t turnEpoch_ = 0;           // our grant generation
  int peerGrantEpoch_ = -1;         // last peer grant epoch accepted
  bool breakReqPending_ = false;    // peer's latest ACK asked for the turn
  double breakSentAt_ = -1.0;       // IRS: last standalone break request
  bool grantAwaitingData_ = false;  // grant sent, peer's first DATA awaited
  double grantRetryAt_ = 0.0;
  int grantTries_ = 0;
  // Handover drain: once a grant is due, stop pulling NEW segments and
  // let the in-flight window empty, so the turn changes hands with
  // nothing outstanding (PACTOR's changeover rule). -1 = not draining.
  double turnDrainSince_ = -1.0;
  bool turnDrainRetx_ = false;  // head-of-line fast-retransmit done this drain
  // Per-ISS-turn utilisation (SKW_TURN_DEBUG "TURNSTAT" at handover):
  // DATA frames, payload bytes and bursts sent while holding the turn.
  int turnFrames_ = 0;
  size_t turnBytes_ = 0;
  int turnBursts_ = 0;
  double turnAirSec_ = 0.0;   // real DATA airtime keyed this turn
  int turnContainerFrames_ = 0;  // frames those bursts actually carried
  void setTurnRole(TurnRole r, double now);
  void queueTurnGrant();
  void grantTurn(double now);
  double turnHandoffSec() const;  // grant/reclaim cadence (RTT-paced)
  bool turnDataWaiting() const;  // anything only the data role could send
  uint8_t ackRxFlags() const;    // mode request + break request bits
  void applySealCap();           // proto message bound (see lossyCoalesce_)
  // Responder-side pin: mode requested of the initiator, -1 = none,
  // kRxModeAuto = asking it to return to AUTO.
  int peerModeRequest_ = -1;
  // An "unpin" (Auto) request has no completion to observe: it rides a
  // few ACKs (loss margin) and then clears, or it unpinned every later
  // operator pin at the initiator for the rest of the session.
  int peerModeAutoSends_ = 0;

  std::vector<cfloat> lastConstellation_;
  uint32_t constellationSeq_ = 0;
  // Channel character, fed from every frame outcome and consumed by the
  // AMC's floor-table choice.
  ChannelClassifier chanClass_;
  // Configured family, restored when a session that adopted a narrower
  // one ends.
  Family configuredFamily_ = Family::Wide;
  void adoptSessionFamily(Family f, double now);
  // Channel memory: the seed is installed into the handshake policy just
  // before each handshake decision, and the session's observed quality is
  // folded back in when the session ends.
  std::optional<ChanMemKey> chanMemKey(const std::string& peer) const;
  void applyChanMemSeed(const std::string& peer, double now);
  void recordChanMem(double now);
  bool wasConnected_ = false;
  CallAckBody lastCallAck_{};     // initiator: stored for makeStart
  CallBody lastCall_{};           // responder: stored for evaluateStart
  std::deque<OutFrame> outQueue_; // control first, then data
  // Proto pipeline (4.1): coalesce/compress/segment above ARQ.
  ProtoTx protoTx_;
  ProtoRx protoRx_;
  double lastNow_ = 0.0;    // for sendData(), which takes no timestamp
  // Debug-trace rate limiter. Per INSTANCE: a function-static here is
  // shared by both engines in a two-node test world, so each station
  // suppressed the other's trace and the log showed roughly half the
  // samples from an arbitrary station.
  double pumpDbgLast_ = -1e9;
  // Which ccaDataGated() branch held, for SKW_PUMP_DEBUG: 0 clear,
  // 1 own-TX draining, 4 post-burst listen turn, 5 IRS (not the sending
  // role), 6 peer burst still airing, 7 awaiting the MODE_ACK.
  int ccaReason_ = 0;
  // Handshake round-trip measurement, used to SEED the ARQ RTT
  // estimator at connect. `hsTxAt_` is when we last emitted a
  // handshake frame that expects a reply; `hsRttSec_` is the measured
  // round trip, applied to arqTx_ the moment the session's ARQ state
  // is built (and re-applied on resume, which rebuilds it).
  double hsTxAt_ = -1.0;
  bool hsTxAmbiguous_ = false;  // the timed frame was sent more than once
  double hsRttSec_ = 0.0;
  void noteHandshakeRtt(double now);
  // In-flight count when the post-burst listen turn began; the turn
  // ends early as soon as the count drops (feedback arrived). 0 = no
  // turn in progress.
  size_t yieldInFlight_ = 0;
  // Post-burst listen turn: when it began, and when the peer's feedback
  // last arrived. The turn's whole purpose is to not key over incoming
  // feedback, so it ends the moment that feedback lands - EVEN when the
  // feedback is a NAK that reports a hole without advancing the window
  // (yieldInFlight_ alone missed that: a persistent head-of-line hole
  // never drops the in-flight count, so the sender waited the full RTT
  // plus the ARQ timeout between each retransmission of the one stuck
  // frame - measured as the floor scenario's one-way 0-byte stall).
  double listenTurnStart_ = -1.0;
  double lastFeedbackAt_ = -1.0;
  // Explicit NAK pending: a peer DATA burst failed its final decode
  // attempt; the next ACK carries kRxFlagRxFail (PACTOR's CS-NAK).
  bool rxFailPending_ = false;
  double lastRxAirtimeSec_ = 0.0;  // see lastRxBurstAirtimeSec()
  bool rxPoll_ = false;  // PHY poll bit of the burst being dispatched
  // Last dispatched burst, for the overlapping-capture de-duplication.
  uint64_t lastRxDigest_ = 0;
  double lastRxFrameAtSec_ = -1e9;  // absolute sync point of that burst
  // Last time a burst's PHY header decoded in session (payload or not):
  // the peer was keyed until about then.
  double lastPeerHeardAt_ = -1.0;
  // Adaptive proto seal cap: unlimited coalescing while the link is
  // clean (a flat cap measurably cost the VHF/ramp throughput gates),
  // bounded to ~8 frames once the retry ratio says it is lossy so a
  // stuck head-of-line fragment cannot zero delivery of a 4 KiB message
  // (measured: floor/churn delivering 0 app bytes while fragments kept
  // arriving). Hysteresis so it does not flap on a single retry.
  bool lossyCoalesce_ = false;
  bool listenFeedbackArrived() const {
    // Guarded by yieldInFlight_ so it fires ONLY during a real
    // post-burst listen turn.
    return yieldInFlight_ > 0 && listenTurnStart_ >= 0.0 &&
           lastFeedbackAt_ >= listenTurnStart_;
  }
  size_t dataBudget_ = 57;  // exact-fill segment size at mode_/profile
  std::vector<std::string> events_;
  // Session resume (4.4): token minted at START_ACK, bound to the
  // family + callsign pair; a failure-mode drop pauses the pipeline and
  // a reconnect within the window continues the sequence spaces
  // (PROTOCOL.md "Session Resume").
  struct ResumeCtx {
    bool valid = false;
    std::array<uint8_t, 8> token{};
    std::string remoteCall;
    Family family = Family::Wide;
    uint16_t caps = 0;
    int mode = 4;
    int maxMode = 13;      // the session's cap continues with it
    CpMode cp = kCpDefault;
    double freshAt = 0.0;  // window runs from mint, refreshed at drop
    uint16_t sessionId = 0;  // a resumed session keeps its id
  };
  bool resumeUsable(double now) const;
  // Envelope session id for a fresh session (never 0: 0 marks
  // pre-session frames). Chosen by the initiator, adopted from START.
  uint16_t newSessionId(double now);
  std::array<uint8_t, 8> mintToken(double now);
  // Deterministic hash over time, both callsigns and a counter: the
  // entropy behind session ids and resume tokens.
  uint64_t sessionHash(double now, uint64_t salt);
  void discardPausedPipeline();  // clean start: paused state is dead
  ResumeCtx resumeCtx_;
  bool pausedForResume_ = false;  // pipeline held across a broken link
  bool sentResumeReq_ = false;    // the last START slot carried RESUME_REQ
  // Any RESUME_REQ went out during this connect attempt: only then is a
  // START_NAK(ResumeInvalid) an answer to us (sentResumeReq_ alone is
  // cleared by a START retry that went out clean).
  bool resumeAttempted_ = false;
  bool resuming_ = false;         // accept path: continue, don't reset
  bool resumed_ = false;
  bool sendResumeAckInstead_ = false;
  uint16_t resumePeerNext_ = 0;   // peer's next-expected ARQ sequence
  uint64_t tokenCtr_ = 0;
  // Fault-mode teardown (ARQ exhaustion, peer's for-cause DISC, idle
  // timeout): keeps the resume token alive; operator DISC does not.
  bool faultDisc_ = false;
#ifdef SKW_HAVE_VOICE
  // Voice service. VOICE_DATA rides its own sequence space and is never
  // ARQ-tracked (late voice is worthless); a talkspurt streams units
  // back-to-back so the preamble cost is paid once.
  void drainVoiceActions(double now);
  void pumpVoice(double now);
  // Queue every encoded unit available (`flush`: also the partial tail).
  void emitVoiceUnits(bool flush);
  VoiceFsm voiceFsm_;
  VoiceEncoder voiceEnc_{VoiceCodec::C700};
  VoiceDecoder voiceDec_;
  JitterBuffer jitter_;
  bool voiceCodecAuto_ = true;
  uint16_t voiceTxSeq_ = 0;
  // Broadcast voice: TX side (open/talking, mode, own sequence) and RX
  // side (the sender tag locked onto and when it was last heard).
  VoicePlan voicePlan_;      // session voice
  VoicePlan bvPlan_;         // broadcast voice
  bool bvOpen_ = false;
  bool bvTalking_ = false;
  int bvMode_ = 8;
  uint16_t bvTxSeq_ = 0;
  bool bvRxActive_ = false;
  uint16_t bvRxTag_ = 0;
  uint16_t bvRxSeq_ = 0;
  double bvRxLastAt_ = 0.0;
  static constexpr double kBcastVoiceIdleSec = 3.0;
  void rxBroadcastVoice(const ParsedFrame& f, double now);
#endif
  void applySessionTimers();
#ifdef SKW_HAVE_VOICE
  uint16_t voiceRxSeq_ = 0;
  bool haveVoiceRxSeq_ = false;
#endif
#ifdef SKW_HAVE_CRYPTO
  // Session crypto. cryptoOn_ = the committed session is encrypted;
  // sealing applies to DATA payload bodies (handshake and broadcast
  // frames are never encrypted - SECURITY.md).
  CryptoSession crypto_;
  bool cryptoOn_ = false;
  // 48-bit extension high-water PER STREAM (0 = ARQ data, 1 = voice): the
  // two counters are independent, and one shared mark made the receiver
  // extend to a different seq48 than the sender once they drifted apart.
  std::array<uint64_t, 2> txSeqHi_{}, rxSeqHi_{};
  // Key rotation is a handshake: KEY_UPDATE is echoed by the responder
  // and the initiator switches only on the echo (retried until then).
  uint16_t pendingEpoch_ = 0;
  double keyUpdateSentAt_ = -1.0;
  double epochRotateAt_ = 0.0;
  uint64_t sealedAtRotate_ = 0;  // framesSealed() at the last rotation
  // A retransmission must reuse the ORIGINAL ciphertext: re-sealing the
  // same seq48 (same nonce) over a changed plaintext/AAD (rv, ackBase)
  // reuses the GCM nonce. Keyed by ARQ seq; pruned as the window moves.
  struct SealedData {
    uint16_t epoch = 0;
    uint16_t ackBase = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> body;
  };
  std::map<uint16_t, SealedData> sealedData_;
  KexContext kexCtx() const;
  std::vector<uint8_t> pendingKexResponse_;  // responder: for START_ACK
  // Initiator: the KEX blob of this connect attempt's START. Every START
  // retry carries the SAME blob - the responder re-acks a duplicate START
  // with its stored START_ACK, which answers the first ephemeral key, so
  // a fresh key per retry made any retried encrypted START fatal.
  std::vector<uint8_t> startKexBlob_;
  void sendKeyUpdate(double now);
  // DATA payload AEAD. AAD = the envelope's first 8 bytes.
  static std::vector<uint8_t> envelopeAad(const LinkEnvelope& env);
  bool sealPayload(const LinkEnvelope& env, std::vector<uint8_t>& body);
  bool openPayload(const LinkEnvelope& env, std::vector<uint8_t>& body);
#endif
  // CW ID (4.5): queued after teardown and every 10 min in-session;
  // emitted only when nothing else wants the transmitter.
  bool cwPending_ = false;
  double lastCwAt_ = 0.0;
  // Observability counters (4.6).
  uint64_t framesOk_ = 0;
  uint64_t framesFailed_ = 0;
  double remoteSnrDb_ = -99.0;
};

}  // namespace skw
