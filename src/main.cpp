// Skywave process entry: CLI, wiring, headless loop (ARCHITECTURE.md).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/constants.hpp"
#include "core/log.hpp"
#include "io/audio.hpp"
#include "tcp/server.hpp"
#include "worker.hpp"
#ifdef SKW_HAVE_VOICE
#include "voice/session.hpp"
#endif
#ifdef SKW_HAVE_GUI
#include "gui/gui.hpp"
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

void printHelp() {
  std::printf(
      "skywave %s - HF software audio modem\n"
      "\n"
      "  --gui                    launch GUI (requires GUI build)\n"
      "  --callsign <CALL>        local callsign\n"
      "  --config <path>          INI load/save path\n"
      "  --family <name>          wide|mid|narrow|ultra\n"
      "  --data-port <n>          TCP data port (default %u)\n"
      "  --ctrl-port <n>          TCP control port (default %u)\n"
      "  --voice-port <n>         TCP voice port (default %u)\n"
      "  --audio-in <index>       PortAudio input device\n"
      "  --audio-out <index>      PortAudio output device\n"
      "  --list-audio             list audio devices and exit\n"
      "  --replay <wav>           feed RX from WAV file (test mode)\n"
      "  --record <wav>           record TX audio to WAV file\n"
      "  --log-level <0-4>        0=TRACE .. 4=ERROR\n"
      "  --log-file <path>        log to file\n"
      "  --version                print version\n"
      "  --help                   this text\n",
      SKW_VERSION, skw::kPortData, skw::kPortCtrl, skw::kPortVoice);
}

struct CliArgs {
  std::string configPath;
  std::string replayPath;
  std::string recordPath;
  bool listAudio = false;
  bool showHelp = false;
  bool showVersion = false;
  bool parseOk = true;
};

CliArgs parseCli(int argc, char** argv, skw::Config& cfg) {
  CliArgs a;
  auto need = [&](int& i, const char* opt) -> const char* {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing argument for %s\n", opt);
      a.parseOk = false;
      return nullptr;
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    if (!std::strcmp(s, "--help")) a.showHelp = true;
    else if (!std::strcmp(s, "--version")) a.showVersion = true;
    else if (!std::strcmp(s, "--list-audio")) a.listAudio = true;
    else if (!std::strcmp(s, "--gui")) cfg.guiEnabled = true;
    else if (!std::strcmp(s, "--callsign")) { if (auto* v = need(i, s)) cfg.callsign = v; }
    else if (!std::strcmp(s, "--config")) { if (auto* v = need(i, s)) a.configPath = v; }
    else if (!std::strcmp(s, "--replay")) { if (auto* v = need(i, s)) a.replayPath = v; }
    else if (!std::strcmp(s, "--record")) { if (auto* v = need(i, s)) a.recordPath = v; }
    else if (!std::strcmp(s, "--family")) {
      if (auto* v = need(i, s)) {
        if (auto f = skw::familyFromString(v)) cfg.family = *f;
        else { std::fprintf(stderr, "bad family: %s\n", v); a.parseOk = false; }
      }
    }
    else if (!std::strcmp(s, "--data-port")) { if (auto* v = need(i, s)) cfg.dataPort = uint16_t(atoi(v)); }
    else if (!std::strcmp(s, "--ctrl-port")) { if (auto* v = need(i, s)) cfg.ctrlPort = uint16_t(atoi(v)); }
    else if (!std::strcmp(s, "--voice-port")) { if (auto* v = need(i, s)) cfg.voicePort = uint16_t(atoi(v)); }
    else if (!std::strcmp(s, "--audio-in")) { if (auto* v = need(i, s)) cfg.inputDevice = atoi(v); }
    else if (!std::strcmp(s, "--audio-out")) { if (auto* v = need(i, s)) cfg.outputDevice = atoi(v); }
    else if (!std::strcmp(s, "--log-level")) { if (auto* v = need(i, s)) cfg.logLevel = atoi(v); }
    else if (!std::strcmp(s, "--log-file")) { if (auto* v = need(i, s)) cfg.logFile = v; }
    else {
      std::fprintf(stderr, "unknown option: %s\n", s);
      a.parseOk = false;
    }
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  skw::Config cfg;
  // Two-pass CLI so --config loads first, then CLI overrides win
  // (CONFIGURATION.md precedence).
  CliArgs pre = parseCli(argc, argv, cfg);
  if (!pre.parseOk) return 2;
  if (pre.showHelp) { printHelp(); return 0; }
  if (pre.showVersion) { std::printf("skywave %s\n", SKW_VERSION); return 0; }
  if (!pre.configPath.empty()) {
    cfg = skw::Config{};
    if (!cfg.load(pre.configPath)) {
      SKW_WARN("config: cannot read %s (starting from defaults)",
               pre.configPath.c_str());
    }
    parseCli(argc, argv, cfg);  // overrides
  }

  skw::log::setLevel(skw::LogLevel(cfg.logLevel));
  if (!cfg.logFile.empty()) skw::log::setFile(cfg.logFile);

  if (pre.listAudio) {
    auto devices = skw::listAudioDevices();
    std::printf("index  in  out  name\n");
    for (const auto& d : devices) {
      std::printf("%5d %3d %4d  %s%s%s\n", d.index, d.maxInputChannels,
                  d.maxOutputChannels, d.name.c_str(),
                  d.defaultInput ? " [default in]" : "",
                  d.defaultOutput ? " [default out]" : "");
    }
    return 0;
  }

  SKW_INFO("skywave %s starting (family=%s, band=%s)", SKW_VERSION,
           skw::toString(cfg.family).c_str(), skw::toString(cfg.band).c_str());
  if (cfg.family == skw::Family::Ultra && !cfg.ultraEnable) {
    SKW_ERROR("family ULTRA requires modem.ultra_enable=true");
    return 2;
  }

  // Rings sized for ~1 s of audio: deep enough for scheduling hiccups.
  skw::SampleRing rxRing(skw::kFsAudio);
  skw::SampleRing txRing(skw::kFsAudio);

  const bool fileMode = !pre.replayPath.empty() || !pre.recordPath.empty();
  auto audio = fileMode
                   ? skw::makeFileBackend(pre.replayPath, pre.recordPath,
                                          rxRing, txRing)
                   : skw::makeDeviceBackend(
                         {cfg.inputDevice, cfg.outputDevice, cfg.inputGainDb,
                          cfg.outputGainDb},
                         rxRing, txRing);
  if (!audio->start()) return 1;

  skw::SteadyClock clock;
  skw::ModemWorker worker(cfg, clock, rxRing, txRing);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  if (fileMode) {
    // File mode runs the worker inline, faster than real time, until the
    // replay is exhausted and the pipeline is drained.
    while (!g_stop) {
      bool srcDone = audio->finished();
      size_t processed = worker.runOnce();
      if (srcDone && processed == 0) break;
    }
    audio->finished();  // final TX drain
  } else {
    // TCP surface (INTERFACES.md): data + control ports polled from this
    // thread; the worker owns the modem and is bridged through its
    // thread-safe queues. Voice port lands in Phase 5.
    skw::TcpServer dataSrv, ctrlSrv, voiceSrv;
    if (!dataSrv.open(cfg.dataPort) || !ctrlSrv.open(cfg.ctrlPort)) {
      SKW_ERROR("cannot bind TCP ports %u/%u", cfg.dataPort, cfg.ctrlPort);
      audio->stop();
      return 1;
    }
#ifdef SKW_HAVE_VOICE
    if (!voiceSrv.open(cfg.voicePort)) {
      SKW_WARN("cannot bind voice port %u (voice media unavailable)",
               cfg.voicePort);
    }
    skw::VoiceRecordParser voiceParse;
    uint32_t voiceTxSeq = 0;
    uint64_t voiceSamples = 0;
#endif
    SKW_INFO("tcp: data=%u control=%u", dataSrv.boundPort(),
             ctrlSrv.boundPort());
    skw::LineBuffer ctrlLines;
    // Delivered payload waiting for a data client. Bounded: an unattended
    // modem receiving a large file must not grow this without limit, and
    // stale payload is worthless to a client that attaches later anyway,
    // so the OLDEST bytes are dropped once the cap is passed.
    constexpr size_t kDataHoldCap = 1u << 20;  // 1 MiB
    std::vector<uint8_t> dataOut;
    bool dataHoldWarned = false;
    worker.start();
#ifdef SKW_HAVE_GUI
    // GUI runs on THIS (main) thread - SDL requires it - while the worker
    // keeps its own thread. A failed window falls back to headless.
    std::optional<skw::Gui> gui;
    if (cfg.guiEnabled) {
      gui.emplace(worker);
      if (!gui->init(cfg.guiWidth, cfg.guiHeight)) {
        SKW_WARN("gui: no window available; continuing headless");
        gui.reset();
      } else {
        gui->setAudioSelection(cfg.inputDevice, cfg.outputDevice);
        gui->setPortInfo(cfg.dataPort, cfg.ctrlPort, cfg.voicePort);
      }
    }
#endif
    while (!g_stop) {
      // Data port: client bytes -> engine; delivered bytes -> client.
      auto inBytes = dataSrv.poll();
      if (!inBytes.empty()) worker.submitAppData(inBytes);
      auto outBytes = worker.takeAppData();
      dataOut.insert(dataOut.end(), outBytes.begin(), outBytes.end());
      if (!dataOut.empty() && dataSrv.clientConnected()) {
        dataSrv.send(dataOut);
        dataOut.clear();
        dataHoldWarned = false;
      } else if (dataOut.size() > kDataHoldCap) {
        const size_t drop = dataOut.size() - kDataHoldCap;
        dataOut.erase(dataOut.begin(), dataOut.begin() + long(drop));
        if (!dataHoldWarned) {
          dataHoldWarned = true;
          SKW_WARN("data port: no client attached; discarding delivered "
                   "payload beyond %zu bytes", kDataHoldCap);
        }
      }
      for (auto& rec : worker.takeMonitorRecords()) {
        if (dataSrv.clientConnected()) dataSrv.send(rec);
      }
#ifdef SKW_HAVE_VOICE
      // Voice port: uplink PCM records -> encoder; decoded downlink PCM
      // back out as records (INTERFACES.md; media is PCM only).
      voiceParse.feed(voiceSrv.poll());
      for (auto& rec : voiceParse.take()) {
        if (rec.type == skw::kVoiceRecUplink) worker.submitVoicePcm(rec.pcm);
      }
      if (voiceSrv.clientConnected()) {
        auto pcm = worker.takeVoicePcm(800);  // 100 ms
        if (!pcm.empty()) {
          skw::VoiceRecord out;
          out.type = skw::kVoiceRecDownlink;
          out.sequence = voiceTxSeq++;
          out.sampleCounter = voiceSamples;
          voiceSamples += pcm.size();
          out.pcm = std::move(pcm);
          voiceSrv.send(out.pack());
        }
      }
#endif
      // Control port: lines -> dispatcher; EVT:/MON: notifications out.
      ctrlLines.feed(ctrlSrv.poll());
      for (auto& line : ctrlLines.takeLines()) {
        ctrlSrv.send(worker.control(line) + "\n");
      }
      for (auto& note : worker.takeNotifications()) {
        if (ctrlSrv.clientConnected()) ctrlSrv.send(note + "\n");
      }
#ifdef SKW_HAVE_GUI
      if (gui) {
        if (!gui->frame()) break;  // window closed
        // Audio device change from the Settings panel. The stream is
        // owned HERE, so the swap happens between frames: stop, reopen
        // on the requested devices, fall back to the old pair if the
        // new one refuses to start. The worker keeps running - the
        // rings just pause while the stream is down.
        if (auto req = gui->takeAudioRequest(); req && !fileMode) {
          audio->stop();
          audio = skw::makeDeviceBackend({req->first, req->second,
                                          cfg.inputGainDb, cfg.outputGainDb},
                                         rxRing, txRing);
          if (audio->start()) {
            cfg.inputDevice = req->first;    // persists via --config save
            cfg.outputDevice = req->second;
            gui->noteAudio("OK: stream running on in=" +
                           std::to_string(req->first) + " out=" +
                           std::to_string(req->second));
          } else {
            SKW_WARN("gui: audio device change failed; reverting");
            audio = skw::makeDeviceBackend({cfg.inputDevice,
                                            cfg.outputDevice,
                                            cfg.inputGainDb,
                                            cfg.outputGainDb},
                                           rxRing, txRing);
            const bool back = audio->start();
            gui->noteAudio(back ? "FAILED: kept previous devices"
                                : "FAILED: previous devices also refused "
                                  "- audio is DOWN");
            gui->setAudioSelection(cfg.inputDevice, cfg.outputDevice);
          }
        }
        continue;                  // vsync paces the loop
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#ifdef SKW_HAVE_GUI
    if (gui) gui->shutdown();
#endif
    worker.stop();
  }

  audio->stop();
  auto t = worker.telemetry();
  SKW_INFO("shutdown: rx=%llu tx=%llu samples",
           (unsigned long long)t.rxSamplesTotal,
           (unsigned long long)t.txSamplesTotal);

  if (!pre.configPath.empty()) cfg.save(pre.configPath);
  return 0;
}
