#include "core/config.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/log.hpp"
#include "core/strutil.hpp"
#include "link/frame.hpp"

namespace skw {
namespace {

std::string lower(std::string s) { return toLower(std::move(s)); }

// Keys earlier builds wrote that no longer do anything. Dropped on load
// with a WARN (not preserved as unknown keys), so the next save removes
// them from the file.
bool isRetiredKey(const std::string& full) {
  static const char* kRetired[] = {
      "tcp.bind_addr",       "modem.impulse_blanker", "modem.multi_notch",
      "modem.freq_align",    "modem.invert_correct",  "modem.turn_taking",
      "gui.waterfall",       "gui.constellation",     "modem.probe_promote",
      "modem.pre_tx_cca",
  };
  for (const char* k : kRetired) {
    if (full == k) return true;
  }
  return false;
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

bool parseInt(const std::string& v, int& out);

}  // namespace

void parseAudioDevice(const std::string& v, int& index, std::string& name) {
  int x = 0;
  if (parseInt(v, x)) {
    index = x;
    name.clear();
  } else {
    index = -1;
    name = v;
  }
}

namespace {

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

// "auto" -> -1, "m7"/"7" -> 7 (any ladder index; callers range-check).
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
  if (lower(v) == "auto") { out = -1; return true; }
  auto cp = cpModeFromString(v);
  if (!cp) return false;
  out = int(*cp);
  return true;
}

std::string cpSelToString(int cp) {
  return cp >= 0 && cp <= 2 ? lower(toString(CpMode(cp))) : "auto";
}

std::string modeSelToString(int m) {
  if (m < 0) return "auto";
  char buf[8];
  snprintf(buf, sizeof buf, "m%d", m);
  return buf;
}

}  // namespace

bool isPlaceholderCall(const std::string& call) {
  return call.empty() || toUpper(call) == kPlaceholderCall;
}

int bandMaxMode(Band b) { return b == Band::VhfFm ? 14 : 13; }

const char* toString(CpMode cp) { return cpModeName(cp); }

std::optional<CpMode> cpModeFromString(std::string s) {
  s = toUpper(std::move(s));
  for (CpMode cp : {CpMode::Short, CpMode::Medium, CpMode::Long}) {
    if (s == toString(cp)) return cp;
  }
  return std::nullopt;
}

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
  return m == HarqMode::Chase ? "chase" : "ir";
}

std::optional<HarqMode> harqModeFromString(std::string s) {
  s = lower(std::move(s));
  if (s == "chase") return HarqMode::Chase;
  if (s == "ir") return HarqMode::Ir;
  return std::nullopt;
}

// The family's identity name (the HF table's): on VHF-FM every family
// index resolves to one "VHF" plan, whose name would not parse back.
std::string toString(Family f) { return lower(kFamilies[size_t(f)].name); }

std::optional<Family> familyFromString(std::string s) {
  s = lower(std::move(s));
  for (int i = 0; i < kNumFamilies; ++i) {
    if (s == lower(kFamilies[size_t(i)].name)) return Family(i);
  }
  return std::nullopt;
}

std::string toString(Band b) {
  return b == Band::Hf ? "hf" : "vhf-fm";
}

std::optional<Band> bandFromString(std::string s) {
  s = lower(std::move(s));
  if (s == "hf") return Band::Hf;
  if (s == "vhf-fm" || s == "vhffm") return Band::VhfFm;
  return std::nullopt;
}

// The one INI tokeniser: load() and the save-time merge both read files
// through it, so "section.key" names and value trimming always agree.
bool parseIniFile(const std::string& path, IniEntries& out,
                  bool* malformed) {
  out.clear();
  if (malformed) *malformed = false;
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
      if (malformed) *malformed = true;
      continue;
    }
    std::string key = lower(trim(line.substr(0, eq)));
    std::string val = trim(line.substr(eq + 1));
    out.emplace_back(section + "." + key, std::move(val));
  }
  return true;
}

bool Config::load(const std::string& path) {
  IniEntries entries;
  if (!parseIniFile(path, entries)) return false;
  applyEntries(entries);
  return true;
}

void Config::applyEntries(const IniEntries& entries) {
  for (const auto& [full, val] : entries) {
    if (full == "modem.band") applyEntry(full, val);
  }
  for (const auto& [full, val] : entries) applyEntry(full, val);
}

void Config::applyEntry(const std::string& full, const std::string& val) {
  {
    bool known = true;
    bool okv = true;
    if (full == "station.callsign") {
      // The wire's own rule (validateCallsign), stored canonical. An
      // invalid callsign leaves the placeholder, which refuses to transmit.
      if (auto v = validateCallsign(val)) callsign = *v;
      else { callsign = kPlaceholderCall; okv = val.empty(); }
    }
    else if (full == "station.cw_id") okv = parseBool(val, cwId);
    else if (full == "station.cw_wpm") okv = parseInt(val, cwWpm);
    else if (full == "audio.input_device") parseAudioDevice(val, inputDevice, inputDeviceName);
    else if (full == "audio.output_device") parseAudioDevice(val, outputDevice, outputDeviceName);
    else if (full == "audio.input_gain_db") okv = parseDouble(val, inputGainDb);
    else if (full == "audio.output_gain_db") okv = parseDouble(val, outputGainDb);
    else if (full == "tcp.data_port") { int p; okv = parseInt(val, p); if (okv) dataPort = uint16_t(p); }
    else if (full == "tcp.ctrl_port") { int p; okv = parseInt(val, p); if (okv) ctrlPort = uint16_t(p); }
    else if (full == "tcp.voice_port") { int p; okv = parseInt(val, p); if (okv) voicePort = uint16_t(p); }
    else if (full == "ptt.method") { auto m = pttMethodFromString(val); okv = m.has_value(); if (m) pttMethod = *m; }
    else if (full == "ptt.target") pttTarget = val;
    else if (full == "ptt.baud") okv = parseInt(val, pttBaud);
    // Lead/tail size silence buffers: a negative value became a huge
    // size_t (bad_alloc), so both are clamped to 0..5000 ms.
    else if (full == "ptt.lead_ms") { okv = parseInt(val, pttLeadMs); pttLeadMs = std::clamp(pttLeadMs, 0, kPttPadMaxMs); }
    else if (full == "ptt.tail_ms") { okv = parseInt(val, pttTailMs); pttTailMs = std::clamp(pttTailMs, 0, kPttPadMaxMs); }
    else if (full == "modem.band") { auto b = bandFromString(val); okv = b.has_value(); if (b) band = *b; }
    else if (full == "modem.family") { auto f = familyFromString(val); okv = f.has_value(); if (f) family = *f; }
    else if (full == "modem.ultra_enable") okv = parseBool(val, ultraEnable);
    else if (full == "modem.family_fallback") okv = parseBool(val, familyFallback);
    else if (full == "modem.css_tier") okv = parseBool(val, cssTier);
    else if (full == "modem.mode") {
      // A pin is one of the band's OFDM modes (what MODE accepts); the
      // CSS rungs M0-M3, and M14 off VHF-FM, are refused.
      int m = 0;
      okv = parseModeSel(val, m) &&
            (m < 0 || (m >= kFirstMode && m <= bandMaxMode(band)));
      if (okv) mode = m;
    }
    else if (full == "modem.max_mode") {
      // A ceiling, not a selector: "auto" (-1) once flowed on as uint8_t
      // 255 - past the AMC floor tables and into START, where the peer
      // rejected it. Auto = the band's highest mode; numbers outside the
      // band's OFDM range clamp into it (with a WARN).
      int m = 0;
      okv = parseModeSel(val, m);
      if (okv) {
        const int top = bandMaxMode(band);
        maxMode = m < 0 ? top : std::clamp(m, kFirstMode, top);
        if (m >= 0 && m != maxMode) {
          SKW_WARN("config: modem.max_mode '%s' outside m%d..m%d; using m%d",
                   val.c_str(), kFirstMode, top, maxMode);
        }
      }
    }
    else if (full == "modem.cp_mode") okv = parseCpSel(val, cpMode);
    else if (full == "modem.listen") okv = parseBool(val, listen);
    else if (full == "modem.max_retries") okv = parseInt(val, maxRetries);
    else if (full == "modem.harq") {
      auto h = harqModeFromString(val);
      okv = h.has_value();
      if (h) {
        harq = *h;
      } else if (lower(val) == "auto") {
        // Earlier builds' "auto" behaved exactly as "ir".
        harq = HarqMode::Ir;
        okv = true;
        SKW_WARN("config: modem.harq = auto is now 'ir' (same behaviour)");
      }
    }
    else if (full == "modem.channel_memory") okv = parseBool(val, channelMemory);
    else if (full == "modem.carrier_masking") okv = parseBool(val, carrierMasking);
    else if (full == "modem.resume") okv = parseBool(val, resume);
    else if (full == "modem.tx_watchdog_s") okv = parseDouble(val, txWatchdogSec);
    else if (full == "crypto.enabled") okv = parseBool(val, cryptoEnabled);
    else if (full == "crypto.psk") {
      // Exactly 32 bytes of hex (what ENCRYPTKEY accepts) or empty. Other
      // text used to be decoded leniently into a garbage key.
      okv = val.empty() || (val.size() == 64 && hexDecode(val).has_value());
      cryptoPsk = okv ? val : std::string();
    }
    else if (full == "voice.codec") voiceCodec = lower(val);
    else if (full == "voice.broadcast_mode") okv = parseModeSel(val, voiceBroadcastMode);
    else if (full == "gui.enabled") okv = parseBool(val, guiEnabled);
    else if (full == "gui.width") okv = parseInt(val, guiWidth);
    else if (full == "gui.height") okv = parseInt(val, guiHeight);
    else if (full == "log.level") okv = parseInt(val, logLevel);
    else if (full == "log.file") logFile = val;
    else known = false;

    if (!known && isRetiredKey(full)) {
      SKW_WARN("config: '%s' is no longer used; dropped", full.c_str());
    } else if (!known) {
      SKW_WARN("config: unknown key '%s' preserved", full.c_str());
      unknownKeys[full] = val;
    } else if (!okv && full == "crypto.psk") {
      // Never echo key material into the log.
      SKW_WARN("config: crypto.psk is not 64 hex digits; no key loaded");
    } else if (!okv && full == "station.callsign") {
      SKW_WARN("config: invalid callsign '%s' (3-12 of A-Z 0-9 /); "
               "transmitting is blocked until one is set",
               val.c_str());
    } else if (!okv) {
      SKW_WARN("config: bad value for '%s': '%s' (keeping default)",
               full.c_str(), val.c_str());
    }
  }
}

IniEntries Config::toEntries() const {
  IniEntries e;
  auto put = [&](const char* full, std::string v) {
    e.emplace_back(full, std::move(v));
  };
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  auto num = [](auto v) {
    std::ostringstream o;
    o << v;
    return o.str();
  };
  auto unknown = [&](const std::string& section) {
    std::string prefix = section + ".";
    for (const auto& [k, v] : unknownKeys) {
      if (k.rfind(prefix, 0) == 0) e.emplace_back(k, v);
    }
  };

  put("station.callsign", callsign);
  put("station.cw_id", b(cwId));
  put("station.cw_wpm", num(cwWpm));
  unknown("station");
  put("audio.input_device",
      inputDeviceName.empty() ? std::to_string(inputDevice) : inputDeviceName);
  put("audio.output_device", outputDeviceName.empty()
                                 ? std::to_string(outputDevice)
                                 : outputDeviceName);
  put("audio.input_gain_db", num(inputGainDb));
  put("audio.output_gain_db", num(outputGainDb));
  unknown("audio");
  put("tcp.data_port", num(dataPort));
  put("tcp.ctrl_port", num(ctrlPort));
  put("tcp.voice_port", num(voicePort));
  unknown("tcp");
  put("ptt.method", toString(pttMethod));
  put("ptt.target", pttTarget);
  put("ptt.baud", num(pttBaud));
  put("ptt.lead_ms", num(pttLeadMs));
  put("ptt.tail_ms", num(pttTailMs));
  unknown("ptt");
  put("modem.band", toString(band));
  put("modem.family", toString(family));
  put("modem.ultra_enable", b(ultraEnable));
  put("modem.family_fallback", b(familyFallback));
  put("modem.css_tier", b(cssTier));
  put("modem.mode", modeSelToString(mode));
  put("modem.max_mode", modeSelToString(maxMode));
  put("modem.cp_mode", cpSelToString(cpMode));
  put("modem.listen", b(listen));
  put("modem.max_retries", num(maxRetries));
  put("modem.harq", toString(harq));
  put("modem.channel_memory", b(channelMemory));
  put("modem.carrier_masking", b(carrierMasking));
  put("modem.resume", b(resume));
  put("modem.tx_watchdog_s", num(txWatchdogSec));
  unknown("modem");
  put("crypto.enabled", b(cryptoEnabled));
  put("crypto.psk", cryptoPsk);
  unknown("crypto");
  put("voice.codec", voiceCodec);
  put("voice.broadcast_mode", modeSelToString(voiceBroadcastMode));
  unknown("voice");
  put("gui.enabled", b(guiEnabled));
  put("gui.width", num(guiWidth));
  put("gui.height", num(guiHeight));
  unknown("gui");
  put("log.level", num(logLevel));
  put("log.file", logFile);
  unknown("log");

  // Unknown keys living in sections we do not own: appended (renderIni
  // groups them per section) so nothing a future version or another tool
  // wrote is ever dropped.
  static const char* kKnownSections[] = {"station", "audio", "tcp", "ptt",
                                         "modem", "crypto", "voice", "gui",
                                         "log"};
  for (const auto& [k, v] : unknownKeys) {
    size_t dot = k.find('.');
    if (dot == std::string::npos) continue;
    std::string section = k.substr(0, dot);
    bool foreign = true;
    for (const char* s : kKnownSections) {
      if (section == s) { foreign = false; break; }
    }
    if (foreign) e.emplace_back(k, v);
  }
  return e;
}

namespace {

// Entries -> INI text, one [section] block per section in order of first
// appearance (keys of a section stay together even if appended late).
std::string renderIni(const IniEntries& entries) {
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> by;
  for (const auto& [k, v] : entries) {
    size_t dot = k.find('.');
    if (dot == std::string::npos) continue;
    std::string section = k.substr(0, dot);
    auto it = by.find(section);
    if (it == by.end()) {
      order.push_back(section);
      it = by.emplace(section, decltype(by)::mapped_type{}).first;
    }
    it->second.emplace_back(k.substr(dot + 1), v);
  }
  std::ostringstream o;
  bool first = true;
  for (const auto& section : order) {
    if (!first) o << "\n";
    first = false;
    o << "[" << section << "]\n";
    for (const auto& [k, v] : by[section]) o << k << " = " << v << "\n";
  }
  return o.str();
}

// Atomic and private: write a sibling temp file created 0600 (the file
// holds the PSK), flush it to disk, then rename over the target. A crash
// mid-write can no longer leave a truncated config, and the PSK is never
// world-readable, not even for an instant.
bool writePrivateAtomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  ::fchmod(fd, 0600);  // O_CREAT mode is masked by umask; be explicit
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += size_t(n);
  }
  const bool wrote = off == data.size() && ::fsync(fd) == 0;
  ::close(fd);
  if (!wrote || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::map<std::string, std::string> toMap(const IniEntries& e) {
  std::map<std::string, std::string> m;
  for (const auto& [k, v] : e) m[k] = v;  // last occurrence wins, as load()
  return m;
}

}  // namespace

bool Config::save(const std::string& path) const {
  return writePrivateAtomic(path, renderIni(toEntries()));
}

IniEntries mergeIniEntries(const Config& base, const Config& live,
                           const IniEntries& disk,
                           std::vector<std::string>* conflicts) {
  const auto B = toMap(base.toEntries());
  const auto D = toMap(disk);
  // Disk values normalised through the same parser (so "yes" == "true"),
  // used only to decide whether the operator really changed a known key.
  Config diskCfg;
  diskCfg.applyEntries(disk);
  const auto Dn = toMap(diskCfg.toEntries());
  const auto schema = toMap(Config{}.toEntries());  // known keys only

  IniEntries out;
  const IniEntries L = live.toEntries();
  std::map<std::string, bool> inLive;
  for (const auto& [k, lv] : L) {
    inLive[k] = true;
    const bool known = schema.count(k) != 0;
    auto bi = B.find(k);
    const bool runtimeChanged = bi == B.end() || bi->second != lv;
    auto di = D.find(k);
    if (runtimeChanged) {
      // Runtime wins. Flag a conflict when the disk also moved away from
      // the base, to a different value than the runtime one.
      bool diskChanged = false;
      std::string dv;
      if (di != D.end()) {
        dv = known ? Dn.at(k) : di->second;
        diskChanged = bi == B.end() || dv != bi->second;
      } else {
        diskChanged = !known && bi != B.end();  // unknown key deleted
      }
      if (diskChanged && dv != lv && conflicts) conflicts->push_back(k);
      out.emplace_back(k, lv);
    } else if (di != D.end()) {
      out.emplace_back(k, di->second);  // operator's text, verbatim
    } else if (known) {
      out.emplace_back(k, lv);  // keep the file a complete schema
    }
    // else: an unknown key the operator deleted while running stays gone.
  }
  // Keys only on disk: the operator added them while the modem ran (keep),
  // unless the base had them and the runtime dropped them.
  for (const auto& [k, dv] : disk) {
    if (inLive.count(k) || B.count(k) || isRetiredKey(k)) continue;
    bool dup = false;
    for (const auto& [ok, ov] : out) {
      if (ok == k) { dup = true; break; }
    }
    if (dup) continue;
    out.emplace_back(k, D.at(k));
  }
  return out;
}

bool Config::saveMerged(const std::string& path, const Config& base) const {
  IniEntries disk;
  bool malformed = false;
  const bool readable = parseIniFile(path, disk, &malformed);
  if (!readable || (disk.empty() && malformed)) {
    // Gone or unparseable: nothing to merge with, write the live config.
    if (readable) {
      SKW_WARN("config: %s is not a parseable INI; overwriting with the "
               "running configuration", path.c_str());
    }
    return save(path);
  }
  std::vector<std::string> conflicts;
  IniEntries merged = mergeIniEntries(base, *this, disk, &conflicts);
  for (const auto& k : conflicts) {
    SKW_WARN("config: '%s' was changed both at runtime and in %s; the "
             "runtime value wins", k.c_str(), path.c_str());
  }
  return writePrivateAtomic(path, renderIni(merged));
}

}  // namespace skw
