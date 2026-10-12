// Audio I/O: PortAudio duplex device backend and file replay/record backend,
// both feeding the same SPSC rings (ARCHITECTURE.md thread model).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "io/ring.hpp"

namespace skw {

struct AudioDeviceInfo {
  int index;
  std::string name;
  int maxInputChannels;
  int maxOutputChannels;
  bool defaultInput;
  bool defaultOutput;
};

// Lists PortAudio devices (initializes/terminates PortAudio internally).
std::vector<AudioDeviceInfo> listAudioDevices();

// Abstract audio transport. start() begins moving samples:
//   RX direction: source -> rxRing (producer side)
//   TX direction: txRing -> sink   (consumer side)
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  // File backends: true when the replay source is exhausted AND the TX ring
  // has been fully drained. Device backends: always false.
  virtual bool finished() = 0;
  // Time from a sample leaving the TX ring (the audio callback consumed
  // it) to it leaving the DAC, as the driver reports it. PTT release
  // waits this long after the ring drains. File backends: 0.
  virtual double outputLatencySec() const { return 0.0; }
};

struct AudioConfig {
  int inputDevice = -1;   // -1 = default
  int outputDevice = -1;
  double inputGainDb = 0.0;
  double outputGainDb = 0.0;
};

// Real-time duplex PortAudio stream at kFsAudio.
std::unique_ptr<AudioBackend> makeDeviceBackend(const AudioConfig& cfg,
                                                SampleRing& rxRing,
                                                SampleRing& txRing);

// File-driven backend: replayPath (may be empty -> silence source of
// `silenceSec`) feeds rxRing in real-time-sized chunks without sleeping;
// txRing drains to recordPath (may be empty -> discard).
std::unique_ptr<AudioBackend> makeFileBackend(const std::string& replayPath,
                                              const std::string& recordPath,
                                              SampleRing& rxRing,
                                              SampleRing& txRing,
                                              double silenceSec = 0.0);

}  // namespace skw
