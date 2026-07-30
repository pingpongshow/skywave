#ifdef SKW_HAVE_GUI

#include "gui/gui.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>

#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "core/log.hpp"
#include "imgui.h"

namespace skw {
namespace {

const char* kFamilyNames[] = {"WIDE", "MID", "NARROW", "ULTRA"};
const char* kCpNames[] = {"AUTO", "SHORT", "MEDIUM", "LONG"};

// Mode combo: index 0 is AUTO, 1..10 are M4..M13.
std::string modeArg(int idx) {
  return idx == 0 ? "AUTO" : "M" + std::to_string(idx + 3);
}

void plotSeries(const char* label, const std::deque<float>& d, float lo,
                float hi) {
  if (d.empty()) return;
  std::vector<float> flat(d.begin(), d.end());
  ImGui::PlotLines(label, flat.data(), int(flat.size()), 0, nullptr, lo, hi,
                   ImVec2(0, 60));
}

}  // namespace

void GuiInstruments::push(const Telemetry& t) {
  auto trim = [](auto& q) {
    while (q.size() > kHistory) q.pop_front();
  };
  rxLevel.push_back(t.rxLevelDbfs);
  txLevel.push_back(t.txLevelDbfs);
  noiseFloor.push_back(t.noiseFloorDbfs);
  modeHistory.push_back(t.mode);
  trim(rxLevel);
  trim(txLevel);
  trim(noiseFloor);
  trim(modeHistory);
}

Gui::Gui(ModemWorker& worker) : worker_(worker) {}
Gui::~Gui() { shutdown(); }

std::string Gui::submit(const std::string& cmd) {
  // THE rule: no privileged path. Everything the GUI changes goes through
  // the same dispatcher the TCP control port uses.
  const std::string resp = worker_.control(cmd);
  log_.push_back({cmd, resp});
  if (log_.size() > 200) log_.erase(log_.begin());
  return resp;
}

void Gui::sampleTelemetry() {
  snap_ = worker_.telemetry();
  inst_.push(snap_);
}

bool Gui::init(int width, int height) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SKW_WARN("gui: SDL_Init failed (%s) - staying headless", SDL_GetError());
    return false;
  }
  auto* win = SDL_CreateWindow("Skywave", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, width, height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!win) {
    SKW_WARN("gui: window creation failed (%s)", SDL_GetError());
    SDL_Quit();
    return false;
  }
  auto* ren = SDL_CreateRenderer(
      win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
  if (!ren) {
    SKW_WARN("gui: renderer creation failed (%s)", SDL_GetError());
    SDL_DestroyWindow(win);
    SDL_Quit();
    return false;
  }
  window_ = win;
  renderer_ = ren;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);
  snprintf(callBuf_, sizeof(callBuf_), "%s", worker_.myCall().c_str());
  const std::string fam = worker_.family();
  for (int i = 0; i < 4; ++i) {
    if (fam == kFamilyNames[i]) familyIdx_ = i;
  }
  monitor_ = worker_.monitoring();
  ready_ = true;
  return true;
}

void Gui::shutdown() {
  if (!ready_) return;
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));
  SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
  SDL_Quit();
  ready_ = false;
}

void Gui::drawSessionPanel() {
  ImGui::Begin("Session");
  ImGui::Text("State: %s", worker_.fsmState().c_str());
  ImGui::Text("Call: %s", worker_.myCall().c_str());
  ImGui::Text("Family: %s   Mode: %s", worker_.family().c_str(),
              worker_.modeStr().c_str());
  ImGui::Text("Busy: %s   PTT: %s", snap_.busy ? "yes" : "no",
              snap_.ptt ? "KEYED" : "-");
  ImGui::Separator();
  ImGui::InputText("Remote", connectBuf_, sizeof(connectBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Connect")) {
    submit(std::string("CONNECT ") + connectBuf_);
  }
  if (ImGui::Button("Disconnect")) submit("DISCONNECT");
  ImGui::SameLine();
  if (ImGui::Button("Abort")) submit("ABORT");
  ImGui::SameLine();
  if (ImGui::Button("CQ")) submit("CQ");
  ImGui::SameLine();
  if (ImGui::Button("Beacon")) submit("BEACON");
  ImGui::End();
}

void Gui::drawLadderPanel() {
  ImGui::Begin("Ladder / AMC");
  ImGui::Text("Committed: M%u", unsigned(snap_.mode));
  if (!inst_.modeHistory.empty()) {
    std::vector<float> hist;
    hist.reserve(inst_.modeHistory.size());
    for (uint8_t m : inst_.modeHistory) hist.push_back(float(m));
    ImGui::PlotLines("mode", hist.data(), int(hist.size()), 0, nullptr, 4.0f,
                     13.0f, ImVec2(0, 60));
  }
  if (ImGui::Combo("MODE", &modeIdx_,
                   "AUTO\0M4\0M5\0M6\0M7\0M8\0M9\0M10\0M11\0M12\0M13\0")) {
    submit("MODE " + modeArg(modeIdx_));
  }
  if (ImGui::Combo("CPMODE", &cpIdx_, "AUTO\0SHORT\0MEDIUM\0LONG\0")) {
    submit(std::string("CPMODE ") + kCpNames[cpIdx_]);
  }
  ImGui::Text("%s", worker_.statusLine().c_str());
  ImGui::End();
}

void Gui::drawInstrumentPanel() {
  ImGui::Begin("Instruments");
  ImGui::Text("RX %.1f dBFS   TX %.1f dBFS", double(snap_.rxLevelDbfs),
              double(snap_.txLevelDbfs));
  ImGui::Text("Noise floor %.1f dBFS", double(snap_.noiseFloorDbfs));
  plotSeries("rx", inst_.rxLevel, -120.0f, 0.0f);
  plotSeries("tx", inst_.txLevel, -120.0f, 0.0f);
  plotSeries("floor", inst_.noiseFloor, -120.0f, 0.0f);
  ImGui::Text("%s", worker_.freqInfo().c_str());
  ImGui::End();
}

void Gui::drawSettingsPanel() {
  ImGui::Begin("Settings");
  ImGui::InputText("MYCALL", callBuf_, sizeof(callBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Apply##call")) {
    submit(std::string("MYCALL ") + callBuf_);
  }
  if (ImGui::Combo("FAMILY", &familyIdx_, "WIDE\0MID\0NARROW\0ULTRA\0")) {
    submit(std::string("FAMILY ") + kFamilyNames[familyIdx_]);
  }
  if (ImGui::Checkbox("MONITOR", &monitor_)) {
    submit(std::string("MONITOR ") + (monitor_ ? "ON" : "OFF"));
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Calibration");
  if (ImGui::Button("TXTEST 5 s")) submit("TXTEST 5");
  ImGui::SameLine();
  if (ImGui::Button("Stop")) submit("TXTEST OFF");
  ImGui::Text("%s", worker_.audioLevel().c_str());
  ImGui::End();
}

void Gui::drawLogPanel() {
  ImGui::Begin("Command log");
  ImGui::TextUnformatted("Every GUI action routes through the dispatcher:");
  ImGui::Separator();
  if (ImGui::BeginChild("loglines")) {
    for (const auto& l : log_) {
      ImGui::TextWrapped("> %s", l.cmd.c_str());
      ImGui::TextWrapped("  %s", l.response.c_str());
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

bool Gui::frame() {
  if (!ready_) return false;
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    ImGui_ImplSDL2_ProcessEvent(&ev);
    if (ev.type == SDL_QUIT) return false;
    if (ev.type == SDL_WINDOWEVENT &&
        ev.window.event == SDL_WINDOWEVENT_CLOSE) {
      return false;
    }
  }
  sampleTelemetry();
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  drawSessionPanel();
  drawLadderPanel();
  drawInstrumentPanel();
  drawSettingsPanel();
  drawLogPanel();
  ImGui::Render();
  auto* ren = static_cast<SDL_Renderer*>(renderer_);
  SDL_SetRenderDrawColor(ren, 18, 18, 22, 255);
  SDL_RenderClear(ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
  SDL_RenderPresent(ren);
  return true;
}

}  // namespace skw

#endif  // SKW_HAVE_GUI
