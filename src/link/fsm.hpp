// Session state machine + timers (PROTOCOL.md "Session Model", "Timers
// and Retries"). Pure logic: time is injected (seconds, monotonic),
// outputs are queued Actions the caller drains and executes. No I/O, no
// PHY dependency - airtime figures arrive via HandshakeTimings, which the
// link layer computes from the PHY airtime query (frameAirtimeSec).
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace skw {

enum class FsmState : uint8_t {
  Idle = 0,
  Calling,       // initiator: CALL sent, awaiting CALL_ACK
  WaitStartAck,  // initiator: START sent, awaiting START_ACK/NAK
  CallAnswered,  // responder: CALL_ACK sent, awaiting START
  Connected,
  Disconnecting,  // DISC sent, awaiting DISC_ACK
};

enum class FsmAction : uint8_t {
  SendCall,
  SendCallAck,
  SendStart,
  SendStartAck,
  SendStartNak,
  SendDisc,
  SendDiscAck,
  SendKeepalive,
  IndicateConnected,
  IndicateDisconnected,
};

enum class FsmDiscCause : uint8_t {
  None = 0,
  LocalRequest,
  PeerRequest,
  CallRetriesExhausted,
  StartRetriesExhausted,
  DiscRetriesExhausted,
  IdleTimeout,
  StartNak,
};

// Airtime-derived timeout inputs (seconds). PROTOCOL.md formula:
// response timeout = own_airtime + expected_response_airtime + 6 s,
// clamped to [12, 45]. DISC retry cadence = 2 * disc_airtime + 3 s.
struct HandshakeTimings {
  double callSec = 4.0;      // CALL frame airtime
  double callAckSec = 4.0;
  double startSec = 4.0;
  double startAckSec = 4.0;
  double discSec = 2.0;
};

struct FsmConfig {
  HandshakeTimings timings;
  int callAttempts = 4;
  int startAttempts = 4;
  int discAttempts = 3;
  double idleDisconnectSec = 120.0;
  double keepaliveSec = 20.0;
  bool keepaliveEnabled = false;  // initiator-only, M8+ sessions only
};

double clampResponseTimeout(double raw);  // [12, 45]

class SessionFsm {
 public:
  explicit SessionFsm(FsmConfig cfg) : cfg_(cfg) {}

  FsmState state() const { return state_; }
  FsmDiscCause lastDiscCause() const { return discCause_; }
  std::optional<FsmAction> nextAction();  // drain output queue

  // --- operator/API commands -----------------------------------------
  void cmdConnect(double now);     // Idle -> Calling (emits SendCall)
  void cmdDisconnect(double now);  // -> Disconnecting (emits SendDisc)

  // --- received frames (already CRC/type validated) -------------------
  void rxCall(double now);       // responder path
  void rxCallAck(double now);
  void rxStart(double now, bool acceptable);  // false -> SendStartNak
  void rxStartAck(double now);
  void rxStartNak(double now);
  void rxDisc(double now);
  void rxDiscAck(double now);
  void rxTraffic(double now);  // any other valid in-session frame

  // --- clock ----------------------------------------------------------
  // Call periodically; fires retries/timeouts/keepalive/idle-disconnect.
  void tick(double now);

  // Timer the caller may schedule the next tick around (absolute time).
  double nextDeadline() const { return deadline_; }

 private:
  void emit(FsmAction a) { queue_.push_back(a); }
  void toIdle(FsmDiscCause cause);
  void armResponse(double now, double ownSec, double responseSec);
  void enterConnected(double now);

  FsmConfig cfg_;
  FsmState state_ = FsmState::Idle;
  FsmDiscCause discCause_ = FsmDiscCause::None;
  std::deque<FsmAction> queue_;
  double deadline_ = 0.0;       // current response/retry deadline
  double timeoutSec_ = 0.0;     // armed timeout length
  int attemptsLeft_ = 0;
  double lastRxTime_ = 0.0;     // idle-disconnect reference
  double lastTxTime_ = 0.0;     // keepalive reference
};

}  // namespace skw
