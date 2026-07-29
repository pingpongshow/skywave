// Modem worker thread scaffold (ARCHITECTURE.md execution model).
//
// Phase 0: the worker is a pass-through (RX ring -> TX ring) proving the
// thread model end to end. Phase 1+ replaces processChunk() with the PHY.
//
// Crossings:
//   audio <-> worker : SPSC sample rings (lock-free)
//   worker -> others : seqlock-style versioned Telemetry snapshot
//   others -> worker : mutex-guarded command queue, applied at chunk bounds
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "io/ring.hpp"

namespace skw {

// POD snapshot published by the worker; readers (GUI, TCP, main) copy it.
struct Telemetry {
  int64_t clockUs = 0;
  uint64_t rxSamplesTotal = 0;
  uint64_t txSamplesTotal = 0;
  float rxLevelDbfs = -120.0f;
  float txLevelDbfs = -120.0f;
  // Link-layer fields arrive in Phase 2+.
};

struct WorkerCommand {
  std::string line;  // control-port command syntax; parsed by the worker
};

class ModemWorker {
 public:
  ModemWorker(const Config& cfg, Clock& clock, SampleRing& rxRing,
              SampleRing& txRing);
  ~ModemWorker();

  void start();
  void stop();

  // Thread-safe: queue a command; the worker applies it between chunks.
  void postCommand(WorkerCommand cmd);

  // Thread-safe: copy the latest consistent telemetry snapshot.
  Telemetry telemetry() const;

  // Test/file-mode hook: run one processing iteration on the calling thread
  // instead of using the background thread. Returns samples processed.
  size_t runOnce();

 private:
  void threadMain();
  void applyPendingCommands();
  size_t processChunk();
  void publishTelemetry();

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

  std::vector<float> chunk_;
};

}  // namespace skw
