// Skywave process entry: CLI, wiring, headless loop (ARCHITECTURE.md).
#include <algorithm>
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
#include "tcp/executor.hpp"
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
      "  --audio-in <index|name>  PortAudio input device (name preferred)\n"
      "  --audio-out <index|name> PortAudio output device (name preferred)\n"
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
    else if (!std::strcmp(s, "--audio-in")) { if (auto* v = need(i, s)) skw::parseAudioDevice(v, cfg.inputDevice, cfg.inputDeviceName); }
    else if (!std::strcmp(s, "--audio-out")) { if (auto* v = need(i, s)) skw::parseAudioDevice(v, cfg.outputDevice, cfg.outputDeviceName); }
    else if (!std::strcmp(s, "--log-level")) { if (auto* v = need(i, s)) cfg.logLevel = atoi(v); }
    else if (!std::strcmp(s, "--log-file")) { if (auto* v = need(i, s)) cfg.logFile = v; }
    else {
      std::fprintf(stderr, "unknown option: %s\n", s);
      a.parseOk = false;
    }
  }
  return a;
}

// Name of the device at a PortAudio index ("" if none): the GUI picks by
// index, the config stores by name.
std::string deviceNameAt(int index) {
  for (const auto& d : skw::listAudioDevices()) {
    if (d.index == index) return d.name;
  }
  return "";
}

// Index to show as selected: a configured name, resolved now.
int selectedDevice(const std::string& name, int index, bool input) {
  return name.empty() ? index : skw::findAudioDevice(name, input);
}

}  // namespace

int main(int argc, char** argv) {
  skw::Config cfg;
  // The configuration exactly as LOADED from the INI (before CLI
  // overrides): the base of the exit-time three-way merge.
  skw::Config loadedCfg;
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
    loadedCfg = cfg;
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
  // RX ring: 8 s (as the ST2 world uses). At 1 s, one long-window decode
  // under load overran it and the audio callback dropped samples silently.
  // Band profile before anything band-dependent is built (the worker
  // re-selects it too, and rebuilds its own converters).
  skw::setActiveBand(cfg.band);
  skw::SampleRing rxRing(skw::kFsAudio * 8);
  // GUI copy of the input (waterfall, level meter): written by the audio
  // callback alongside rxRing, read by the GUI on the main thread.
  skw::SampleRing guiTap(skw::kFsAudio * 2);
  skw::SampleRing* const guiTapPtr = cfg.guiEnabled ? &guiTap : nullptr;
  skw::SampleRing txRing(skw::kFsAudio);

  const bool fileMode = !pre.replayPath.empty() || !pre.recordPath.empty();
  auto audio = fileMode
                   ? skw::makeFileBackend(pre.replayPath, pre.recordPath,
                                          rxRing, txRing)
                   : skw::makeDeviceBackend(
                         {cfg.inputDevice, cfg.outputDevice,
                          cfg.inputDeviceName, cfg.outputDeviceName,
                          cfg.inputGainDb, cfg.outputGainDb, guiTapPtr},
                         rxRing, txRing);
  if (!audio->start()) return 1;

  skw::SteadyClock clock;
  skw::ModemWorker worker(cfg, clock, rxRing, txRing);
  worker.setOutputLatencySec(audio->outputLatencySec());

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
    uint32_t voiceGen = voiceSrv.clientGeneration();
#endif
    SKW_INFO("tcp: data=%u control=%u", dataSrv.boundPort(),
             ctrlSrv.boundPort());
    skw::LineBuffer ctrlLines;
    uint32_t ctrlGen = ctrlSrv.clientGeneration();
    skw::ControlExecutor ctrlExec(
        [&worker](const std::string& l) { return worker.control(l); });
    // Data-port intake stops while the modem's own TX backlog (coalesce
    // window + sealed messages) is this deep: unread bytes then stay in
    // the kernel socket buffer and TCP flow control pushes back on the
    // client. Several minutes of air time even at the fastest modes, so
    // normal transfers never notice.
    constexpr size_t kDataInBacklogCap = 4u << 20;  // 4 MiB
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
      gui->setAudioTap(guiTapPtr);
      if (!gui->init(cfg.guiWidth, cfg.guiHeight)) {
        SKW_WARN("gui: no window available; continuing headless");
        gui.reset();
      } else {
        gui->setAudioSelection(
            selectedDevice(cfg.inputDeviceName, cfg.inputDevice, true),
            selectedDevice(cfg.outputDeviceName, cfg.outputDevice, false));
        gui->setPortInfo(cfg.dataPort, cfg.ctrlPort, cfg.voicePort);
      }
    }
#endif
    while (!g_stop) {
      // Data port: client bytes -> engine; delivered bytes -> client.
      const size_t backlog = worker.txBufferBytes();
      const size_t room =
          backlog >= kDataInBacklogCap ? 0 : kDataInBacklogCap - backlog;
      auto inBytes =
          dataSrv.poll(std::min(room, skw::TcpServer::kDefaultReadCap));
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
      {
        auto vin = voiceSrv.poll();
        if (voiceSrv.clientGeneration() != voiceGen) {
          voiceGen = voiceSrv.clientGeneration();
          voiceParse.reset();  // a new client starts on a record boundary
        }
        voiceParse.feed(vin);
      }
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
      // A dropped client's unterminated line ("CONNECT W1A") must not
      // prefix the next client's first command.
      auto ctrlIn = ctrlSrv.poll();
      if (ctrlSrv.clientGeneration() != ctrlGen) {
        ctrlGen = ctrlSrv.clientGeneration();
        ctrlLines.reset();
      }
      ctrlLines.feed(ctrlIn);
      // Commands run on the executor's thread (it may wait out a decode);
      // answers go back in order, and only to the client that asked.
      for (auto& line : ctrlLines.takeLines()) ctrlExec.post(line, ctrlGen);
      for (auto& r : ctrlExec.takeResults()) {
        if (r.tag == ctrlGen && ctrlSrv.clientConnected()) {
          ctrlSrv.send(r.response + "\n");
        }
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
          audio = skw::makeDeviceBackend({req->first, req->second, "", "",
                                          cfg.inputGainDb, cfg.outputGainDb, guiTapPtr},
                                         rxRing, txRing);
          if (audio->start()) {
            worker.setOutputLatencySec(audio->outputLatencySec());
            // Persists via --config save, BY NAME: the index is only
            // valid until the next device comes or goes.
            cfg.inputDevice = req->first;
            cfg.outputDevice = req->second;
            cfg.inputDeviceName = deviceNameAt(req->first);
            cfg.outputDeviceName = deviceNameAt(req->second);
            gui->noteAudio("OK: stream running on in=" +
                           std::to_string(req->first) + " out=" +
                           std::to_string(req->second));
          } else {
            SKW_WARN("gui: audio device change failed; reverting");
            audio = skw::makeDeviceBackend({cfg.inputDevice,
                                            cfg.outputDevice,
                                            cfg.inputDeviceName,
                                            cfg.outputDeviceName,
                                            cfg.inputGainDb,
                                            cfg.outputGainDb, guiTapPtr},
                                           rxRing, txRing);
            const bool back = audio->start();
            if (back) worker.setOutputLatencySec(audio->outputLatencySec());
            gui->noteAudio(back ? "FAILED: kept previous devices"
                                : "FAILED: previous devices also refused "
                                  "- audio is DOWN");
            gui->setAudioSelection(
            selectedDevice(cfg.inputDeviceName, cfg.inputDevice, true),
            selectedDevice(cfg.outputDeviceName, cfg.outputDevice, false));
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
  SKW_INFO("shutdown: rx=%llu tx=%llu samples, rx overrun=%llu samples",
           (unsigned long long)t.rxSamplesTotal,
           (unsigned long long)t.txSamplesTotal,
           (unsigned long long)rxRing.dropped());
  SKW_INFO("shutdown: rx decode %llu attempts, %.1f s total, mean %.1f ms, "
           "max %.1f ms",
           (unsigned long long)t.rxDecodeCalls, double(t.rxDecodeUsTotal) * 1e-6,
           t.rxDecodeCalls ? double(t.rxDecodeUsTotal) / double(t.rxDecodeCalls) * 1e-3
                           : 0.0,
           double(t.rxDecodeUsMax) * 1e-3);

  // Save what the modem is running NOW: the worker's copy carries every
  // control-port change (MYCALL, FAMILY, PTT*, ENCRYPTKEY...), while
  // main's `cfg` is the startup snapshot plus the audio devices that only
  // main manages. Saving the startup copy threw away all runtime changes.
  // Merged, not overwritten: a key the operator edited in the file while
  // we ran survives unless it was also changed at runtime (runtime wins,
  // WARN logged). CLI overrides differ from the loaded base, so they
  // persist exactly as before.
  if (!pre.configPath.empty()) {
    skw::Config out = worker.config();
    out.inputDevice = cfg.inputDevice;
    out.outputDevice = cfg.outputDevice;
    out.inputDeviceName = cfg.inputDeviceName;
    out.outputDeviceName = cfg.outputDeviceName;
    if (!out.saveMerged(pre.configPath, loadedCfg)) {
      SKW_WARN("config: cannot write %s", pre.configPath.c_str());
    }
  }
  return 0;
}
