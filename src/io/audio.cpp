#include "io/audio.hpp"

#include <portaudio.h>

#include <cmath>
#include <cstring>

#include "core/constants.hpp"
#include "core/log.hpp"
#include "io/wav.hpp"

namespace skw {
namespace {

constexpr unsigned kFramesPerBuffer = 256;  // ~5.3 ms at 48 kHz

float dbToLinear(double db) { return float(std::pow(10.0, db / 20.0)); }

// ------------------------------------------------------------- device backend
class DeviceBackend final : public AudioBackend {
 public:
  DeviceBackend(const AudioConfig& cfg, SampleRing& rx, SampleRing& tx)
      : cfg_(cfg), rx_(rx), tx_(tx),
        inGain_(dbToLinear(cfg.inputGainDb)),
        outGain_(dbToLinear(cfg.outputGainDb)) {}

  ~DeviceBackend() override { stop(); }

  double outputLatencySec() const override { return outLatencySec_; }

  bool start() override {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      SKW_ERROR("portaudio init: %s", Pa_GetErrorText(err));
      return false;
    }
    paUp_ = true;

    PaStreamParameters inP{}, outP{};
    inP.device = cfg_.inputDevice >= 0 ? cfg_.inputDevice
                                       : Pa_GetDefaultInputDevice();
    outP.device = cfg_.outputDevice >= 0 ? cfg_.outputDevice
                                         : Pa_GetDefaultOutputDevice();
    if (inP.device == paNoDevice || outP.device == paNoDevice) {
      SKW_ERROR("portaudio: no input/output device");
      return false;
    }
    inP.channelCount = 1;
    inP.sampleFormat = paFloat32;
    inP.suggestedLatency =
        Pa_GetDeviceInfo(inP.device)->defaultLowInputLatency;
    outP.channelCount = 1;
    outP.sampleFormat = paFloat32;
    outP.suggestedLatency =
        Pa_GetDeviceInfo(outP.device)->defaultLowOutputLatency;

    err = Pa_OpenStream(&stream_, &inP, &outP, kFsAudio, kFramesPerBuffer,
                        paNoFlag, &DeviceBackend::paCallback, this);
    if (err != paNoError) {
      SKW_ERROR("portaudio open: %s", Pa_GetErrorText(err));
      return false;
    }
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
      SKW_ERROR("portaudio start: %s", Pa_GetErrorText(err));
      return false;
    }
    if (const PaStreamInfo* si = Pa_GetStreamInfo(stream_)) {
      outLatencySec_ = si->outputLatency;
    }
    SKW_INFO("audio: duplex stream running (in=%d out=%d, %d Hz, output "
             "latency %.1f ms)",
             inP.device, outP.device, kFsAudio, outLatencySec_ * 1e3);
    return true;
  }

  void stop() override {
    if (stream_) {
      Pa_StopStream(stream_);
      Pa_CloseStream(stream_);
      stream_ = nullptr;
    }
    if (paUp_) {
      Pa_Terminate();
      paUp_ = false;
    }
  }

  bool finished() override { return false; }

 private:
  // Hard real-time context: no allocation, no locks, no logging.
  static int paCallback(const void* input, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo*,
                        PaStreamCallbackFlags, void* user) {
    auto* self = static_cast<DeviceBackend*>(user);
    const float* in = static_cast<const float*>(input);
    float* out = static_cast<float*>(output);

    if (in) {
      float scaled[kFramesPerBuffer];
      unsigned n = frames < kFramesPerBuffer ? unsigned(frames)
                                             : kFramesPerBuffer;
      for (unsigned i = 0; i < n; ++i) scaled[i] = in[i] * self->inGain_;
      self->rx_.write({scaled, n});
    }
    if (out) {
      size_t got = self->tx_.read({out, size_t(frames)});
      for (size_t i = 0; i < got; ++i) out[i] *= self->outGain_;
      // Ring underrun -> silence, never garbage.
      std::memset(out + got, 0, (frames - got) * sizeof(float));
    }
    return paContinue;
  }

  AudioConfig cfg_;
  SampleRing& rx_;
  SampleRing& tx_;
  float inGain_;
  float outGain_;
  double outLatencySec_ = 0.0;
  PaStream* stream_ = nullptr;
  bool paUp_ = false;
};

// --------------------------------------------------------------- file backend
class FileBackend final : public AudioBackend {
 public:
  FileBackend(std::string replayPath, std::string recordPath, SampleRing& rx,
              SampleRing& tx, double silenceSec)
      : replayPath_(std::move(replayPath)), recordPath_(std::move(recordPath)),
        rx_(rx), tx_(tx), silenceSec_(silenceSec) {}

  bool start() override {
    if (!replayPath_.empty()) {
      WavData wav;
      if (!wavRead(replayPath_, wav)) {
        SKW_ERROR("replay: cannot read %s", replayPath_.c_str());
        return false;
      }
      if (wav.sampleRate != kFsAudio) {
        SKW_ERROR("replay: %s is %d Hz, need %d Hz", replayPath_.c_str(),
                  wav.sampleRate, kFsAudio);
        return false;
      }
      source_ = std::move(wav.samples);
      SKW_INFO("replay: %zu samples (%.2f s) from %s", source_.size(),
               double(source_.size()) / kFsAudio, replayPath_.c_str());
    } else {
      source_.assign(size_t(silenceSec_ * kFsAudio), 0.0f);
    }
    return true;
  }

  void stop() override {
    if (!recordPath_.empty()) {
      if (wavWrite(recordPath_, recorded_.data(), recorded_.size(), kFsAudio)) {
        SKW_INFO("record: wrote %zu samples (%.2f s) to %s", recorded_.size(),
                 double(recorded_.size()) / kFsAudio, recordPath_.c_str());
      } else {
        SKW_ERROR("record: cannot write %s", recordPath_.c_str());
      }
    }
  }

  // Called from the main loop: push a chunk of replay into rxRing and drain
  // txRing into the record buffer. Runs faster than real time.
  bool finished() override {
    if (srcPos_ < source_.size()) {
      size_t chunk = std::min<size_t>(kFramesPerBuffer,
                                      source_.size() - srcPos_);
      size_t pushed = rx_.write({source_.data() + srcPos_, chunk});
      srcPos_ += pushed;
    }
    float buf[1024];
    size_t got;
    while ((got = tx_.read({buf, sizeof(buf) / sizeof(buf[0])})) > 0) {
      recorded_.insert(recorded_.end(), buf, buf + got);
    }
    return srcPos_ >= source_.size() && rx_.readAvailable() == 0;
  }

 private:
  std::string replayPath_;
  std::string recordPath_;
  SampleRing& rx_;
  SampleRing& tx_;
  double silenceSec_;
  std::vector<float> source_;
  size_t srcPos_ = 0;
  std::vector<float> recorded_;
};

}  // namespace

std::vector<AudioDeviceInfo> listAudioDevices() {
  std::vector<AudioDeviceInfo> out;
  if (Pa_Initialize() != paNoError) return out;
  int n = Pa_GetDeviceCount();
  int defIn = Pa_GetDefaultInputDevice();
  int defOut = Pa_GetDefaultOutputDevice();
  for (int i = 0; i < n; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) continue;
    out.push_back({i, info->name ? info->name : "?", info->maxInputChannels,
                   info->maxOutputChannels, i == defIn, i == defOut});
  }
  Pa_Terminate();
  return out;
}

std::unique_ptr<AudioBackend> makeDeviceBackend(const AudioConfig& cfg,
                                                SampleRing& rxRing,
                                                SampleRing& txRing) {
  return std::make_unique<DeviceBackend>(cfg, rxRing, txRing);
}

std::unique_ptr<AudioBackend> makeFileBackend(const std::string& replayPath,
                                              const std::string& recordPath,
                                              SampleRing& rxRing,
                                              SampleRing& txRing,
                                              double silenceSec) {
  return std::make_unique<FileBackend>(replayPath, recordPath, rxRing, txRing,
                                       silenceSec);
}

}  // namespace skw
