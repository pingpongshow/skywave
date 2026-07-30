#include "link/engine.hpp"

#include <algorithm>

#include "dsp/morse.hpp"

namespace skw {

namespace {

FsmConfig fsmConfigFor(const HandshakePolicy& p) {
  // Airtime-derived handshake timings from the real PHY (PROTOCOL.md).
  std::vector<uint8_t> callSized(48), startSized(24), discSized(16);
  auto air = [&](std::span<const uint8_t> body) {
    TxFrameParams tp;
    tp.family = p.family;
    tp.sig = ChirpSig::Calling;
    tp.mode = 4;
    tp.payload = body;
    return frameAirtimeSec(tp);
  };
  FsmConfig c;
  c.timings.callSec = air(callSized);
  c.timings.callAckSec = air(callSized);
  c.timings.startSec = air(startSized);
  c.timings.startAckSec = air(startSized);
  c.timings.discSec = air(discSized);
  return c;
}

int macWindow(int mode) {
  if (mode <= 6) return 2;
  if (mode <= 9) return 4;
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
  if (mode <= 6) return 1;
  if (mode <= 9) return 2;
  return 4;
}

// Resume window (PROTOCOL.md "Session Resume").
constexpr double kResumeWindowSec = 600.0;

}  // namespace

LinkEngine::LinkEngine(EngineConfig cfg)
    : cfg_(std::move(cfg)),
      fsm_(fsmConfigFor(cfg_.policy)),
      arqTx_(2, cfg_.arqMaxRetries),
      arqRx_(8) {}

bool LinkEngine::idleClean() const {
  return outQueue_.empty() && protoTx_.idle() && arqTx_.inFlight() == 0 &&
         !ackPending_;
}

void LinkEngine::connectTo(const std::string& call, double now) {
  remoteCall_ = call;
  isInitiator_ = true;
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
  return protoRx_.takeBytes();
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
    const ChirpSig sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
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
    f.sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
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
               mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
               std::min(mode_, 8));
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
    queueControl(LinkType::BcastData, b.pack(), ChirpSig::Calling, 4);
  }
  return true;
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
                              ChirpSig sig, int mode) {
  OutFrame f;
  f.env.type = t;
  f.env.sessionId = sessionId_;
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
      case FsmAction::SendCall:
        queueControl(LinkType::Call,
                     makeCall(cfg_.policy, remoteCall_).pack(),
                     ChirpSig::Calling, 4);
        break;
      case FsmAction::SendCallAck: {
        auto ack = respondToCall(cfg_.policy, lastCall_, lastRxQuality_);
        if (ack) {
          queueControl(LinkType::CallAck, ack->pack(), ChirpSig::Calling, 4);
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
          queueControl(LinkType::ResumeReq, rr.pack(), ChirpSig::Calling,
                       4);
          break;
        }
        sentResumeReq_ = false;
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
        queueControl(LinkType::Start, s.pack(), ChirpSig::Calling, 4);
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
          queueControl(LinkType::ResumeAck, ra.pack(), ChirpSig::Calling,
                       4);
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
        queueControl(LinkType::StartAck, sa.pack(), ChirpSig::Calling, 4);
        break;
      }
      case FsmAction::SendStartNak:
        queueControl(LinkType::StartNak,
                     StartNakBody{StartNakReason::Busy}.pack(),
                     ChirpSig::Calling, 4);
        break;
      case FsmAction::SendDisc:
        queueControl(LinkType::Disc,
                     DiscBody{faultDisc_ ? DiscReason::RetryExhausted
                                         : DiscReason::Normal}
                         .pack(),
                     ChirpSig::Robust, 4);
        break;
      case FsmAction::SendDiscAck:
        queueControl(LinkType::DiscAck, {}, ChirpSig::Robust, 4);
        break;
      case FsmAction::SendKeepalive:
        queueControl(LinkType::Keepalive, KeepaliveBody{20}.pack(),
                     ChirpSig::Fast, std::max(mode_, 8));
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
        AmcConfig ac;
        ac.family = cfg_.policy.family;
        ac.maxMode = cfg_.policy.maxMode;
        amc_.emplace(ac, uint8_t(mode_), 0.0);
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
          if (pausedForResume_) discardPausedPipeline();
          resumed_ = false;
          const int w = macWindow(mode_);
          arqTx_ = ArqTx(w, cfg_.arqMaxRetries);
          arqRx_ = ArqRx(w);
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
  if (fsm_.state() != FsmState::Connected) return;
  // Retransmits first (arbiter priority), then new segments.
  if (auto r = arqTx_.nextRetransmit(now)) {
    // A copy of this sequence may already be queued (aggressive timeout
    // requeues while the TX queue is still draining): let the queued
    // copy fly instead of stacking duplicates into the queue.
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
    f.sig = f.mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
    outQueue_.push_back(std::move(f));
    return;
  }
  for (int k = 0; k < framesPerBurst(mode_); ++k) {
    if (!arqTx_.canSend()) return;
    auto seg = protoTx_.nextSegment(dataBudget_, now);
    if (!seg) return;
    std::vector<uint8_t> stored;
    stored.reserve(1 + seg->bytes.size());
    stored.push_back(seg->flags);
    stored.insert(stored.end(), seg->bytes.begin(), seg->bytes.end());
    auto seq = arqTx_.send(std::move(stored), now);
    if (!seq) return;
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
    f.sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
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
  tp.frameType = uint8_t(f.env.type);
  tp.seqLow = uint8_t(f.env.sequence & 0xFF);
  tp.cpMode = cpMode_;
  tp.payload = raw;
  // Spread modes (M4-M7) skip masking: spreading already averages over
  // interference (LINK_ADAPTATION.md).
  if (f.mode >= 8 && txMask_.maskedCount(cfg_.policy.family) > 0) {
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
  const int fpb = cfg_.burstAggregation ? framesPerBurst(mode_) : 1;
  dataBudget_ = dataPayloadBudget(mode_, fpb, (mode_ <= 7 ? 1 : 2) * fpb);
  std::vector<uint8_t> seg(
      static_cast<size_t>(fpb) * (dataBudget_ + (fpb == 1 ? 13 : 15)));
  TxFrameParams tp;
  tp.family = cfg_.policy.family;
  tp.sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
  tp.mode = mode_;
  tp.cpMode = cpMode_;
  tp.payload = seg;
  std::vector<uint8_t> ackSized(kAckBodyBytes + kFrameOverhead);
  TxFrameParams ta = tp;
  ta.payload = ackSized;
  ta.mode = mode_ <= 6 ? 4 : mode_;
  arqTx_.setTimeout(frameAirtimeSec(tp) + frameAirtimeSec(ta) + 3.0);
}

void LinkEngine::applyMode(int mode, CpMode cp, double now) {
  mode_ = mode;
  cpMode_ = cp;
  updateArqTimeout();
  if (amc_) amc_->onModeApplied(uint8_t(mode), now);
}

void LinkEngine::onCfoReport(double reportedHz) {
  const double target = txShiftHz_ - reportedHz / 2.0;
  const double step = std::clamp(target - txShiftHz_, -2.0, 2.0);
  txShiftHz_ = std::clamp(txShiftHz_ + step, -30.0, 30.0);
}

void LinkEngine::requestModeChange(int target, CpMode cp, double now) {
  if (pendingMode_ >= 0 || (target == mode_ && cp == cpMode_)) return;
  pendingMode_ = target;
  pendingCp_ = cp;
  ++modeEpoch_;
  modeChangeTries_ = 0;
  // Demotions transmit at the TARGET (lower) mode so a degraded peer can
  // still decode them; promotions ride the current committed mode.
  const int txMode = std::min(target, mode_);
  queueControl(LinkType::ModeChange,
               ModeChangeBody{uint8_t(target), uint8_t(cp), modeEpoch_}
                   .pack(),
               txMode <= 7 ? ChirpSig::Robust : ChirpSig::Fast, txMode);
  modeChangeDeadline_ = now + arqTx_.timeoutSec();
}

bool LinkEngine::pinMode(int mode, double now) {
  if (fsm_.state() != FsmState::Connected || !amc_ || !isInitiator_) {
    return false;
  }
  if (mode < kFirstMode || mode >= kNumModes) return false;
  amc_->pin(uint8_t(mode), now);
  requestModeChange(mode, cpMode_, now);
  return true;
}

void LinkEngine::unpinMode() {
  if (amc_) amc_->unpin();
}

bool LinkEngine::modePinned() const { return amc_ && amc_->pinned(); }

std::optional<std::vector<cfloat>> LinkEngine::tick(double now) {
  lastNow_ = now;
  fsm_.tick(now);
  drainFsmActions(now);
  arqTx_.tick(now);
  if (arqTx_.failed() && fsm_.state() == FsmState::Connected) {
    faultDisc_ = true;  // dead link, not operator intent: resumable
    fsm_.cmdDisconnect(now);
    drainFsmActions(now);
  }
  // Mode-change retry / expiry, then AMC evaluation.
  if (fsm_.state() == FsmState::Connected) {
    if (pendingMode_ >= 0 && now >= modeChangeDeadline_) {
      if (++modeChangeTries_ >= 3) {
        pendingMode_ = -1;  // give up; stay at the committed mode
      } else {
        const int txMode = std::min(pendingMode_, mode_);
        queueControl(LinkType::ModeChange,
                     ModeChangeBody{uint8_t(pendingMode_),
                                    uint8_t(pendingCp_), modeEpoch_}
                         .pack(),
                     txMode <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                     txMode);
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
    // Carrier-mask exchange (per direction; either side may originate).
    if (maskDet_ && cfg_.maskingEnabled && (sessionCaps_ & kCapMasking) &&
        mode_ >= 8) {
      if (pendingMaskEpoch_ >= 0 && now >= maskRetryAt_) {
        if (++maskTries_ >= 4) {
          pendingMaskEpoch_ = -1;  // abandon this mask generation
        } else {
          MaskUpdateBody mu;
          mu.epoch = uint8_t(pendingMaskEpoch_);
          mu.family = uint8_t(cfg_.policy.family);
          mu.mask = pendingRxMask_.bits;
          queueControl(LinkType::MaskUpdate, mu.pack(),
                       mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                       std::min(mode_, 8));
          // Airtime-derived like every other timer: a fixed short
          // interval abandoned the exchange before the MASK_ACK could
          // even ride back behind a data container.
          maskRetryAt_ = now + arqTx_.timeoutSec();
        }
      }
      if (pendingMaskEpoch_ < 0) {
        CarrierMask want = maskDet_->proposal(now);
        if (!(want == rxMask_) &&
            want.maskedCount(cfg_.policy.family) > 0) {
          pendingRxMask_ = want;
          pendingMaskEpoch_ = (maskEpoch_ + 1) & 0x0F;
          maskEpoch_ = pendingMaskEpoch_;
          maskTries_ = 0;
          MaskUpdateBody mu;
          mu.epoch = uint8_t(pendingMaskEpoch_);
          mu.family = uint8_t(cfg_.policy.family);
          mu.mask = pendingRxMask_.bits;
          queueControl(LinkType::MaskUpdate, mu.pack(),
                       mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                       std::min(mode_, 8));
          // Airtime-derived like every other timer: a fixed short
          // interval abandoned the exchange before the MASK_ACK could
          // even ride back behind a data container.
          maskRetryAt_ = now + arqTx_.timeoutSec();
        }
      }
    }
    if (amc_ && isInitiator_ && pendingMode_ < 0 && cfg_.cpAuto &&
        haveSpread_ && cpWant_ != cpMode_ && cpWantStreak_ >= 25 &&
        now - lastCpChange_ > 30.0) {
      // CP correction outranks ladder moves: a CP too short for the
      // channel is a safety problem, and AMC churn (probe retries)
      // must not starve it.
      lastCpChange_ = now;
      requestModeChange(mode_, cpWant_, now);
    } else if (amc_ && isInitiator_ && pendingMode_ < 0) {
      auto d = amc_->evaluate(now);
      static const bool amcDbg = getenv("SKW_AMC_DEBUG") != nullptr;
      if (amcDbg) {
        fprintf(stderr,
                "amc t=%.0f mode=%d eff=%.1f retry=%.2f decision=%d->%d\n",
                now, mode_, amc_->effectiveSnrDb(now), amc_->retryRatio(),
                int(d.kind), int(d.targetMode));
      }
      if (d.kind == AmcDecision::Kind::Promote &&
          probeGate_.required(d.targetMode) &&
          !probeGate_.cleared(d.targetMode)) {
        // Fragile mode: verify live before committing. The PROBE rides
        // the CANDIDATE mode; its probeId also travels in the PHY header
        // seqLow so the peer can report a payload decode failure.
        if (auto req = probeGate_.begin(d.targetMode, now)) {
          for (int i = 0; i < req->count; ++i) {
            OutFrame f;
            f.env.type = LinkType::Probe;
            f.env.sessionId = sessionId_;
            f.env.sequence = req->probeId;
            f.env.ackBase = arqRx_.ackBase();
            f.body = ProbeBody{req->probeId, uint8_t(req->mode)}.pack();
            f.sig = ChirpSig::Fast;
            f.mode = req->mode;
            outQueue_.push_back(std::move(f));
          }
        }
      } else if (d.kind != AmcDecision::Kind::None) {
        if (d.kind == AmcDecision::Kind::Promote &&
            probeGate_.required(d.targetMode)) {
          probeGate_.consumeClearance();
        }
        requestModeChange(d.targetMode,
                          cfg_.cpAuto && cpWantStreak_ >= 25 ? cpWant_
                                                             : cpMode_,
                          now);
      }
    }
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
          int8_t(std::clamp(lastRxQuality_.snrDb * 2.0, -127.0, 127.0)), 0,
          int8_t(std::clamp(lastRxQuality_.cfoHz / 0.5, -127.0, 127.0)));
      OutFrame f;
      f.env.type = LinkType::Ack;
      f.env.sessionId = sessionId_;
      f.env.sequence = 0;
      f.env.ackBase = arqRx_.ackBase();
      f.body = ack.pack();
      // Feedback at least as robust as forward traffic (M4 for M4-M6).
      f.sig = mode_ <= 6 ? ChirpSig::Robust : ChirpSig::Fast;
      f.mode = mode_ <= 6 ? 4 : mode_;
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
  OutFrame f = std::move(outQueue_.front());
  outQueue_.pop_front();
  if (f.env.type == LinkType::Ack && fsm_.state() == FsmState::Connected) {
    // Refresh ACK feedback at transmit time - see the ackPending_ note.
    f.env.ackBase = arqRx_.ackBase();
    f.body = arqRx_.makeAck(
                 int8_t(std::clamp(lastRxQuality_.snrDb * 2.0, -127.0,
                                   127.0)),
                 0,
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
      group.push_back(std::move(outQueue_.front()));
      outQueue_.pop_front();
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
      if (group[0].mode >= 8 &&
          txMask_.maskedCount(cfg_.policy.family) > 0) {
        tp.mask = &txMask_;
      }
      auto burst = buildFrameBaseband(tp);
      applyTxShift(burst);
      return burst;
    }
    f = std::move(group[0]);
  }
  return buildBurst(f);
}

bool LinkEngine::rxBurst(std::span<const cfloat> samples, double now) {
  lastNow_ = now;
  RxStage stage;
  auto res =
      receiveFrame(cfg_.policy.family, samples, &stage, &harq_, &rxMask_);
  if (res && maskDet_ && fsm_.state() == FsmState::Connected &&
      mode_ >= 8 && !res->carrierSnrDb.empty()) {
    maskDet_->onSnapshot(res->carrierSnrDb, now);
  }
  if (res && fsm_.state() == FsmState::Connected) {
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
    cpWantStreak_ = want == cpWant_ ? cpWantStreak_ + 1 : 1;
    cpWant_ = want;
  }
  if (res) {
    if (res->payloadOk) {
      ++framesOk_;
    } else {
      ++framesFailed_;
    }
  }
  if (!res || !res->payloadOk) {
    if (amc_ && stage == RxStage::Ok) amc_->onFrameResult(false, false);
    // A PROBE whose header decoded but payload failed still gets its
    // verdict: the probeId rides the PHY header's seqLow.
    if (res && !res->payloadOk &&
        res->header.frameType == uint8_t(LinkType::Probe) &&
        fsm_.state() == FsmState::Connected) {
      queueControl(LinkType::ProbeAck,
                   ProbeAckBody{res->header.seqLow, 0, kProbeFailed}.pack(),
                   mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
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
  onFrame(*parsed, q, now);
  return true;
}

void LinkEngine::onFrame(const ParsedFrame& f, const QualityMeasurement& q,
                         double now) {
  lastRxQuality_ = q;
  fsm_.rxTraffic(now);
  probeGate_.onFrameObserved();
  if (amc_ && fsm_.state() == FsmState::Connected) {
    amc_->onLocalSnr(q.snrDb);
  }
  switch (f.env.type) {
    case LinkType::Call: {
      auto call = CallBody::parse(f.body);
      if (!call || call->dstCall != cfg_.policy.ownCall) return;
      lastCall_ = *call;
      remoteCall_ = call->srcCall;
      isInitiator_ = false;
      fsm_.rxCall(now);
      break;
    }
    case LinkType::CallAck: {
      auto ack = CallAckBody::parse(f.body);
      if (!ack) return;
      lastCallAck_ = *ack;
      onCfoReport(ack->cfoHalfHz * 0.5);
      fsm_.rxCallAck(now);
      break;
    }
    case LinkType::Start: {
      auto start = StartBody::parse(f.body);
      if (!start) return;
      auto r = evaluateStart(cfg_.policy, lastCall_, *start, q);
      bool ok = std::holds_alternative<StartAckBody>(r);
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
      if (nak && nak->reason == StartNakReason::ResumeInvalid &&
          sentResumeReq_) {
        // Peer rejected the token: fall back to a clean START in place
        // (never a session failure). The paused pipeline is dead - the
        // peer kept nothing to continue from.
        sentResumeReq_ = false;
        resumeCtx_.valid = false;
        discardPausedPipeline();
        StartBody s = makeStart(cfg_.policy, lastCallAck_, lastRxQuality_);
        sessionCaps_ = s.caps;
        protoTx_.setZstd((sessionCaps_ & kCapZstd) != 0);
        queueControl(LinkType::Start, s.pack(), ChirpSig::Calling, 4);
        return;  // FSM stays in WaitStartAck for the real START_ACK
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
                     ChirpSig::Calling, 4);
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
      mode_ = resumeCtx_.mode;
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
      mode_ = resumeCtx_.mode;
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
      if (amc_) amc_->onFrameResult(d->rv > 0, true);
      // Stash the proto flags with the payload so they survive the
      // out-of-order window and reach the proto layer in order.
      std::vector<uint8_t> stored;
      stored.reserve(1 + d->payload.size());
      stored.push_back(f.env.flags &
                       (kFlagCompressed | kFlagMoreFragments));
      stored.insert(stored.end(), d->payload.begin(), d->payload.end());
      const auto acc = arqRx_.onData(f.env.sequence, std::move(stored));
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
      ackPending_ = true;
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
      if (amc_) amc_->onRemoteSnr(a->snrHalfDb * 0.5, now);
      remoteSnrDb_ = a->snrHalfDb * 0.5;
      onCfoReport(a->cfoHalfHz * 0.5);
      arqTx_.onAck(*a, now);
      break;
    }
    case LinkType::ModeChange: {
      auto mc = ModeChangeBody::parse(f.body);
      if (!mc || fsm_.state() != FsmState::Connected) return;
      if (isInitiator_) return;  // only the initiator drives mode control
      // Echo the epoch at the TARGET mode (the initiator may already have
      // committed it), then apply.
      queueControl(LinkType::ModeAck, ModeAckBody{mc->epoch}.pack(),
                   mc->targetMode <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                   std::min(int(mc->targetMode), mode_));
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
      const uint8_t quality =
          q.snrDb >= ref + 1.0 ? kProbeClean : kProbeMarginal;
      queueControl(LinkType::ProbeAck,
                   ProbeAckBody{p->probeId,
                                int8_t(std::clamp(q.snrDb * 2.0, -127.0,
                                                  127.0)),
                                quality}
                       .pack(),
                   mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
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
      txMask_.bits = mu->mask;
      static const bool maskDbg = getenv("SKW_MASK_DEBUG") != nullptr;
      if (maskDbg) {
        fprintf(stderr, "%s rxMASKUPD epoch=%d masked=%d\n",
                cfg_.policy.ownCall.c_str(), mu->epoch,
                txMask_.maskedCount(cfg_.policy.family));
      }
      queueControl(LinkType::MaskAck, MaskAckBody{mu->epoch}.pack(),
                   mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
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
    default:
      break;  // unknown-to-this-phase types are ignored, never fatal
  }
  drainFsmActions(now);
}

}  // namespace skw
