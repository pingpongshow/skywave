#include <cmath>
#include <cstdio>
#include <vector>

#include "core/constants.hpp"
#include "io/wav.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(wav_roundtrip_pcm16) {
  // 100 ms of 1 kHz tone at 48 kHz.
  std::vector<float> tone(4800);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * double(i) / kFsAudio);
  }
  const char* path = "skw_test_tone.wav";
  CHECK(wavWrite(path, tone.data(), tone.size(), kFsAudio));

  WavData back;
  CHECK(wavRead(path, back));
  CHECK_EQ(back.sampleRate, kFsAudio);
  CHECK_EQ(back.samples.size(), tone.size());

  // PCM16 quantization: max error 1/32768.
  float maxErr = 0;
  for (size_t i = 0; i < tone.size(); ++i) {
    maxErr = std::max(maxErr, std::fabs(back.samples[i] - tone[i]));
  }
  CHECK(maxErr < 1.0f / 32000.0f);
  std::remove(path);
}

TEST_CASE(wav_rejects_garbage) {
  const char* path = "skw_test_garbage.wav";
  FILE* f = fopen(path, "wb");
  fputs("this is not a wav file at all, not even close", f);
  fclose(f);
  WavData d;
  CHECK(!wavRead(path, d));
  std::remove(path);
}

TEST_CASE(wav_missing_file) {
  WavData d;
  CHECK(!wavRead("skw_no_such_file.wav", d));
}

SKYTEST_MAIN
