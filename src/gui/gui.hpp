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
#include <string>
#include <vector>

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
  int cpIdx_ = 0;     // 0 = AUTO
  bool monitor_ = false;
};

}  // namespace skw

#endif  // SKW_HAVE_GUI
