#include "link/engine.hpp"

#include <algorithm>

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

}  // namespace

size_t dataSegmentBytes(int mode) {
  // Rough codeword-budget fill: robust modes keep frames short (airtime),
  // fast modes fill N=1944 codewords.
  if (mode <= 7) return 90;
  return 300;
}

LinkEngine::LinkEngine(EngineConfig cfg)
    : cfg_(std::move(cfg)),
      fsm_(fsmConfigFor(cfg_.policy)),
      arqTx_(2, cfg_.arqMaxRetries),
      arqRx_(8) {}

bool LinkEngine::idleClean() const {
  return outQueue_.empty() && txBuffer_.empty() && arqTx_.inFlight() == 0 &&
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
  txBuffer_.insert(txBuffer_.end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> LinkEngine::takeReceivedData() {
  auto out = std::move(rxData_);
  rxData_.clear();
  return out;
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

void LinkEngine::drainFsmActions(double /*now*/) {
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
        StartBody s = makeStart(cfg_.policy, lastCallAck_, lastRxQuality_);
        sessionCaps_ = s.caps;
        queueControl(LinkType::Start, s.pack(), ChirpSig::Calling, 4);
        break;
      }
      case FsmAction::SendStartAck:
        // mode_ was fixed in onFrame when START was evaluated.
        queueControl(LinkType::StartAck, StartAckBody{uint8_t(mode_), {}}.pack(),
                     ChirpSig::Calling, 4);
        break;
      case FsmAction::SendStartNak:
        queueControl(LinkType::StartNak,
                     StartNakBody{StartNakReason::Busy}.pack(),
                     ChirpSig::Calling, 4);
        break;
      case FsmAction::SendDisc:
        queueControl(LinkType::Disc, DiscBody{DiscReason::Normal}.pack(),
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
        const int w = macWindow(mode_);
        arqTx_ = ArqTx(w, cfg_.arqMaxRetries);
        arqRx_ = ArqRx(w);
        updateArqTimeout();
        break;
      }
      case FsmAction::IndicateDisconnected:
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
        txBuffer_.clear();
        ackPending_ = false;
        break;
    }
  }
}

void LinkEngine::pumpData(double now) {
  if (fsm_.state() != FsmState::Connected) return;
  // Retransmits first (arbiter priority), then new segments.
  if (auto r = arqTx_.nextRetransmit(now)) {
    OutFrame f;
    f.env.type = LinkType::Data;
    f.env.sessionId = sessionId_;
    f.env.sequence = r->seq;
    f.env.ackBase = arqRx_.ackBase();
    f.body = DataBody{r->rv, r->payload}.pack();
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
    if (txBuffer_.empty() || !arqTx_.canSend()) return;
    const size_t n = std::min(txBuffer_.size(), dataSegmentBytes(mode_));
    std::vector<uint8_t> seg(txBuffer_.begin(), txBuffer_.begin() + long(n));
    txBuffer_.erase(txBuffer_.begin(), txBuffer_.begin() + long(n));
    auto seq = arqTx_.send(seg, now);
    if (!seq) return;
    OutFrame f;
    f.env.type = LinkType::Data;
    f.env.sessionId = sessionId_;
    f.env.sequence = *seq;
    f.env.ackBase = arqRx_.ackBase();
    f.body = DataBody{0, std::move(seg)}.pack();
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
  std::vector<uint8_t> seg(dataSegmentBytes(mode_));
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
  fsm_.tick(now);
  drainFsmActions(now);
  arqTx_.tick(now);
  if (arqTx_.failed() && fsm_.state() == FsmState::Connected) {
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
    probeGate_.tick(now);
    // Carrier-mask exchange (per direction; either side may originate).
    if (maskDet_ && cfg_.maskingEnabled && (sessionCaps_ & kCapMasking) &&
        mode_ >= 8) {
      if (pendingMaskEpoch_ >= 0 && now >= maskRetryAt_) {
        if (++maskTries_ >= 2) {
          pendingMaskEpoch_ = -1;  // abandon this mask generation
        } else {
          MaskUpdateBody mu;
          mu.epoch = uint8_t(pendingMaskEpoch_);
          mu.family = uint8_t(cfg_.policy.family);
          mu.mask = pendingRxMask_.bits;
          queueControl(LinkType::MaskUpdate, mu.pack(),
                       mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                       std::min(mode_, 8));
          maskRetryAt_ = now + 2.0;
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
          maskRetryAt_ = now + 2.0;
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
    auto ack = arqRx_.makeAck(
        int8_t(std::clamp(lastRxQuality_.snrDb * 2.0, -127.0, 127.0)), 0,
        int8_t(std::clamp(lastRxQuality_.cfoHz / 0.5, -127.0, 127.0)));
    // Feedback at least as robust as forward traffic (M4 for M4-M6).
    queueControl(LinkType::Ack, ack.pack(),
                 mode_ <= 6 ? ChirpSig::Robust : ChirpSig::Fast,
                 mode_ <= 6 ? 4 : mode_);
  }
  pumpData(now);
  if (outQueue_.empty()) return std::nullopt;
  OutFrame f = std::move(outQueue_.front());
  outQueue_.pop_front();
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

void LinkEngine::rxBurst(std::span<const cfloat> samples, double now) {
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
    return;
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
    return;
  }
  auto parsed = parseFrame(res->payload);
  if (!parsed) return;
  QualityMeasurement q;
  q.snrDb = res->snrDb;
  q.selectivity = 0;
  q.cfoHz = res->cfoHz;
  onFrame(*parsed, q, now);
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
      const bool ok = std::holds_alternative<StartAckBody>(r);
      if (ok) {
        mode_ = std::get<StartAckBody>(r).finalMode;
        sessionCaps_ = start->caps;
      }
      fsm_.rxStart(now, ok);
      break;
    }
    case LinkType::StartAck: {
      auto ack = StartAckBody::parse(f.body);
      if (!ack) return;
      mode_ = ack->finalMode;
      fsm_.rxStartAck(now);
      break;
    }
    case LinkType::StartNak:
      fsm_.rxStartNak(now);
      break;
    case LinkType::Data: {
      auto d = DataBody::parse(f.body);
      if (!d || fsm_.state() != FsmState::Connected) return;
      if (amc_) amc_->onFrameResult(d->rv > 0, true);
      arqRx_.onData(f.env.sequence, std::move(d->payload));
      for (auto& p : arqRx_.takeDelivered()) {
        rxData_.insert(rxData_.end(), p.begin(), p.end());
      }
      ackPending_ = true;
      break;
    }
    case LinkType::Ack: {
      auto a = AckBody::parse(f.body);
      if (!a) return;
      if (amc_) amc_->onRemoteSnr(a->snrHalfDb * 0.5, now);
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
    case LinkType::Disc:
      fsm_.rxDisc(now);
      break;
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
      queueControl(LinkType::MaskAck, MaskAckBody{mu->epoch}.pack(),
                   mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast,
                   std::min(mode_, 8));
      break;
    }
    case LinkType::MaskAck: {
      auto ma = MaskAckBody::parse(f.body);
      if (!ma || pendingMaskEpoch_ < 0 ||
          ma->epoch != uint8_t(pendingMaskEpoch_)) {
        return;
      }
      rxMask_ = pendingRxMask_;  // peer now zeroes these toward us
      pendingMaskEpoch_ = -1;
      break;
    }
    case LinkType::Keepalive:
      break;  // rxTraffic already refreshed the idle timer
    default:
      break;  // unknown-to-this-phase types are ignored, never fatal
  }
  drainFsmActions(now);
}

}  // namespace skw
