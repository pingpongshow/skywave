// onair_sounder: one-way on-air test transmissions (docs/ONAIR_TEST_PLAN.md).
//
// Three steps, kept apart so the RF chain sits between them:
//
//   gen    build a sounding: real Skywave PHY frames at a list of modes,
//          round-robin interleaved (every mode sees the same propagation
//          over the run), grouped into keyed segments with unkeyed pauses,
//          CW ID at both ends. Writes a 48 kHz WAV plus a manifest.
//   play   play the WAV to an audio device (SmartSDR DAX TX), keying the
//          rig per segment through a TCP PTT server (flexptt.py, or any
//          server speaking skywave's `KEY\n`/`UNKEY\n` protocol).
//   score  decode a RECEIVED recording (Web-888/KiwiSDR WAV, any rate)
//          against the manifest: per-frame and per-mode FER, the
//          receiver's SNR/EVM/CFO/delay-spread/pilot-stability per frame,
//          CFO drift rates, and the TX-vs-RX sample clock offset.
//
// The frames are ordinary PHY frames (frame type 61, unassigned - a
// Skywave modem listening on the channel ignores them) whose payload is a
// known pseudo-random sequence keyed by the frame's sequence number, so the
// scorer needs nothing but the manifest. Nothing here touches the link
// layer: this measures the PHY through a real transmitter, receiver and
// ionosphere.
#include <portaudio.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/config.hpp"
#include "core/constants.hpp"
#include "dsp/chirp.hpp"
#include "dsp/css.hpp"
#include "dsp/morse.hpp"
#include "dsp/updown.hpp"
#include "io/wav.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"

using namespace skw;

namespace {

constexpr uint8_t kSoundFrameType = 61;  // unassigned: modems ignore it
constexpr double kPi = 3.14159265358979323846;

[[noreturn]] void die(const char* fmt, const char* a = "") {
  std::fprintf(stderr, "onair_sounder: ");
  std::fprintf(stderr, fmt, a);
  std::fprintf(stderr, "\n");
  std::exit(2);
}

std::vector<std::string> split(const std::string& s, char d) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find(d, p);
    if (q == std::string::npos) q = s.size();
    if (q > p) out.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

// "4..13" or "4,8,13" or a mix ("0..3,8").
std::vector<int> parseModes(const std::string& s) {
  std::vector<int> out;
  for (const auto& tok : split(s, ',')) {
    const size_t r = tok.find("..");
    if (r != std::string::npos) {
      const int a = std::atoi(tok.substr(0, r).c_str());
      const int b = std::atoi(tok.substr(r + 2).c_str());
      for (int m = a; m <= b; ++m) out.push_back(m);
    } else {
      out.push_back(std::atoi(tok.c_str()));
    }
  }
  for (int m : out) {
    if (m < 0 || m > 13) die("mode out of range 0-13 in '%s'", s.c_str());
  }
  return out;
}

std::vector<double> parseList(const std::string& s) {
  std::vector<double> out;
  for (const auto& tok : split(s, ',')) out.push_back(std::atof(tok.c_str()));
  return out;
}

ChirpSig sigForMode(int mode) {
  if (mode <= 3) return ChirpSig::Css;
  return mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
}

// Default payloads match bertool: short robust frames, 300 B fast ones.
// CSS frames are seconds long even at 30 B.
size_t defaultPayloadLen(int mode) {
  if (mode <= 3) return 30;
  return mode <= 7 ? 60 : 300;
}

// Known payload: seq (BE16), mode, then xorshift keyed by run seed + seq.
std::vector<uint8_t> makePayload(uint32_t runSeed, int seq, int mode,
                                 size_t len) {
  std::vector<uint8_t> p(len);
  uint64_t x = (uint64_t(runSeed) << 20) ^ (uint64_t(seq) * 0x9E3779B97F4A7C15ull)
               ^ 0xD1B54A32D192ED03ull;
  for (size_t i = 0; i < len; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    p[i] = uint8_t(x >> 24);
  }
  if (len >= 3) {
    p[0] = uint8_t(seq >> 8);
    p[1] = uint8_t(seq);
    p[2] = uint8_t(mode);
  }
  return p;
}

// --- manifest -----------------------------------------------------------------

struct FrameRec {
  int seq = 0;
  int mode = 0;
  int segment = 0;
  double levelDb = 0.0;  // digital level relative to the modem's own
  double startSec = 0.0; // preamble start in the WAV
  double airSec = 0.0;
  size_t payloadLen = 0;
};

struct Segment {
  double startSec = 0.0;  // first sample to key for (includes lead pad)
  double endSec = 0.0;
};

struct Manifest {
  Family family = Family::Wide;
  uint32_t runSeed = 1;
  std::string callsign;
  std::vector<Segment> segments;
  std::vector<FrameRec> frames;
};

bool writeManifest(const std::string& path, const Manifest& m) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f, "# onair_sounder manifest v1\n");
  std::fprintf(f, "family %s\nseed %u\ncall %s\nrate %d\n",
               plan(m.family).name, m.runSeed,
               m.callsign.empty() ? "-" : m.callsign.c_str(), kFsAudio);
  for (size_t i = 0; i < m.segments.size(); ++i) {
    std::fprintf(f, "segment %zu %.6f %.6f\n", i, m.segments[i].startSec,
                 m.segments[i].endSec);
  }
  std::fprintf(f, "# frame seq mode segment level_db start_s air_s payload\n");
  for (const auto& r : m.frames) {
    std::fprintf(f, "frame %d %d %d %.1f %.6f %.6f %zu\n", r.seq, r.mode,
                 r.segment, r.levelDb, r.startSec, r.airSec, r.payloadLen);
  }
  return std::fclose(f) == 0;
}

bool readManifest(const std::string& path, Manifest& m) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    char key[32] = {};
    if (line[0] == '#' || std::sscanf(line, "%31s", key) != 1) continue;
    if (!std::strcmp(key, "family")) {
      char v[32] = {};
      std::sscanf(line, "%*s %31s", v);
      auto fam = familyFromString(v);
      if (!fam) { std::fclose(f); return false; }
      m.family = *fam;
    } else if (!std::strcmp(key, "seed")) {
      std::sscanf(line, "%*s %u", &m.runSeed);
    } else if (!std::strcmp(key, "call")) {
      char v[64] = {};
      std::sscanf(line, "%*s %63s", v);
      m.callsign = std::strcmp(v, "-") ? v : "";
    } else if (!std::strcmp(key, "segment")) {
      Segment s;
      std::sscanf(line, "%*s %*d %lf %lf", &s.startSec, &s.endSec);
      m.segments.push_back(s);
    } else if (!std::strcmp(key, "frame")) {
      FrameRec r;
      std::sscanf(line, "%*s %d %d %d %lf %lf %lf %zu", &r.seq, &r.mode,
                  &r.segment, &r.levelDb, &r.startSec, &r.airSec,
                  &r.payloadLen);
      m.frames.push_back(r);
    }
  }
  std::fclose(f);
  return !m.frames.empty();
}

std::string manifestPathFor(const std::string& wav) {
  const size_t dot = wav.rfind('.');
  return (dot == std::string::npos ? wav : wav.substr(0, dot)) + ".manifest";
}

// --- gen ----------------------------------------------------------------------

struct GenArgs {
  Family family = Family::Wide;
  std::vector<int> modes = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  int reps = 5;
  std::vector<double> levels = {0.0};
  std::string call;
  bool noId = false;
  std::string out = "sounding.wav";
  double gapSec = 0.4;      // silence between frames inside a segment
  double segmentSec = 30.0; // max keyed time per segment
  double pauseSec = 3.0;    // unkeyed time between segments
  double leadSec = 0.15;    // silence at segment start (rig keying)
  int payloadLen = 0;
  uint32_t seed = 1;
  // The PHY emits unit-RMS baseband with an 8 dB PAPR allowance, which
  // upconverts to ~+9 dBFS peaks: a real interface (DAX, USB codec, a
  // 16-bit WAV) clips them. -10 dB puts the peak at ~-1 dBFS.
  double gainDb = -10.0;
  int wpm = 20;
};

void appendSilence(std::vector<cfloat>& bb, double sec) {
  bb.insert(bb.end(), size_t(sec * kFsBaseband), cfloat(0, 0));
}

int cmdGen(const GenArgs& a) {
  if (a.call.empty() && !a.noId) {
    die("--call is required for an on-air sounding (or --no-id for a "
        "dummy-load bench run)");
  }
  Manifest m;
  m.family = a.family;
  m.runSeed = a.seed;
  m.callsign = a.call;

  // Frame order: levels outermost (a level sweep is a bench measurement of
  // the TX chain's compression knee; keep each level's frames together),
  // then reps, then modes round-robin.
  struct Planned { int mode; double level; };
  std::vector<Planned> order;
  for (double lv : a.levels) {
    for (int r = 0; r < a.reps; ++r) {
      for (int mode : a.modes) order.push_back({mode, lv});
    }
  }

  std::vector<cfloat> bb;  // whole sounding at 12 kHz baseband
  auto segOpen = [&]() {
    Segment s;
    s.startSec = double(bb.size()) / kFsBaseband;
    appendSilence(bb, a.leadSec);
    m.segments.push_back(s);
  };
  auto segClose = [&](bool last) {
    appendSilence(bb, 0.05);
    m.segments.back().endSec = double(bb.size()) / kFsBaseband;
    if (!last) appendSilence(bb, a.pauseSec);
  };

  segOpen();
  if (!a.noId) {
    auto id = renderMorseBaseband(a.call + " TEST", a.wpm);
    bb.insert(bb.end(), id.begin(), id.end());
    appendSilence(bb, a.gapSec);
  }
  int seq = 0;
  for (const auto& p : order) {
    const size_t len = a.payloadLen > 0 ? size_t(std::min(a.payloadLen, 4095))
                                        : defaultPayloadLen(p.mode);
    const auto payload = makePayload(a.seed, seq, p.mode, len);
    TxFrameParams tp;
    tp.family = a.family;
    tp.sig = sigForMode(p.mode);
    tp.mode = p.mode;
    tp.frameType = kSoundFrameType;
    tp.seqLow = uint8_t(seq);
    tp.payload = payload;
    auto frame = buildFrameBaseband(tp);
    const double air = double(frame.size()) / kFsBaseband;
    // A frame that would overrun the segment starts a new one; a frame
    // longer than a whole segment (CSS deep tier) gets one to itself.
    const double segUsed =
        double(bb.size()) / kFsBaseband - m.segments.back().startSec;
    if (segUsed > a.leadSec + 0.01 && segUsed + air > a.segmentSec) {
      segClose(false);
      segOpen();
    }
    const float g = float(std::pow(10.0, p.level / 20.0));
    FrameRec r;
    r.seq = seq;
    r.mode = p.mode;
    r.segment = int(m.segments.size()) - 1;
    r.levelDb = p.level;
    r.startSec = double(bb.size()) / kFsBaseband;
    r.airSec = air;
    r.payloadLen = len;
    m.frames.push_back(r);
    for (const auto& v : frame) bb.push_back(v * g);
    appendSilence(bb, a.gapSec);
    ++seq;
  }
  if (!a.noId) {
    auto id = renderMorseBaseband(a.call, a.wpm);
    bb.insert(bb.end(), id.begin(), id.end());
  }
  segClose(true);

  // Upconvert in one pass; the chain's group delay shifts every frame by
  // the same amount, so the manifest times stay consistent with each
  // other (the scorer aligns on decoded frames, not absolute time).
  Upconverter up;
  std::vector<float> audio;
  up.process(bb, audio);
  const float g = float(std::pow(10.0, a.gainDb / 20.0));
  double peak = 0.0, pow = 0.0;
  for (auto& v : audio) {
    v *= g;
    peak = std::max(peak, double(std::fabs(v)));
    pow += double(v) * v;
  }
  if (!wavWrite(a.out, audio.data(), audio.size(), kFsAudio)) {
    die("cannot write %s", a.out.c_str());
  }
  const std::string mpath = manifestPathFor(a.out);
  if (!writeManifest(mpath, m)) die("cannot write %s", mpath.c_str());

  double keyed = 0.0;
  for (const auto& s : m.segments) keyed += s.endSec - s.startSec;
  const auto& fp = plan(a.family);
  std::printf("wrote %s (%.1f s, %zu frames, %zu segments, %.0f s keyed)\n",
              a.out.c_str(), double(audio.size()) / kFsAudio,
              m.frames.size(), m.segments.size(), keyed);
  std::printf("wrote %s\n", mpath.c_str());
  std::printf("family %s occupies %.0f-%.0f Hz audio: set the receiver "
              "passband to cover it (USB, AGC off)\n",
              fp.name, fp.firstBin * kSubcarrierHz,
              (fp.lastBin + 1) * kSubcarrierHz);
  std::printf("level: peak %.1f dBFS, frame RMS ~%.1f dBFS%s\n",
              20 * std::log10(std::max(peak, 1e-9)),
              10 * std::log10(std::max(pow / double(audio.size()), 1e-12)),
              peak > 1.0 ? "  ** CLIPS: lower --gain-db **" : "");
  return 0;
}

// --- play ---------------------------------------------------------------------

class PttClient {
 public:
  explicit PttClient(const std::string& target) {
    if (target.empty()) return;
    std::string host = "127.0.0.1", port = "4532";  // kPttTcpDefaultPort
    const size_t c = target.rfind(':');
    if (c != std::string::npos) {
      if (c > 0) host = target.substr(0, c);
      port = target.substr(c + 1);
    } else {
      host = target;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
      die("PTT: cannot resolve %s", target.c_str());
    }
    for (addrinfo* p = res; p; p = p->ai_next) {
      fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd_ < 0) continue;
      if (::connect(fd_, p->ai_addr, p->ai_addrlen) == 0) break;
      ::close(fd_);
      fd_ = -1;
    }
    freeaddrinfo(res);
    if (fd_ < 0) die("PTT: cannot connect to %s", target.c_str());
  }
  ~PttClient() {
    if (fd_ >= 0) {
      send("UNKEY\n");
      ::close(fd_);
    }
  }
  bool active() const { return fd_ >= 0; }
  void key() { if (!send("KEY\n")) die("PTT: KEY failed"); }
  void unkey() { send("UNKEY\n"); }

 private:
  bool send(const char* s) {
    if (fd_ < 0) return true;
    const size_t n = std::strlen(s);
    return ::send(fd_, s, n, 0) == ssize_t(n);
  }
  int fd_ = -1;
};

int findDevice(const std::string& sel) {
  const int n = Pa_GetDeviceCount();
  char* end = nullptr;
  const long idx = std::strtol(sel.c_str(), &end, 10);
  if (!sel.empty() && *end == '\0') {
    if (idx < 0 || idx >= n) die("no audio device index %s", sel.c_str());
    return int(idx);
  }
  for (int i = 0; i < n; ++i) {
    const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
    if (d && d->maxOutputChannels > 0 && std::strstr(d->name, sel.c_str())) {
      return i;
    }
  }
  die("no output device matching '%s' (see: onair_sounder devices)",
      sel.c_str());
}

int cmdDevices() {
  if (Pa_Initialize() != paNoError) die("PortAudio init failed");
  for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
    const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
    std::printf("%3d  in %d  out %d  %s\n", i, d->maxInputChannels,
                d->maxOutputChannels, d->name);
  }
  Pa_Terminate();
  return 0;
}

struct PlayArgs {
  std::string wav;
  std::string device;
  std::string ptt;        // host[:port] of a KEY/UNKEY server
  double leadMs = 100.0;  // key -> first sample (on top of the WAV's pad)
  double tailMs = 50.0;   // after the device has played the last sample
  int fromSegment = 0;
};

int cmdPlay(const PlayArgs& a) {
  WavData w;
  if (!wavRead(a.wav, w)) die("cannot read %s", a.wav.c_str());
  if (w.sampleRate != kFsAudio) die("expected a 48 kHz sounding WAV");
  Manifest m;
  if (!readManifest(manifestPathFor(a.wav), m)) {
    die("cannot read the manifest next to %s", a.wav.c_str());
  }
  if (a.device.empty()) die("--device is required (see: onair_sounder devices)");
  if (Pa_Initialize() != paNoError) die("PortAudio init failed");
  const int dev = findDevice(a.device);
  PaStreamParameters op{};
  op.device = dev;
  op.channelCount = 1;
  op.sampleFormat = paFloat32;
  op.suggestedLatency = Pa_GetDeviceInfo(dev)->defaultHighOutputLatency;
  PaStream* stream = nullptr;
  // DAX and many virtual devices are stereo-only: fall back to 2 channels.
  int ch = 1;
  if (Pa_OpenStream(&stream, nullptr, &op, kFsAudio, 1024, paClipOff, nullptr,
                    nullptr) != paNoError) {
    op.channelCount = ch = 2;
    if (Pa_OpenStream(&stream, nullptr, &op, kFsAudio, 1024, paClipOff,
                      nullptr, nullptr) != paNoError) {
      die("cannot open '%s' at 48 kHz", Pa_GetDeviceInfo(dev)->name);
    }
  }
  if (Pa_StartStream(stream) != paNoError) die("cannot start audio stream");
  const double outLat = Pa_GetStreamInfo(stream)->outputLatency;
  std::printf("device %d '%s', %d ch, output latency %.0f ms%s\n", dev,
              Pa_GetDeviceInfo(dev)->name, ch, outLat * 1e3,
              a.ptt.empty() ? ", NO PTT (VOX or manual keying)" : "");

  PttClient ptt(a.ptt);
  std::vector<float> buf;
  auto writeSamples = [&](const float* x, size_t n) {
    const size_t blk = 1024;
    for (size_t off = 0; off < n; off += blk) {
      const size_t k = std::min(blk, n - off);
      buf.assign(k * size_t(ch), 0.0f);
      for (size_t i = 0; i < k; ++i) {
        for (int c = 0; c < ch; ++c) buf[i * size_t(ch) + size_t(c)] = x ? x[off + i] : 0.0f;
      }
      Pa_WriteStream(stream, buf.data(), unsigned(k));
    }
  };
  for (size_t s = size_t(a.fromSegment); s < m.segments.size(); ++s) {
    const auto& seg = m.segments[s];
    const size_t i0 = size_t(seg.startSec * kFsAudio);
    const size_t i1 = std::min(w.samples.size(), size_t(seg.endSec * kFsAudio));
    int nFrames = 0;
    for (const auto& f : m.frames) nFrames += f.segment == int(s);
    std::printf("segment %zu/%zu: %.1f s, %d frames\n", s + 1,
                m.segments.size(), double(i1 - i0) / kFsAudio, nFrames);
    std::fflush(stdout);
    if (ptt.active()) {
      ptt.key();
      usleep(useconds_t(a.leadMs * 1e3));
    }
    writeSamples(w.samples.data() + i0, i1 - i0);
    // Blocking writes return once the samples are QUEUED: hold the key
    // until the device has played them (the same rule as the modem's
    // own PTT release).
    usleep(useconds_t((outLat + a.tailMs * 1e-3) * 1e6));
    if (ptt.active()) ptt.unkey();
    if (s + 1 < m.segments.size()) {
      // The key-lead and playout waits already used part of the pause.
      const double waited = (ptt.active() ? a.leadMs * 1e-3 : 0.0) +
                            outLat + a.tailMs * 1e-3;
      const double pause = m.segments[s + 1].startSec - seg.endSec - waited;
      writeSamples(nullptr, size_t(std::max(0.0, pause) * kFsAudio));
    }
  }
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  std::printf("done\n");
  return 0;
}

// --- score --------------------------------------------------------------------

// Band-limited resampler (windowed sinc, Blackman), any ratio. Only used
// once per recording, so clarity beats speed.
std::vector<float> resample(const std::vector<float>& x, double inRate,
                            double outRate) {
  if (std::fabs(inRate - outRate) < 1e-9) return x;
  const double ratio = outRate / inRate;
  const double cutoff = 0.45 * std::min(1.0, ratio);  // of the input rate
  const int half = 48;
  const size_t nOut = size_t(double(x.size()) * ratio);
  std::vector<float> y(nOut);
  for (size_t n = 0; n < nOut; ++n) {
    const double t = double(n) / ratio;  // position in input samples
    const long c = long(std::floor(t));
    double acc = 0.0;
    for (long k = c - half + 1; k <= c + half; ++k) {
      if (k < 0 || k >= long(x.size())) continue;
      const double d = t - double(k);
      const double s = std::fabs(d) < 1e-12
                           ? 2 * cutoff
                           : std::sin(2 * kPi * cutoff * d) / (kPi * d);
      const double wpos = (d + half) / (2.0 * half);
      const double win = 0.42 - 0.5 * std::cos(2 * kPi * wpos) +
                         0.08 * std::cos(4 * kPi * wpos);
      acc += double(x[size_t(k)]) * s * win;
    }
    y[n] = float(acc);  // h = 2fc sinc(2fc d): unity gain at DC
  }
  return y;
}

struct ScoreArgs {
  std::string wav;
  std::string manifest;
  std::string csv;
  double shiftHz = 0.0;  // pre-mix the recording (receiver tuned off)
  double rateHz = 0.0;   // override the WAV header's sample rate
};

struct Decode {
  double rxSec;  // preamble start in the recording
  RxFrameResult res;
  float quality;
};

enum class Outcome { Ok, PayFail, HdrFail, Missed };

const char* outcomeName(Outcome o) {
  switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::PayFail: return "payload_fail";
    case Outcome::HdrFail: return "header_fail";
    case Outcome::Missed: return "missed";
  }
  return "?";
}

struct FrameScore {
  Outcome out = Outcome::Missed;
  double rxSec = -1.0;
  double snrDb = NAN, evmSnrDb = NAN, cfoHz = NAN, delayMs = NAN,
         pilotStab = NAN;
  float quality = 0.0f;
};

// Least-squares line y = a + b x.
bool fitLine(const std::vector<double>& x, const std::vector<double>& y,
             double& a, double& b) {
  const size_t n = x.size();
  if (n < 2) return false;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < n; ++i) {
    sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i];
  }
  const double den = double(n) * sxx - sx * sx;
  if (std::fabs(den) < 1e-12) return false;
  b = (double(n) * sxy - sx * sy) / den;
  a = (sy - b * sx) / double(n);
  return true;
}

int cmdScore(const ScoreArgs& a) {
  WavData w;
  if (!wavRead(a.wav, w)) die("cannot read %s", a.wav.c_str());
  Manifest m;
  if (!readManifest(a.manifest, m)) die("cannot read manifest %s", a.manifest.c_str());
  const double inRate = a.rateHz > 0 ? a.rateHz : double(w.sampleRate);
  std::printf("recording %s: %.1f s at %.1f Hz; manifest %zu frames, family %s\n",
              a.wav.c_str(), double(w.samples.size()) / inRate, inRate,
              m.frames.size(), plan(m.family).name);

  auto audio = resample(w.samples, inRate, kFsAudio);
  Downconverter down;
  std::vector<cfloat> bb;
  down.process(audio, bb);
  // --shift: the receiver was tuned off the sounding's dial frequency.
  // Mixing the complex baseband is exact; mixing the real audio is not.
  if (a.shiftHz != 0.0) {
    for (size_t i = 0; i < bb.size(); ++i) {
      const double ph = -2 * kPi * a.shiftHz * double(i) / kFsBaseband;
      bb[i] *= cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
  }
  const double gd = down.groupDelay12k();

  // Acquisition over the whole recording, then one decode per detection.
  // A window runs the longest manifest frame past the detection (the
  // receiver takes the FIRST frame in it and the header bounds its
  // length, so trailing content does no harm); detections that fall
  // inside a frame already decoded are sidelobes or partial matches of
  // that frame and are skipped. (Cutting each window at the next
  // detection instead truncated long robust frames at such sidelobes.)
  ChirpDetector det(m.family);
  std::vector<ChirpDetection> dets;
  const size_t chunk = 4096;
  for (size_t off = 0; off < bb.size(); off += chunk) {
    const size_t k = std::min(chunk, bb.size() - off);
    det.process(std::span<const cfloat>(bb.data() + off, k), dets);
  }
  double maxAir = 0.0;
  for (const auto& f : m.frames) maxAir = std::max(maxAir, f.airSec);
  std::vector<Decode> decodes;
  std::vector<double> undecodedDetSec;
  double busyUntil = -1.0;  // end of the last decoded frame, 12 kHz samples
  for (size_t i = 0; i < dets.size(); ++i) {
    if (dets[i].startSample < busyUntil) continue;
    const double pre = 0.15 * kFsBaseband;
    const double s0 = std::max(0.0, dets[i].startSample - pre);
    const double s1 = std::min(dets[i].startSample + (maxAir + 0.5) * kFsBaseband,
                               double(bb.size()));
    if (s1 <= s0 + 100) continue;
    std::span<const cfloat> win(bb.data() + size_t(s0), size_t(s1 - s0));
    RxStage st = RxStage::NoDetect;
    auto res = receiveFrame(m.family, win, &st);
    const double rxSec = (dets[i].startSample - gd) / kFsBaseband;
    if (res) {
      std::vector<uint8_t> dummy(res->header.length);
      TxFrameParams tp;
      tp.family = m.family;
      tp.sig = res->sig;
      tp.mode = res->header.mode;
      tp.cpMode = res->header.cpMode;
      tp.cssHeaderCopies = res->header.cssHeaderCopies;
      tp.payload = dummy;
      busyUntil = dets[i].startSample + computeGeometry(tp).totalSamples - pre;
      decodes.push_back({rxSec, std::move(*res), dets[i].quality});
    } else {
      undecodedDetSec.push_back(rxSec);
    }
  }

  // Time alignment from frames whose payload proves their seq.
  std::map<int, size_t> bySeq;
  for (size_t i = 0; i < m.frames.size(); ++i) bySeq[m.frames[i].seq] = i;
  std::vector<double> txT, rxT;
  std::vector<FrameScore> sc(m.frames.size());
  std::vector<bool> used(decodes.size(), false);
  for (size_t d = 0; d < decodes.size(); ++d) {
    const auto& r = decodes[d].res;
    if (r.header.frameType != kSoundFrameType || !r.payloadOk ||
        r.payload.size() < 3) {
      continue;
    }
    const int seq = (int(r.payload[0]) << 8) | r.payload[1];
    auto it = bySeq.find(seq);
    if (it == bySeq.end()) continue;
    const auto& f = m.frames[it->second];
    if (r.payload != makePayload(m.runSeed, seq, f.mode, f.payloadLen)) continue;
    used[d] = true;
    auto& s = sc[it->second];
    s.out = Outcome::Ok;
    s.rxSec = decodes[d].rxSec;
    txT.push_back(f.startSec);
    rxT.push_back(decodes[d].rxSec);
  }
  // Alignment. Audio is contiguous only WITHIN a keyed segment: between
  // segments the player's PTT waits (and the operator's own pauses, when
  // playing by hand) add real time the WAV does not contain. So the clock
  // rate is the pooled within-segment slope, and each segment gets its
  // own offset; a segment with no decoded frame falls back to the global
  // line through all of them.
  std::vector<int> txSeg;
  for (size_t i = 0; i < m.frames.size(); ++i) {
    if (sc[i].out == Outcome::Ok) txSeg.push_back(m.frames[i].segment);
  }
  double off = 0.0, slope = 1.0;
  bool aligned = fitLine(txT, rxT, off, slope) ||
                 (txT.size() == 1 && ((off = rxT[0] - txT[0]), true));
  int clockPairs = 0;
  double slopeSe = NAN;
  {
    std::map<int, std::pair<double, double>> mean;  // seg -> (tx, rx)
    std::map<int, int> cnt;
    for (size_t k = 0; k < txT.size(); ++k) {
      mean[txSeg[k]].first += txT[k];
      mean[txSeg[k]].second += rxT[k];
      ++cnt[txSeg[k]];
    }
    for (auto& [g, mr] : mean) { mr.first /= cnt[g]; mr.second /= cnt[g]; }
    double sxy = 0, sxx = 0;
    for (size_t k = 0; k < txT.size(); ++k) {
      const double dx = txT[k] - mean[txSeg[k]].first;
      sxy += dx * (rxT[k] - mean[txSeg[k]].second);
      sxx += dx * dx;
      clockPairs += cnt[txSeg[k]] > 1;
    }
    slope = sxx > 1.0 ? sxy / sxx : 1.0;
    // Standard error of the slope: detection timing is quantized to
    // ChirpDetector::kGridStep and jitters with noise, and segments are
    // short, so a single run pins the clock only to tens of ppm.
    double rss = 0.0;
    for (size_t k = 0; k < txT.size(); ++k) {
      const double e = (rxT[k] - mean[txSeg[k]].second) -
                       slope * (txT[k] - mean[txSeg[k]].first);
      rss += e * e;
    }
    const int dof = int(txT.size()) - int(mean.size()) - 1;
    if (dof > 0 && sxx > 1.0) slopeSe = std::sqrt(rss / dof / sxx);
  }
  std::map<int, double> segOff;
  {
    std::map<int, int> cnt;
    for (size_t k = 0; k < txT.size(); ++k) {
      segOff[txSeg[k]] += rxT[k] - slope * txT[k];
      ++cnt[txSeg[k]];
    }
    for (auto& [g, o] : segOff) o /= cnt[g];
  }
  if (!aligned) {
    std::printf("no frame decoded: cannot align the recording to the "
                "manifest (%zu detections, %zu headers)\n",
                dets.size(), decodes.size());
  }
  auto expectedRx = [&](const FrameRec& f) {
    auto it = segOff.find(f.segment);
    return it != segOff.end() ? it->second + slope * f.startSec
                              : off + slope * f.startSec;
  };
  const double tol = 0.35;  // s: detection/timing slack per frame

  if (aligned) {
    // Headers whose payload failed: match by mode + seqLow + time.
    for (size_t d = 0; d < decodes.size(); ++d) {
      if (used[d]) continue;
      const auto& r = decodes[d].res;
      for (size_t i = 0; i < m.frames.size(); ++i) {
        const auto& f = m.frames[i];
        if (sc[i].out != Outcome::Missed || r.header.mode != f.mode ||
            r.header.seqLow != uint8_t(f.seq) ||
            std::fabs(decodes[d].rxSec - expectedRx(f)) > tol) {
          continue;
        }
        used[d] = true;
        sc[i].out = Outcome::PayFail;
        sc[i].rxSec = decodes[d].rxSec;
        break;
      }
    }
    // Detections with no usable header near an expected frame start.
    for (double t : undecodedDetSec) {
      for (size_t i = 0; i < m.frames.size(); ++i) {
        if (sc[i].out == Outcome::Missed &&
            std::fabs(t - expectedRx(m.frames[i])) <= tol) {
          sc[i].out = Outcome::HdrFail;
          sc[i].rxSec = t;
          break;
        }
      }
    }
    // Copy the receiver's measurements onto every frame with a header.
    for (size_t d = 0; d < decodes.size(); ++d) {
      for (size_t i = 0; i < m.frames.size(); ++i) {
        if (sc[i].rxSec != decodes[d].rxSec) continue;
        const auto& r = decodes[d].res;
        sc[i].snrDb = r.snrDb;
        sc[i].evmSnrDb = r.evmSnrDb > -90 ? r.evmSnrDb : NAN;
        sc[i].cfoHz = r.cfoHz;
        sc[i].delayMs = r.delaySpreadMs;
        sc[i].pilotStab = r.pilotStability;
        sc[i].quality = decodes[d].quality;
      }
    }
  }

  // --- reports ---
  if (!a.csv.empty()) {
    FILE* f = std::fopen(a.csv.c_str(), "w");
    if (!f) die("cannot write %s", a.csv.c_str());
    std::fprintf(f, "seq,mode,segment,level_db,tx_s,rx_s,outcome,snr_db,"
                    "evm_snr_db,cfo_hz,delay_ms,pilot_stab,det_quality\n");
    for (size_t i = 0; i < m.frames.size(); ++i) {
      const auto& fr = m.frames[i];
      const auto& s = sc[i];
      std::fprintf(f, "%d,%d,%d,%.1f,%.3f,%.3f,%s,%.2f,%.2f,%.3f,%.3f,%.4f,%.3f\n",
                   fr.seq, fr.mode, fr.segment, fr.levelDb, fr.startSec,
                   s.rxSec, outcomeName(s.out), s.snrDb, s.evmSnrDb, s.cfoHz,
                   s.delayMs, s.pilotStab, double(s.quality));
    }
    std::fclose(f);
    std::printf("per-frame results: %s\n", a.csv.c_str());
  }

  struct Agg { int sent = 0, ok = 0, pay = 0, hdr = 0, miss = 0;
               double snr = 0, evm = 0; int nSnr = 0, nEvm = 0; };
  std::map<std::pair<double, int>, Agg> agg;  // (level, mode)
  for (size_t i = 0; i < m.frames.size(); ++i) {
    auto& g = agg[{m.frames[i].levelDb, m.frames[i].mode}];
    ++g.sent;
    switch (sc[i].out) {
      case Outcome::Ok: ++g.ok; break;
      case Outcome::PayFail: ++g.pay; break;
      case Outcome::HdrFail: ++g.hdr; break;
      case Outcome::Missed: ++g.miss; break;
    }
    if (!std::isnan(sc[i].snrDb)) { g.snr += sc[i].snrDb; ++g.nSnr; }
    if (!std::isnan(sc[i].evmSnrDb)) { g.evm += sc[i].evmSnrDb; ++g.nEvm; }
  }
  const bool sweep = agg.size() > 0 &&
      std::any_of(m.frames.begin(), m.frames.end(),
                  [](const FrameRec& f) { return f.levelDb != 0.0; });
  std::printf("\n%s%-5s %5s %4s %6s %6s %6s %6s %8s %8s %7s\n",
              sweep ? "level  " : "", "mode", "sent", "ok", "payF", "hdrF",
              "miss", "FER%", "snr_dB", "evm_dB", "floor");
  for (const auto& [key, g] : agg) {
    const int mode = key.second;
    const double fl = mode >= 4 ? floorDb(m.family, mode) : cssFloorDb(m.family, mode);
    if (sweep) std::printf("%5.1f  ", key.first);
    std::printf("M%-4d %5d %4d %6d %6d %6d %6.1f %8.1f %8.1f %7.1f\n", mode,
                g.sent, g.ok, g.pay, g.hdr, g.miss,
                100.0 * (g.sent - g.ok) / std::max(1, g.sent),
                g.nSnr ? g.snr / g.nSnr : NAN, g.nEvm ? g.evm / g.nEvm : NAN,
                fl);
  }

  // CFO over time: the drift rates 8.8 needs and the link's AFC sees.
  std::vector<double> ct, cf;
  for (size_t i = 0; i < m.frames.size(); ++i) {
    if (!std::isnan(sc[i].cfoHz) && sc[i].out != Outcome::HdrFail) {
      ct.push_back(sc[i].rxSec);
      cf.push_back(sc[i].cfoHz);
    }
  }
  double ca = 0, cb = 0;
  if (fitLine(ct, cf, ca, cb)) {
    double maxRate = 0.0, resid = 0.0;
    for (size_t i = 1; i < ct.size(); ++i) {
      const double dt = ct[i] - ct[i - 1];
      if (dt > 0.5 && dt < 20.0) {
        maxRate = std::max(maxRate, std::fabs(cf[i] - cf[i - 1]) / dt);
      }
    }
    for (size_t i = 0; i < ct.size(); ++i) {
      const double e = cf[i] - (ca + cb * ct[i]);
      resid += e * e;
    }
    std::printf("\nCFO: mean %.2f Hz, linear drift %.4f Hz/s over %.0f s, "
                "rms about the trend %.2f Hz, worst frame-to-frame rate "
                "%.3f Hz/s (%zu frames)\n",
                ca + cb * (ct.front() + ct.back()) / 2, cb,
                ct.back() - ct.front(), std::sqrt(resid / double(ct.size())),
                maxRate, ct.size());
  }
  if (aligned && clockPairs >= 2) {
    std::printf("clock: RX vs TX sample rate %+.1f +- %.1f ppm (%d frames, "
                "within-segment)%s\n",
                (slope - 1.0) * 1e6, slopeSe * 1e6, clockPairs,
                std::fabs(slope - 1.0) > 200e-6 ? "  ** large: check the "
                "recording's true sample rate (--rate) **" : "");
  }
  std::printf("acquisition: %zu detections, %zu headers decoded\n",
              dets.size(), decodes.size());
  return 0;
}

void usage() {
  std::printf(
      "onair_sounder - one-way on-air PHY tests (docs/ONAIR_TEST_PLAN.md)\n\n"
      "  onair_sounder gen   --call CALL [--family wide] [--modes 4..13]\n"
      "                      [--reps 5] [--levels 0,-3,-6] [--out f.wav]\n"
      "                      [--gap 0.4] [--segment 30] [--pause 3]\n"
      "                      [--payload N] [--seed N] [--gain-db -10] [--no-id]\n"
      "  onair_sounder play  --wav f.wav --device <index|name>\n"
      "                      [--ptt host:port] [--lead-ms 100] [--tail-ms 50]\n"
      "                      [--from-segment N]\n"
      "  onair_sounder score --wav recording.wav --manifest f.manifest\n"
      "                      [--csv out.csv] [--shift HZ] [--rate HZ]\n"
      "  onair_sounder devices\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }
  const std::string cmd = argv[1];
  auto need = [&](int& i) -> const char* {
    if (i + 1 >= argc) die("%s needs a value", argv[i]);
    return argv[++i];
  };
  if (cmd == "devices") return cmdDevices();
  if (cmd == "gen") {
    GenArgs a;
    for (int i = 2; i < argc; ++i) {
      const std::string s = argv[i];
      if (s == "--family") {
        auto f = familyFromString(need(i));
        if (!f) die("unknown family");
        a.family = *f;
      } else if (s == "--modes") a.modes = parseModes(need(i));
      else if (s == "--reps") a.reps = std::max(1, std::atoi(need(i)));
      else if (s == "--levels") a.levels = parseList(need(i));
      else if (s == "--call") a.call = need(i);
      else if (s == "--no-id") a.noId = true;
      else if (s == "--out") a.out = need(i);
      else if (s == "--gap") a.gapSec = std::atof(need(i));
      else if (s == "--segment") a.segmentSec = std::atof(need(i));
      else if (s == "--pause") a.pauseSec = std::atof(need(i));
      else if (s == "--payload") a.payloadLen = std::atoi(need(i));
      else if (s == "--seed") a.seed = uint32_t(std::strtoul(need(i), nullptr, 10));
      else if (s == "--gain-db") a.gainDb = std::atof(need(i));
      else if (s == "--wpm") a.wpm = std::atoi(need(i));
      else die("unknown option %s", s.c_str());
    }
    for (double lv : a.levels) {
      if (lv > 0.0) die("--levels are <= 0 dB (relative to the modem's own level)");
    }
    return cmdGen(a);
  }
  if (cmd == "play") {
    PlayArgs a;
    for (int i = 2; i < argc; ++i) {
      const std::string s = argv[i];
      if (s == "--wav") a.wav = need(i);
      else if (s == "--device") a.device = need(i);
      else if (s == "--ptt") a.ptt = need(i);
      else if (s == "--lead-ms") a.leadMs = std::atof(need(i));
      else if (s == "--tail-ms") a.tailMs = std::atof(need(i));
      else if (s == "--from-segment") a.fromSegment = std::max(0, std::atoi(need(i)));
      else die("unknown option %s", s.c_str());
    }
    if (a.wav.empty()) die("--wav is required");
    return cmdPlay(a);
  }
  if (cmd == "score") {
    ScoreArgs a;
    for (int i = 2; i < argc; ++i) {
      const std::string s = argv[i];
      if (s == "--wav") a.wav = need(i);
      else if (s == "--manifest") a.manifest = need(i);
      else if (s == "--csv") a.csv = need(i);
      else if (s == "--shift") a.shiftHz = std::atof(need(i));
      else if (s == "--rate") a.rateHz = std::atof(need(i));
      else die("unknown option %s", s.c_str());
    }
    if (a.wav.empty() || a.manifest.empty()) die("--wav and --manifest are required");
    return cmdScore(a);
  }
  usage();
  return 2;
}
