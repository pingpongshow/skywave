// Half-duplex TX/RX arbiter (ARCHITECTURE.md, DSP.md "Pre-TX CCA Gate",
// INTERFACES.md PTT): priority queue (control-urgent > retransmit > data >
// keepalive > beacon), clear-channel gate with bypass + 5 s proceed-anyway
// cap, PTT sequencing (key -> lead -> audio -> tail -> unkey), TX watchdog,
// TXTEST. Pure logic: time injected, events drained by the worker.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace skw {

enum class TxPriority : uint8_t {
  ControlUrgent = 0,  // ACK/NAK, START_ACK, DISC_ACK, MASK_ACK, handshake
  Retransmit = 1,
  Data = 2,
  Keepalive = 3,
  Beacon = 4,  // also CW ID
};

struct TxRequest {
  uint32_t id = 0;          // caller's burst handle
  TxPriority prio = TxPriority::Data;
  bool bypassCca = false;   // urgent control / handshake / robust recovery
  double airtimeSec = 1.0;  // burst audio duration
};

struct ArbiterConfig {
  double leadSec = 0.050;   // key -> audio
  double tailSec = 0.020;   // audio end -> unkey
  double watchdogSec = 120.0;
  double ccaPollSec = 0.250;
  double ccaCapSec = 5.0;   // then proceed anyway (HF fairness, not CSMA)
  bool preTxCca = true;
};

enum class TxState : uint8_t { Rx = 0, CcaWait, Lead, Audio, Tail };

class TxArbiter {
 public:
  explicit TxArbiter(ArbiterConfig cfg) : cfg_(cfg) {}

  struct Event {
    enum class Kind : uint8_t {
      KeyPtt,
      BeginAudio,   // id = burst
      EndAudio,     // id = burst
      UnkeyPtt,
      WatchdogAbort,  // id = aborted burst; arbiter unkeys and drops it
    };
    Kind kind;
    uint32_t id = 0;
  };

  TxState state() const { return state_; }
  size_t queued() const { return queue_.size(); }
  void setBusy(bool busy) { busy_ = busy; }  // fused busy detector input
  void setConnected(bool c) { connected_ = c; }

  void enqueue(const TxRequest& r);

  // TXTEST: IDLE only, nothing queued, RX state; duration capped 30 s.
  // Returns the burst id or nullopt if refused.
  std::optional<uint32_t> txTest(double now, double seconds);

  // Advance the sequencer; then drain events with nextEvent().
  void tick(double now);
  std::optional<Event> nextEvent();

 private:
  void emit(Event::Kind k, uint32_t id = 0) { events_.push_back({k, id}); }
  void startBurst(double now);

  ArbiterConfig cfg_;
  TxState state_ = TxState::Rx;
  bool busy_ = false;
  bool connected_ = false;
  std::deque<TxRequest> queue_;  // kept priority-sorted, FIFO within class
  std::deque<Event> events_;
  TxRequest current_{};
  double stateSince_ = 0.0;
  double ccaStart_ = 0.0;
  double keyTime_ = 0.0;
  double nextCcaPoll_ = 0.0;
  static constexpr uint32_t kTxTestId = 0xFFFFFFFFu;
};

}  // namespace skw
