#include "link/arbiter.hpp"

#include <algorithm>

namespace skw {

void TxArbiter::enqueue(const TxRequest& r) {
  // Insert keeping priority order, FIFO within the same class.
  auto it = queue_.begin();
  while (it != queue_.end() && uint8_t(it->prio) <= uint8_t(r.prio)) ++it;
  queue_.insert(it, r);
}

std::optional<uint32_t> TxArbiter::txTest(double now, double seconds) {
  if (connected_ || state_ != TxState::Rx || !queue_.empty()) {
    return std::nullopt;
  }
  TxRequest r;
  r.id = kTxTestId;
  r.prio = TxPriority::Beacon;
  r.bypassCca = true;  // operator-initiated calibration
  r.airtimeSec = std::clamp(seconds, 0.5, 30.0);
  queue_.push_front(r);
  tick(now);
  return kTxTestId;
}

void TxArbiter::startBurst(double now) {
  state_ = TxState::Lead;
  stateSince_ = now;
  keyTime_ = now;
  emit(Event::Kind::KeyPtt);
}

void TxArbiter::tick(double now) {
  switch (state_) {
    case TxState::Rx: {
      if (queue_.empty()) break;
      current_ = queue_.front();
      queue_.pop_front();
      if (cfg_.preTxCca && !current_.bypassCca && busy_) {
        state_ = TxState::CcaWait;
        stateSince_ = now;
        ccaStart_ = now;
        nextCcaPoll_ = now + cfg_.ccaPollSec;
      } else {
        startBurst(now);
      }
      break;
    }
    case TxState::CcaWait: {
      if (now - ccaStart_ >= cfg_.ccaCapSec) {
        startBurst(now);  // proceed anyway
        break;
      }
      if (now >= nextCcaPoll_) {
        nextCcaPoll_ = now + cfg_.ccaPollSec;
        if (!busy_) startBurst(now);
      }
      break;
    }
    case TxState::Lead: {
      if (now - stateSince_ >= cfg_.leadSec) {
        state_ = TxState::Audio;
        stateSince_ = now;
        emit(Event::Kind::BeginAudio, current_.id);
      }
      break;
    }
    case TxState::Audio: {
      if (now - keyTime_ >= cfg_.watchdogSec) {
        emit(Event::Kind::WatchdogAbort, current_.id);
        emit(Event::Kind::EndAudio, current_.id);
        emit(Event::Kind::UnkeyPtt);
        state_ = TxState::Rx;
        stateSince_ = now;
        break;
      }
      if (now - stateSince_ >= current_.airtimeSec) {
        state_ = TxState::Tail;
        stateSince_ = now;
        emit(Event::Kind::EndAudio, current_.id);
      }
      break;
    }
    case TxState::Tail: {
      if (now - stateSince_ >= cfg_.tailSec) {
        state_ = TxState::Rx;
        stateSince_ = now;
        emit(Event::Kind::UnkeyPtt);
        // Chain into the next queued burst on the following tick.
      }
      break;
    }
  }
}

std::optional<TxArbiter::Event> TxArbiter::nextEvent() {
  if (events_.empty()) return std::nullopt;
  Event e = events_.front();
  events_.pop_front();
  return e;
}

}  // namespace skw
