#include "worker.hpp"

#include <chrono>
#include <cmath>

#include "core/constants.hpp"
#include "core/log.hpp"

namespace skw {
namespace {
// Worker hop: one OFDM symbol at MEDIUM CP in audio samples keeps latency
// bounded and gives Phase 1 a natural processing granule.
constexpr size_t kChunkSamples = size_t(kFftAudio) + size_t(kCpSamplesAudio[1]);

float levelDbfs(const float* s, size_t n) {
  if (n == 0) return -120.0f;
  double acc = 0.0;
  for (size_t i = 0; i < n; ++i) acc += double(s[i]) * s[i];
  double rms = std::sqrt(acc / double(n));
  return rms > 1e-9 ? float(20.0 * std::log10(rms)) : -120.0f;
}
}  // namespace

ModemWorker::ModemWorker(const Config& cfg, Clock& clock, SampleRing& rxRing,
                         SampleRing& txRing)
    : cfg_(cfg), clock_(clock), rx_(rxRing), tx_(txRing) {
  chunk_.resize(kChunkSamples);
}

ModemWorker::~ModemWorker() { stop(); }

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
    // Phase 2 wires the command dispatcher; scaffold just logs.
    SKW_DEBUG("worker: command '%s'", c.line.c_str());
  }
}

size_t ModemWorker::processChunk() {
  size_t got = rx_.read(chunk_);
  if (got == 0) return 0;

  working_.clockUs = clock_.nowUs();
  working_.rxSamplesTotal += got;
  working_.rxLevelDbfs = levelDbfs(chunk_.data(), got);

  // Phase 0 pass-through: the "modem" copies RX to TX unchanged.
  size_t put = tx_.write({chunk_.data(), got});
  working_.txSamplesTotal += put;
  working_.txLevelDbfs = working_.rxLevelDbfs;
  if (put < got) {
    SKW_WARN("worker: tx ring overrun, dropped %zu samples", got - put);
  }
  publishTelemetry();
  return got;
}

size_t ModemWorker::runOnce() {
  applyPendingCommands();
  return processChunk();
}

void ModemWorker::threadMain() {
  SKW_INFO("worker: started (chunk %zu samples)", kChunkSamples);
  while (running_.load(std::memory_order_relaxed)) {
    applyPendingCommands();
    if (processChunk() == 0) {
      // Nothing to do; sleep well under one chunk duration.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  SKW_INFO("worker: stopped");
}

}  // namespace skw
