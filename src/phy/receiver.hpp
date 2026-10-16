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
#include <unordered_map>
#include <vector>

#include "phy/framer.hpp"

namespace skw {

enum class RxStage : uint8_t { NoDetect, ZcFail, HeaderFail, Ok };

// HARQ chase soft-combining store (PROTOCOL.md "HARQ"): per-codeword LLR
// accumulators keyed by frame identity (frameType, seqLow, mode, length,
// codeword index). A retransmission carries identical codewords, so its
// LLRs ADD; the summed vector decodes ~3 dB deeper at 2 transmissions.
// Entries clear on success; the store is bounded (oldest dropped).
class HarqChase {
 public:
  // Combine `llr` with any stored accumulator for this key (in place),
  // then remember the result for the next attempt. Returns the number of
  // transmissions now combined (>= 1).
  int combine(uint64_t key, std::vector<float>& llr);
  void onDecodeSuccess(uint64_t key) { store_.erase(key); }
  void clear() { store_.clear(); }
  size_t entries() const { return store_.size(); }

  static uint64_t frameKey(uint8_t frameType, uint8_t seqLow, uint8_t mode,
                           uint16_t length, uint32_t cwIndex) {
    return uint64_t(frameType) | (uint64_t(seqLow) << 8) |
           (uint64_t(mode) << 16) | (uint64_t(length) << 24) |
           (uint64_t(cwIndex) << 40);
  }

  // --- IR (rate-stepdown) info-bit priors -----------------------------
  // A failed rate>1/2 payload stores its systematic-bit channel LLRs in
  // stream order under a MODE-AGNOSTIC key; the r1/2 step-down
  // retransmission adds them to the systematic positions of its decode.
  // Chase never touches these (same-rate retransmissions combine full
  // codeword LLRs above instead).
  static uint64_t infoKey(uint8_t frameType, uint8_t seqLow,
                          uint16_t length) {
    return 0x8000000000000000ull | uint64_t(frameType) |
           (uint64_t(seqLow) << 8) | (uint64_t(length) << 24);
  }
  void storeInfoPriors(uint64_t key, std::vector<float> llrs);
  const std::vector<float>* infoPriors(uint64_t key) const;
  void dropInfoPriors(uint64_t key) { priors_.erase(key); }

 private:
  struct Entry {
    std::vector<float> llr;
    int count = 1;
    uint64_t stamp = 0;
  };
  std::unordered_map<uint64_t, Entry> store_;
  std::unordered_map<uint64_t, std::vector<float>> priors_;
  uint64_t stampCtr_ = 0;
};

// The r1/2 step-down mode for IR retransmissions: M9->M8, M11->M10.
// Returns the input mode when no same-constellation r1/2 rung exists
// (M12/M13: 64QAM r1/2 is not in the ladder - IR degenerates to chase,
// documented deviation) or the mode is already r1/2.
int irStepDownMode(int mode);

struct RxFrameResult {
  PhyHeader header;
  bool payloadOk = false;          // all payload codewords decoded
  std::vector<uint8_t> payload;    // dewhitened link payload (header.length)
  ChirpSig sig = ChirpSig::Fast;
  bool inverted = false;
  double cfoHz = 0.0;
  // Sync point the receiver locked to (ZC block / CSS sync), in samples
  // from the start of the span it was given: two captures of the same
  // burst report the same absolute position. -1 = unknown.
  double frameStartSample = -1.0;
  double snrDb = 0.0;              // tracker estimate at end of frame
  // snrDb is a MEASUREMENT (the tracker's noise estimate converged over
  // >= 8 pilot-bearing symbols) rather than its seed guess. OFDM frames
  // short enough to fit in a few symbols - a fast-tier ACK - report the
  // seed; CSS frames measure on the preamble and are always converged.
  bool snrConverged = true;
  // Data-aided SNR (dB, per carrier, UNCALIBRATED): after a successful
  // payload decode the decoded bits are re-encoded and re-mapped, and
  // the noise is taken from the error vector between each equalized
  // (despread, posterior-H) symbol and its ideal point. Unlike the
  // tracker's figure it contains no prediction lag, so under Doppler it
  // does not count channel motion as noise. -99 when the payload did
  // not decode or the frame has no payload codewords. Instrumentation
  // for the estimator choice (bertool `evm` column).
  double evmSnrDb = -99.0;
  std::vector<float> carrierSnrDb;  // per active carrier, at frame end
  double delaySpreadMs = 0.0;       // tap-extent estimate, at frame end
  // Pilot-residual energy relative to channel power: 0 = perfectly
  // tracked, rising as the channel outruns the estimator. Feeds the
  // AMC's M11+ stability gate and the channel-character classifier.
  double pilotStability = 0.0;
  // Frequency selectivity: spread of per-carrier SNR across the band,
  // in AMC index units (0..255). Feeds the M12+ gate.
  uint8_t selectivity = 0;
  // Equalized payload symbols for the GUI constellation view. Bounded;
  // empty unless the caller asked for it.
  std::vector<cfloat> constellation;
};

// Detect and decode the first frame of the given family in rx.
// Returns nullopt when no preamble is detected or the header fails;
// stageOut (optional) reports how far the receiver got. When harq is
// provided, failed payload codewords are chase-combined with earlier
// receptions of the same frame identity before the retry decode.
// payloadMask: session carrier mask applied to PAYLOAD symbols only
// (header symbols always use the full carrier set).
// wantConstellation: fill RxFrameResult::constellation with equalized
// payload symbols for the GUI instrument. Off by default - it costs a
// divide per symbol and an allocation, neither of which the modem path
// should pay.
// CFO branch candidates between the chirp and ZC stages (see
// receiver.cpp): each a CFO hypothesis, its delay-Doppler-corrected ZC
// block start, and the residual fit range for the full ZC refinement,
// in the order receiveFrame tries them against the header decode.
struct CfoBranch {
  double cfoHz;
  double start;
  double fitRangeHz;
};
std::vector<CfoBranch> resolveCfoBranches(Family family,
                                          std::span<const cfloat> sig,
                                          ChirpSig chirpSig, double cfoHz,
                                          double zcCoarse, int zcCount);

std::optional<RxFrameResult> receiveFrame(Family family,
                                          std::span<const cfloat> rx,
                                          RxStage* stageOut = nullptr,
                                          HarqChase* harq = nullptr,
                                          const CarrierMask* payloadMask =
                                              nullptr,
                                          bool wantConstellation = false);

}  // namespace skw
