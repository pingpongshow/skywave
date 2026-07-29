// Minimal WAV support: 16-bit PCM and 32-bit float, mono, read/write.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skw {

struct WavData {
  int sampleRate = 0;
  std::vector<float> samples;  // mono, [-1, 1]
};

// Reads mono (or channel 0 of multichannel) PCM16/float32 WAV.
bool wavRead(const std::string& path, WavData& out);
// Writes mono PCM16 at the given rate.
bool wavWrite(const std::string& path, const float* samples, size_t count,
              int sampleRate);

}  // namespace skw
