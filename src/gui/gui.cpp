#ifdef SKW_HAVE_GUI

#include "gui/gui.hpp"

#include <cctype>
#include <SDL.h>

#include <algorithm>
#include <cstring>
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

// The command log is on screen (and in screenshots): a secret argument
// is never put in it. ENCRYPTKEY's argument is the PSK itself.
std::string Gui::redactForLog(const std::string& cmd) {
  const size_t a = cmd.find_first_not_of(" \t");
  if (a == std::string::npos) return cmd;
  const size_t b = cmd.find_first_of(" \t", a);
  std::string verb = cmd.substr(a, b == std::string::npos ? b : b - a);
  for (char& ch : verb) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  if (verb != "ENCRYPTKEY" || b == std::string::npos) return cmd;
  return cmd.substr(0, b) + " ********";
}

Gui::Gui(ModemWorker& worker) : worker_(worker) {}
Gui::~Gui() { shutdown(); }

std::string Gui::submit(const std::string& cmd) {
  // THE rule: no privileged path. Everything the GUI changes goes through
  // the same dispatcher the TCP control port uses.
  const std::string resp = worker_.control(cmd);
  log_.push_back({redactForLog(cmd), resp});
  if (log_.size() > 200) log_.erase(log_.begin());
  return resp;
}

void Gui::sampleTelemetry() {
  snap_ = worker_.telemetry();
  inst_.push(snap_);
  // Status STRINGS take the modem mutex; polling them every vsync
  // frame (8 acquisitions/frame at 60 fps) contended hard against the
  // worker under decode load. 4 Hz is indistinguishable to a human and
  // two orders of magnitude less lock traffic.
  const double nowSec =
      double(SDL_GetTicks64()) / 1000.0;
  if (nowSec - lastStrPollSec_ >= 0.25) {
    lastStrPollSec_ = nowSec;
    strFsmState_ = worker_.fsmState();
    strMyCall_ = worker_.myCall();
    strFamily_ = worker_.family();
    strMode_ = worker_.modeStr();
    strStatus_ = worker_.statusLine();
    strFreqInfo_ = worker_.freqInfo();
    strAudioLevel_ = worker_.audioLevel();
    netBps_ = worker_.netBps();
  }
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
  if (auto u = worker_.feature("ULTRA")) ultraOn_ = *u;
  listenOn_ = worker_.listening();
  cwidOn_ = worker_.cwId();
  maxModeIdx_ = std::clamp(worker_.maxMode() - 4, 0, 9);
  snprintf(pttMethodBuf_, sizeof(pttMethodBuf_), "%s",
           worker_.pttGet("METHOD").c_str());
  snprintf(pttPortBuf_, sizeof(pttPortBuf_), "%s",
           worker_.pttGet("PORT").c_str());
  pttLead_ = atoi(worker_.pttGet("LEAD").c_str());
  pttTail_ = atoi(worker_.pttGet("TAIL").c_str());
  audioDevs_ = listAudioDevices();
  ready_ = true;
  return true;
}

void Gui::setPortInfo(int data, int ctrl, int voice) {
  portData_ = data;
  portCtrl_ = ctrl;
  portVoice_ = voice;
}

void Gui::setAudioSelection(int inDev, int outDev) {
  audioInSel_ = inDev;
  audioOutSel_ = outDev;
}

std::optional<std::pair<int, int>> Gui::takeAudioRequest() {
  auto r = audioReq_;
  audioReq_.reset();
  return r;
}

void Gui::noteAudio(const std::string& result) {
  log_.push_back({"(audio device change)", result});
  if (log_.size() > 200) log_.erase(log_.begin());
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
  ImGui::Text("State: %s", strFsmState_.c_str());
  ImGui::Text("Call: %s", strMyCall_.c_str());
  ImGui::Text("Family: %s   Mode: %s", strFamily_.c_str(),
              strMode_.c_str());
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
  ImGui::Text("Committed: M%u   Net rate: %.0f bps", unsigned(snap_.mode),
              netBps_);
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
  ImGui::Text("%s", strStatus_.c_str());
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
  ImGui::Text("%s", strFreqInfo_.c_str());
  ImGui::End();
}

// Waterfall: RX spectrum history. Each telemetry spectrum becomes one
// row; rows scroll upward. Drawn straight into the ImDrawList as filled
// rects - no texture upload, no GPU state, and it stays correct when the
// window is resized mid-frame.
void Gui::drawWaterfallPanel() {
  ImGui::Begin("Waterfall");

  if (waterfall_.size() != size_t(kWaterfallRows) * kSpectrumBins) {
    waterfall_.assign(size_t(kWaterfallRows) * kSpectrumBins, -120.0f);
  }
  // Only consume a spectrum the worker actually refreshed - the GUI
  // renders far faster than the ~10 Hz spectrum cadence and duplicating
  // rows would make the scroll rate lie about time.
  if (snap_.spectrumSeq != lastSpectrumSeq_) {
    lastSpectrumSeq_ = snap_.spectrumSeq;
    float* row = waterfall_.data() + size_t(waterfallHead_) * kSpectrumBins;
    for (int i = 0; i < kSpectrumBins; ++i) row[i] = snap_.spectrumDbfs[i];
    waterfallHead_ = (waterfallHead_ + 1) % kWaterfallRows;

    if (wfAutoScale_) {
      // Track the visible dynamic range so a quiet band still shows
      // structure and a loud one does not saturate to white.
      float lo = 0.0f, hi = -200.0f;
      for (int i = 0; i < kSpectrumBins; ++i) {
        lo = std::min(lo, snap_.spectrumDbfs[i]);
        hi = std::max(hi, snap_.spectrumDbfs[i]);
      }
      if (hi > lo + 6.0f) {
        wfFloorDb_ += 0.10f * (lo - wfFloorDb_);
        wfCeilDb_ += 0.10f * (hi - wfCeilDb_);
      }
    }
  }

  ImGui::Checkbox("Auto scale", &wfAutoScale_);
  ImGui::SameLine();
  ImGui::Text("%.0f .. %.0f dBFS", double(wfFloorDb_), double(wfCeilDb_));

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float w = std::max(avail.x, 64.0f);
  const float h = std::max(avail.y, 64.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cw = w / float(kSpectrumBins);
  const float ch = h / float(kWaterfallRows);
  const float span = std::max(wfCeilDb_ - wfFloorDb_, 1.0f);

  for (int r = 0; r < kWaterfallRows; ++r) {
    // Newest row at the TOP: walk backwards from the write head.
    const int src = (waterfallHead_ - 1 - r + 2 * kWaterfallRows) %
                    kWaterfallRows;
    const float* row = waterfall_.data() + size_t(src) * kSpectrumBins;
    const float y = origin.y + float(r) * ch;
    for (int c = 0; c < kSpectrumBins; ++c) {
      const float norm =
          std::clamp((row[c] - wfFloorDb_) / span, 0.0f, 1.0f);
      // Blue -> cyan -> yellow -> red, the conventional waterfall ramp.
      float rr, gg, bb;
      if (norm < 0.33f) {
        const float u = norm / 0.33f;
        rr = 0.0f; gg = u * 0.7f; bb = 0.35f + 0.65f * u;
      } else if (norm < 0.66f) {
        const float u = (norm - 0.33f) / 0.33f;
        rr = u; gg = 0.7f + 0.3f * u; bb = 1.0f - u;
      } else {
        const float u = (norm - 0.66f) / 0.34f;
        rr = 1.0f; gg = 1.0f - u; bb = 0.0f;
      }
      dl->AddRectFilled(ImVec2(origin.x + float(c) * cw, y),
                        ImVec2(origin.x + float(c + 1) * cw, y + ch),
                        ImGui::ColorConvertFloat4ToU32(
                            ImVec4(rr, gg, bb, 1.0f)));
    }
  }

  // Mask overlay: excised carriers marked along the top edge. The mask
  // is indexed by CARRIER, the display by FFT bin, so map through the
  // family plan rather than assuming they line up.
  if (snap_.maskCarriers > 0) {
    const int nCar = snap_.maskCarriers;
    for (int c = 0; c < nCar; ++c) {
      const bool usable =
          (snap_.maskBits[size_t(c >> 3)] >> (c & 7)) & 1;
      if (usable) continue;
      const float frac = float(c) / float(nCar);
      const float x = origin.x + frac * w;
      dl->AddRectFilled(ImVec2(x, origin.y),
                        ImVec2(x + std::max(cw, 2.0f), origin.y + 4.0f),
                        IM_COL32(255, 0, 255, 220));
    }
  }
  ImGui::Dummy(ImVec2(w, h));
  ImGui::End();
}

// Constellation: equalized payload symbols from the last decoded frame.
// A tight cluster pattern means the equalizer is tracking; smearing is
// the visual signature of the fading the tracker cannot follow.
void Gui::drawConstellationPanel() {
  ImGui::Begin("Constellation");
  ImGui::Text("M%u   %u points", unsigned(snap_.mode),
              unsigned(snap_.constellationN));

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float side = std::max(std::min(avail.x, avail.y), 64.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(origin.x + side * 0.5f, origin.y + side * 0.5f);
  const float scale = side * 0.35f;  // unit amplitude at 35% of the box

  dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                    IM_COL32(12, 12, 18, 255));
  dl->AddLine(ImVec2(origin.x, c.y), ImVec2(origin.x + side, c.y),
              IM_COL32(70, 70, 80, 255));
  dl->AddLine(ImVec2(c.x, origin.y), ImVec2(c.x, origin.y + side),
              IM_COL32(70, 70, 80, 255));

  for (int i = 0; i < int(snap_.constellationN); ++i) {
    const float x = c.x + snap_.constellationI[i] * scale;
    const float y = c.y - snap_.constellationQ[i] * scale;
    // Fade the older points so the eye reads recency.
    const float age = float(i) / float(std::max<int>(snap_.constellationN, 1));
    dl->AddCircleFilled(ImVec2(x, y), 2.0f,
                        ImGui::ColorConvertFloat4ToU32(
                            ImVec4(0.4f + 0.6f * age, 1.0f, 0.5f, 0.85f)));
  }
  ImGui::Dummy(ImVec2(side, side));
  ImGui::End();
}

// Security page (SECURITY.md). Everything routes through submit(), i.e.
// the same command dispatcher the TCP control port uses, so the GUI has
// no privileged path to the crypto state - and a non-crypto build simply
// reports what the modem answers (ERROR:UNSUPPORTED).
void Gui::drawCryptoSection() {
  ImGui::Separator();
  ImGui::TextUnformatted("Security");

  if (securityLine_.empty()) securityLine_ = submit("SECURITY");
  ImGui::TextUnformatted(securityLine_.c_str());
  ImGui::SameLine();
  if (ImGui::Button("Refresh##sec")) securityLine_ = submit("SECURITY");

  const bool unsupported =
      securityLine_.rfind("ERROR:UNSUPPORTED", 0) == 0;
  if (unsupported) {
    ImGui::TextDisabled(
        "This build has no encryption support (ENABLE_CRYPTO=OFF).");
    return;
  }

  // The PSK is typed as hex and masked; it is installed and then wiped
  // from the input so it does not linger in the UI.
  ImGui::InputText("PSK (hex)", pskBuf_, sizeof(pskBuf_),
                   ImGuiInputTextFlags_Password |
                       ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Install##psk")) {
    securityLine_ = submit(std::string("ENCRYPTKEY ") + pskBuf_);
    std::memset(pskBuf_, 0, sizeof(pskBuf_));
    if (!securityLine_.empty() && securityLine_.rfind("ERROR:", 0) != 0) {
      securityLine_ = submit("SECURITY");
    }
  }

  if (ImGui::Checkbox("Request encryption", &encryptOn_)) {
    securityLine_ = submit(std::string("ENCRYPTION ") +
                           (encryptOn_ ? "ON" : "OFF"));
    // The modem refuses ON without a PSK and AES-NI; reflect its verdict
    // rather than the click.
    if (securityLine_.rfind("ERROR:", 0) == 0) encryptOn_ = !encryptOn_;
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate keys")) {
    securityLine_ = submit("KEYROTATE");
  }
  ImGui::TextDisabled(
      "Key and encryption changes are accepted while IDLE only; a session "
      "with a PSK mismatch is refused, never downgraded to cleartext.");
}

void Gui::drawSettingsPanel() {
  // Closeable: lives behind Modem > Settings...
  if (!ImGui::Begin("Settings", &showSettings_)) {
    ImGui::End();
    return;
  }
  ImGui::InputText("MYCALL", callBuf_, sizeof(callBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Apply##call")) {
    submit(std::string("MYCALL ") + callBuf_);
  }
  if (ImGui::Combo("FAMILY", &familyIdx_, "WIDE\0MID\0NARROW\0ULTRA\0")) {
    submit(std::string("FAMILY ") + kFamilyNames[familyIdx_]);
  }
  if (ImGui::Checkbox("ULTRA enable", &ultraOn_)) {
    submit(std::string("ULTRA ") + (ultraOn_ ? "ON" : "OFF"));
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("LISTEN", &listenOn_)) {
    submit(std::string("LISTEN ") + (listenOn_ ? "ON" : "OFF"));
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("MONITOR", &monitor_)) {
    submit(std::string("MONITOR ") + (monitor_ ? "ON" : "OFF"));
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("CWID", &cwidOn_)) {
    submit(std::string("CWID ") + (cwidOn_ ? "ON" : "OFF"));
  }
  if (ImGui::Combo("MAXMODE", &maxModeIdx_,
                   "M4\0M5\0M6\0M7\0M8\0M9\0M10\0M11\0M12\0M13\0")) {
    submit("MAXMODE M" + std::to_string(maxModeIdx_ + 4));
  }
  ImGui::Separator();
  ImGui::TextUnformatted("PTT");
  ImGui::InputText("Method", pttMethodBuf_, sizeof(pttMethodBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Set##pttm")) {
    submit(std::string("PTTMETHOD ") + pttMethodBuf_);
  }
  ImGui::InputText("Port", pttPortBuf_, sizeof(pttPortBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Set##pttp")) {
    submit(std::string("PTTPORT ") + pttPortBuf_);
  }
  ImGui::SetNextItemWidth(120);
  if (ImGui::InputInt("Lead ms", &pttLead_)) {
    pttLead_ = std::clamp(pttLead_, 0, 2000);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  if (ImGui::InputInt("Tail ms", &pttTail_)) {
    pttTail_ = std::clamp(pttTail_, 0, 2000);
  }
  ImGui::SameLine();
  if (ImGui::Button("Set##pttlt")) {
    submit("PTTLEAD " + std::to_string(pttLead_));
    submit("PTTTAIL " + std::to_string(pttTail_));
  }
  drawCryptoSection();
  ImGui::Text("TCP ports: data %d  ctrl %d  voice %d  (restart to change)",
              portData_, portCtrl_, portVoice_);
  ImGui::Separator();
  ImGui::TextUnformatted("Audio devices");
  // Combos show "index: name"; selection is by DEVICE INDEX so a
  // rescan that reorders the list cannot silently retarget.
  auto deviceCombo = [&](const char* label, bool wantInput, int& sel) {
    const AudioDeviceInfo* cur = nullptr;
    for (const auto& d : audioDevs_) {
      if (d.index == sel) cur = &d;
    }
    char preview[96];
    if (cur) {
      snprintf(preview, sizeof(preview), "%d: %s", cur->index,
               cur->name.c_str());
    } else {
      snprintf(preview, sizeof(preview), "%s",
               sel < 0 ? "(system default)" : "(missing device)");
    }
    if (ImGui::BeginCombo(label, preview)) {
      for (const auto& d : audioDevs_) {
        if (wantInput ? d.maxInputChannels <= 0 : d.maxOutputChannels <= 0) {
          continue;
        }
        char item[96];
        snprintf(item, sizeof(item), "%d: %s%s", d.index, d.name.c_str(),
                 (wantInput ? d.defaultInput : d.defaultOutput) ? " *" : "");
        if (ImGui::Selectable(item, d.index == sel)) sel = d.index;
      }
      ImGui::EndCombo();
    }
  };
  deviceCombo("Input", true, audioInSel_);
  deviceCombo("Output", false, audioOutSel_);
  if (ImGui::Button("Apply audio")) {
    audioReq_ = std::make_pair(audioInSel_, audioOutSel_);
    log_.push_back({"(audio device change)",
                    "requested in=" + std::to_string(audioInSel_) +
                        " out=" + std::to_string(audioOutSel_)});
  }
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) audioDevs_ = listAudioDevices();
  ImGui::Separator();
  ImGui::TextUnformatted("Calibration");
  if (ImGui::Button("TXTEST 5 s")) submit("TXTEST 5");
  ImGui::SameLine();
  if (ImGui::Button("Stop")) submit("TXTEST OFF");
  ImGui::Text("%s", strAudioLevel_.c_str());
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
  // Menu bar: Settings lives in its own menu-invoked window; View
  // toggles the operating panels.
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Modem")) {
      if (ImGui::MenuItem("Settings...", nullptr, showSettings_)) {
        showSettings_ = !showSettings_;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Session", nullptr, &showSession_);
      ImGui::MenuItem("Ladder / AMC", nullptr, &showLadder_);
      ImGui::MenuItem("Instruments", nullptr, &showInstruments_);
      ImGui::MenuItem("Waterfall", nullptr, &showWaterfall_);
      ImGui::MenuItem("Constellation", nullptr, &showConstellation_);
      ImGui::MenuItem("Command log", nullptr, &showLog_);
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  // First-run layout: without explicit positions every panel spawns
  // stacked at the top-left. FirstUseEver keeps user rearrangements
  // (imgui.ini) authoritative afterwards.
  if (showSession_) {
    ImGui::SetNextWindowPos(ImVec2(10, 34), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 230), ImGuiCond_FirstUseEver);
    drawSessionPanel();
  }
  if (showLadder_) {
    ImGui::SetNextWindowPos(ImVec2(10, 274), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 270), ImGuiCond_FirstUseEver);
    drawLadderPanel();
  }
  if (showInstruments_) {
    ImGui::SetNextWindowPos(ImVec2(420, 34), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
    drawInstrumentPanel();
  }
  if (showWaterfall_) {
    ImGui::SetNextWindowPos(ImVec2(420, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
    drawWaterfallPanel();
  }
  if (showConstellation_) {
    ImGui::SetNextWindowPos(ImVec2(950, 34), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 330), ImGuiCond_FirstUseEver);
    drawConstellationPanel();
  }
  if (showSettings_) {
    ImGui::SetNextWindowPos(ImVec2(180, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    drawSettingsPanel();
  }
  if (showLog_) {
    ImGui::SetNextWindowPos(ImVec2(10, 554), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(930, 220), ImGuiCond_FirstUseEver);
    drawLogPanel();
  }
  ImGui::Render();
  auto* ren = static_cast<SDL_Renderer*>(renderer_);
  // Retina/HiDPI: the drawable is scaled relative to window points;
  // without matching the renderer scale, the whole UI draws at half
  // size in the top-left quadrant and mouse hits land offset (the
  // official imgui SDL_Renderer example does exactly this).
  const ImGuiIO& io = ImGui::GetIO();
  SDL_RenderSetScale(ren, io.DisplayFramebufferScale.x,
                     io.DisplayFramebufferScale.y);
  SDL_SetRenderDrawColor(ren, 18, 18, 22, 255);
  SDL_RenderClear(ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
  SDL_RenderPresent(ren);
  return true;
}

}  // namespace skw

#endif  // SKW_HAVE_GUI
