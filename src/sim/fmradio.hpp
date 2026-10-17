// FM radio emulator for the VHF-FM band profile (WAVEFORM.md Phase 7):
// the PC -> soundcard -> FM transceiver data port -> RF channel -> FM
// receiver -> data port -> soundcard -> PC path, at the REAL 48 kHz audio
// domain. The SSB radio chain (radiochain.hpp) cannot carry the VHF
// profile: its 300-2700 Hz rig passband removes most of the 300-6900 Hz
// signal.
//
//   TX rig : drive level, data-port audio response, optional
//            pre-emphasis, deviation limiter, frequency modulator
//   path   : flat Rayleigh/Rician flutter (mobile Doppler, sum of
//            sinusoids), RF frequency offset, complex AWGN at a CNR
//            referenced to the receiver's IF bandwidth
//   RX rig : IF channel filter, limiter-discriminator (so the FM threshold
//            and its click noise emerge from the physics rather than being
//            modelled), optional de-emphasis, data-port audio response,
//            soundcard clock offset
//
// Deviation is set like a rig's data-port level: audio amplitude 1.0
// (digital full scale) deviates the carrier by devHzPerUnit. The
// discriminator output is rescaled by the same constant, so a clean path
// returns the input level.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace skw {

struct FmRadioParams {
  // --- TX rig -----------------------------------------------------------
  double carrierLeadMs = 20.0;   // carrier keyed (unmodulated) before the
                                 // first audio sample - the modem's PTT
                                 // lead. The carrier-onset click falls
                                 // here, not on the frame.
  double carrierTailMs = 10.0;   // carrier held after the last audio
                                 // sample (PTT tail); then it drops and
                                 // the receiver's squelch closes
  double keyupMs = 0.0;          // audio lost from key-up: TX PLL/PA
                                 // settling plus RX squelch opening;
                                 // beyond carrierLeadMs it eats the frame
  double driveLevelDb = 0.0;     // operator level error (+hot / -quiet)
  double txLowHz = 300.0;        // data-port audio passband
  double txHighHz = 7000.0;
  double preEmphasisUs = 0.0;    // 0 = flat data port; 750 = voice path
  double devHzPerUnit = 3000.0;  // deviation at audio full scale
  double devLimitHz = 0.0;       // instantaneous deviation clip; 0 = off

  // --- path -------------------------------------------------------------
  double cnrDb = 99.0;           // carrier-to-noise in the IF bandwidth
  double ifBwHz = 16000.0;       // RX IF filter (25 kHz channel); 11000
                                 // for a 12.5 kHz channel
  double rfOffsetHz = 0.0;       // TX/RX LO mismatch
  double flutterHz = 0.0;        // max Doppler of the flat fade; 0 = static
  double ricianK = 0.0;          // fade K factor (0 = Rayleigh)

  // --- RX rig -------------------------------------------------------------
  double deEmphasisUs = 0.0;
  double rxLowHz = 300.0;
  double rxHighHz = 7000.0;
  double soundcardPpm = 0.0;
  uint64_t seed = 1;
};

// Data-port presets used by tests, fmsweep and the ST2 VHF world.
FmRadioParams fmDataPort();          // 25 kHz channel, flat 9600 port
FmRadioParams fmDataPortNarrow();    // 12.5 kHz channel (IF 11 kHz, 2.5 kHz dev)
FmRadioParams fmVoicePort();         // misconfigured: 1200/mic path with
                                     // 750 us emphasis and 300-3000 Hz audio

// Run 48 kHz real TX audio through the emulated FM link; returns the
// 48 kHz real audio the receiving soundcard captures. The output is longer
// than the input by the filters' tails.
std::vector<float> fmRadio(const FmRadioParams& p, std::span<const float> tx48k);

}  // namespace skw
