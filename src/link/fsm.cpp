#include "link/fsm.hpp"

#include <algorithm>

namespace skw {

double clampResponseTimeout(double raw) {
  // The ceiling must accommodate CSS deep-tier legs (a single MID M0
  // control frame runs ~35 s on air; the responder's answered-state
  // hold spans the initiator's whole retry ladder). Values are always
  // airtime-derived, so the ceiling is a runaway guard, not a tuning.
  return std::clamp(raw, 12.0, 600.0);
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

// MEASURED AND REJECTED 2026-09-07. A handshake response costs the peer
// airtime AND decode latency, and only airtime was counted - the
// receiver's capture ladder (DSP.md) can leave a control frame undecoded
// for 5-10 s past its arrival. The kill/resume trace looked like a
// straightforward case of that: the caller's START_ACK wait clamped to
// the 12 s floor while the answerer took 17 s to decode the START and
// reply, so the caller gave up at t+30 s and the answerer completed to
// CONNECTED at t+36 s - a half-open session against a peer that had gone
// back to listening.
//
// Adding a 15 s peer-decode allowance here moved the caller's give-up
// from t+30 s to t+33 s and changed nothing else: still half-open. That
// is 14 s spent in WaitStartAck against a 26 s armed timeout, so the
// response timeout is NOT what ends the attempt. Whatever tears the
// caller down at that point is above the FSM. Reverted rather than
// shipped, because a timing change that does not fix the thing it was
// aimed at is just a slower handshake.
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
  // Calling is also accepted: the ENGINE routes a CALL here from that
  // state only when it decided to YIELD a simultaneous-connect collision
  // (deterministic callsign tie-break) - the station becomes responder.
  if (state_ != FsmState::Idle && state_ != FsmState::CallAnswered &&
      state_ != FsmState::Calling) {
    return;
  }
  state_ = FsmState::CallAnswered;
  emit(FsmAction::SendCallAck);
  // Hold the answered state through the initiator's ENTIRE START retry
  // ladder: near the deep-tier floor each leg runs ~50% one-shot FER,
  // and a one-window hold expires exactly when the initiator's first
  // START retry would land (observed live at -13 dB CCIR Moderate).
  armResponse(now, cfg_.timings.callAckSec,
              cfg_.timings.startSec * cfg_.startAttempts);
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
          // Randomised backoff, deterministic per (station, attempt):
          // 0.6x..1.4x of the response timeout. A simultaneous caller
          // draws from a different seed, so the two ladders separate
          // by up to 0.8 x timeout within one retry and one of them
          // hears the other's CALL (see FsmConfig::jitterSeed).
          uint32_t h = cfg_.jitterSeed ^ (0x9E3779B9u * uint32_t(attemptsLeft_ + 1));
          h ^= h >> 16;
          h *= 0x85EBCA6Bu;
          h ^= h >> 13;
          const double u = double(h & 0xFFFFu) / 65536.0;
          deadline_ = now + timeoutSec_ * (0.6 + 0.8 * u);
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
