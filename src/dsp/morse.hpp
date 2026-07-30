// CW ID renderer (PROTOCOL.md "Broadcast Traffic"): on-air Morse
// identification, rendered at 12 kHz complex baseband so the worker's
// normal TX path (upconverter) carries it. Pure audio-layer feature -
// no protocol interaction; a receiving modem just hears a keyed tone.
//
// Timing: PARIS convention - dit = 1.2 / wpm seconds, dah = 3 dits,
// intra-element gap 1 dit, inter-character 3 dits, word gap 7 dits.
// Keying uses raised-cosine edges (no key clicks).
#pragma once

#include <complex>
#include <string>
#include <vector>

namespace skw {

using cfloat = std::complex<float>;

// Characters: A-Z a-z 0-9 '/' '-' '.' '?' and space; anything else is
// skipped. `audioToneHz` is the on-air tone (baseband = tone - center).
std::vector<cfloat> renderMorseBaseband(const std::string& text, int wpm,
                                        double audioToneHz = 800.0,
                                        double amplitude = 0.5);

// On-air duration of the rendered text (for timers and tests).
double morseDurationSec(const std::string& text, int wpm);

}  // namespace skw
