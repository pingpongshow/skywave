// INI configuration per CONFIGURATION.md: full schema, defaults, load/save,
// unknown-key preservation, validation.
#pragma once

#include <map>
#include <optional>
#include <string>

#include "core/constants.hpp"

namespace skw {

enum class PttMethod : uint8_t { None, Rts, Dtr, Vox, Tcp };
enum class HarqMode : uint8_t { Auto, Chase, Ir };
// Band lives in core/constants.hpp (it selects the numerology profile).

struct Config {
  // [station]
  std::string callsign = "N0CALL";
  bool cwId = false;
  int cwWpm = 20;
  // [audio]
  int inputDevice = -1;
  int outputDevice = -1;
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
  bool probePromote = true;
  bool carrierMasking = true;
  // Scheduled ISS/IRS turn-taking (PROTOCOL.md "Turn-Taking"), advertised
  // as CAP_TURN_SCHED. Off = legacy occupancy-gate scheduler; needed only
  // to call a pre-CAP_TURN_SCHED build, which rejects the bit.
  bool turnTaking = true;
  bool resume = true;
  bool preTxCca = true;
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
  bool save(const std::string& path) const;

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
std::string toString(Band b);
std::optional<Band> bandFromString(std::string s);
bool callsignValid(const std::string& call);

}  // namespace skw
