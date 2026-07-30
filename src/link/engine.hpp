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
#include <optional>
#include <string>
#include <vector>

#include "link/advisor.hpp"
#include "link/amc.hpp"
#include "link/arq.hpp"
#include "link/mask.hpp"
#include "link/probe.hpp"
#include "link/frame.hpp"
#include "link/fsm.hpp"
#include "link/handshake.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "proto/filesvc.hpp"
#include "proto/pipeline.hpp"
#ifdef SKW_HAVE_CRYPTO
#include "crypto/session.hpp"
#endif
#ifdef SKW_HAVE_VOICE
#include "voice/codec.hpp"
#include "voice/session.hpp"
#endif

namespace skw {

struct EngineConfig {
  HandshakePolicy policy;
  int arqMaxRetries = 8;
  bool maskingEnabled = true;  // negotiated via kCapMasking as well
  bool burstAggregation = true;  // MAC burst container (M7+ profiles)
  bool cpAuto = true;            // delay-spread-driven CPMODE
  bool cwId = false;             // CW ID after disconnect / 10-min
  int cwWpm = 20;
  // Crypto (SECURITY.md, crypto builds only): request encryption for
  // outgoing/accepted sessions. A requested-but-unachievable session is
  // refused - the link NEVER silently falls back to cleartext.
  bool encryptionRequested = false;
  std::vector<uint8_t> psk;      // 32 bytes when installed
};

class LinkEngine {
 public:
  explicit LinkEngine(EngineConfig cfg);

  // --- application API -------------------------------------------------
  void connectTo(const std::string& call, double now);
  void disconnect(double now);
  void sendData(std::span<const uint8_t> bytes);  // buffered until sent
  std::vector<uint8_t> takeReceivedData();

  // --- file transfer (4.3) ---------------------------------------------
  // One transfer per direction at a time; false if one is active.
  bool sendFile(const std::string& name, std::vector<uint8_t> data,
                uint64_t mtime);
  bool fileTxActive() const { return fileTx_.active(); }
  std::optional<ReceivedFile> takeReceivedFile() {
    return fileRx_.takeCompleted();
  }
  uint64_t fileRxCheckpoint() const { return fileRx_.checkpointOffset(); }
  // Control-port notification lines ("EVT:..."), drained by the caller.
  std::vector<std::string> takeEvents();

  // --- session resume (4.4) --------------------------------------------
  // True while the current connection was established via RESUME.
  bool sessionResumed() const { return resumed_; }
  bool hasResumeContext() const { return resumeCtx_.valid; }

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
  bool openVoice(std::optional<VoiceCodec> want, double now);
  void closeVoice(double now);
  bool setVoicePtt(bool on, double now);
  VoiceState voiceState() const { return voiceFsm_.state(); }
  VoiceCodec voiceCodec() const { return voiceFsm_.codec(); }
  bool voiceCodecAuto() const { return voiceCodecAuto_; }
  // Microphone PCM in (8 kHz mono), speaker PCM out (jitter-buffered).
  void pushVoicePcm(std::span<const int16_t> pcm);
  std::vector<int16_t> takeVoicePcm(size_t samples);
  size_t voiceTxQueued() const;
  int voiceRxDepthMs() const { return jitter_.depthMs(); }
  uint32_t voiceConcealed() const { return voiceDec_.concealedFrames(); }
#endif

  // --- broadcast (4.5) ---------------------------------------------------
  // Connectionless frames: calling signature + robust header + M4.
  // Usable while idle (and alongside a session; the arbiter interleaves).
  void sendCq();
  void sendBeacon(double snrFloorDb);
  // Fragments into <= 16 BCAST_DATA frames (4-bit index/count); false
  // if the payload exceeds 16 * 256 bytes or is empty.
  bool sendBroadcast(std::span<const uint8_t> bytes);

  FsmState state() const { return fsm_.state(); }
  FsmDiscCause lastDiscCause() const { return fsm_.lastDiscCause(); }
  int sessionMode() const { return mode_; }
  size_t txBacklog() const { return protoTx_.backlogBytes(); }
  bool idleClean() const;  // nothing pending anywhere

  // --- mode control (3.2) ---------------------------------------------
  // Manual pin/unpin (MODE Mx / MODE AUTO). Pinning coordinates a
  // MODE_CHANGE to the pinned mode; fast-demote safety stays active.
  bool pinMode(int mode, double now);
  void unpinMode();
  bool modePinned() const;
  double txShiftHz() const { return txShiftHz_; }
  CpMode cpMode() const { return cpMode_; }
  double lastRxCfoHz() const { return lastRxQuality_.cfoHz; }

  // --- observability / control surface (4.6) ---------------------------
  void abortSession(double now);       // immediate, no DISC exchange
  bool setCpPolicy(const std::string& p, double now);  // AUTO/SHORT/...
  std::string cpPolicy() const;
  const std::string& remoteCall() const { return remoteCall_; }
  uint16_t sessionCaps() const { return sessionCaps_; }
  double lastRxSnrDb() const { return lastRxQuality_.snrDb; }
  double remoteSnrDb() const;   // from ACK reports; -99 when unknown
  double retryRatio() const;
  uint64_t framesOk() const { return framesOk_; }
  uint64_t framesFailed() const { return framesFailed_; }
  double netBps() const {
    return netRate(cfg_.policy.family, mode_, cpMode_);
  }

  // --- scheduling ------------------------------------------------------
  // Advance timers and, if a burst is due, return its baseband samples.
  std::optional<std::vector<cfloat>> tick(double now);
  // Feed a received capture (burst + padding); engine decodes and
  // reacts. Returns true when a frame decoded payload-clean (the
  // worker's windowed retry stops there).
  bool rxBurst(std::span<const cfloat> samples, double now);

 private:
  struct OutFrame {
    LinkEnvelope env;
    std::vector<uint8_t> body;
    ChirpSig sig;
    int mode;
  };
  void queueControl(LinkType t, std::vector<uint8_t> body, ChirpSig sig,
                    int mode);
  void drainFsmActions(double now);
  void pumpData(double now);
  std::vector<cfloat> buildBurst(const OutFrame& f);
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
  // and MODE_CHANGE; the responder's channel view reaches it via ACK SNR
  // reports. Two independent controllers would collide mid-air (observed:
  // mode oscillation and pin override). Responder-side manual pinning is
  // a v1 limitation, noted in ROADMAP.
  bool isInitiator_ = false;
  std::string remoteCall_;
  int mode_ = 4;                  // committed session mode
  CpMode cpMode_ = activeProfile().cpDefault;  // committed session CP
  // TX frequency alignment (PROTOCOL.md): split correction -reported/2,
  // slewed <= 2 Hz per adjustment, clamped +-30 Hz per side.
  double txShiftHz_ = 0.0;
  void onCfoReport(double reportedHz);
  double spreadEmaMs_ = 0.0;      // delay-spread EMA for CPMODE AUTO
  bool haveSpread_ = false;
  int cpWantStreak_ = 0;          // consecutive frames agreeing on a CP
  CpMode cpWant_ = kCpDefault;
  double lastCpChange_ = 0.0;
  uint16_t sessionCaps_ = 0;      // committed capability intersection
  bool harqIr() const { return (sessionCaps_ & kCapHarqIr) != 0; }
  // Handshake-on-CSS (6.2): the initiator alternates CALL attempts onto
  // CSS M0 when enabled; the responder mirrors the transport the CALL
  // arrived on. All handshake control follows hsSig()/hsMode().
  bool hsOnCss_ = false;
  int callTxCount_ = 0;
  bool lastRxWasCss_ = false;
  std::vector<uint8_t> lastStartAckBody_;  // duplicate-START re-ack
  double deepIdleSec_ = 120.0;  // idle window for CSS-mode sessions
  int dataSinceAck_ = 0;        // ACK batching (M13+)
  double lastDataRxAt_ = 0.0;
  double ackFlushSec_ = 0.5;
  ChirpSig hsSig() const {
    return hsOnCss_ ? ChirpSig::Css : ChirpSig::Calling;
  }
  int hsMode() const { return hsOnCss_ ? 0 : 4; }
  // In-flight MODE_CHANGE we initiated (awaiting MODE_ACK).
  int pendingMode_ = -1;
  CpMode pendingCp_ = kCpDefault;
  uint8_t modeEpoch_ = 0;
  double modeChangeDeadline_ = 0.0;
  int modeChangeTries_ = 0;
  uint16_t sessionId_ = 0;
  bool ackPending_ = false;       // send an ACK on the next tick
  QualityMeasurement lastRxQuality_{};
  CallAckBody lastCallAck_{};     // initiator: stored for makeStart
  CallBody lastCall_{};           // responder: stored for evaluateStart
  std::deque<OutFrame> outQueue_; // control first, then data
  // Proto pipeline (4.1): coalesce/compress/segment above ARQ.
  ProtoTx protoTx_;
  ProtoRx protoRx_;
  double lastNow_ = 0.0;    // for sendData(), which takes no timestamp
  size_t dataBudget_ = 57;  // exact-fill segment size at mode_/profile
  // File service (4.3).
  void pumpFile();
  void drainFileRx();
  FileTx fileTx_;
  FileRx fileRx_;
  uint32_t nextFileId_ = 1;
  std::vector<std::string> events_;
  // Session resume (4.4): token minted at START_ACK, bound to the
  // family + callsign pair; a failure-mode drop pauses the pipeline and
  // a reconnect within the window continues sequence spaces + file
  // checkpoints (PROTOCOL.md "Session Resume").
  struct ResumeCtx {
    bool valid = false;
    std::array<uint8_t, 8> token{};
    std::string remoteCall;
    Family family = Family::Wide;
    uint16_t caps = 0;
    int mode = 4;
    CpMode cp = kCpDefault;
    double freshAt = 0.0;  // window runs from mint, refreshed at drop
  };
  bool resumeUsable(double now) const;
  std::array<uint8_t, 8> mintToken(double now);
  void discardPausedPipeline();  // clean start: paused state is dead
  ResumeCtx resumeCtx_;
  bool pausedForResume_ = false;  // pipeline held across a broken link
  bool sentResumeReq_ = false;    // this handshake attempted RESUME
  bool resuming_ = false;         // accept path: continue, don't reset
  bool resumed_ = false;
  bool sendResumeAckInstead_ = false;
  uint16_t resumePeerNext_ = 0;   // peer's next-expected ARQ sequence
  bool resumePeerHasCkpt_ = false;
  uint32_t resumePeerCkptId_ = 0;
  uint64_t resumePeerCkptOff_ = 0;
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
  VoiceFsm voiceFsm_;
  VoiceEncoder voiceEnc_{VoiceCodec::C700};
  VoiceDecoder voiceDec_;
  JitterBuffer jitter_;
  bool voiceCodecAuto_ = true;
  uint16_t voiceTxSeq_ = 0;
  uint16_t voiceRxSeq_ = 0;
  bool haveVoiceRxSeq_ = false;
#endif
#ifdef SKW_HAVE_CRYPTO
  // Session crypto. cryptoOn_ = the committed session is encrypted;
  // sealing applies to DATA payload bodies (handshake and broadcast
  // frames are never encrypted - SECURITY.md).
  CryptoSession crypto_;
  bool cryptoOn_ = false;
  uint64_t txSeqHi_ = 0, rxSeqHi_ = 0;  // 48-bit extension high-water
  uint16_t pendingEpoch_ = 0;
  double epochRotateAt_ = 0.0;
  KexContext kexCtx() const;
  std::vector<uint8_t> pendingKexResponse_;  // responder: for START_ACK
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
