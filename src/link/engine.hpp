// LinkEngine: the glue that runs a complete modem link - SessionFsm +
// handshake policy + ARQ + link frames over the real PHY (frame builder /
// receiver). Burst-oriented and time-injected: tick() may produce one TX
// burst (baseband samples); rxBurst() consumes a received capture.
// The worker (and the two-modem harness) own audio and scheduling.
//
// Phase-2 simplifications (documented in ROADMAP):
//  - one link frame per PHY frame (burst container lands in Phase 3)
//  - control frames ride sequence 0 outside ARQ (FSM retries cover them);
//    DATA frames carry ARQ sequence numbers
//  - fixed per-mode payload segment budget (proto-layer pipeline in 3.x)
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "link/amc.hpp"
#include "link/arq.hpp"
#include "link/frame.hpp"
#include "link/fsm.hpp"
#include "link/handshake.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"

namespace skw {

struct EngineConfig {
  HandshakePolicy policy;
  int arqMaxRetries = 8;
};

class LinkEngine {
 public:
  explicit LinkEngine(EngineConfig cfg);

  // --- application API -------------------------------------------------
  void connectTo(const std::string& call, double now);
  void disconnect(double now);
  void sendData(std::span<const uint8_t> bytes);  // buffered until sent
  std::vector<uint8_t> takeReceivedData();

  FsmState state() const { return fsm_.state(); }
  FsmDiscCause lastDiscCause() const { return fsm_.lastDiscCause(); }
  int sessionMode() const { return mode_; }
  size_t txBacklog() const { return txBuffer_.size(); }
  bool idleClean() const;  // nothing pending anywhere

  // --- mode control (3.2) ---------------------------------------------
  // Manual pin/unpin (MODE Mx / MODE AUTO). Pinning coordinates a
  // MODE_CHANGE to the pinned mode; fast-demote safety stays active.
  bool pinMode(int mode, double now);
  void unpinMode();
  bool modePinned() const;

  // --- scheduling ------------------------------------------------------
  // Advance timers and, if a burst is due, return its baseband samples.
  std::optional<std::vector<cfloat>> tick(double now);
  // Feed a received capture (burst + padding); engine decodes and reacts.
  void rxBurst(std::span<const cfloat> samples, double now);

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
  void onFrame(const ParsedFrame& f, const QualityMeasurement& q,
               double now);

  void applyMode(int mode, double now);
  void requestModeChange(int target, double now);

  EngineConfig cfg_;
  SessionFsm fsm_;
  ArqTx arqTx_;
  ArqRx arqRx_;
  std::optional<Amc> amc_;
  HarqChase harq_;  // chase soft-combining across retransmissions
  // Mode-control ownership: the session INITIATOR drives AMC decisions
  // and MODE_CHANGE; the responder's channel view reaches it via ACK SNR
  // reports. Two independent controllers would collide mid-air (observed:
  // mode oscillation and pin override). Responder-side manual pinning is
  // a v1 limitation, noted in ROADMAP.
  bool isInitiator_ = false;
  std::string remoteCall_;
  int mode_ = 4;                  // committed session mode
  uint16_t sessionCaps_ = 0;      // committed capability intersection
  bool harqIr() const { return (sessionCaps_ & kCapHarqIr) != 0; }
  // In-flight MODE_CHANGE we initiated (awaiting MODE_ACK).
  int pendingMode_ = -1;
  uint8_t modeEpoch_ = 0;
  double modeChangeDeadline_ = 0.0;
  int modeChangeTries_ = 0;
  uint16_t sessionId_ = 0;
  bool ackPending_ = false;       // send an ACK on the next tick
  QualityMeasurement lastRxQuality_{};
  CallAckBody lastCallAck_{};     // initiator: stored for makeStart
  CallBody lastCall_{};           // responder: stored for evaluateStart
  std::deque<OutFrame> outQueue_; // control first, then data
  std::deque<uint8_t> txBuffer_;  // app bytes awaiting segmentation
  std::vector<uint8_t> rxData_;
};

// Per-mode DATA payload segment budget (bytes) - fills the codeword
// budget approximately; exact proto-layer sizing arrives in Phase 3.
size_t dataSegmentBytes(int mode);

}  // namespace skw
