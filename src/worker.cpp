#include "worker.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/constants.hpp"
#include "sim/chansim.hpp"
#include "core/log.hpp"
#include "link/frame.hpp"
#include "dsp/fft.hpp"
#include "phy/framer.hpp"

namespace skw {
namespace {

// Worker hop: ~one OFDM symbol of audio keeps latency bounded and gives
// the DSP a natural processing granule (fixed size; profile-agnostic).
constexpr size_t kChunkSamples = size_t(kFftAudio) + 192;

// RX capture-window retry ladder (baseband seconds after the detected
// preamble start): the frame length is unknown until the self-describing
// header decodes, so the window grows until the engine takes the frame.
// The last rung exceeds the longest legal frame on air.
// The 60/90 s rungs exist for the CSS deep tier: a MID/NARROW M0
// handshake frame runs 35-55 s on air.
constexpr double kRxWindowSec[] = {2.5, 5.0, 10.0, 20.0, 34.0, 60.0, 90.0};
constexpr int kRxTries = 7;
constexpr double kRxPreMarginSec = 0.15;
// Trailing margin past the header-implied burst end before a failing
// capture is finalized (PTT tail, shuttle/driver jitter).
constexpr double kBurstEndMarginSec = 0.5;
constexpr double kRxHistorySec = 94.0;
// Cap on simultaneously open captures. MEASURED 2026-09-07 (kill/resume
// hammer, first run of a gate that had never been wired up): when a peer
// dies mid-session the survivor retransmits for ~110 s, and a station
// that cannot decode any of it - a freshly restarted peer, or any
// listener - opens one capture per detected preamble and retires none.
// The backlog reached 33-38 captures, the history buffer pinned at its
// full 94 s (every capture's start must be kept), and since each capture
// ends in a 90 s-window HARQ decode the receiver fell BEHIND REAL TIME:
// the front capture gained ~1 s of audio per 1.2 s of wall clock. The
// station was then deaf for minutes. The peer's subsequent CALLs were
// heard (block level showed signal) and never decoded, so the session
// could not be re-established at all.
//
// A newly detected preamble is worth strictly more than a capture that
// has already failed several rungs, so the oldest is dropped. Eight is
// far above any legitimate overlap - a real one is 1-3, and the CSS deep
// tier's 35-55 s frames still only ever have a couple open at once.
// SKW_MAX_PENDING_RX is the A/B.
inline size_t maxPendingRx() {
  static const size_t v = [] {
    const char* e = std::getenv("SKW_MAX_PENDING_RX");
    const long n = e ? std::atol(e) : 8;
    return n > 0 ? size_t(n) : size_t(8);
  }();
  return v;
}

// How long a busy period is presumed to be a station before the chirp
// detector's silence reclassifies it as noise. Must exceed the detector's
// decision lag (~2.0 s fast / ~3.3 s for a 4-rep calling signature, see
// the ROADMAP 1.6 deviation) or we would key over real preambles.
constexpr double kBusyClassifySec = 4.0;

// Quality bar for opening a capture window from the ALTERNATE family's
// detector. The primary detector's own threshold is tuned against its
// family's acquisition floor; a cross-family detection is a rarer event
// with more ways to be wrong, and each false one costs a real frame a
// decode attempt from the per-chunk budget.
constexpr float kAltFamilyMinQuality = 0.35f;

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
  // Channel memory persists to the config directory (LINK_ADAPTATION.md).
  chanMemPath_ = defaultChanMemPath();
  if (!chanMemPath_.empty() && chanMem_.load(chanMemPath_)) {
    SKW_INFO("chanmem: %zu entries from %s", chanMem_.entries(),
             chanMemPath_.c_str());
  }
  rebuildEngine();
}

ModemWorker::~ModemWorker() {
  stop();
  saveChanMem();
}

// Busy-source discrimination for the LBT gate (DSP.md "Pre-TX CCA").
//
// `chanBusy_` is an honest energy meter and stays that way for STATUS and
// EVT:BUSY. But deferring to *energy* means a continuous in-band jammer
// reads as permanently occupied and starves a jammed-but-workable link -
// measured as 0 bytes delivered where the pre-LBT modem still passed
// traffic. What listen-before-talk actually needs to know is whether a
// STATION is on air, and a chirp preamble is exactly that evidence.
//
// The detector needs a couple of seconds to reach a verdict, so a fresh
// busy period is assumed to be a peer until that lag has passed: keying
// over a burst whose preamble we have not yet classified is the failure
// this whole mechanism exists to prevent.
bool ModemWorker::peerBusyLocked(double now) const {
  if (!chanBusy_) return false;
  if (busyIsPeer_) return true;
  if (busySince_ < 0.0) return true;  // busy but unclassified: defer
  return (now - busySince_) < kBusyClassifySec;
}

// RX spectrum for the GUI waterfall. Welch-style: a Hann-windowed FFT
// over a fixed block, magnitude-averaged into the telemetry bins. Rate
// limited to ~10 Hz - the GUI renders at frame rate and a spectrum per
// audio chunk would be pure waste in the worker's hot path.
void ModemWorker::updateSpectrumLocked(std::span<const cfloat> bb) {
  constexpr int kFftN = 2 * kSpectrumBins;  // real bins = N/2
  specAcc_.insert(specAcc_.end(), bb.begin(), bb.end());
  if (specAcc_.size() < size_t(kFftN)) return;

  const double now = nowSec();
  if (now - specLastAt_ < 0.1) {
    // Keep the buffer bounded while rate-limited.
    if (specAcc_.size() > size_t(4 * kFftN)) {
      specAcc_.erase(specAcc_.begin(),
                     specAcc_.end() - long(2 * kFftN));
    }
    return;
  }
  specLastAt_ = now;

  static const Fft fft(kFftN);
  std::vector<cfloat> in(kFftN), out(kFftN);
  const size_t base = specAcc_.size() - size_t(kFftN);
  for (int i = 0; i < kFftN; ++i) {
    // Hann window: without it a strong carrier smears across the whole
    // display and the waterfall shows nothing useful.
    const float w =
        0.5f * (1.0f - std::cos(2.0f * float(M_PI) * float(i) /
                                float(kFftN - 1)));
    in[size_t(i)] = specAcc_[base + size_t(i)] * w;
  }
  fft.forward(in.data(), out.data());
  specAcc_.clear();

  if (specAvg_.size() != size_t(kSpectrumBins)) {
    specAvg_.assign(size_t(kSpectrumBins), 0.0f);
  }
  // The baseband is complex and centred on the waveform centre, so bin 0
  // is DC and the display spans the positive half.
  for (int k = 0; k < kSpectrumBins; ++k) {
    const float mag = std::abs(out[size_t(k)]) / float(kFftN);
    specAvg_[size_t(k)] += 0.35f * (mag - specAvg_[size_t(k)]);
    working_.spectrumDbfs[k] =
        specAvg_[size_t(k)] > 1e-9f
            ? float(20.0 * std::log10(double(specAvg_[size_t(k)])))
            : -120.0f;
  }
  working_.spectrumSeq = ++specSeq_;
}

// Passive monitor pass over a capture window. Runs for BOTH decode paths
// (the burst-end fast path and the retry ladder) so third-party traffic is
// reported regardless of which one took the frame.
void ModemWorker::runMonitorLocked(std::span<const cfloat> win) {
  if (!monitorOn_ || !monitor_) return;
  monitor_->setLocalConnected(engine_->state() == FsmState::Connected);
  auto ev = monitor_->rxBurst(win);
  // Broadcast chains complete inside the Monitor; drain them whether or
  // not this particular frame produced an event line.
  auto bcasts = monitor_->takeBroadcasts();
  if (!ev && bcasts.empty()) return;

  std::lock_guard lock(bridgeMutex_);
  if (ev) {
    notes_.push_back(ev->monLine);
    if (!ev->binaryRecord.empty()) {
      monRecords_.push_back(ev->binaryRecord);
    }
  }
  // A reassembled CQ/BEACON/BCAST_DATA payload was being built by the
  // Monitor and then thrown away - nothing surfaced it on either port.
  // It now rides the control port as its own notification, which is the
  // only surface available while a session holds the data port.
  // Each entry carries the callsign its chain was attributed to, so
  // overlapping senders are reported separately rather than merged.
  for (const auto& [src, payload] : bcasts) {
    notes_.push_back(evtBroadcast(src, payload));
  }
}

// --- channel memory ----------------------------------------------------------

std::string ModemWorker::defaultChanMemPath() {
  // SKW_CHANMEM_PATH overrides the location; set it empty (or to "-") to
  // run without persistence at all, which is what the tests do so a suite
  // run never writes to the operator's real store.
  if (const char* over = getenv("SKW_CHANMEM_PATH")) {
    const std::string p(over);
    return (p.empty() || p == "-") ? std::string{} : p;
  }
  const char* home = getenv("HOME");
  if (!home || !*home) return {};
  const std::string dir = std::string(home) + "/.skywave";
  // Best-effort: a missing directory simply means no persistence.
  ::mkdir(dir.c_str(), 0700);
  return dir + "/chanmem.bin";
}

// Written on teardown and, so a crash cannot lose a day of learning, at
// most every 60 s while sessions are ending.
void ModemWorker::saveChanMem() {
  if (chanMemPath_.empty() || chanMem_.entries() == 0) return;
  chanMem_.save(chanMemPath_);
}

// --- physical keying ---------------------------------------------------------

void ModemWorker::pttKeyLocked(double now) {
  if (pttKeyed_) return;
  pttKeyed_ = true;
  pttKeyedAt_ = now;
  txWatchdogTripped_ = false;
  if (ptt_ && !ptt_->key()) {
    SKW_WARN("ptt: %s key failed: %s", ptt_->name(),
             ptt_->lastError().c_str());
  }
  working_.ptt = true;
}

void ModemWorker::pttUnkeyLocked() {
  if (!pttKeyed_) return;
  pttKeyed_ = false;
  if (ptt_ && !ptt_->unkey()) {
    SKW_WARN("ptt: %s unkey failed: %s", ptt_->name(),
             ptt_->lastError().c_str());
  }
  working_.ptt = false;
}

// TX watchdog (DSP.md / INTERFACES.md): a transmission that outlives
// modem.tx_watchdog_s force-releases the key and discards whatever audio is
// still staged. This is a safety net against a wedged modem holding a
// transmitter down, not a tuning knob - tripping it is always a bug, so it
// is loud and it surfaces on the control port.
void ModemWorker::pttWatchdogLocked(double now) {
  if (!pttKeyed_ || cfg_.txWatchdogSec <= 0.0) return;
  if (now - pttKeyedAt_ < cfg_.txWatchdogSec) return;

  SKW_ERROR("TX WATCHDOG: keyed %.1f s (limit %.1f s) - forcing unkey",
            now - pttKeyedAt_, cfg_.txWatchdogSec);
  pttUnkeyLocked();
  txWatchdogTripped_ = true;
  txPending_.clear();
  txPendingOff_ = 0;
  txTestUntil_ = 0.0;
  {
    std::lock_guard lock(bridgeMutex_);
    notes_.push_back(evtTxWatchdog(now - pttKeyedAt_));
  }
}

void ModemWorker::rebuildEngine() {
  // Numerology profile FIRST: every DSP object constructed below reads
  // the active band's family plans and center.
  setActiveBand(cfg_.band);
  const BandProfile& prof = activeProfile();
  EngineConfig ec;
  ec.policy.ownCall = cfg_.callsign;
  ec.policy.family = cfg_.family;
  ec.policy.maxMode = uint8_t(cfg_.maxMode);
  uint16_t caps = kCapZstd | kCapBurstMac;
  // Scheduled turn-taking (PROTOCOL.md "Turn-Taking"): INI
  // `modem.turn_taking` (default on); SKW_NO_TURNSCHED is the A/B
  // override for measuring against the legacy occupancy-gate scheduler.
  if (cfg_.turnTaking && !getenv("SKW_NO_TURNSCHED")) caps |= kCapTurnSched;
  if (cfg_.resume) caps |= kCapResume;
  if (cfg_.carrierMasking && prof.masking) caps |= kCapMasking;
  if (cfg_.harq != HarqMode::Chase) caps |= kCapHarqIr;
  const bool css = cfg_.cssTier && prof.cssTier;
  if (css) caps |= kCapCssTier;
  ec.policy.cssEnable = css;
  ec.policy.caps = caps;
  ec.arqMaxRetries = cfg_.maxRetries;
  // `CHANMEM OFF` withholds the pointer, which disables seeding AND
  // recording - a stored flag that only gated the seed would keep
  // polluting the store while claiming to be off.
  ec.chanMem = cfg_.channelMemory ? &chanMem_ : nullptr;
  ec.freqBucketKhz = freqBucketKhz_;
  ec.maskingEnabled = cfg_.carrierMasking && prof.masking;
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
  // The constellation tap costs a divide per symbol and an allocation
  // per frame, so it is paid for only when something renders it.
  engine_->setWantConstellation(cfg_.guiEnabled);
  if (cfg_.cpMode >= 0) {
    static const char* kCp[] = {"SHORT", "MEDIUM", "LONG"};
    engine_->setCpPolicy(kCp[std::clamp(cfg_.cpMode, 0, 2)], nowSec());
  }
  monitor_.emplace(cfg_.family);
  // Physical keying follows the config; releasing the old driver first
  // guarantees a method change cannot strand a keyed transmitter.
  if (ptt_) pttUnkeyLocked();
  ptt_ = makePttDriver(cfg_.pttMethod, cfg_.pttTarget, cfg_.pttBaud);
  if (cfg_.pttMethod != PttMethod::None && !ptt_->healthy()) {
    SKW_WARN("ptt: %s unavailable (%s) - transmitting without keying",
             ptt_->name(), ptt_->lastError().c_str());
  }
  det_ = ChirpDetector(cfg_.family);
  detAlt_.reset();
  if (cfg_.familyFallback) {
    const Family alt = narrowerFamily(cfg_.family);
    if (alt != cfg_.family) detAlt_.emplace(alt);
  }
  detsAlt_.clear();
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
      // Noise floor = MINIMUM block level over a 15 s window (longer
      // than any legal burst, so a transmission cannot pull the floor
      // up mid-burst; a >15 s continuous interferer becomes the new
      // floor, which is exactly what LBT should adapt to). The old
      // ratchet (fast down / +0.02 dB per block up) started at
      // -120 dBFS and needed ~3 MINUTES to climb to a real ambient
      // floor - during which EVERY block read "busy" and the whole
      // LBT/CCA machinery ran on cap-expiries instead of real channel
      // state (the root of the pinned-busy pathologies and the long
      // handshake turn gaps observed live).
      floorBlocks_.push_back(blockDbfs_);
      if (floorBlocks_.size() > 150) floorBlocks_.pop_front();
      floorDbfs_ =
          *std::min_element(floorBlocks_.begin(), floorBlocks_.end());
      const bool wasBusy = chanBusy_;
      chanBusy_ = blockDbfs_ > floorDbfs_ + 6.0f;
      if (chanBusy_ && !wasBusy) {
        busySince_ = now;      // start of a new busy period
        busyIsPeer_ = false;   // nothing has claimed it yet
      } else if (!chanBusy_) {
        busySince_ = -1.0;
        busyIsPeer_ = false;
      }
    }
  }

  updateSpectrumLocked(bb);

  hist_.insert(hist_.end(), bb.begin(), bb.end());
  dets_.clear();
  det_.process(bb, dets_);
  // Second acquisition path, active only while no session exists.
  //
  // Alternate-family detections are ADDITIVE to the decode budget, so a
  // false one costs a real frame its capture attempt. Two guards keep
  // that cheap: the primary family wins outright when it detected
  // anything in this chunk, and the alternate must clear a higher
  // quality bar than the primary's own threshold before it opens a
  // window.
  if (detAlt_ && dets_.empty() &&
      engine_->state() != FsmState::Connected &&
      engine_->state() != FsmState::Disconnecting) {
    detsAlt_.clear();
    detAlt_->process(bb, detsAlt_);
    for (const auto& d : detsAlt_) {
      if (d.quality >= kAltFamilyMinQuality) dets_.push_back(d);
    }
  } else if (detAlt_) {
    // Keep the detector's internal history coherent without acting on it,
    // so it is warm the moment the session ends.
    detsAlt_.clear();
    detAlt_->process(bb, detsAlt_);
  }
  for (const auto& d : dets_) {
    const double margin = kRxPreMarginSec * kFsBaseband;
    const double s = std::max(0.0, d.startSample - margin);
    // Bounded backlog: see maxPendingRx(). Dropping the OLDEST keeps the
    // freshest evidence and, because the history trim below must retain
    // every open capture's start, also bounds hist_.
    while (pendingRx_.size() >= maxPendingRx()) {
      pendingRx_.pop_front();
      ++rxCapturesDropped_;
    }
    pendingRx_.push_back({uint64_t(s), 0});
    // A chirp preamble proves this busy period is a STATION, not noise.
    // The claim is sticky for the whole busy period and is dropped the
    // moment the channel clears, so one stray classification cannot mark
    // a jammer as a peer indefinitely.
    busyIsPeer_ = true;
  }

  // Grow each pending capture along the retry ladder until the engine
  // takes the frame (or the ladder ends and the monitor gets one look).
  const uint64_t histEnd = histBase_ + hist_.size();
  // Decode-attempt budget PER CALL: long-window attempts (up to 90 s of
  // audio through the full receiver) run under the modem mutex, and
  // draining a capture's whole ladder - or several captures - in one
  // pass stalled the control port for 20+ s during retransmission
  // churn. Two attempts per chunk bounds the hold time; pending
  // captures simply continue next round (~30 ms later in real time).
  int decodeBudget = 2;
  for (auto it = pendingRx_.begin(); it != pendingRx_.end();) {
    bool done = false;
    // BURST-END attempt: the ladder's window sizes are coarse
    // (2.5/5/10/20... s), so a burst slightly longer than one window
    // used to sit UNDECODED until the next window's worth of audio
    // accumulated - 4+ s of dead air after the burst already ended,
    // observed live as 5-10 s turn-taking gaps on MID/NARROW (whose
    // handshake bursts outgrow the 5 s window; WIDE's fit). When the
    // channel just went QUIET, decode NOW with exactly the audio that
    // exists - once per quiet period per capture.
    if (getenv("SKW_RXLADDER_DEBUG") &&
        now - ladderDbgAt_ > 1.0) {
      ladderDbgAt_ = now;
      fprintf(stderr,
              "%s ladder t=%.1f busy=%d blk=%.1f floor=%.1f tries=%d "
              "haveSec=%.1f pend=%zu histSec=%.1f\n",
              cfg_.callsign.c_str(), now, chanBusy_ ? 1 : 0,
              double(blockDbfs_), double(floorDbfs_), it->tries,
              double(histEnd - it->start) / kFsBaseband, pendingRx_.size(),
              double(hist_.size()) / kFsBaseband);
      if (rxCapturesDropped_ != lastDroppedDbg_) {
        fprintf(stderr, "%s capture-backlog dropped=%llu\n",
                cfg_.callsign.c_str(),
                (unsigned long long)rxCapturesDropped_);
        lastDroppedDbg_ = rxCapturesDropped_;
      }
    }
    // BURST-END FINALIZATION - runs FIRST, ahead of the quiet-try and the
    // rungs, so a capture whose burst is provably in hand retires before
    // anything else spends the per-chunk decode budget on it. Every rung
    // past the burst's actual end re-decodes the SAME samples plus more
    // trailing noise: a marginal frame that failed with the whole burst
    // in hand fails identically at 10/20/34/60/90 s, and only the LAST
    // rung runs HARQ combining, counts FFAIL, feeds the AMC - and (now)
    // answers the peer with a NAK. That climb cost 30-90 s per failing
    // frame and was the measured floor of one-way throughput under
    // fading. The PHY header survives payload failure and states the
    // burst's exact airtime, so once that much audio (plus pre-roll and
    // tail margin) exists, run the final attempt on exactly that window
    // and retire the capture. Header-less captures (noise, or a burst
    // too deep to sync) keep the ladder.
    // SANITY: the header's 12-bit length admits 4095 bytes - ~1000 s of
    // implied airtime at the CSS deep tier, from a genuinely long frame
    // or a header whose CRC-16 passed falsely at -6 dB. A span beyond
    // the ladder's own 90 s envelope never drives finalization; such a
    // capture keeps the ordinary ladder and retires at the 90 s rung.
    if (!done && it->airtimeSec > 0.0 &&
        it->airtimeSec <= kRxWindowSec[kRxTries - 1] && it->tries < kRxTries &&
        decodeBudget > 0) {
      const uint64_t span = uint64_t(
          (it->airtimeSec + kRxPreMarginSec + kBurstEndMarginSec) *
          kFsBaseband);
      if (histEnd >= it->start + span) {
        const uint64_t w0 = std::max(it->start, histBase_);
        const uint64_t w1 = std::min(histEnd, it->start + span);
        std::span<const cfloat> win(hist_.data() + size_t(w0 - histBase_),
                                    size_t(w1 - w0));
        const bool engineEligible =
            listen_ || engine_->state() != FsmState::Idle;
        if (getenv("SKW_RXLADDER_DEBUG")) {
          fprintf(stderr, "%s burst-end-final t=%.1f air=%.1f tries=%d\n",
                  cfg_.callsign.c_str(), now, it->airtimeSec, it->tries);
        }
        --decodeBudget;
        if (engineEligible) engine_->rxBurst(win, now, /*final=*/true);
        runMonitorLocked(win);
        done = true;
      }
    }
    if (!done && decodeBudget > 0 && it->tries < kRxTries && !chanBusy_ &&
        it->quietTriedTries != it->tries && histEnd > it->start) {
      const uint64_t have = histEnd - it->start;
      const uint64_t nextNeed =
          uint64_t(kRxWindowSec[it->tries] * kFsBaseband);
      // Only worthwhile between ladder rungs: enough audio that the
      // PREVIOUS rung failed (or 1.5 s minimum), but short of the next.
      const double prevSec = it->tries > 0 ? kRxWindowSec[it->tries - 1]
                                           : 1.5;
      if (have < nextNeed && have > uint64_t(prevSec * kFsBaseband)) {
        it->quietTriedTries = it->tries;
        if (getenv("SKW_RXLADDER_DEBUG")) {
          fprintf(stderr, "%s quiet-try t=%.1f haveSec=%.1f tries=%d\n",
                  cfg_.callsign.c_str(), now,
                  double(have) / kFsBaseband, it->tries);
        }
        const uint64_t w0 = std::max(it->start, histBase_);
        std::span<const cfloat> win(hist_.data() + size_t(w0 - histBase_),
                                    size_t(histEnd - w0));
        const bool engineEligible =
            listen_ || engine_->state() != FsmState::Idle;
        --decodeBudget;
        if (engineEligible && engine_->rxBurst(win, now, false)) {
          // The burst-end fast path used to return without ever running
          // the monitor, so every frame it caught was invisible to
          // MONITOR clients.
          runMonitorLocked(win);
          done = true;
        } else if (engineEligible) {
          it->airtimeSec =
              std::max(it->airtimeSec, engine_->lastRxBurstAirtimeSec());
        }
      }
    }
    while (!done && it->tries < kRxTries && decodeBudget > 0) {
      const uint64_t need =
          it->start + uint64_t(kRxWindowSec[it->tries] * kFsBaseband);
      if (histEnd < need) break;  // wait for more audio
      const uint64_t w0 = std::max(it->start, histBase_);
      std::span<const cfloat> win(hist_.data() + size_t(w0 - histBase_),
                                  size_t(std::min(need, histEnd) - w0));
      const bool engineEligible =
          listen_ || engine_->state() != FsmState::Idle;
      const bool decoded =
          engineEligible
              ? engine_->rxBurst(win, now, it->tries == kRxTries - 1)
              : false;
      --decodeBudget;
      ++it->tries;
      if (decoded || it->tries >= kRxTries) {
        runMonitorLocked(win);
        done = true;
        break;
      }
      if (engineEligible) {
        it->airtimeSec =
            std::max(it->airtimeSec, engine_->lastRxBurstAirtimeSec());
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

// SKW_TX_IMPAIR (TESTING.md, hardware impairment runs): apply the
// Watterson channel simulator to our OWN transmit baseband before the
// sound device, so REAL audio paths (ST3 virtual cables, real rigs on a
// bench) carry controlled-impairment signal with real device timing.
// Format: "preset=awgn|good|moderate|poor,snr=<dB>,cfo=<Hz>,ppm=<f>,
//          dropms=<ms>,dropevery=<sec>"  (all keys optional)
namespace {
struct TxImpair {
  bool on = false;
  ChansimParams base;
  double dropMs = 0.0, dropEvery = 0.0;
};
TxImpair parseTxImpair() {
  TxImpair t;
  const char* e = getenv("SKW_TX_IMPAIR");
  if (!e || !*e) return t;
  t.on = true;
  t.base = ChansimParams{};
  t.base.fading = false;
  t.base.snr3kDb = 99.0;
  std::string spec(e);
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string kv = spec.substr(pos, end - pos);
    pos = end + 1;
    size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "preset") {
      if (v == "good") t.base = ccirGood();
      else if (v == "moderate") t.base = ccirModerate();
      else if (v == "poor") t.base = ccirPoor();
      else { t.base = ChansimParams{}; t.base.fading = false; }
      t.base.snr3kDb = 99.0;
    } else if (k == "snr") t.base.snr3kDb = std::atof(v.c_str());
    else if (k == "cfo") t.base.cfoHz = std::atof(v.c_str());
    else if (k == "ppm") t.base.clockPpm = std::atof(v.c_str());
    else if (k == "dropms") t.dropMs = std::atof(v.c_str());
    else if (k == "dropevery") t.dropEvery = std::atof(v.c_str());
  }
  return t;
}
}  // namespace

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
      static const bool txDbg = getenv("SKW_TX_DEBUG") != nullptr;
      if (txDbg) {
        fprintf(stderr, "%s TXBURST t=%.1f n=%zu (%.2fs)\n",
                cfg_.callsign.c_str(), now, burst->size(),
                double(burst->size()) / kFsBaseband);
      }
      // PTT lead/tail: silence padding the rig's keying swallows.
      const size_t lead = size_t(cfg_.pttLeadMs) * kFsBaseband / 1000;
      const size_t tail = size_t(cfg_.pttTailMs) * kFsBaseband / 1000;
      std::vector<cfloat> padded(lead, cfloat(0, 0));
      padded.insert(padded.end(), burst->begin(), burst->end());
      padded.insert(padded.end(), tail, cfloat(0, 0));
      static const TxImpair txImpair = parseTxImpair();
      if (txImpair.on) {
        ChansimParams p = txImpair.base;
        // SNR references THIS burst's measured power, so `snr=` means
        // what it says regardless of PAPR normalization.
        double pow = 0.0;
        for (const auto& v : padded) {
          pow += double(v.real()) * v.real() + double(v.imag()) * v.imag();
        }
        p.refSignalPower = pow / double(padded.size());
        p.seed = 0x5eed + (++txImpairSeq_) * 7919;
        padded = Chansim::run(p, padded);
        if (txImpair.dropEvery > 0.0 && txImpair.dropMs > 0.0 &&
            now - lastTxDropAt_ >= txImpair.dropEvery) {
          lastTxDropAt_ = now;
          const size_t n =
              std::min(padded.size(),
                       size_t(txImpair.dropMs * 1e-3 * kFsBaseband));
          const size_t at = padded.size() > n
                                ? (txImpairSeq_ * 2654435761u) %
                                      (padded.size() - n)
                                : 0;
          std::fill(padded.begin() + long(at),
                    padded.begin() + long(at + n), cfloat(0, 0));
        }
      }
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
    // Key BEFORE the first sample enters the ring. An empty ring means the
    // audio callback starts consuming within one callback period, so the
    // burst's own lead-silence padding is what actually covers the rig's
    // keying latency - which is exactly what ptt_lead_ms is specified to do.
    if (!pttKeyed_) pttKeyLocked(now);
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
    // Everything staged has been handed to the ring; release the key only
    // once the ring itself has drained, or the tail would be cut off.
    if (pttKeyed_ && tx_.readAvailable() == 0) pttUnkeyLocked();
  }
  pttWatchdogLocked(now);
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
    // Leaving an in-progress handshake for Idle is a failed ATTEMPT.
    const bool wasAttempting =
        st == FsmState::Idle && (lastState_ == FsmState::Calling ||
                                 lastState_ == FsmState::WaitStartAck ||
                                 lastState_ == FsmState::CallAnswered);
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
    // A FAILED CONNECTION ATTEMPT used to report NOTHING. The event was
    // gated on having reached Connected, so a station that called, was
    // answered, and then fell back to LISTENING told the application
    // only that its state had changed - no cause, no distinction
    // between "nobody answered", "the peer refused" and "the peer never
    // acknowledged my START". That hole is why the kill/resume
    // reconnect failure had to be chased through per-frame RX traces.
    // Attempts now report too, with a cause specific enough to act on.
    if (wasConnected || wasAttempting) {
      const auto cause = engine_->lastDiscCause();
      const char* reason = "LINK";
      switch (cause) {
        case FsmDiscCause::LocalRequest:
        case FsmDiscCause::PeerRequest:
          reason = "NORMAL";
          break;
        case FsmDiscCause::CallRetriesExhausted:
          reason = wasConnected ? "LINK" : "NO_ANSWER";
          break;
        case FsmDiscCause::StartRetriesExhausted:
          reason = wasConnected ? "LINK" : "NO_START_ACK";
          break;
        case FsmDiscCause::StartNak:
          reason = wasConnected ? "LINK" : "REFUSED";
          break;
        case FsmDiscCause::IdleTimeout:
          reason = wasConnected ? "LINK" : "TIMEOUT";
          break;
        default:
          break;
      }
      notes_.push_back(evtDisconnected(reason));
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

  // EVT:SNR (INTERFACES.md). Rate-limited by CHANGE, not by clock: a
  // 1 dB step is the smallest move worth a UI update, and a quiet link
  // then costs nothing. Without this a client can only learn link quality
  // by polling STATUS.
  if (st == FsmState::Connected) {
    const double local = engine_->lastRxSnrDb();
    const double remote = engine_->remoteSnrDb();
    const bool remoteValid = remote > -98.0;
    if (std::abs(local - lastSnrEvt_) >= 1.0 ||
        (remoteValid && std::abs(remote - lastRemoteSnrEvt_) >= 1.0)) {
      lastSnrEvt_ = local;
      lastRemoteSnrEvt_ = remoteValid ? remote : lastRemoteSnrEvt_;
      std::lock_guard lock(bridgeMutex_);
      notes_.push_back(evtSnr(local, remote, remoteValid));
    }
  } else {
    lastSnrEvt_ = -999.0;
    lastRemoteSnrEvt_ = -999.0;
  }

  // EVT:CFO_WARN (INTERFACES.md): the TX alignment shift has run past the
  // +-40 Hz envelope, which means the dial itself needs retuning - the
  // per-side clamp cannot correct any further. Edge-triggered with 5 Hz of
  // hysteresis so a value parked at the clamp does not spam the client.
  {
    const double shift = std::abs(engine_->txShiftHz());
    if (!cfoWarned_ && shift >= 40.0) {
      cfoWarned_ = true;
      std::lock_guard lock(bridgeMutex_);
      notes_.push_back(evtCfoWarn(engine_->txShiftHz()));
    } else if (cfoWarned_ && shift < 35.0) {
      cfoWarned_ = false;
    }
  }
}

size_t ModemWorker::processChunk() {
  std::lock_guard lock(modemMutex_);
  const double now = nowSec();
  const size_t availBefore = rx_.readAvailable();
  pumpRxLocked(now);
  engine_->setChannelBusy(peerBusyLocked(now));  // Pre-TX CCA gate input
  engine_->setTxQueueSec(
      (double(txPending_.size() - txPendingOff_) + double(tx_.readAvailable())) /
      double(kFsAudio));
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
  working_.ptt = pttKeyed_;
  // GUI instrument taps.
  if (engine_->constellationSeq() != working_.constellationSeq) {
    working_.constellationSeq = engine_->constellationSeq();
    const auto& pts = engine_->lastConstellation();
    // Take the MOST RECENT points: the tail of a frame is the freshest
    // view of the channel, and the head can still be settling.
    const size_t n = std::min<size_t>(pts.size(), kConstellationPts);
    working_.constellationN = uint16_t(n);
    for (size_t i = 0; i < n; ++i) {
      const auto& z = pts[pts.size() - n + i];
      working_.constellationI[i] = z.real();
      working_.constellationQ[i] = z.imag();
    }
  }
  {
    const auto& m = engine_->rxMask();
    working_.maskCarriers = uint16_t(plan(cfg_.family).carriers());
    for (size_t i = 0; i < sizeof(working_.maskBits); ++i) {
      working_.maskBits[i] = m.bits[i];
    }
  }
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
    } else {
      // Fairness gap: under continuous audio this loop re-acquires the
      // modem mutex back-to-back and pthread mutexes are NOT fair - the
      // GUI/control threads starved for SECONDS (beachball) while the
      // detector ran chunk after chunk. One millisecond costs ~3% of
      // the 30.7 ms real-time chunk budget and guarantees waiters a
      // window.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

double ModemWorker::netBps() {
  std::lock_guard lock(modemMutex_);
  return engine_->netBps();
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
  if (name == "ULTRA") return cfg_.ultraEnable;
  if (name == "PROBE") return cfg_.probePromote;
  if (name == "MASKING") return cfg_.carrierMasking;
  if (name == "RESUME") return cfg_.resume;
  if (name == "ULTRA") return cfg_.ultraEnable;
  return std::nullopt;
}

bool ModemWorker::setFeature(const std::string& name, bool on) {
  std::lock_guard lock(modemMutex_);
  if (engine_->state() != FsmState::Idle) return false;
  if (name == "CHANMEM") {
    cfg_.channelMemory = on;
  } else if (name == "ULTRA") {
    // GUI/control-port gate for the ULTRA family (config
    // modem.ultra_enable). Without this the FAMILY ULTRA command was
    // simply refused with no way to enable it at runtime.
    cfg_.ultraEnable = on;
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
      {"TURN",
       !engine_->turnSchedActive() ? "-"
       : engine_->turnIss()        ? "ISS"
                                   : "IRS",
       true},
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

// `FREQINFO <hz>`: the dial frequency the companion app is tuned to. It
// keys channel memory into 1 kHz buckets so the store distinguishes a peer
// worked on 40 m from the same peer on 20 m. 0 clears it.
bool ModemWorker::setDialFreqHz(double hz) {
  if (hz < 0.0 || hz > 3e10 || !std::isfinite(hz)) return false;
  std::lock_guard lock(modemMutex_);
  freqBucketKhz_ = hz > 0.0 ? int32_t(hz / 1000.0) : -1;
  engine_->setFreqBucketKhz(freqBucketKhz_);
  return true;
}

std::string ModemWorker::freqInfo() {
  std::lock_guard lock(modemMutex_);
  return "CENTER=" + fmt1(centerHz()) +
         " SHIFT=" + fmt1(engine_->txShiftHz()) +
         " CFO=" + fmt1(engine_->lastRxCfoHz());
}

std::string ModemWorker::pttGet(const std::string& key) {
  std::lock_guard lock(modemMutex_);
  // Canonical UPPERCASE on the wire like every other response; core
  // toString() stays lowercase for INI files (CONFIGURATION.md). The
  // same split was fixed for FAMILY at the M5 gate and missed here.
  if (key == "METHOD") return upperStr(toString(cfg_.pttMethod));
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
