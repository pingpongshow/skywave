// Radio-chain emulator (TESTING.md "System Test Layers", ST1): models the
// full PC -> soundcard -> SSB transceiver -> ionosphere -> transceiver ->
// soundcard -> PC path at the REAL 48 kHz audio domain, so the entire
// modem stack (including the 48k<->12k converters) is exercised under
// rig-realistic distortion before any on-air test:
//
//   TX rig : PTT switching clip, drive-level error, ALC soft compression,
//            TX audio bandpass (crystal-filter proxy)
//   path   : Watterson fading / AWGN / carrier-point CFO (chansim at
//            complex baseband via the real converters)
//   RX rig : RX audio bandpass, mains hum + harmonic, DC offset,
//            impulse noise (static crashes), soundcard clock offset
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sim/chansim.hpp"

namespace skw {

struct RadioChainParams {
  // --- TX rig -----------------------------------------------------------
  double pttLeadClipMs = 0.0;   // audio muted while the rig keys up
  double pttTailClipMs = 0.0;   // audio muted as the rig unkeys early
  double driveLevelDb = 0.0;    // operator gain error (+hot / -quiet)
  double alcKnee = 0.0;         // soft-compression knee amplitude; 0 = off
  double txBandLowHz = 300.0;   // rig TX audio passband
  double txBandHighHz = 2700.0;

  // --- path (complex-baseband channel) -----------------------------------
  ChansimParams channel;        // fading / AWGN / CFO / clock ppm

  // --- RX rig -------------------------------------------------------------
  double rxBandLowHz = 300.0;
  double rxBandHighHz = 2700.0;
  double humHz = 0.0;           // mains fundamental (50 / 60); 0 = off
  double humAmp = 0.0;          // amplitude of fundamental (2nd at half)
  double dcOffset = 0.0;
  double impulsePerSec = 0.0;   // static-crash bursts (Poisson)
  double impulseAmp = 0.0;
  double soundcardPpm = 0.0;    // RX soundcard clock vs TX soundcard
  uint64_t seed = 1;
};

// Run 48 kHz real TX audio through the emulated chain; returns the 48 kHz
// real audio the receiving modem's soundcard would capture.
std::vector<float> radioChain(const RadioChainParams& p,
                              std::span<const float> tx48k);

}  // namespace skw
