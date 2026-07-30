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

namespace skw {

struct EngineConfig {
  HandshakePolicy policy;
  int arqMaxRetries = 8;
  bool maskingEnabled = true;  // negotiated via kCapMasking as well
  bool burstAggregation = true;  // MAC burst container (M7+ profiles)
  bool cpAuto = true;            // delay-spread-driven CPMODE
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
  CpMode cpMode_ = kCpDefault;    // committed session CP
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
};

}  // namespace skw
