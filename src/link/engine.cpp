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
      case FsmAction::SendStart:
        queueControl(LinkType::Start,
                     makeStart(cfg_.policy, lastCallAck_, lastRxQuality_)
                         .pack(),
                     ChirpSig::Calling, 4);
        break;
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
        const int w = macWindow(mode_);
        arqTx_ = ArqTx(w, cfg_.arqMaxRetries);
        arqRx_ = ArqRx(w);
        // ACK timeout from real airtimes at the session mode.
        std::vector<uint8_t> seg(dataSegmentBytes(mode_));
        TxFrameParams tp;
        tp.family = cfg_.policy.family;
        tp.sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
        tp.mode = mode_;
        tp.payload = seg;
        std::vector<uint8_t> ackSized(kAckBodyBytes + kFrameOverhead);
        TxFrameParams ta = tp;
        ta.payload = ackSized;
        ta.mode = std::min(mode_, 6) <= 6 ? 4 : mode_;
        arqTx_.setTimeout(frameAirtimeSec(tp) + frameAirtimeSec(ta) + 3.0);
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
    f.sig = mode_ <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
    f.mode = mode_;
    outQueue_.push_back(std::move(f));
    return;
  }
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

std::vector<cfloat> LinkEngine::buildBurst(const OutFrame& f) {
  auto raw = packFrame(f.env, f.body);
  TxFrameParams tp;
  tp.family = cfg_.policy.family;
  tp.sig = f.sig;
  tp.mode = f.mode;
  tp.frameType = uint8_t(f.env.type);
  tp.seqLow = uint8_t(f.env.sequence & 0xFF);
  tp.payload = raw;
  return buildFrameBaseband(tp);
}

std::optional<std::vector<cfloat>> LinkEngine::tick(double now) {
  fsm_.tick(now);
  drainFsmActions(now);
  arqTx_.tick(now);
  if (arqTx_.failed() && fsm_.state() == FsmState::Connected) {
    fsm_.cmdDisconnect(now);
    drainFsmActions(now);
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
  return buildBurst(f);
}

void LinkEngine::rxBurst(std::span<const cfloat> samples, double now) {
  RxStage stage;
  auto res = receiveFrame(cfg_.policy.family, samples, &stage);
  if (!res || !res->payloadOk) return;
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
  switch (f.env.type) {
    case LinkType::Call: {
      auto call = CallBody::parse(f.body);
      if (!call || call->dstCall != cfg_.policy.ownCall) return;
      lastCall_ = *call;
      remoteCall_ = call->srcCall;
      fsm_.rxCall(now);
      break;
    }
    case LinkType::CallAck: {
      auto ack = CallAckBody::parse(f.body);
      if (!ack) return;
      lastCallAck_ = *ack;
      fsm_.rxCallAck(now);
      break;
    }
    case LinkType::Start: {
      auto start = StartBody::parse(f.body);
      if (!start) return;
      auto r = evaluateStart(cfg_.policy, lastCall_, *start, q);
      const bool ok = std::holds_alternative<StartAckBody>(r);
      if (ok) mode_ = std::get<StartAckBody>(r).finalMode;
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
      arqTx_.onAck(*a, now);
      break;
    }
    case LinkType::Disc:
      fsm_.rxDisc(now);
      break;
    case LinkType::DiscAck:
      fsm_.rxDiscAck(now);
      break;
    case LinkType::Keepalive:
      break;  // rxTraffic already refreshed the idle timer
    default:
      break;  // unknown-to-this-phase types are ignored, never fatal
  }
  drainFsmActions(now);
}

}  // namespace skw
