#include "worker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/constants.hpp"
#include "core/log.hpp"
#include "link/frame.hpp"
#include "phy/framer.hpp"

namespace skw {
namespace {

// Worker hop: one OFDM symbol at MEDIUM CP in audio samples keeps latency
// bounded and gives the DSP a natural processing granule.
constexpr size_t kChunkSamples =
    size_t(kFftAudio) + size_t(kCpSamplesAudio[1]);

// RX capture-window retry ladder (baseband seconds after the detected
// preamble start): the frame length is unknown until the self-describing
// header decodes, so the window grows until the engine takes the frame.
// The last rung exceeds the longest legal frame on air.
// The 60/90 s rungs exist for the CSS deep tier: a MID/NARROW M0
// handshake frame runs 35-55 s on air.
constexpr double kRxWindowSec[] = {2.5, 5.0, 10.0, 20.0, 34.0, 60.0, 90.0};
constexpr int kRxTries = 7;
constexpr double kRxPreMarginSec = 0.15;
constexpr double kRxHistorySec = 94.0;

float levelDbfs(double energy, size_t n) {
  if (n == 0) return -120.0f;
  double rms = std::sqrt(energy / double(n));
  return rms > 1e-9 ? float(20.0 * std::log10(rms)) : -120.0f;
}

std::string fmt1(double v) {
  char b[32];
  snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

// Control-port responses are canonical UPPERCASE (like MYCALL / MODE /
// CPMODE); core toString() stays lowercase for INI files and logs, which
// CONFIGURATION.md defines that way.
std::string upperStr(std::string s) {
  for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

}  // namespace

ModemWorker::ModemWorker(const Config& cfg, Clock& clock, SampleRing& rxRing,
                         SampleRing& txRing)
    : cfg_(cfg), clock_(clock), rx_(rxRing), tx_(txRing) {
  chunk_.resize(kChunkSamples);
  listen_ = cfg_.listen;
  rebuildEngine();
}

ModemWorker::~ModemWorker() { stop(); }

void ModemWorker::rebuildEngine() {
  EngineConfig ec;
  ec.policy.ownCall = cfg_.callsign;
  ec.policy.family = cfg_.family;
  ec.policy.maxMode = uint8_t(cfg_.maxMode);
  uint16_t caps = kCapZstd | kCapBurstMac;
  if (cfg_.resume) caps |= kCapResume;
  if (cfg_.carrierMasking) caps |= kCapMasking;
  if (cfg_.harq != HarqMode::Chase) caps |= kCapHarqIr;
  if (cfg_.cssTier) caps |= kCapCssTier;
  ec.policy.cssEnable = cfg_.cssTier;
  ec.policy.caps = caps;
  ec.arqMaxRetries = cfg_.maxRetries;
  ec.maskingEnabled = cfg_.carrierMasking;
  ec.cpAuto = cfg_.cpMode < 0;
  ec.cwId = cfg_.cwId;
  ec.cwWpm = cfg_.cwWpm;
#ifdef SKW_HAVE_CRYPTO
  // Encryption is advertised only with a usable PSK and hardware AES.
  if (cfg_.cryptoPsk.size() == kPskBytes * 2) {
    ec.psk.reserve(kPskBytes);
    for (size_t i = 0; i + 1 < cfg_.cryptoPsk.size(); i += 2) {
      auto nib = [](char ch) {
        return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
      };
      ec.psk.push_back(uint8_t(nib(cfg_.cryptoPsk[i]) << 4 |
                               nib(cfg_.cryptoPsk[i + 1])));
    }
  }
  if (cfg_.cryptoEnabled && ec.psk.size() == kPskBytes &&
      CryptoSession::available()) {
    ec.encryptionRequested = true;
    ec.policy.caps |= kCapEncryption;
  }
#endif
  engine_.emplace(std::move(ec));
  if (cfg_.cpMode >= 0) {
    static const char* kCp[] = {"SHORT", "MEDIUM", "LONG"};
    engine_->setCpPolicy(kCp[std::clamp(cfg_.cpMode, 0, 2)], nowSec());
  }
  monitor_.emplace(cfg_.family);
  det_ = ChirpDetector(cfg_.family);
  down_.reset();
  hist_.clear();
  histBase_ = 0;
  pendingRx_.clear();
  lastState_ = FsmState::Idle;
  lastMode_ = -1;
}

void ModemWorker::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&ModemWorker::threadMain, this);
}

void ModemWorker::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void ModemWorker::postCommand(WorkerCommand cmd) {
  std::lock_guard lock(cmdMutex_);
  pendingCmds_.push_back(std::move(cmd));
}

std::string ModemWorker::control(const std::string& line) {
  return handleControlLine(line, *this);
}

Telemetry ModemWorker::telemetry() const {
  Telemetry snap;
  uint32_t v0, v1;
  do {
    v0 = telemetryVersion_.load(std::memory_order_acquire);
    snap = telemetry_;
    v1 = telemetryVersion_.load(std::memory_order_acquire);
  } while (v0 != v1 || (v0 & 1u));
  return snap;
}

void ModemWorker::publishTelemetry() {
  telemetryVersion_.fetch_add(1, std::memory_order_release);  // -> odd
  telemetry_ = working_;
  telemetryVersion_.fetch_add(1, std::memory_order_release);  // -> even
}

void ModemWorker::applyPendingCommands() {
  std::vector<WorkerCommand> cmds;
  {
    std::lock_guard lock(cmdMutex_);
    cmds.swap(pendingCmds_);
  }
  for (auto& c : cmds) {
    const std::string resp = control(c.line);
    SKW_DEBUG("cmd '%s' -> '%s'", c.line.c_str(), resp.c_str());
  }
}

// --- data / notification bridges ---------------------------------------------

void ModemWorker::submitAppData(std::span<const uint8_t> bytes) {
  std::lock_guard lock(modemMutex_);
  engine_->sendData(bytes);
}

std::vector<uint8_t> ModemWorker::takeAppData() {
  std::lock_guard lock(bridgeMutex_);
  auto out = std::move(appRx_);
  appRx_.clear();
  return out;
}

std::vector<std::string> ModemWorker::takeNotifications() {
  std::lock_guard lock(bridgeMutex_);
  auto out = std::move(notes_);
  notes_.clear();
  return out;
}

std::vector<std::vector<uint8_t>> ModemWorker::takeMonitorRecords() {
  std::lock_guard lock(bridgeMutex_);
  auto out = std::move(monRecords_);
  monRecords_.clear();
  return out;
}

// --- processing ----------------------------------------------------------------

void ModemWorker::pumpRxLocked(double now) {
  // 48 kHz audio -> baseband, metering + channel power on the way.
  size_t got = rx_.read(chunk_);
  if (got == 0) return;
  rxTotal_ += got;
  double e = 0.0;
  for (size_t i = 0; i < got; ++i) e += double(chunk_[i]) * chunk_[i];
  rxLevel_ = levelDbfs(e, got);

  std::vector<cfloat> bb;
  down_.process({chunk_.data(), got}, bb);
  if (bb.empty()) return;

  // Noise/CCA blocks (~100 ms of baseband).
  for (auto& v : bb) {
    powAcc_ += double(v.real()) * v.real() + double(v.imag()) * v.imag();
    if (++powN_ >= size_t(kFsBaseband / 10)) {
      blockDbfs_ = levelDbfs(powAcc_, powN_);
      powAcc_ = 0.0;
      powN_ = 0;
      if (blockDbfs_ < floorDbfs_ + 1.0f) {
        floorDbfs_ = blockDbfs_;  // fast acquisition downward
      } else {
        floorDbfs_ += 0.02f;      // slow rise tracks drifting bands
      }
      chanBusy_ = blockDbfs_ > floorDbfs_ + 6.0f;
    }
  }

  hist_.insert(hist_.end(), bb.begin(), bb.end());
  dets_.clear();
  det_.process(bb, dets_);
  for (const auto& d : dets_) {
    const double margin = kRxPreMarginSec * kFsBaseband;
    const double s = std::max(0.0, d.startSample - margin);
    pendingRx_.push_back({uint64_t(s), 0});
  }

  // Grow each pending capture along the retry ladder until the engine
  // takes the frame (or the ladder ends and the monitor gets one look).
  const uint64_t histEnd = histBase_ + hist_.size();
  for (auto it = pendingRx_.begin(); it != pendingRx_.end();) {
    bool done = false;
    while (it->tries < kRxTries) {
      const uint64_t need =
          it->start + uint64_t(kRxWindowSec[it->tries] * kFsBaseband);
      if (histEnd < need) break;  // wait for more audio
      const uint64_t w0 = std::max(it->start, histBase_);
      std::span<const cfloat> win(hist_.data() + size_t(w0 - histBase_),
                                  size_t(std::min(need, histEnd) - w0));
      const bool engineEligible =
          listen_ || engine_->state() != FsmState::Idle;
      const bool decoded =
          engineEligible ? engine_->rxBurst(win, now) : false;
      ++it->tries;
      if (decoded || it->tries >= kRxTries) {
        if (monitorOn_ && monitor_) {
          monitor_->setLocalConnected(engine_->state() ==
                                      FsmState::Connected);
          if (auto ev = monitor_->rxBurst(win)) {
            std::lock_guard lock(bridgeMutex_);
            notes_.push_back(ev->monLine);
            if (!ev->binaryRecord.empty()) {
              monRecords_.push_back(ev->binaryRecord);
            }
          }
        }
        done = true;
        break;
      }
    }
    it = done ? pendingRx_.erase(it) : ++it;
  }

  // Trim history: keep what open captures still need, bounded.
  uint64_t keepFrom =
      histEnd > uint64_t(kRxHistorySec * kFsBaseband)
          ? histEnd - uint64_t(kRxHistorySec * kFsBaseband)
          : 0;
  for (const auto& p : pendingRx_) keepFrom = std::min(keepFrom, p.start);
  if (keepFrom > histBase_) {
    hist_.erase(hist_.begin(), hist_.begin() + long(keepFrom - histBase_));
    histBase_ = keepFrom;
  }
}

void ModemWorker::pumpTxLocked(double now) {
  // New burst from the engine (only once the previous one fully left).
  if (txPendingOff_ >= txPending_.size()) {
    txPending_.clear();
    txPendingOff_ = 0;
    std::optional<std::vector<cfloat>> burst = engine_->tick(now);
    if (!burst && txTestUntil_ > now &&
        engine_->state() == FsmState::Idle) {
      // TXTEST: representative OFDM calibration frame (M8, MEDIUM CP).
      std::vector<uint8_t> dummy(200);
      for (size_t i = 0; i < dummy.size(); ++i) dummy[i] = uint8_t(i * 73);
      TxFrameParams tp;
      tp.family = cfg_.family;
      tp.sig = ChirpSig::Fast;
      tp.mode = 8;
      tp.cpMode = CpMode::Medium;
      tp.frameType = 61;  // unassigned type; receivers ignore it
      tp.payload = dummy;
      burst = buildFrameBaseband(tp);
    }
    if (burst) {
      // PTT lead/tail: silence padding the rig's keying swallows.
      const size_t lead = size_t(cfg_.pttLeadMs) * kFsBaseband / 1000;
      const size_t tail = size_t(cfg_.pttTailMs) * kFsBaseband / 1000;
      std::vector<cfloat> padded(lead, cfloat(0, 0));
      padded.insert(padded.end(), burst->begin(), burst->end());
      padded.insert(padded.end(), tail, cfloat(0, 0));
      std::vector<float> audio;
      up_.process(padded, audio);
      const float g = float(std::pow(10.0, cfg_.outputGainDb / 20.0));
      for (auto& v : audio) v *= g;
      txPending_ = std::move(audio);
      txPendingOff_ = 0;
    }
  }
  // Drain staged audio into the ring as space frees up.
  if (txPendingOff_ < txPending_.size()) {
    const size_t n = std::min(tx_.writeAvailable(),
                              txPending_.size() - txPendingOff_);
    if (n > 0) {
      std::span<const float> out(txPending_.data() + txPendingOff_, n);
      tx_.write(out);
      txTotal_ += n;
      double e = 0.0;
      for (float v : out) e += double(v) * v;
      txLevel_ = levelDbfs(e, n);
      txPendingOff_ += n;
    }
  } else {
    txLevel_ = -120.0f;
  }
}

void ModemWorker::drainEngineOutputsLocked() {
  auto data = engine_->takeReceivedData();
  auto evts = engine_->takeEvents();
  if (!data.empty() || !evts.empty()) {
    std::lock_guard lock(bridgeMutex_);
    appRx_.insert(appRx_.end(), data.begin(), data.end());
    for (auto& e : evts) notes_.push_back(std::move(e));
  }
}

void ModemWorker::updateStateEventsLocked() {
  const FsmState st = engine_->state();
  if (st != lastState_) {
    const bool wasConnected = lastState_ == FsmState::Connected;
    lastState_ = st;
    std::lock_guard lock(bridgeMutex_);
    notes_.push_back(evtState(st == FsmState::Idle && listen_
                                  ? "LISTENING"
                                  : fsmStateNameLocked(st)));
    if (st == FsmState::Connected) {
      notes_.push_back(evtConnected(engine_->remoteCall(),
                                    upperStr(toString(cfg_.family)),
                                    engine_->sessionMode()));
    }
    if (wasConnected) {
      const auto cause = engine_->lastDiscCause();
      notes_.push_back(evtDisconnected(
          cause == FsmDiscCause::LocalRequest ||
                  cause == FsmDiscCause::PeerRequest
              ? "NORMAL"
              : "LINK"));
    }
  }
  // Operator pinned a mode before the session existed: apply it now.
  if (st == FsmState::Connected && cfg_.mode >= 4 &&
      !engine_->modePinned()) {
    engine_->pinMode(cfg_.mode, nowSec());
  }
  const int mode = engine_->sessionMode();
  if (st == FsmState::Connected && mode != lastMode_) {
    lastMode_ = mode;
    std::lock_guard lock(bridgeMutex_);
    notes_.push_back(evtMode(mode, engine_->netBps()));
  }
  if (chanBusy_ != lastBusyEvt_) {
    lastBusyEvt_ = chanBusy_;
    std::lock_guard lock(bridgeMutex_);
    notes_.push_back(evtBusy(chanBusy_));
  }
}

size_t ModemWorker::processChunk() {
  std::lock_guard lock(modemMutex_);
  const double now = nowSec();
  const size_t availBefore = rx_.readAvailable();
  pumpRxLocked(now);
  pumpTxLocked(now);
  drainEngineOutputsLocked();
  updateStateEventsLocked();

  working_.clockUs = clock_.nowUs();
  working_.rxSamplesTotal = rxTotal_;
  working_.txSamplesTotal = txTotal_;
  working_.rxLevelDbfs = rxLevel_;
  working_.txLevelDbfs = txLevel_;
  working_.noiseFloorDbfs = floorDbfs_;
  working_.busy = chanBusy_;
  working_.ptt = txPendingOff_ < txPending_.size();
  working_.fsmState = uint8_t(engine_->state());
  working_.mode = uint8_t(engine_->sessionMode());
  publishTelemetry();
  return std::min(availBefore, kChunkSamples) +
         (txPendingOff_ < txPending_.size() ? 1 : 0);
}

size_t ModemWorker::runOnce() {
  applyPendingCommands();
  return processChunk();
}

void ModemWorker::threadMain() {
  SKW_INFO("worker: started");
  while (running_.load(std::memory_order_relaxed)) {
    applyPendingCommands();
    const size_t processed = processChunk();
    if (processed == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  SKW_INFO("worker: stopped");
}

// --- ControlBackend ------------------------------------------------------------

std::string ModemWorker::fsmStateNameLocked(FsmState st) {
  switch (st) {
    case FsmState::Idle: return "IDLE";
    case FsmState::Calling: return "CALLING";
    case FsmState::WaitStartAck:
    case FsmState::CallAnswered: return "CONNECTING";
    case FsmState::Connected: return "CONNECTED";
    case FsmState::Disconnecting: return "DISCONNECTING";
  }
  return "IDLE";
}

std::string ModemWorker::myCall() {
  std::lock_guard lock(modemMutex_);
  return cfg_.callsign;
}

bool ModemWorker::setMyCall(const std::string& call) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  auto v = validateCallsign(call);
  if (!v) return false;
  cfg_.callsign = *v;
  rebuildEngine();
  return true;
}

bool ModemWorker::connectTo(const std::string& call) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  if (!validateCallsign(call)) return false;
  engine_->connectTo(call, nowSec());
  return true;
}

bool ModemWorker::disconnect() {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() == FsmState::Idle) return false;
  engine_->disconnect(nowSec());
  return true;
}

void ModemWorker::abortSession() {
  std::lock_guard lock(modemMutex_);
  engine_->abortSession(nowSec());
}

bool ModemWorker::setListen(bool on) {
  std::lock_guard lock(modemMutex_);
  listen_ = on;
  return true;
}

bool ModemWorker::listening() {
  std::lock_guard lock(modemMutex_);
  return listen_;
}

std::string ModemWorker::fsmState() {
  std::lock_guard lock(modemMutex_);
  const FsmState st = engine_->state();
  if (st == FsmState::Idle && listen_) return "LISTENING";
  return fsmStateNameLocked(st);
}

bool ModemWorker::busy() {
  std::lock_guard lock(modemMutex_);
  return chanBusy_;
}

size_t ModemWorker::txBufferBytes() {
  std::lock_guard lock(modemMutex_);
  return engine_->txBacklog();
}

std::string ModemWorker::family() {
  std::lock_guard lock(modemMutex_);
  return upperStr(toString(cfg_.family));
}

bool ModemWorker::setFamily(const std::string& fam) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  auto f = familyFromString(fam);
  if (!f) return false;
  if (*f == Family::Ultra && !cfg_.ultraEnable) return false;
  cfg_.family = *f;
  rebuildEngine();
  return true;
}

int ModemWorker::maxMode() {
  std::lock_guard lock(modemMutex_);
  return cfg_.maxMode;
}

bool ModemWorker::setMaxMode(int mode) {
  std::lock_guard lock(modemMutex_);
  cfg_.maxMode = mode;
  if (engine_->state() == FsmState::Idle) rebuildEngine();
  return true;
}

bool ModemWorker::setMonitor(bool on) {
  std::lock_guard lock(modemMutex_);
  monitorOn_ = on;
  return true;
}

bool ModemWorker::monitoring() {
  std::lock_guard lock(modemMutex_);
  return monitorOn_;
}

bool ModemWorker::txTest(double seconds) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  txTestUntil_ = nowSec() + seconds;
  return true;
}

std::string ModemWorker::version() { return SKW_VERSION; }

bool ModemWorker::cq() {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  engine_->sendCq();
  listen_ = true;  // CQ implies listening for answers
  return true;
}

bool ModemWorker::beacon() {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  // SNR floor estimate: dBFS floor relative to a nominal full-scale
  // signal; a crude v1 mapping until calibrated RX metering lands.
  engine_->sendBeacon(std::max(-20.0, -(double(floorDbfs_) + 80.0)));
  return true;
}

bool ModemWorker::bcast(const std::vector<uint8_t>& payload) {
  std::lock_guard lock(modemMutex_);
  return engine_->sendBroadcast(payload);
}

bool ModemWorker::cwId() {
  std::lock_guard lock(modemMutex_);
  return cfg_.cwId;
}

void ModemWorker::setCwId(bool on) {
  std::lock_guard lock(modemMutex_);
  cfg_.cwId = on;
  if (engine_->state() == FsmState::Idle) rebuildEngine();
}

std::string ModemWorker::modeStr() {
  std::lock_guard lock(modemMutex_);
  const int m = engine_->sessionMode();
  if (engine_->modePinned() || cfg_.mode >= 4) {
    return "M" + std::to_string(cfg_.mode >= 4 ? cfg_.mode : m);
  }
  return "AUTO(M" + std::to_string(m) + ")";
}

bool ModemWorker::setMode(const std::string& m) {
  std::lock_guard lock(modemMutex_);
  if (m == "AUTO") {
    cfg_.mode = -1;
    engine_->unpinMode();
    return true;
  }
  if (m.size() < 2 || m[0] != 'M') return false;
  const int want = atoi(m.c_str() + 1);
  if (want < 4 || want > 13) return false;
  cfg_.mode = want;
  if (engine_->state() == FsmState::Connected) {
    return engine_->pinMode(want, nowSec());
  }
  return true;  // applied when the next session connects
}

std::string ModemWorker::cpModeStr() {
  std::lock_guard lock(modemMutex_);
  return engine_->cpPolicy();
}

bool ModemWorker::setCpMode(const std::string& m) {
  std::lock_guard lock(modemMutex_);
  static const char* kNames[] = {"SHORT", "MEDIUM", "LONG"};
  if (m == "AUTO") {
    cfg_.cpMode = -1;
  } else {
    for (int i = 0; i < 3; ++i) {
      if (m == kNames[i]) cfg_.cpMode = i;
    }
  }
  return engine_->setCpPolicy(m, nowSec());
}

std::string ModemWorker::harqStr() {
  std::lock_guard lock(modemMutex_);
  return toString(cfg_.harq);
}

bool ModemWorker::setHarq(const std::string& m) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;  // caps renegotiate
  auto h = harqModeFromString(m);
  if (!h) return false;
  cfg_.harq = *h;
  rebuildEngine();
  return true;
}

std::optional<bool> ModemWorker::feature(const std::string& name) {
  std::lock_guard lock(modemMutex_);
  if (name == "CHANMEM") return cfg_.channelMemory;
  if (name == "PROBE") return cfg_.probePromote;
  if (name == "MASKING") return cfg_.carrierMasking;
  if (name == "RESUME") return cfg_.resume;
  return std::nullopt;
}

bool ModemWorker::setFeature(const std::string& name, bool on) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  if (name == "CHANMEM") {
    cfg_.channelMemory = on;
  } else if (name == "PROBE") {
    cfg_.probePromote = on;
  } else if (name == "MASKING") {
    cfg_.carrierMasking = on;
  } else if (name == "RESUME") {
    cfg_.resume = on;
  } else {
    return false;
  }
  rebuildEngine();
  return true;
}

std::string ModemWorker::statusFieldsLocked(bool json) {
  const FsmState st = engine_->state();
  const std::string state =
      st == FsmState::Idle && listen_ ? "LISTENING" : fsmStateNameLocked(st);
  const bool conn = st == FsmState::Connected;
  const std::string remote =
      conn && !engine_->remoteCall().empty() ? engine_->remoteCall() : "-";
  const int m = engine_->sessionMode();
  const std::string mode =
      engine_->modePinned() ? "M" + std::to_string(m)
                            : "AUTO(M" + std::to_string(m) + ")";
  const double rsnr = engine_->remoteSnrDb();
  struct KV {
    const char* k;
    std::string v;
    bool quoted;
  };
  const uint16_t caps = engine_->sessionCaps();
  std::vector<KV> kv = {
      {"STATE", state, true},
      {"CALL", cfg_.callsign, true},
      {"REMOTE", remote, true},
      {"FAMILY", upperStr(toString(cfg_.family)), true},
      {"MODE", mode, true},
      {"NET", fmt1(engine_->netBps()), false},
      {"SNR", fmt1(engine_->lastRxSnrDb()), false},
      {"RSNR", rsnr <= -98.0 ? std::string("-") : fmt1(rsnr),
       rsnr <= -98.0},
      {"CFO", fmt1(engine_->txShiftHz()), false},
      {"RETRY", fmt1(engine_->retryRatio()), false},
      {"BUSY", chanBusy_ ? "1" : "0", false},
      {"LISTEN", listen_ ? "1" : "0", false},
      {"COMPRESS", (caps & kCapZstd) ? "1" : "0", false},
      {"CRYPTO", "0", false},
      {"EPOCH", "0", false},
      {"RESUME", (caps & kCapResume) ? "1" : "0", false},
      {"MASK", std::to_string(engine_->rxMaskedCount()), false},
      {"VOICE", "off", true},
      {"TXBUF", std::to_string(engine_->txBacklog()), false},
      {"FOK", std::to_string(engine_->framesOk()), false},
      {"FFAIL", std::to_string(engine_->framesFailed()), false},
  };
  std::string out;
  if (json) {
    out = "{";
    for (size_t i = 0; i < kv.size(); ++i) {
      std::string key = kv[i].k;
      for (auto& c : key) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
      }
      out += "\"" + key + "\":";
      out += kv[i].quoted ? "\"" + kv[i].v + "\"" : kv[i].v;
      if (i + 1 < kv.size()) out += ",";
    }
    out += "}";
  } else {
    for (size_t i = 0; i < kv.size(); ++i) {
      if (i) out += " ";
      out += std::string(kv[i].k) + "=" + kv[i].v;
    }
  }
  return out;
}

std::string ModemWorker::statusLine() {
  std::lock_guard lock(modemMutex_);
  return statusFieldsLocked(false);
}

std::string ModemWorker::statusJson() {
  std::lock_guard lock(modemMutex_);
  return statusFieldsLocked(true);
}

std::string ModemWorker::noiseFloor() {
  std::lock_guard lock(modemMutex_);
  return fmt1(floorDbfs_) + " BUSY=" + (chanBusy_ ? "1" : "0");
}

std::string ModemWorker::audioLevel() {
  std::lock_guard lock(modemMutex_);
  return "RX=" + fmt1(rxLevel_) + " TX=" + fmt1(txLevel_);
}

std::string ModemWorker::freqInfo() {
  std::lock_guard lock(modemMutex_);
  return "CENTER=" + fmt1(kCenterHz) +
         " SHIFT=" + fmt1(engine_->txShiftHz()) +
         " CFO=" + fmt1(engine_->lastRxCfoHz());
}

std::string ModemWorker::pttGet(const std::string& key) {
  std::lock_guard lock(modemMutex_);
  if (key == "METHOD") return toString(cfg_.pttMethod);
  if (key == "PORT") return cfg_.pttTarget.empty() ? "-" : cfg_.pttTarget;
  if (key == "BAUD") return std::to_string(cfg_.pttBaud);
  if (key == "LEAD") return std::to_string(cfg_.pttLeadMs);
  if (key == "TAIL") return std::to_string(cfg_.pttTailMs);
  return "-";
}

bool ModemWorker::cryptoSupported() {
#ifdef SKW_HAVE_CRYPTO
  return CryptoSession::available();
#else
  return false;
#endif
}

std::string ModemWorker::securityLine() {
#ifdef SKW_HAVE_CRYPTO
  std::lock_guard lock(modemMutex_);
  return std::string("AUTH=") + (engine_->cryptoAuth() ? "ON" : "OFF") +
         " ENCRYPT=" + (engine_->cryptoActive() ? "ON" : "OFF") +
         " EPOCH=" + std::to_string(engine_->cryptoEpoch());
#else
  return "";
#endif
}

bool ModemWorker::setEncryption(bool on) {
#ifdef SKW_HAVE_CRYPTO
  std::lock_guard lock(modemMutex_);
  // Rejected while a session exists, and ON requires an installed PSK
  // plus AES-NI (SECURITY.md).
  if (engine_->state() != FsmState::Idle) return false;
  if (on && (cfg_.cryptoPsk.size() != kPskBytes * 2 ||
             !CryptoSession::available())) {
    return false;
  }
  cfg_.cryptoEnabled = on;
  rebuildEngine();
  return true;
#else
  (void)on;
  return false;
#endif
}

bool ModemWorker::setPsk(const std::string& hex) {
#ifdef SKW_HAVE_CRYPTO
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  if (hex.size() != kPskBytes * 2) return false;
  for (char ch : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
  }
  cfg_.cryptoPsk = hex;
  rebuildEngine();
  return true;
#else
  (void)hex;
  return false;
#endif
}

bool ModemWorker::keyRotate() {
#ifdef SKW_HAVE_CRYPTO
  std::lock_guard lock(modemMutex_);
  return engine_->rotateKeys(nowSec());
#else
  return false;
#endif
}

#ifdef SKW_HAVE_VOICE
const char* ModemWorker::voiceModeNameLocked() const {
  switch (voiceMode_) {
    case VoiceMode::Connected: return "CONNECTED";
    case VoiceMode::Broadcast: return "BROADCAST";
    default: return "OFF";
  }
}
#endif

bool ModemWorker::voiceSupported() {
#ifdef SKW_HAVE_VOICE
  return true;
#else
  return false;
#endif
}

#ifdef SKW_HAVE_VOICE
namespace {
const char* voiceCodecName(VoiceCodec c) {
  switch (c) {
    case VoiceCodec::C700: return "700C";
    case VoiceCodec::C1300: return "1300";
    case VoiceCodec::C2400: return "2400";
    default: return "3200";
  }
}
const char* voiceStateName(VoiceState s) {
  switch (s) {
    case VoiceState::Closed: return "CLOSED";
    case VoiceState::Opening: return "OPENING";
    case VoiceState::Open: return "OPEN";
    case VoiceState::Requesting: return "REQUESTING";
    case VoiceState::Talking: return "TALKING";
    default: return "RECEIVING";
  }
}
}  // namespace
#endif

std::string ModemWorker::voiceModeStr() {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  switch (voiceMode_) {
    case VoiceMode::Connected: return "CONNECTED";
    case VoiceMode::Broadcast: return "BROADCAST";
    default: return "OFF";
  }
#else
  return "OFF";
#endif
}

bool ModemWorker::setVoiceMode(const std::string& m) {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  if (m == "OFF") {
    voiceMode_ = VoiceMode::Off;
    engine_->closeVoice(nowSec());
    return true;
  }
  if (m == "CONNECTED") {
    voiceMode_ = VoiceMode::Connected;
    return true;
  }
  if (m == "BROADCAST") {
    voiceMode_ = VoiceMode::Broadcast;
    return true;
  }
  return false;
#else
  (void)m;
  return false;
#endif
}

std::string ModemWorker::voiceCodecStr() {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  const std::string active = voiceCodecName(engine_->voiceCodec());
  return voiceCodecWant_ ? active : "AUTO(" + active + ")";
#else
  return "AUTO";
#endif
}

bool ModemWorker::setVoiceCodec(const std::string& c) {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  if (c == "AUTO") {
    voiceCodecWant_.reset();
    return true;
  }
  if (c == "700C") {
    voiceCodecWant_ = VoiceCodec::C700;
  } else if (c == "1300") {
    voiceCodecWant_ = VoiceCodec::C1300;
  } else if (c == "2400") {
    voiceCodecWant_ = VoiceCodec::C2400;
  } else if (c == "3200") {
    voiceCodecWant_ = VoiceCodec::C3200;
  } else {
    return false;
  }
  return true;
#else
  (void)c;
  return false;
#endif
}

bool ModemWorker::voiceStart() {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  if (voiceMode_ == VoiceMode::Off) return false;
  return engine_->openVoice(voiceCodecWant_, nowSec());
#else
  return false;
#endif
}

bool ModemWorker::voiceStop() {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  engine_->closeVoice(nowSec());
  return true;
#else
  return false;
#endif
}

bool ModemWorker::voicePtt(bool on) {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  return engine_->setVoicePtt(on, nowSec());
#else
  (void)on;
  return false;
#endif
}

std::string ModemWorker::voiceStatusLine() {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  const VoiceState st = engine_->voiceState();
  const std::string codec = voiceCodecWant_
                                ? voiceCodecName(engine_->voiceCodec())
                                : std::string("AUTO(") +
                                      voiceCodecName(engine_->voiceCodec()) +
                                      ")";
  return std::string("MODE=") + voiceModeNameLocked() + " CODEC=" + codec +
         " STATE=" + voiceStateName(st) +
         " PTT=" + (st == VoiceState::Talking ||
                            st == VoiceState::Requesting
                        ? "ON"
                        : "OFF") +
         " GRANTED=" + (st == VoiceState::Talking ? "ON" : "OFF") +
         " TXQ=" + std::to_string(engine_->voiceTxQueued()) +
         " RXQ=" + std::to_string(engine_->voiceRxDepthMs() * 8) +
         " TRANSPORT=M" + std::to_string(engine_->sessionMode());
#else
  return "";
#endif
}

void ModemWorker::submitVoicePcm(std::span<const int16_t> pcm) {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  engine_->pushVoicePcm(pcm);
#else
  (void)pcm;
#endif
}

std::vector<int16_t> ModemWorker::takeVoicePcm(size_t samples) {
#ifdef SKW_HAVE_VOICE
  std::lock_guard lock(modemMutex_);
  return engine_->takeVoicePcm(samples);
#else
  (void)samples;
  return {};
#endif
}

bool ModemWorker::pttSet(const std::string& key, const std::string& val) {
  std::lock_guard lock(modemMutex_);
  if (key == "METHOD") {
    auto m = pttMethodFromString(val);
    if (!m) return false;
    cfg_.pttMethod = *m;
    return true;
  }
  if (key == "PORT") {
    cfg_.pttTarget = val;
    return true;
  }
  const int n = atoi(val.c_str());
  if (key == "BAUD") {
    if (n <= 0) return false;
    cfg_.pttBaud = n;
    return true;
  }
  if (key == "LEAD" || key == "TAIL") {
    if (n < 0 || n > 5000) return false;
    (key == "LEAD" ? cfg_.pttLeadMs : cfg_.pttTailMs) = n;
    return true;
  }
  return false;
}

}  // namespace skw
