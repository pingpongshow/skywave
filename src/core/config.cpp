#include "core/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/log.hpp"

namespace skw {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool parseBool(const std::string& v, bool& out) {
  std::string s = lower(v);
  if (s == "true" || s == "1" || s == "yes" || s == "on") { out = true; return true; }
  if (s == "false" || s == "0" || s == "no" || s == "off") { out = false; return true; }
  return false;
}

bool parseInt(const std::string& v, int& out) {
  try {
    size_t pos = 0;
    int x = std::stoi(v, &pos);
    if (pos != v.size()) return false;
    out = x;
    return true;
  } catch (...) { return false; }
}

bool parseDouble(const std::string& v, double& out) {
  try {
    size_t pos = 0;
    double x = std::stod(v, &pos);
    if (pos != v.size()) return false;
    out = x;
    return true;
  } catch (...) { return false; }
}

// "auto" -> -1, "m7"/"7" -> 7 (clamped to active ladder for mode fields).
bool parseModeSel(const std::string& v, int& out) {
  std::string s = lower(v);
  if (s == "auto") { out = -1; return true; }
  if (!s.empty() && s[0] == 'm') s = s.substr(1);
  int x;
  if (!parseInt(s, x) || x < 0 || x >= kNumModes) return false;
  out = x;
  return true;
}

bool parseCpSel(const std::string& v, int& out) {
  std::string s = lower(v);
  if (s == "auto") { out = -1; return true; }
  if (s == "short") { out = 0; return true; }
  if (s == "medium") { out = 1; return true; }
  if (s == "long") { out = 2; return true; }
  return false;
}

std::string cpSelToString(int cp) {
  switch (cp) {
    case 0: return "short";
    case 1: return "medium";
    case 2: return "long";
    default: return "auto";
  }
}

std::string modeSelToString(int m) {
  if (m < 0) return "auto";
  char buf[8];
  snprintf(buf, sizeof buf, "m%d", m);
  return buf;
}

}  // namespace

bool callsignValid(const std::string& call) {
  if (call.size() < 3 || call.size() > 12) return false;
  std::string up = call;
  std::transform(up.begin(), up.end(), up.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  if (up == "N0CALL") return false;
  for (char c : up) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/')) return false;
  }
  return true;
}

bool Config::callsignValid() const { return skw::callsignValid(callsign); }

std::string toString(PttMethod m) {
  switch (m) {
    case PttMethod::None: return "none";
    case PttMethod::Rts: return "rts";
    case PttMethod::Dtr: return "dtr";
    case PttMethod::Vox: return "vox";
    case PttMethod::Tcp: return "tcp";
  }
  return "none";
}

std::optional<PttMethod> pttMethodFromString(std::string s) {
  s = lower(std::move(s));
  if (s == "none") return PttMethod::None;
  if (s == "rts") return PttMethod::Rts;
  if (s == "dtr") return PttMethod::Dtr;
  if (s == "vox") return PttMethod::Vox;
  if (s == "tcp") return PttMethod::Tcp;
  return std::nullopt;
}

std::string toString(HarqMode m) {
  switch (m) {
    case HarqMode::Auto: return "auto";
    case HarqMode::Chase: return "chase";
    case HarqMode::Ir: return "ir";
  }
  return "auto";
}

std::optional<HarqMode> harqModeFromString(std::string s) {
  s = lower(std::move(s));
  if (s == "auto") return HarqMode::Auto;
  if (s == "chase") return HarqMode::Chase;
  if (s == "ir") return HarqMode::Ir;
  return std::nullopt;
}

std::string toString(Family f) { return lower(plan(f).name); }

std::optional<Family> familyFromString(std::string s) {
  s = lower(std::move(s));
  for (int i = 0; i < kNumFamilies; ++i) {
    if (s == lower(kFamilies[size_t(i)].name)) return Family(i);
  }
  return std::nullopt;
}

std::string toString(Band b) { return b == Band::Hf ? "hf" : "?"; }

std::optional<Band> bandFromString(std::string s) {
  s = lower(std::move(s));
  if (s == "hf") return Band::Hf;
  return std::nullopt;
}

bool Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;

  std::string section;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = lower(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      SKW_WARN("config: ignoring malformed line: %s", line.c_str());
      continue;
    }
    std::string key = lower(trim(line.substr(0, eq)));
    std::string val = trim(line.substr(eq + 1));
    std::string full = section + "." + key;

    bool known = true;
    bool okv = true;
    if (full == "station.callsign") callsign = val;
    else if (full == "station.cw_id") okv = parseBool(val, cwId);
    else if (full == "station.cw_wpm") okv = parseInt(val, cwWpm);
    else if (full == "audio.input_device") okv = parseInt(val, inputDevice);
    else if (full == "audio.output_device") okv = parseInt(val, outputDevice);
    else if (full == "audio.input_gain_db") okv = parseDouble(val, inputGainDb);
    else if (full == "audio.output_gain_db") okv = parseDouble(val, outputGainDb);
    else if (full == "tcp.data_port") { int p; okv = parseInt(val, p); if (okv) dataPort = uint16_t(p); }
    else if (full == "tcp.ctrl_port") { int p; okv = parseInt(val, p); if (okv) ctrlPort = uint16_t(p); }
    else if (full == "tcp.voice_port") { int p; okv = parseInt(val, p); if (okv) voicePort = uint16_t(p); }
    else if (full == "tcp.bind_addr") bindAddr = val;
    else if (full == "ptt.method") { auto m = pttMethodFromString(val); okv = m.has_value(); if (m) pttMethod = *m; }
    else if (full == "ptt.target") pttTarget = val;
    else if (full == "ptt.baud") okv = parseInt(val, pttBaud);
    else if (full == "ptt.lead_ms") okv = parseInt(val, pttLeadMs);
    else if (full == "ptt.tail_ms") okv = parseInt(val, pttTailMs);
    else if (full == "modem.band") { auto b = bandFromString(val); okv = b.has_value(); if (b) band = *b; }
    else if (full == "modem.family") { auto f = familyFromString(val); okv = f.has_value(); if (f) family = *f; }
    else if (full == "modem.ultra_enable") okv = parseBool(val, ultraEnable);
    else if (full == "modem.family_fallback") okv = parseBool(val, familyFallback);
    else if (full == "modem.css_tier") okv = parseBool(val, cssTier);
    else if (full == "modem.mode") okv = parseModeSel(val, mode);
    else if (full == "modem.max_mode") okv = parseModeSel(val, maxMode);
    else if (full == "modem.cp_mode") okv = parseCpSel(val, cpMode);
    else if (full == "modem.listen") okv = parseBool(val, listen);
    else if (full == "modem.max_retries") okv = parseInt(val, maxRetries);
    else if (full == "modem.harq") { auto h = harqModeFromString(val); okv = h.has_value(); if (h) harq = *h; }
    else if (full == "modem.channel_memory") okv = parseBool(val, channelMemory);
    else if (full == "modem.probe_promote") okv = parseBool(val, probePromote);
    else if (full == "modem.carrier_masking") okv = parseBool(val, carrierMasking);
    else if (full == "modem.resume") okv = parseBool(val, resume);
    else if (full == "modem.pre_tx_cca") okv = parseBool(val, preTxCca);
    else if (full == "modem.impulse_blanker") okv = parseBool(val, impulseBlanker);
    else if (full == "modem.multi_notch") okv = parseBool(val, multiNotch);
    else if (full == "modem.freq_align") okv = parseBool(val, freqAlign);
    else if (full == "modem.invert_correct") okv = parseBool(val, invertCorrect);
    else if (full == "modem.tx_watchdog_s") okv = parseDouble(val, txWatchdogSec);
    else if (full == "crypto.enabled") okv = parseBool(val, cryptoEnabled);
    else if (full == "crypto.psk") cryptoPsk = val;
    else if (full == "voice.codec") voiceCodec = lower(val);
    else if (full == "voice.broadcast_mode") okv = parseModeSel(val, voiceBroadcastMode);
    else if (full == "gui.enabled") okv = parseBool(val, guiEnabled);
    else if (full == "gui.width") okv = parseInt(val, guiWidth);
    else if (full == "gui.height") okv = parseInt(val, guiHeight);
    else if (full == "gui.waterfall") okv = parseBool(val, guiWaterfall);
    else if (full == "gui.constellation") okv = parseBool(val, guiConstellation);
    else if (full == "log.level") okv = parseInt(val, logLevel);
    else if (full == "log.file") logFile = val;
    else known = false;

    if (!known) {
      SKW_WARN("config: unknown key '%s' preserved", full.c_str());
      unknownKeys[full] = val;
    } else if (!okv) {
      SKW_WARN("config: bad value for '%s': '%s' (keeping default)",
               full.c_str(), val.c_str());
    }
  }
  return true;
}

bool Config::save(const std::string& path) const {
  std::ostringstream o;
  auto unknown = [&](const std::string& section) {
    std::string prefix = section + ".";
    for (const auto& [k, v] : unknownKeys) {
      if (k.rfind(prefix, 0) == 0) o << k.substr(prefix.size()) << " = " << v << "\n";
    }
  };

  o << "[station]\n"
    << "callsign = " << callsign << "\n"
    << "cw_id = " << (cwId ? "true" : "false") << "\n"
    << "cw_wpm = " << cwWpm << "\n";
  unknown("station");
  o << "\n[audio]\n"
    << "input_device = " << inputDevice << "\n"
    << "output_device = " << outputDevice << "\n"
    << "input_gain_db = " << inputGainDb << "\n"
    << "output_gain_db = " << outputGainDb << "\n";
  unknown("audio");
  o << "\n[tcp]\n"
    << "data_port = " << dataPort << "\n"
    << "ctrl_port = " << ctrlPort << "\n"
    << "voice_port = " << voicePort << "\n"
    << "bind_addr = " << bindAddr << "\n";
  unknown("tcp");
  o << "\n[ptt]\n"
    << "method = " << toString(pttMethod) << "\n"
    << "target = " << pttTarget << "\n"
    << "baud = " << pttBaud << "\n"
    << "lead_ms = " << pttLeadMs << "\n"
    << "tail_ms = " << pttTailMs << "\n";
  unknown("ptt");
  o << "\n[modem]\n"
    << "band = " << toString(band) << "\n"
    << "family = " << toString(family) << "\n"
    << "ultra_enable = " << (ultraEnable ? "true" : "false") << "\n"
    << "family_fallback = " << (familyFallback ? "true" : "false") << "\n"
    << "css_tier = " << (cssTier ? "true" : "false") << "\n"
    << "mode = " << modeSelToString(mode) << "\n"
    << "max_mode = " << modeSelToString(maxMode) << "\n"
    << "cp_mode = " << cpSelToString(cpMode) << "\n"
    << "listen = " << (listen ? "true" : "false") << "\n"
    << "max_retries = " << maxRetries << "\n"
    << "harq = " << toString(harq) << "\n"
    << "channel_memory = " << (channelMemory ? "true" : "false") << "\n"
    << "probe_promote = " << (probePromote ? "true" : "false") << "\n"
    << "carrier_masking = " << (carrierMasking ? "true" : "false") << "\n"
    << "resume = " << (resume ? "true" : "false") << "\n"
    << "pre_tx_cca = " << (preTxCca ? "true" : "false") << "\n"
    << "impulse_blanker = " << (impulseBlanker ? "true" : "false") << "\n"
    << "multi_notch = " << (multiNotch ? "true" : "false") << "\n"
    << "freq_align = " << (freqAlign ? "true" : "false") << "\n"
    << "invert_correct = " << (invertCorrect ? "true" : "false") << "\n"
    << "tx_watchdog_s = " << txWatchdogSec << "\n";
  unknown("modem");
  o << "\n[crypto]\n"
    << "enabled = " << (cryptoEnabled ? "true" : "false") << "\n"
    << "psk = " << cryptoPsk << "\n";
  unknown("crypto");
  o << "\n[voice]\n"
    << "codec = " << voiceCodec << "\n"
    << "broadcast_mode = " << modeSelToString(voiceBroadcastMode) << "\n";
  unknown("voice");
  o << "\n[gui]\n"
    << "enabled = " << (guiEnabled ? "true" : "false") << "\n"
    << "width = " << guiWidth << "\n"
    << "height = " << guiHeight << "\n"
    << "waterfall = " << (guiWaterfall ? "true" : "false") << "\n"
    << "constellation = " << (guiConstellation ? "true" : "false") << "\n";
  unknown("gui");
  o << "\n[log]\n"
    << "level = " << logLevel << "\n"
    << "file = " << logFile << "\n";
  unknown("log");

  // Unknown keys living in sections we do not own: group and append them so
  // nothing a future version (or another tool) wrote is ever dropped.
  static const char* kKnownSections[] = {"station", "audio", "tcp", "ptt",
                                         "modem", "crypto", "voice", "gui",
                                         "log"};
  std::string lastForeignSection;
  for (const auto& [k, v] : unknownKeys) {
    size_t dot = k.find('.');
    if (dot == std::string::npos) continue;
    std::string section = k.substr(0, dot);
    bool foreign = true;
    for (const char* s : kKnownSections) {
      if (section == s) { foreign = false; break; }
    }
    if (!foreign) continue;
    if (section != lastForeignSection) {
      o << "\n[" << section << "]\n";
      lastForeignSection = section;
    }
    o << k.substr(dot + 1) << " = " << v << "\n";
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << o.str();
  return bool(out);
}

}  // namespace skw
