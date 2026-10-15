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
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "io/ptt.hpp"
#include "dsp/chirp.hpp"
#include "dsp/updown.hpp"
#include "io/ring.hpp"
#include "link/chanmem.hpp"
#include "link/engine.hpp"
#include "link/handshake.hpp"
#include "link/monitor.hpp"
#include "tcp/control.hpp"

namespace skw {

// POD snapshot published by the worker; readers (GUI, TCP, main) copy it.
// Spectral / constellation taps for the GUI instruments (INTERFACES.md
// "Built-in GUI"). Fixed-size so the whole Telemetry struct stays
// trivially copyable through the seqlock - a vector here would allocate
// under the publish and break the no-allocation rule for the worker.
inline constexpr int kSpectrumBins = 128;    // 0..6 kHz of the baseband
inline constexpr int kConstellationPts = 96;  // most recent data carriers

struct Telemetry {
  int64_t clockUs = 0;
  uint64_t rxSamplesTotal = 0;
  uint64_t rxCapturesOpened = 0;   // detections that opened a capture
  uint64_t rxCapturesDropped = 0;  // captures evicted by the backlog cap
  uint64_t rxDecodeCalls = 0;      // receive attempts (capture ladder)
  uint64_t rxDecodeUsTotal = 0;    // their summed wall time, us
  uint64_t rxDecodeUsMax = 0;      // slowest single attempt, us
  uint64_t txSamplesTotal = 0;
  float rxLevelDbfs = -120.0f;
  float txLevelDbfs = -120.0f;
  float noiseFloorDbfs = -120.0f;
  bool busy = false;
  bool ptt = false;
  uint8_t fsmState = 0;
  uint8_t mode = 4;

  // --- instrument taps -------------------------------------------------
  // RX spectrum in dBFS, bin i covering i * (kFsBaseband/2) / kSpectrumBins
  // Hz from DC. Refreshed from the receive path, not per symbol: the GUI
  // renders at frame rate and does not need every OFDM symbol.
  float spectrumDbfs[kSpectrumBins] = {};
  uint32_t spectrumSeq = 0;      // bumps when a new spectrum lands
  // Equalized data-carrier symbols from the most recent decode, for the
  // constellation view. `constellationN` is how many are valid.
  float constellationI[kConstellationPts] = {};
  float constellationQ[kConstellationPts] = {};
  uint16_t constellationN = 0;
  uint32_t constellationSeq = 0;
  // Carrier mask overlay: bit c set = carrier c excised this session.
  uint8_t maskBits[24] = {};
  uint16_t maskCarriers = 0;     // carriers in the active family plan
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

  // Thread-safe: the audio stream's output latency (AudioBackend::
  // outputLatencySec). PTT stays keyed this long - plus a margin - after
  // the TX ring drains, so the burst tail still in the driver/DAC buffer
  // actually airs. Set by the audio owner after every stream (re)start.
  void setOutputLatencySec(double s) { outLatencySec_.store(s); }

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
  bool setDialFreqHz(double hz) override;
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
  bool ingestChunkLocked(double now);   // one chunk: convert, meter, detect
  bool timedRxBurst(std::span<const cfloat> win, double now, bool final);
  void decodePendingLocked(double now);  // capture ladder, bounded budget
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
    // Rung index at which the last burst-end ("quiet") attempt was made:
    // ONE such attempt per rung interval. The previous key (history end
    // sample) advanced every chunk, so a failing capture inside a long
    // quiet gap re-ran a full receiveFrame - a chirp scan over tens of
    // seconds of audio - every ~30 ms until the next rung: measured as
    // ~36k attempts in a 560 s deep-tier soak, the whole decode budget
    // every chunk, the simulation running ~5x slower than real time.
    int quietTriedTries = -1;
    // Header-implied burst airtime learned from a failed attempt (0 =
    // no header yet). Once that much audio is in hand the burst is
    // complete and the capture goes straight to its FINAL attempt.
    double airtimeSec = 0.0;
    // Shortest frame the detected preamble can start (preamble + sync +
    // header, no payload), and whether the attempt at that point ran.
    double minAirSec = 0.0;
    bool minAirTried = false;
  };
  std::deque<PendingRx> pendingRx_;
  uint64_t rxDropsReported_ = 0;  // RX ring overrun samples already logged
  double rxDropWarnAt_ = -1e9;
  // Captures dropped because the backlog hit its cap (see maxPendingRx()
  // in worker.cpp). Nonzero means the receiver was offered more preambles
  // than it could work through; sustained growth means it is shedding.
  uint64_t rxCapturesDropped_ = 0;
  uint64_t rxCapturesOpened_ = 0;
  uint64_t rxDecodeCalls_ = 0, rxDecodeUsTotal_ = 0, rxDecodeUsMax_ = 0;
  uint64_t lastDroppedDbg_ = 0;
  std::vector<ChirpDetection> dets_;
  // Acquisition for the next-narrower family. A chirp sweep is
  // family-specific, so without this a listener cannot even OPEN a
  // capture window for a caller that stepped down the ladder - the
  // engine's family scan would never get a window to look at. Run only
  // while no session exists, so an established link pays nothing.
  std::optional<ChirpDetector> detAlt_;
  std::vector<ChirpDetection> detsAlt_;

  // TX staging: upconverted audio waiting for ring space.
  std::vector<float> txPending_;

  // --- physical keying (INTERFACES.md "PTT") ---------------------------
  // The driver is asserted just before the first sample of a burst enters
  // an empty TX ring and released once the ring has drained, so the
  // burst's own lead/tail silence padding covers the rig's keying
  // latency. `txWatchdogAt_` is the hard safety net: any transmission
  // longer than modem.tx_watchdog_s force-releases the key.
  // Channel memory (LINK_ADAPTATION.md): owned here so it survives
  // engine rebuilds and is persisted to the config directory. The engine
  // gets a pointer only while `modem.channel_memory` is on, which is what
  // makes `CHANMEM OFF` a real switch rather than a stored flag.
  ChannelMemory chanMem_;
  std::string chanMemPath_;
  int32_t freqBucketKhz_ = -1;   // from `FREQINFO <hz>`

  std::unique_ptr<PttDriver> ptt_;
  bool pttKeyed_ = false;
  double pttKeyedAt_ = 0.0;
  // Time the TX ring was first seen empty with PTT still keyed (< 0: not
  // draining). Release waits outLatencySec_ + kPttPlayoutMarginSec past it.
  double txDrainedAt_ = -1.0;
  std::atomic<double> outLatencySec_{0.0};
  bool txWatchdogTripped_ = false;
  // True when the busy channel is (or may still be) a station rather
  // than noise; only this defers transmission.
  bool peerBusyLocked(double now) const;
  // True while a detected preamble's burst cannot have finished airing.
  bool captureAiringLocked() const;
  // Monitor pass over a capture. With `final` false a header-only decode
  // is NOT published (the frame may simply not have arrived yet): its
  // header airtime is returned so the capture can wait for the whole
  // burst. Returns true when something was published.
  bool runMonitorLocked(std::span<const cfloat> win, bool final = true,
                        double* airtimeSec = nullptr);
  // GUI instrument taps: a periodic Welch-style spectrum over the RX
  // baseband, and the equalized constellation from the last decode.
  void updateSpectrumLocked(std::span<const cfloat> bb);
  std::vector<cfloat> specAcc_;   // accumulating window of baseband
  std::vector<float> specAvg_;    // running average magnitude, linear
  uint32_t specSeq_ = 0;
  double specLastAt_ = -1e9;
  static std::string defaultChanMemPath();
  void saveChanMem();
  void pttKeyLocked(double now);
  void pttUnkeyLocked();
  void pttWatchdogLocked(double now);
  double ladderDbgAt_ = -1e9;  // SKW_RXLADDER_DEBUG rate limit
  std::deque<float> floorBlocks_;  // 15 s of block levels (floor = min)
  uint64_t txImpairSeq_ = 0;   // SKW_TX_IMPAIR per-burst channel seed
  double lastTxDropAt_ = 0.0;  // SKW_TX_IMPAIR dropout scheduler
  double lastClipWarnAt_ = -1e9;  // rate limit for the TX clip warning
  size_t txPendingOff_ = 0;
  double txTestUntil_ = 0.0;

  // Channel measurement (baseband power blocks).
  double powAcc_ = 0.0;
  size_t powN_ = 0;
  float blockDbfs_ = -120.0f;
  float floorDbfs_ = -120.0f;
  bool chanBusy_ = false;
  double busySince_ = -1.0;   // start of the current busy period
  bool busyIsPeer_ = false;   // a chirp preamble claimed this period

#ifdef SKW_HAVE_VOICE
  VoiceMode voiceMode_ = VoiceMode::Off;
  std::optional<VoiceCodec> voiceCodecWant_;  // nullopt = AUTO
#endif

  // Notification/state-change tracking.
  FsmState lastState_ = FsmState::Idle;
  int lastMode_ = -1;
  bool lastBusyEvt_ = false;
  double lastSnrEvt_ = -999.0;        // EVT:SNR change threshold
  double lastRemoteSnrEvt_ = -999.0;
  bool cfoWarned_ = false;            // EVT:CFO_WARN edge state

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
