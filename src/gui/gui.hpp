// GUI shell (ARCHITECTURE.md "GUI", ROADMAP 5.6-5.8): SDL2 + Dear ImGui on
// the MAIN thread; the modem worker keeps running on its own thread and the
// GUI only ever reads published snapshots and writes through the control
// dispatcher. Compiled only under ENABLE_GUI (SKW_HAVE_GUI).
//
// Load-bearing rule (M5 gate): every GUI mutation goes through
// ModemWorker::control() - the same line protocol the TCP port uses. The
// GUI has no privileged path into modem state, so anything it can do a
// script can do, and there is exactly one place where state changes are
// validated.
//
// The GUI never waits on the modem: the worker holds its mutex through
// whole decode attempts (0.5-1.3 s), so status comes from the worker's
// published WorkerStatus, widget commands run on a ControlExecutor
// thread, and the waterfall and RX meter read their own copy of the
// input audio (the audio-callback tap) instead of worker telemetry.
#pragma once
#ifdef SKW_HAVE_GUI

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dsp/fft.hpp"
#include "io/audio.hpp"
#include "io/ring.hpp"
#include "tcp/executor.hpp"
#include "worker.hpp"

struct ImFont;

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
  std::string time;  // HH:MM:SS when answered
};

class Gui {
 public:
  explicit Gui(ModemWorker& worker);
  ~Gui();
  Gui(const Gui&) = delete;
  Gui& operator=(const Gui&) = delete;

  // Create the window + ImGui context. False on failure (caller falls
  // back to headless). The size is a request; the window is fitted to the
  // display it opens on.
  bool init(int width, int height);
  // One frame: pump SDL events, build the UI, present. Returns false when
  // the user closed the window.
  bool frame();
  void shutdown();

  // Synchronous dispatcher call, logged exactly as the widgets' commands
  // are (tests; the widgets themselves use the asynchronous path).
  std::string submit(const std::string& cmd);
  // The form of `cmd` the on-screen log shows (secrets masked).
  static std::string redactForLog(const std::string& cmd);
  // --- audio device selection (Settings) ------------------------------
  // The AUDIO STREAM is owned by main(), not the worker, so a device
  // change is a request the main loop performs between frames: it
  // stops the old backend, opens the new one, and reports the outcome
  // back via noteAudio().
  void setAudioSelection(int inDev, int outDev);  // current devices
  void setPortInfo(int data, int ctrl, int voice);
  // The audio callback's copy of the input (may be null: file mode, or
  // tests) - the waterfall then falls back to the worker's spectrum.
  void setAudioTap(SampleRing* tap) { tap_ = tap; }
  std::optional<std::pair<int, int>> takeAudioRequest();
  void noteAudio(const std::string& result);
  const std::deque<GuiLogLine>& log() const { return log_; }
  const GuiInstruments& instruments() const { return inst_; }
  void sampleTelemetry();  // pull a snapshot into the instrument history

  // --- widget state and actions ----------------------------------------
  // What the widgets show and edit. Every widget calls exactly one action
  // below, and test_gui drives the same actions headlessly. A command the
  // modem refuses puts its widget back to what the modem is running and
  // explains why in the notice line: a widget never shows a value the
  // modem is not using.
  struct Controls {
    char call[16] = {0};
    char connect[16] = {0};
    int family = 0;     // kFamilyNames index
    int mode = 0;       // 0 = AUTO, else M4..M13
    int cp = 0;         // 0 = AUTO
    int maxMode = 9;    // 0 = M4 .. 9 = M13
    bool listen = true, monitor = false, cwid = false, ultra = false;
    int pttMethod = 0;  // kPttMethods index
    char pttPort[64] = {0};
    int pttLead = 50, pttTail = 20;
    char psk[80] = {0};
    bool encrypt = false;
  };
  struct Notice {
    std::string text;
    bool error = false;
    uint64_t seq = 0;  // bumps on every new notice
  };
  Controls& controls() { return ctl_; }
  const Notice& notice() const { return notice_; }
  // Read every widget's value from the modem (start-up; blocking).
  void syncFromModem();
  // Re-read the worker's published status and follow changes made
  // elsewhere (the TCP control port, the companion app).
  void refreshStatus();
  // Tests: wait for queued commands, then apply their answers.
  bool waitIdle(int timeoutMs);

  void actConnect();
  void actDisconnect();
  void actCq();
  void actBeacon();
  void actAbort();
  void actMode(int prev);
  void actCp(int prev);
  void actCall();
  void actFamily(int prev);
  void actMaxMode(int prev);
  void actListen();
  void actMonitor();
  void actCwid();
  void actUltra();
  void actTxTest(bool on);
  void actPttMethod(int prev);
  void actPttPort();
  void actPttTimes();
  void actInstallPsk();
  void actEncryption();
  void actKeyRotate();
  void actSecurityRefresh();

 private:
  // A setting command: on an ERROR answer run `revert` and explain; on
  // success run `ok` (if any).
  void apply(const std::string& cmd, std::function<void()> revert,
             std::function<void(const std::string&)> ok = nullptr);
  void setNotice(std::string text, bool error);
  bool needsCallsign();  // placeholder callsign: explain and refuse
  void drawNotice();
  void saveScreenshot(const char* path);
  void runDebugScript();  // SKW_GUI_SCRIPT (gui.cpp)
  struct ScriptStep {
    int frame;
    std::string cmd;
  };
  std::vector<ScriptStep> script_;
  size_t scriptPos_ = 0;
  bool scriptLoaded_ = false, scriptRelease_ = false, scriptQuit_ = false;
  std::string pendingShot_;
  int frameNo_ = 0;
  // Asynchronous widget command: queued to the executor, logged (and
  // `then` called with the answer) when it completes.
  void send(const std::string& cmd,
            std::function<void(const std::string&)> then = nullptr);
  void drainResults();
  void pushLog(const std::string& cmd, const std::string& resp);
  void loadFonts(float scale);
  void applyTheme();
  void fitWindowToDisplay(int reqW, int reqH);

  // Panels (fixed layout: nothing floats over anything else).
  void drawTopBar();
  void drawSessionCard();
  void drawLinkCard();
  void drawLevelsCard();
  void drawWaterfallCard(float height);
  void drawConstellationCard(float side);
  void drawLogCard();
  void drawSettingsWindow();
  void drawStationTab();
  void drawAudioTab();
  void drawPttTab();
  void drawSecurityTab();

  // Waterfall source: audio tap (preferred) or worker spectrum rows.
  void pumpSpectrum();
  void pushWaterfallRow(const std::vector<float>& dbRow);
  void uploadWaterfall();

  ModemWorker& worker_;
  void* window_ = nullptr;    // SDL_Window
  void* renderer_ = nullptr;  // SDL_Renderer
  bool ready_ = false;
  float dpiScale_ = 1.0f;
  ImFont* fontUi_ = nullptr;
  ImFont* fontBig_ = nullptr;
  ImFont* fontMono_ = nullptr;
  std::unique_ptr<ControlExecutor> exec_;
  std::map<uint64_t, std::function<void(const std::string&)>> pending_;
  uint64_t nextTag_ = 1;

  Telemetry snap_{};
  WorkerStatus status_;
  std::map<std::string, std::string> fields_;  // parsed STATUS line
  double lastStatusSec_ = -1.0;
  GuiInstruments inst_;
  std::deque<GuiLogLine> log_;

  // Waterfall: kWfCols x kWfRows RGBA, newest row on top.
  static constexpr int kWfCols = 512;
  static constexpr int kWfRows = 240;
  static constexpr int kSpecFft = 4096;  // 11.7 Hz bins at 48 kHz
  static constexpr int kSpecHop = 2400;  // 20 rows/s
  SampleRing* tap_ = nullptr;
  std::vector<float> tapBuf_;            // recent input samples
  std::unique_ptr<Fft> fft_;
  std::vector<uint32_t> wfPixels_;
  void* wfTexture_ = nullptr;            // SDL_Texture
  bool wfDirty_ = false;
  uint32_t lastSpectrumSeq_ = 0;
  float wfFloorDb_ = -100.0f, wfCeilDb_ = -40.0f;
  bool wfAutoScale_ = true;
  float wfMaxHz_ = 3000.0f;              // display span (2 x band centre)
  float rxMeterDb_ = -120.0f;            // from the tap, smoothed
  std::deque<float> rxMeterHist_;

  Controls ctl_;
  Notice notice_;
  double noticeAt_ = -1e9;  // SDL seconds when the notice was set
  // Last modem values seen in the published status: a widget follows the
  // modem only when that value CHANGES, so an edit in progress is kept.
  std::string seenFamily_, seenMode_, seenCall_, seenListen_;
  // Last values the modem accepted for the PTT text fields (revert target).
  std::string pttPortSaved_;
  int pttLeadSaved_ = 50, pttTailSaved_ = 20;
  int portData_ = 0, portCtrl_ = 0, portVoice_ = 0;
  std::string securityLine_;
  bool showSettings_ = false;
  std::vector<AudioDeviceInfo> audioDevs_;
  int audioInSel_ = -1;   // device INDEX (not combo position)
  int audioOutSel_ = -1;
  std::optional<std::pair<int, int>> audioReq_;
};

}  // namespace skw

#endif  // SKW_HAVE_GUI
