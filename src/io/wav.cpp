#include "io/wav.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.hpp"

namespace skw {
namespace {

struct Fourcc {
  char c[4];
  bool is(const char* s) const { return std::memcmp(c, s, 4) == 0; }
};

uint32_t rd32(FILE* f, bool& ok) {
  uint8_t b[4];
  if (fread(b, 1, 4, f) != 4) { ok = false; return 0; }
  return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
         (uint32_t(b[3]) << 24);
}
uint16_t rd16(FILE* f, bool& ok) {
  uint8_t b[2];
  if (fread(b, 1, 2, f) != 2) { ok = false; return 0; }
  return uint16_t(b[0] | (b[1] << 8));
}
void wr32(FILE* f, uint32_t v) {
  uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  fwrite(b, 1, 4, f);
}
void wr16(FILE* f, uint16_t v) {
  uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
  fwrite(b, 1, 2, f);
}

}  // namespace

bool wavRead(const std::string& path, WavData& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = true;
  Fourcc riff{}, wave{};
  ok = fread(riff.c, 1, 4, f) == 4;
  rd32(f, ok);  // riff size
  ok = ok && fread(wave.c, 1, 4, f) == 4;
  if (!ok || !riff.is("RIFF") || !wave.is("WAVE")) {
    fclose(f);
    return false;
  }

  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  bool haveFmt = false, haveData = false;
  std::vector<uint8_t> data;

  while (ok) {
    Fourcc id{};
    if (fread(id.c, 1, 4, f) != 4) break;  // normal EOF
    uint32_t size = rd32(f, ok);
    if (!ok) break;
    if (id.is("fmt ")) {
      format = rd16(f, ok);
      channels = rd16(f, ok);
      rate = rd32(f, ok);
      rd32(f, ok);  // byte rate
      rd16(f, ok);  // block align
      bits = rd16(f, ok);
      if (size > 16) fseek(f, long(size - 16), SEEK_CUR);
      haveFmt = true;
    } else if (id.is("data")) {
      data.resize(size);
      ok = fread(data.data(), 1, size, f) == size;
      haveData = true;
    } else {
      fseek(f, long(size + (size & 1)), SEEK_CUR);
    }
  }
  fclose(f);
  if (!ok || !haveFmt || !haveData || channels == 0) return false;

  out.sampleRate = int(rate);
  out.samples.clear();
  const size_t frameBytes = size_t(channels) * bits / 8;
  if (frameBytes == 0) return false;
  const size_t frames = data.size() / frameBytes;
  out.samples.reserve(frames);

  if (format == 1 && bits == 16) {
    for (size_t i = 0; i < frames; ++i) {
      const uint8_t* p = data.data() + i * frameBytes;  // channel 0
      int16_t s = int16_t(p[0] | (p[1] << 8));
      out.samples.push_back(float(s) / 32768.0f);
    }
  } else if (format == 3 && bits == 32) {
    for (size_t i = 0; i < frames; ++i) {
      float v;
      std::memcpy(&v, data.data() + i * frameBytes, 4);
      out.samples.push_back(v);
    }
  } else {
    SKW_ERROR("wav: unsupported format %u/%u-bit in %s", format, bits,
              path.c_str());
    return false;
  }
  return true;
}

bool wavWrite(const std::string& path, const float* samples, size_t count,
              int sampleRate) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t dataBytes = uint32_t(count * 2);
  fwrite("RIFF", 1, 4, f);
  wr32(f, 36 + dataBytes);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f);
  wr32(f, 16);
  wr16(f, 1);  // PCM
  wr16(f, 1);  // mono
  wr32(f, uint32_t(sampleRate));
  wr32(f, uint32_t(sampleRate) * 2);
  wr16(f, 2);
  wr16(f, 16);
  fwrite("data", 1, 4, f);
  wr32(f, dataBytes);
  for (size_t i = 0; i < count; ++i) {
    float v = std::clamp(samples[i], -1.0f, 1.0f);
    int16_t s = int16_t(std::lround(v * 32767.0f));
    wr16(f, uint16_t(s));
  }
  bool ok = ferror(f) == 0;
  fclose(f);
  return ok;
}

}  // namespace skw
