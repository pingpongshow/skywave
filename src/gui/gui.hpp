// GUI shell (ARCHITECTURE.md "GUI", ROADMAP 5.6-5.8): SDL2 + Dear ImGui on
// the MAIN thread; the modem worker keeps running on its own thread and the
// GUI only ever reads telemetry snapshots and writes through the control
// dispatcher. Compiled only under ENABLE_GUI (SKW_HAVE_GUI).
//
// Load-bearing rule (M5 gate): every GUI mutation goes through
// ModemWorker::control() - the same line protocol the TCP port uses. The
// GUI has no privileged path into modem state, so anything it can do a
// script can do, and there is exactly one place where state changes are
// validated.
#pragma once
#ifdef SKW_HAVE_GUI

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "io/audio.hpp"
#include "worker.hpp"

namespace skw {

// Rolling instrument data the GUI derives from telemetry snapshots.
struct GuiInstruments {
  static constexpr size_t kHistory = 240;  // ~2 min at 2 Hz
  std::deque<float> rxLevel, txLevel, noiseFloor;
  std::deque<uint8_t> modeHistory;
  void push(const Telemetry& t);
};

// Command log line (what the GUI sent + what the dispatcher answered).
struct GuiLogLine {
  std::string cmd;
  std::string response;
};

class Gui {
 public:
  explicit Gui(ModemWorker& worker);
  ~Gui();
  Gui(const Gui&) = delete;
  Gui& operator=(const Gui&) = delete;

  // Create the window + ImGui context. False on failure (caller falls
  // back to headless).
  bool init(int width, int height);
  // One frame: pump SDL events, build the UI, present. Returns false when
  // the user closed the window.
  bool frame();
  void shutdown();

  // Exposed for tests: run the dispatcher exactly as the widgets do.
  std::string submit(const std::string& cmd);
  // --- audio device selection (Settings panel) ------------------------
  // The AUDIO STREAM is owned by main(), not the worker, so a device
  // change is a request the main loop performs between frames: it
  // stops the old backend, opens the new one, and reports the outcome
  // back via noteAudio().
  void setAudioSelection(int inDev, int outDev);  // current devices
  void setPortInfo(int data, int ctrl, int voice);
  std::optional<std::pair<int, int>> takeAudioRequest();
  void noteAudio(const std::string& result);
  const std::vector<GuiLogLine>& log() const { return log_; }
  const GuiInstruments& instruments() const { return inst_; }
  void sampleTelemetry();  // pull a snapshot into the instrument history

 private:
  void drawSessionPanel();
  void drawLadderPanel();
  void drawInstrumentPanel();
  void drawSettingsPanel();
  void drawLogPanel();

  ModemWorker& worker_;
  void* window_ = nullptr;    // SDL_Window
  void* renderer_ = nullptr;  // SDL_Renderer
  bool ready_ = false;
  Telemetry snap_{};
  GuiInstruments inst_;
  std::vector<GuiLogLine> log_;
  // Settings-panel edit buffers (applied via submit()).
  char callBuf_[16] = {0};
  char connectBuf_[16] = {0};
  int familyIdx_ = 0;
  int modeIdx_ = 0;   // 0 = AUTO, else M4..M13
  // Cached status strings (refreshed at 4 Hz in sampleTelemetry - the
  // string helpers take the modem mutex and must not run per frame).
  std::string strFsmState_, strMyCall_, strFamily_, strMode_;
  std::string strStatus_, strFreqInfo_, strAudioLevel_;
  double netBps_ = 0.0;
  int portData_ = 0, portCtrl_ = 0, portVoice_ = 0;
  bool ultraOn_ = false, listenOn_ = true, cwidOn_ = false;
  int maxModeIdx_ = 9;      // 0 = M4 .. 9 = M13
  char pttMethodBuf_[24] = {0};
  char pttPortBuf_[64] = {0};
  int pttLead_ = 50, pttTail_ = 20;
  // Window visibility (menu-driven). Settings lives behind the menu
  // bar; the operating panels are on by default.
  bool showSettings_ = false;
  bool showSession_ = true, showLadder_ = true;
  bool showInstruments_ = true, showLog_ = true;
  double lastStrPollSec_ = -1.0;
  // Audio device picker state.
  std::vector<AudioDeviceInfo> audioDevs_;
  int audioInSel_ = -1;   // device INDEX (not combo position)
  int audioOutSel_ = -1;
  std::optional<std::pair<int, int>> audioReq_;
  int cpIdx_ = 0;     // 0 = AUTO
  bool monitor_ = false;
};

}  // namespace skw

#endif  // SKW_HAVE_GUI
