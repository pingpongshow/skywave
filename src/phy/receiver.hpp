// One-shot frame receiver (ROADMAP 1.10-1.12): chirp acquisition -> ZC
// refinement -> header decode -> payload decode.
//
// The DSP.md sync states map onto: ChirpDetector (SEARCH/COARSE),
// zcRefine (FINE), ChannelTracker (TRACK + FLYWHEEL). Spectrum inversion
// is auto-corrected from the chirp signature. The streaming worker
// integration (continuous state machine) arrives in Phase 2.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "phy/framer.hpp"

namespace skw {

enum class RxStage : uint8_t { NoDetect, ZcFail, HeaderFail, Ok };

struct RxFrameResult {
  PhyHeader header;
  bool payloadOk = false;          // all payload codewords decoded
  std::vector<uint8_t> payload;    // dewhitened link payload (header.length)
  ChirpSig sig = ChirpSig::Fast;
  bool inverted = false;
  double cfoHz = 0.0;
  double snrDb = 0.0;              // tracker estimate at end of frame
};

// Detect and decode the first frame of the given family in rx.
// Returns nullopt when no preamble is detected or the header fails;
// stageOut (optional) reports how far the receiver got.
std::optional<RxFrameResult> receiveFrame(Family family,
                                          std::span<const cfloat> rx,
                                          RxStage* stageOut = nullptr);

}  // namespace skw
