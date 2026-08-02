// Modem worker (ARCHITECTURE.md execution model): owns the LinkEngine,
// the 48k<->12k converters, streaming RX acquisition, the monitor, and
// channel measurements. This is where the pure-logic link stack meets
// real-time audio.
//
// Crossings:
//   audio <-> worker : SPSC sample rings (lock-free, 48 kHz float)
//   worker <-> others: modemMutex_ guards the modem objects; control
//                      dispatch and the data/notification bridges lock it
//                      briefly (the DSP hot path holds it per chunk)
//   telemetry        : seqlock-style versioned snapshot (lock-free read)
//
// Streaming RX: a rolling baseband history feeds the ChirpDetector;
// each detection opens a capture window that grows on a retry schedule
// until the engine decodes the frame or the window cap passes (frame
// length is unknown until the self-describing header decodes, so the
// worker cannot size the capture up front).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "dsp/chirp.hpp"
#include "dsp/updown.hpp"
#include "io/ring.hpp"
#include "link/engine.hpp"
#include "link/monitor.hpp"
#include "tcp/control.hpp"

namespace skw {

// POD snapshot published by the worker; readers (GUI, TCP, main) copy it.
struct Telemetry {
  int64_t clockUs = 0;
  uint64_t rxSamplesTotal = 0;
  uint64_t txSamplesTotal = 0;
  float rxLevelDbfs = -120.0f;
  float txLevelDbfs = -120.0f;
  float noiseFloorDbfs = -120.0f;
  bool busy = false;
  bool ptt = false;
  uint8_t fsmState = 0;
  uint8_t mode = 4;
};

struct WorkerCommand {
  std::string line;  // control-port command syntax; parsed by the worker
};

class ModemWorker : public ControlBackend {
 public:
  ModemWorker(const Config& cfg, Clock& clock, SampleRing& rxRing,
              SampleRing& txRing);
  ~ModemWorker() override;

  void start();
  void stop();

  // Thread-safe: queue a command; the worker applies it between chunks.
  void postCommand(WorkerCommand cmd);
  // Thread-safe synchronous control dispatch (the TCP owner calls this).
  std::string control(const std::string& line);

  // Thread-safe: copy the latest consistent telemetry snapshot.
  Telemetry telemetry() const;

  // --- data-port bridge (thread-safe) -----------------------------------
  void submitAppData(std::span<const uint8_t> bytes);
  std::vector<uint8_t> takeAppData();
  // EVT:/MON: notification lines for the control port.
  std::vector<std::string> takeNotifications();
  // Monitor binary records for the data port (suppressed while connected).
  std::vector<std::vector<uint8_t>> takeMonitorRecords();

  // Test/file-mode hook: run one processing iteration on the calling thread
  // instead of using the background thread. Returns samples processed.
  size_t runOnce();

  // --- ControlBackend (locks internally) ---------------------------------
  double netBps();
  std::string myCall() override;
  bool setMyCall(const std::string& call) override;
  bool connectTo(const std::string& call) override;
  bool disconnect() override;
  void abortSession() override;
  bool setListen(bool on) override;
  bool listening() override;
  std::string fsmState() override;
  std::string statusLine() override;
  bool busy() override;
  size_t txBufferBytes() override;
  std::string family() override;
  bool setFamily(const std::string& fam) override;
  int maxMode() override;
  bool setMaxMode(int mode) override;
  bool setMonitor(bool on) override;
  bool monitoring() override;
  bool txTest(double seconds) override;
  std::string version() override;
  bool cq() override;
  bool beacon() override;
  bool bcast(const std::vector<uint8_t>& payload) override;
  bool cwId() override;
  void setCwId(bool on) override;
  std::string modeStr() override;
  bool setMode(const std::string& m) override;
  std::string cpModeStr() override;
  bool setCpMode(const std::string& m) override;
  std::string harqStr() override;
  bool setHarq(const std::string& m) override;
  std::optional<bool> feature(const std::string& name) override;
  bool setFeature(const std::string& name, bool on) override;
  std::string statusJson() override;
  std::string noiseFloor() override;
  std::string audioLevel() override;
  std::string freqInfo() override;
  std::string pttGet(const std::string& key) override;
  bool pttSet(const std::string& key, const std::string& val) override;
  bool cryptoSupported() override;
  std::string securityLine() override;
  bool setEncryption(bool on) override;
  bool setPsk(const std::string& hex) override;
  bool keyRotate() override;
  bool voiceSupported() override;
  std::string voiceModeStr() override;
  bool setVoiceMode(const std::string& m) override;
  std::string voiceCodecStr() override;
  bool setVoiceCodec(const std::string& c) override;
  bool voiceStart() override;
  bool voiceStop() override;
  bool voicePtt(bool on) override;
  std::string voiceStatusLine() override;

  // --- voice port bridge (thread-safe; main.cpp owns port 9102) ----------
  void submitVoicePcm(std::span<const int16_t> pcm);
  std::vector<int16_t> takeVoicePcm(size_t samples);

 private:
  void threadMain();
  void applyPendingCommands();
  size_t processChunk();
  void publishTelemetry();
  double nowSec() { return double(clock_.nowUs()) * 1e-6; }

  // Locked helpers (modemMutex_ held).
  static std::string fsmStateNameLocked(FsmState st);
#ifdef SKW_HAVE_VOICE
  const char* voiceModeNameLocked() const;
#endif
  void rebuildEngine();          // IDLE-only reconfiguration
  void pumpRxLocked(double now);
  void pumpTxLocked(double now);
  void drainEngineOutputsLocked();
  void updateStateEventsLocked();
  std::string statusFieldsLocked(bool json);

  Config cfg_;
  Clock& clock_;
  SampleRing& rx_;
  SampleRing& tx_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  std::mutex cmdMutex_;
  std::vector<WorkerCommand> pendingCmds_;

  // Seqlock: even version = stable; writer bumps to odd, writes, bumps even.
  mutable std::atomic<uint32_t> telemetryVersion_{0};
  Telemetry telemetry_{};
  Telemetry working_{};

  // --- modem state (modemMutex_) -----------------------------------------
  mutable std::mutex modemMutex_;
  std::optional<LinkEngine> engine_;
  std::optional<Monitor> monitor_;
  bool monitorOn_ = false;
  bool listen_ = true;
  Downconverter down_;
  Upconverter up_;
  ChirpDetector det_{Family::Wide};

  // RX history at 12 kHz baseband; histBase_ = absolute index of hist_[0]
  // in the detector's sample count.
  std::vector<cfloat> hist_;
  uint64_t histBase_ = 0;
  struct PendingRx {
    uint64_t start;    // absolute baseband sample of the preamble start
    int tries = 0;
    uint64_t quietTriedAt = 0;  // hist end of the last burst-end attempt
  };
  std::deque<PendingRx> pendingRx_;
  std::vector<ChirpDetection> dets_;

  // TX staging: upconverted audio waiting for ring space.
  std::vector<float> txPending_;
  double ladderDbgAt_ = -1e9;  // SKW_RXLADDER_DEBUG rate limit
  std::deque<float> floorBlocks_;  // 15 s of block levels (floor = min)
  uint64_t txImpairSeq_ = 0;   // SKW_TX_IMPAIR per-burst channel seed
  double lastTxDropAt_ = 0.0;  // SKW_TX_IMPAIR dropout scheduler
  size_t txPendingOff_ = 0;
  double txTestUntil_ = 0.0;

  // Channel measurement (baseband power blocks).
  double powAcc_ = 0.0;
  size_t powN_ = 0;
  float blockDbfs_ = -120.0f;
  float floorDbfs_ = -120.0f;
  bool chanBusy_ = false;

#ifdef SKW_HAVE_VOICE
  VoiceMode voiceMode_ = VoiceMode::Off;
  std::optional<VoiceCodec> voiceCodecWant_;  // nullopt = AUTO
#endif

  // Notification/state-change tracking.
  FsmState lastState_ = FsmState::Idle;
  int lastMode_ = -1;
  bool lastBusyEvt_ = false;

  // Bridges (their own mutex; touched by TCP owner + worker).
  std::mutex bridgeMutex_;
  std::vector<uint8_t> appRx_;
  std::vector<std::string> notes_;
  std::vector<std::vector<uint8_t>> monRecords_;

  std::vector<float> chunk_;
  uint64_t rxTotal_ = 0, txTotal_ = 0;
  float rxLevel_ = -120.0f, txLevel_ = -120.0f;
};

}  // namespace skw
