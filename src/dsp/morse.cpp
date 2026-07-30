#include "dsp/morse.hpp"

#include <cctype>
#include <cmath>

#include "core/constants.hpp"

namespace skw {
namespace {

const char* morseCode(char c) {
  switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";
    case '/': return "-..-.";
    case '-': return "-....-";
    case '.': return ".-.-.-";
    case '?': return "..--..";
    default: return nullptr;
  }
}

// Text -> per-unit key line (true = tone), 1 unit = 1 dit.
std::vector<bool> keyUnits(const std::string& text) {
  std::vector<bool> k;
  bool atWordStart = true;
  for (char c : text) {
    if (c == ' ') {
      if (!atWordStart) {
        // Inter-char gap of 3 already emitted; widen to the 7-unit
        // word gap.
        k.insert(k.end(), 4, false);
        atWordStart = true;
      }
      continue;
    }
    const char* code = morseCode(c);
    if (!code) continue;
    for (const char* e = code; *e; ++e) {
      const int units = (*e == '-') ? 3 : 1;
      k.insert(k.end(), size_t(units), true);
      k.push_back(false);  // intra-element gap
    }
    k.insert(k.end(), 2, false);  // total 3 units after the character
    atWordStart = false;
  }
  return k;
}

}  // namespace

std::vector<cfloat> renderMorseBaseband(const std::string& text, int wpm,
                                        double audioToneHz,
                                        double amplitude) {
  if (wpm < 5) wpm = 5;
  const auto keys = keyUnits(text);
  const double ditSec = 1.2 / double(wpm);
  const size_t ditN = size_t(ditSec * kFsBaseband);
  const double fBb = audioToneHz - centerHz();
  // Raised-cosine keying edges: 5 ms or a quarter dit, whichever is
  // shorter (fast CW must still reach full amplitude).
  const size_t ramp = std::min(size_t(0.005 * kFsBaseband), ditN / 4);

  std::vector<float> env(keys.size() * ditN, 0.0f);
  for (size_t u = 0; u < keys.size(); ++u) {
    if (!keys[u]) continue;
    for (size_t i = 0; i < ditN; ++i) env[u * ditN + i] = 1.0f;
  }
  // Shape every on/off transition.
  for (size_t i = 0; i + 1 < env.size(); ++i) {
    if (env[i] == env[i + 1]) continue;
    const bool rising = env[i + 1] > env[i];
    for (size_t r = 0; r < ramp && i + 1 + r < env.size(); ++r) {
      const double x = 0.5 - 0.5 * std::cos(M_PI * double(r + 1) /
                                            double(ramp + 1));
      env[i + 1 + r] = float(rising ? x : 1.0 - x);
    }
    i += ramp;
  }

  std::vector<cfloat> out(env.size());
  for (size_t i = 0; i < out.size(); ++i) {
    const double ph = 2.0 * M_PI * fBb * double(i) / kFsBaseband;
    out[i] = cfloat(float(amplitude * env[i] * std::cos(ph)),
                    float(amplitude * env[i] * std::sin(ph)));
  }
  return out;
}

double morseDurationSec(const std::string& text, int wpm) {
  if (wpm < 5) wpm = 5;
  return double(keyUnits(text).size()) * 1.2 / double(wpm);
}

}  // namespace skw
