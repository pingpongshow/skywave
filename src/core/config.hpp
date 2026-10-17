// INI configuration per CONFIGURATION.md: full schema, defaults, load/save,
// unknown-key preservation, validation.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/constants.hpp"

namespace skw {

enum class PttMethod : uint8_t { None, Rts, Dtr, Vox, Tcp };
enum class HarqMode : uint8_t { Auto, Chase, Ir };
// Upper bound for ptt.lead_ms / ptt.tail_ms (and PTTLEAD / PTTTAIL).
inline constexpr int kPttPadMaxMs = 5000;
// Band lives in core/constants.hpp (it selects the numerology profile).

// Ordered "section.key" -> raw value pairs: the INI form of a config.
using IniEntries = std::vector<std::pair<std::string, std::string>>;

struct Config {
  // [station]
  std::string callsign = "N0CALL";
  bool cwId = false;
  int cwWpm = 20;
  // [audio]
  // A device is chosen by NAME (preferred: PortAudio indices shift
  // whenever any device comes or goes - an SDR client's virtual DAX
  // devices, a Continuity microphone) or by index. A non-empty name wins
  // and is resolved to the current index each time the stream opens.
  int inputDevice = -1;
  int outputDevice = -1;
  std::string inputDeviceName;
  std::string outputDeviceName;
  double inputGainDb = 0.0;
  double outputGainDb = 0.0;
  // [tcp]
  uint16_t dataPort = kPortData;
  uint16_t ctrlPort = kPortCtrl;
  uint16_t voicePort = kPortVoice;
  std::string bindAddr = "127.0.0.1";
  // [ptt]
  PttMethod pttMethod = PttMethod::None;
  std::string pttTarget;
  int pttBaud = 38400;
  int pttLeadMs = 50;
  int pttTailMs = 20;
  // [modem]
  Band band = Band::Hf;
  Family family = Family::Wide;
  bool ultraEnable = false;
  bool familyFallback = true;
  bool cssTier = true;           // CSS deep tier (M0-M3) + deep handshake
  int mode = -1;                 // -1 = auto, else pinned 4..13
  int maxMode = 13;
  int cpMode = -1;               // -1 = auto, else CpMode index
  bool listen = true;
  int maxRetries = 10;
  HarqMode harq = HarqMode::Auto;
  bool channelMemory = true;
  // (No probe_promote / pre_tx_cca keys: probe-before-promote and the
  // pre-TX CCA gate are always on in the engine. The switches were
  // stored and never read; an old INI's keys are kept as unknown keys.)
  bool carrierMasking = true;
  // Scheduled ISS/IRS turn-taking (PROTOCOL.md "Turn-Taking"), advertised
  // as CAP_TURN_SCHED. Off = legacy occupancy-gate scheduler; needed only
  // to call a pre-CAP_TURN_SCHED build, which rejects the bit.
  bool turnTaking = true;
  bool resume = true;
  bool impulseBlanker = false;
  bool multiNotch = false;
  bool freqAlign = true;
  bool invertCorrect = true;
  double txWatchdogSec = kTxWatchdogDefaultSec;
  // [crypto]
  bool cryptoEnabled = false;
  std::string cryptoPsk;
  // [voice]
  std::string voiceCodec = "auto";
  int voiceBroadcastMode = 9;
  // [gui]
  bool guiEnabled = false;
  int guiWidth = 520;
  int guiHeight = 720;
  bool guiWaterfall = true;
  bool guiConstellation = true;
  // [log]
  int logLevel = 2;
  std::string logFile;

  // Unknown keys ("section.key" -> raw value) preserved across load/save.
  std::map<std::string, std::string> unknownKeys;

  // Load from INI file; returns false on I/O failure. Parse problems are
  // logged and the affected key keeps its default.
  bool load(const std::string& path);
  // Overwrite `path` with this config (atomic, mode 0600: holds the PSK).
  bool save(const std::string& path) const;
  // Exit-time save that keeps the operator's hand edits: a three-way merge
  // of `base` (the config as loaded at startup), *this (live) and the file
  // as it is on disk now. Per key: changed at runtime (live != base) ->
  // live value; otherwise -> the disk value. Changed on both sides ->
  // runtime wins and a WARN names the key. A missing or unparseable file
  // gets the live config, as save(). Same atomic 0600 write as save().
  bool saveMerged(const std::string& path, const Config& base) const;

  // Apply one "section.key" = value (unknown keys go to unknownKeys).
  void applyEntry(const std::string& fullKey, const std::string& value);
  // The full schema plus unknown keys, in save() order, canonical text.
  IniEntries toEntries() const;

  // True when the callsign is usable over the air (CONFIGURATION.md rules).
  bool callsignValid() const;
};

// Enum <-> text helpers (shared by config, TCP commands, GUI).
std::string toString(PttMethod m);
std::optional<PttMethod> pttMethodFromString(std::string s);
std::string toString(HarqMode m);
std::optional<HarqMode> harqModeFromString(std::string s);
std::string toString(Family f);
std::optional<Family> familyFromString(std::string s);
// `input_device` / `output_device` / --audio-in / --audio-out: an integer
// is a PortAudio index (-1 = default) and clears the name; anything else
// is a device name and resets the index to -1.
void parseAudioDevice(const std::string& v, int& index, std::string& name);
std::string toString(Band b);
std::optional<Band> bandFromString(std::string s);
bool callsignValid(const std::string& call);

// Read an INI file into ordered entries (comments/blank lines skipped,
// section and key lowercased, values trimmed). False = cannot be opened.
// `malformed` (optional) reports lines without '='.
bool parseIniFile(const std::string& path, IniEntries& out,
                  bool* malformed = nullptr);
// The three-way merge behind Config::saveMerged; `conflicts` collects keys
// changed both at runtime and on disk (runtime value kept).
IniEntries mergeIniEntries(const Config& base, const Config& live,
                           const IniEntries& disk,
                           std::vector<std::string>* conflicts = nullptr);

}  // namespace skw
