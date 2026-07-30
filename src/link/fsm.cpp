#include "link/fsm.hpp"

#include <algorithm>

namespace skw {

double clampResponseTimeout(double raw) {
  return std::clamp(raw, 12.0, 45.0);
}

std::optional<FsmAction> SessionFsm::nextAction() {
  if (queue_.empty()) return std::nullopt;
  FsmAction a = queue_.front();
  queue_.pop_front();
  return a;
}

void SessionFsm::toIdle(FsmDiscCause cause) {
  state_ = FsmState::Idle;
  discCause_ = cause;
  deadline_ = 0.0;
  attemptsLeft_ = 0;
  emit(FsmAction::IndicateDisconnected);
}

void SessionFsm::armResponse(double now, double ownSec, double responseSec) {
  timeoutSec_ = clampResponseTimeout(ownSec + responseSec + 6.0);
  deadline_ = now + timeoutSec_;
}

void SessionFsm::enterConnected(double now) {
  state_ = FsmState::Connected;
  discCause_ = FsmDiscCause::None;
  deadline_ = 0.0;
  lastRxTime_ = now;
  lastTxTime_ = now;
  emit(FsmAction::IndicateConnected);
}

void SessionFsm::cmdConnect(double now) {
  if (state_ != FsmState::Idle) return;
  state_ = FsmState::Calling;
  attemptsLeft_ = cfg_.callAttempts - 1;
  emit(FsmAction::SendCall);
  armResponse(now, cfg_.timings.callSec, cfg_.timings.callAckSec);
}

void SessionFsm::cmdDisconnect(double now) {
  if (state_ == FsmState::Idle || state_ == FsmState::Disconnecting) return;
  state_ = FsmState::Disconnecting;
  attemptsLeft_ = cfg_.discAttempts - 1;
  emit(FsmAction::SendDisc);
  timeoutSec_ = 2.0 * cfg_.timings.discSec + 3.0;
  deadline_ = now + timeoutSec_;
  discCause_ = FsmDiscCause::LocalRequest;
}

void SessionFsm::cmdAbort(double /*now*/) {
  if (state_ == FsmState::Idle) return;
  state_ = FsmState::Idle;
  deadline_ = 0.0;
  discCause_ = FsmDiscCause::LocalRequest;
  emit(FsmAction::IndicateDisconnected);
}

void SessionFsm::rxCall(double now) {
  // Responder answer path. A CALL while connected to the same peer is a
  // reconnect attempt; session-level dedup happens above the FSM.
  if (state_ != FsmState::Idle && state_ != FsmState::CallAnswered) return;
  state_ = FsmState::CallAnswered;
  emit(FsmAction::SendCallAck);
  // Await START for one response window; no retries on the responder
  // side (the initiator owns retry budgets).
  armResponse(now, cfg_.timings.callAckSec, cfg_.timings.startSec);
}

void SessionFsm::rxCallAck(double now) {
  if (state_ != FsmState::Calling) return;
  state_ = FsmState::WaitStartAck;
  attemptsLeft_ = cfg_.startAttempts - 1;
  emit(FsmAction::SendStart);
  armResponse(now, cfg_.timings.startSec, cfg_.timings.startAckSec);
}

void SessionFsm::rxStart(double now, bool acceptable) {
  if (state_ != FsmState::CallAnswered) return;
  if (!acceptable) {
    emit(FsmAction::SendStartNak);
    toIdle(FsmDiscCause::StartNak);
    return;
  }
  emit(FsmAction::SendStartAck);
  enterConnected(now);
}

void SessionFsm::rxStartAck(double now) {
  if (state_ != FsmState::WaitStartAck) return;
  enterConnected(now);
}

void SessionFsm::rxStartNak(double /*now*/) {
  if (state_ != FsmState::WaitStartAck) return;
  toIdle(FsmDiscCause::StartNak);
}

void SessionFsm::rxDisc(double /*now*/) {
  if (state_ == FsmState::Idle) return;
  emit(FsmAction::SendDiscAck);
  toIdle(FsmDiscCause::PeerRequest);
}

void SessionFsm::rxDiscAck(double /*now*/) {
  if (state_ != FsmState::Disconnecting) return;
  toIdle(discCause_ == FsmDiscCause::None ? FsmDiscCause::LocalRequest
                                          : discCause_);
}

void SessionFsm::rxTraffic(double now) {
  if (state_ == FsmState::Connected) lastRxTime_ = now;
}

void SessionFsm::tick(double now) {
  switch (state_) {
    case FsmState::Idle:
      break;
    case FsmState::Calling:
      if (now >= deadline_ && deadline_ > 0.0) {
        if (attemptsLeft_ > 0) {
          --attemptsLeft_;
          emit(FsmAction::SendCall);
          deadline_ = now + timeoutSec_;
        } else {
          toIdle(FsmDiscCause::CallRetriesExhausted);
        }
      }
      break;
    case FsmState::WaitStartAck:
      if (now >= deadline_ && deadline_ > 0.0) {
        if (attemptsLeft_ > 0) {
          --attemptsLeft_;
          emit(FsmAction::SendStart);
          deadline_ = now + timeoutSec_;
        } else {
          toIdle(FsmDiscCause::StartRetriesExhausted);
        }
      }
      break;
    case FsmState::CallAnswered:
      // Responder gives up quietly if START never arrives.
      if (now >= deadline_ && deadline_ > 0.0) {
        toIdle(FsmDiscCause::IdleTimeout);
      }
      break;
    case FsmState::Connected:
      if (now - lastRxTime_ >= cfg_.idleDisconnectSec) {
        // Unilateral teardown: peer is gone; still send one DISC.
        emit(FsmAction::SendDisc);
        toIdle(FsmDiscCause::IdleTimeout);
        break;
      }
      if (cfg_.keepaliveEnabled &&
          now - lastTxTime_ >= cfg_.keepaliveSec) {
        emit(FsmAction::SendKeepalive);
        lastTxTime_ = now;
      }
      break;
    case FsmState::Disconnecting:
      if (now >= deadline_ && deadline_ > 0.0) {
        if (attemptsLeft_ > 0) {
          --attemptsLeft_;
          emit(FsmAction::SendDisc);
          deadline_ = now + timeoutSec_;
        } else {
          toIdle(FsmDiscCause::DiscRetriesExhausted);
        }
      }
      break;
  }
}

}  // namespace skw
