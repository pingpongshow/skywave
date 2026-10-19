#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "link/frame.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {
std::string tmpPath(const char* name) {
  return std::string("skw_test_") + name + ".ini";
}
}  // namespace

TEST_CASE(config_defaults) {
  Config c;
  CHECK_EQ(c.dataPort, kPortData);
  CHECK_EQ(c.ctrlPort, kPortCtrl);
  CHECK_EQ(c.voicePort, kPortVoice);
  CHECK(c.family == Family::Wide);
  CHECK(c.band == Band::Hf);
  CHECK(!c.ultraEnable);
  CHECK(c.familyFallback);
  CHECK_EQ(c.mode, -1);
  CHECK(isPlaceholderCall(c.callsign));  // N0CALL blocks TX
  CHECK(c.harq == HarqMode::Ir);
  CHECK_EQ(c.guiWidth, 0);  // 0 = fit to the display
  CHECK_EQ(c.guiHeight, 0);
}

TEST_CASE(config_roundtrip_all_keys) {
  Config c;
  c.callsign = "W1AW";
  c.cwId = true;
  c.cwWpm = 25;
  c.inputDevice = 3;
  c.outputGainDb = -2.5;
  c.dataPort = 9200;
  c.pttMethod = PttMethod::Tcp;
  c.pttTarget = "127.0.0.1:4533";
  c.pttLeadMs = 80;
  c.family = Family::Mid;
  c.ultraEnable = true;
  c.mode = 7;
  c.maxMode = 11;
  c.cpMode = 2;
  c.listen = false;
  c.harq = HarqMode::Chase;
  c.carrierMasking = false;
  c.txWatchdogSec = 90;
  c.cryptoEnabled = true;
  c.cryptoPsk = "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff";
  c.voiceCodec = "1300";
  c.guiEnabled = true;
  c.logLevel = 0;

  auto path = tmpPath("roundtrip");
  CHECK(c.save(path));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, "W1AW");
  CHECK(d.cwId);
  CHECK_EQ(d.cwWpm, 25);
  CHECK_EQ(d.inputDevice, 3);
  CHECK_NEAR(d.outputGainDb, -2.5, 1e-12);
  CHECK_EQ(d.dataPort, 9200);
  CHECK(d.pttMethod == PttMethod::Tcp);
  CHECK_EQ(d.pttTarget, "127.0.0.1:4533");
  CHECK_EQ(d.pttLeadMs, 80);
  CHECK(d.family == Family::Mid);
  CHECK(d.ultraEnable);
  CHECK_EQ(d.mode, 7);
  CHECK_EQ(d.maxMode, 11);
  CHECK_EQ(d.cpMode, 2);
  CHECK(!d.listen);
  CHECK(d.harq == HarqMode::Chase);
  CHECK(!d.carrierMasking);
  CHECK_NEAR(d.txWatchdogSec, 90.0, 1e-12);
  CHECK(d.cryptoEnabled);
  CHECK_EQ(d.cryptoPsk, c.cryptoPsk);
  CHECK_EQ(d.voiceCodec, "1300");
  CHECK(d.guiEnabled);
  CHECK_EQ(d.logLevel, 0);
  std::remove(path.c_str());
}

TEST_CASE(config_unknown_keys_preserved) {
  auto path = tmpPath("unknown");
  {
    std::ofstream f(path);
    f << "[modem]\nfamily = narrow\nfuture_knob = 42\n\n[newsection]\nx = y\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK(c.family == Family::Narrow);
  CHECK_EQ(c.unknownKeys.size(), 2u);
  CHECK(c.save(path));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.unknownKeys.at("modem.future_knob"), "42");
  CHECK_EQ(d.unknownKeys.at("newsection.x"), "y");
  std::remove(path.c_str());
}

// Audio devices by NAME (PortAudio indices shift as devices come and go):
// a name survives save/load verbatim, spaces included; a number is still
// an index and clears any name.
TEST_CASE(config_audio_device_by_name) {
  auto path = tmpPath("audioname");
  {
    std::ofstream f(path);
    f << "[audio]\ninput_device = VB-Cable\noutput_device = AetherSDR TX\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.inputDeviceName, std::string("VB-Cable"));
  CHECK_EQ(c.outputDeviceName, std::string("AetherSDR TX"));
  CHECK_EQ(c.inputDevice, -1);
  CHECK(c.save(path));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.outputDeviceName, std::string("AetherSDR TX"));
  CHECK_EQ(d.inputDeviceName, std::string("VB-Cable"));

  int idx = 7;
  std::string name = "old";
  parseAudioDevice("3", idx, name);
  CHECK_EQ(idx, 3);
  CHECK(name.empty());
  parseAudioDevice("-1", idx, name);
  CHECK_EQ(idx, -1);
  parseAudioDevice("BlackHole 2ch", idx, name);
  CHECK_EQ(idx, -1);
  CHECK_EQ(name, std::string("BlackHole 2ch"));
  std::remove(path.c_str());
}

TEST_CASE(config_bad_values_keep_defaults) {
  auto path = tmpPath("bad");
  {
    std::ofstream f(path);
    f << "[modem]\nmode = m99\nharq = banana\nmax_retries = ten\n"
      << "[tcp]\ndata_port = notaport\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.mode, -1);
  CHECK(c.harq == HarqMode::Ir);
  CHECK_EQ(c.maxRetries, 10);
  CHECK_EQ(c.dataPort, kPortData);
  std::remove(path.c_str());
}

TEST_CASE(config_mode_selectors) {
  auto path = tmpPath("modes");
  {
    std::ofstream f(path);
    f << "[modem]\nmode = M9\nmax_mode = 12\ncp_mode = short\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.mode, 9);
  CHECK_EQ(c.maxMode, 12);
  CHECK_EQ(c.cpMode, 0);
  std::remove(path.c_str());
}

TEST_CASE(config_max_mode_validated) {
  // C2: "auto" used to become -1 -> uint8_t 255 in the engine. It now
  // means the band's highest mode; numbers clamp into the band's range.
  auto path = tmpPath("maxmode");
  {
    std::ofstream f(path);
    f << "[modem]\nmax_mode = auto\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.maxMode, 13);  // HF: M14 is VHF-FM only
  {
    // The band decides the range wherever it appears in the file.
    std::ofstream f(path);
    f << "[modem]\nmax_mode = auto\nmode = m14\nband = vhf-fm\n";
  }
  Config v;
  CHECK(v.load(path));
  CHECK_EQ(v.maxMode, 14);
  CHECK_EQ(v.mode, 14);
  {
    std::ofstream f(path);
    f << "[modem]\nmax_mode = m14\nmode = m14\n";
  }
  Config h;
  CHECK(h.load(path));
  CHECK_EQ(h.maxMode, 13);  // clamped (WARN)
  CHECK_EQ(h.mode, -1);     // refused (WARN): not an HF mode
  {
    std::ofstream f(path);
    f << "[modem]\nmax_mode = m2\n";
  }
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.maxMode, kFirstMode);
  CHECK(d.save(path));  // and what it saves loads back the same
  Config e;
  CHECK(e.load(path));
  CHECK_EQ(e.maxMode, kFirstMode);
  std::remove(path.c_str());
}

TEST_CASE(config_ptt_pads_clamped) {
  // Review D: a negative lead/tail became a huge size_t (bad_alloc).
  auto path = tmpPath("pttpad");
  {
    std::ofstream f(path);
    f << "[ptt]\nlead_ms = -50\ntail_ms = 999999\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.pttLeadMs, 0);
  CHECK_EQ(c.pttTailMs, kPttPadMaxMs);
  std::remove(path.c_str());
}

TEST_CASE(config_save_is_private_and_atomic) {
  // C8: the file carries the PSK - it must be 0600 (even over an existing
  // 0644 file) and replaced atomically (no temp file left behind).
  auto path = tmpPath("private");
  {
    std::ofstream f(path);
    f << "stale\n";
  }
  ::chmod(path.c_str(), 0644);
  Config c;
  c.cryptoPsk = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  CHECK(c.save(path));
  struct stat st {};
  CHECK(::stat(path.c_str(), &st) == 0);
  CHECK_EQ(int(st.st_mode & 0777), 0600);
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.cryptoPsk, c.cryptoPsk);
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  CHECK(::access(tmp.c_str(), F_OK) != 0);
  // An unwritable target directory fails cleanly.
  CHECK(!c.save("/nonexistent-skywave-dir/skywave.ini"));
  std::remove(path.c_str());
}

TEST_CASE(callsign_rules) {
  // One rule: the wire's (validateCallsign), plus the placeholder check.
  CHECK(validateCallsign("W1AW").has_value());
  CHECK(validateCallsign("VE3ABC/P").has_value());
  CHECK(!validateCallsign("").has_value());
  CHECK(!validateCallsign("AB").has_value());             // too short
  CHECK(!validateCallsign("ABCDEFGHIJKLM").has_value());  // too long
  CHECK(!validateCallsign("W1 AW").has_value());          // space
  CHECK(isPlaceholderCall("N0CALL"));
  CHECK(isPlaceholderCall("n0call"));
  CHECK(isPlaceholderCall(""));
  CHECK(!isPlaceholderCall("W1AW"));
}

// A2: the INI callsign goes through the same rule as MYCALL - stored
// uppercase, and an invalid one leaves the placeholder (TX blocked).
TEST_CASE(config_callsign_validated_on_load) {
  auto path = tmpPath("callsign");
  {
    std::ofstream f(path);
    f << "[station]\ncallsign = k1aaa/p\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.callsign, "K1AAA/P");
  {
    std::ofstream f(path);
    f << "[station]\ncallsign = K1 AAA\n";
  }
  Config d;
  d.callsign = "K2BBB";
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, std::string(kPlaceholderCall));
  // The CLI path (--callsign) applies the same entry.
  Config e;
  e.applyEntry("station.callsign", "w3ccc");
  CHECK_EQ(e.callsign, "W3CCC");
  e.applyEntry("station.callsign", "!!");
  CHECK(isPlaceholderCall(e.callsign));
  std::remove(path.c_str());
}

// A3: crypto.psk must be 64 hex digits; anything else loads as no key
// (it used to decode leniently into a garbage key).
TEST_CASE(config_psk_validated_on_load) {
  auto path = tmpPath("psk");
  const std::string good(64, 'a');
  {
    std::ofstream f(path);
    f << "[crypto]\npsk = " << good << "\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.cryptoPsk, good);
  for (const std::string& bad : {std::string(64, 'g'), std::string(62, 'a'),
                                std::string(63, 'a') + "z"}) {
    {
      std::ofstream f(path);
      f << "[crypto]\npsk = " << bad << "\n";
    }
    Config d;
    CHECK(d.load(path));
    CHECK(d.cryptoPsk.empty());
  }
  std::remove(path.c_str());
}

// A9 / A5-A7: HARQ "auto" from an older file is IR; retired keys are
// dropped (not preserved as unknown keys) and vanish on the next save.
TEST_CASE(config_old_keys) {
  auto path = tmpPath("oldkeys");
  {
    std::ofstream f(path);
    f << "[tcp]\nbind_addr = 0.0.0.0\n[modem]\nharq = auto\n"
      << "impulse_blanker = true\nmulti_notch = false\nfreq_align = true\n"
      << "invert_correct = true\nturn_taking = true\n"
      << "[gui]\nwaterfall = true\nconstellation = false\nwidth = 1200\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK(c.harq == HarqMode::Ir);
  CHECK(c.unknownKeys.empty());
  CHECK_EQ(c.guiWidth, 1200);
  CHECK(c.save(path));
  IniEntries e;
  CHECK(parseIniFile(path, e));
  for (const auto& [k, v] : e) {
    CHECK(k != "tcp.bind_addr" && k != "modem.impulse_blanker" &&
          k != "modem.turn_taking" && k != "gui.waterfall");
  }
  // The exit-time merge does not resurrect them from the disk file.
  {
    std::ofstream f(path);
    f << "[tcp]\nbind_addr = 0.0.0.0\n[modem]\nmulti_notch = true\n";
  }
  Config base;
  CHECK(base.load(path));
  Config live = base;
  live.family = Family::Mid;
  CHECK(live.saveMerged(path, base));
  CHECK(parseIniFile(path, e));
  for (const auto& [k, v] : e) {
    CHECK(k != "tcp.bind_addr" && k != "modem.multi_notch");
  }
  std::remove(path.c_str());
}

// --- Exit-time three-way merge (Config::saveMerged) ----------------------
// base = config as loaded at startup, live = running config, disk = the
// file as the operator left it while the modem ran.

namespace {
void writeFile(const std::string& path, const std::string& text) {
  std::ofstream f(path);
  f << text;
}
}  // namespace

TEST_CASE(config_merge_runtime_change_saved) {
  auto path = tmpPath("merge_runtime");
  writeFile(path, "[station]\ncallsign = W1AW\n[modem]\nmax_retries = 7\n");
  Config base;
  CHECK(base.load(path));
  Config live = base;
  live.callsign = "K1ABC";  // e.g. MYCALL over the control port
  CHECK(live.saveMerged(path, base));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, "K1ABC");
  CHECK_EQ(d.maxRetries, 7);
  struct stat st {};
  CHECK(::stat(path.c_str(), &st) == 0);
  CHECK_EQ(int(st.st_mode & 0777), 0600);
  std::remove(path.c_str());
}

TEST_CASE(config_merge_disk_edit_survives) {
  auto path = tmpPath("merge_disk");
  const std::string psk1(64, '1'), psk2(64, 'b');
  writeFile(path, "[station]\ncallsign = W1AW\n[ptt]\nlead_ms = 50\n"
                  "[crypto]\npsk = " + psk1 + "\n");
  Config base;
  CHECK(base.load(path));
  Config live = base;
  live.callsign = "K1ABC";  // runtime change to one key...
  // ...while the operator edits OTHER keys in the file.
  writeFile(path, "[station]\ncallsign = W1AW\n[ptt]\nlead_ms = 120\n"
                  "[crypto]\npsk = " + psk2 + "\n[modem]\nlisten = no\n");
  CHECK(live.saveMerged(path, base));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, "K1ABC");
  CHECK_EQ(d.pttLeadMs, 120);
  CHECK_EQ(d.cryptoPsk, psk2);
  CHECK(!d.listen);
  std::remove(path.c_str());
}

TEST_CASE(config_merge_both_changed_runtime_wins) {
  auto path = tmpPath("merge_both");
  writeFile(path, "[station]\ncallsign = W1AW\n[ptt]\nbaud = 9600\n");
  Config base;
  CHECK(base.load(path));
  Config live = base;
  live.callsign = "K1ABC";
  live.pttBaud = 19200;
  writeFile(path, "[station]\ncallsign = N2XYZ\n[ptt]\nbaud = 19200\n");
  std::vector<std::string> conflicts;
  IniEntries disk;
  CHECK(parseIniFile(path, disk));
  mergeIniEntries(base, live, disk, &conflicts);
  // baud moved to the same value on both sides: not a conflict.
  CHECK_EQ(conflicts.size(), 1u);
  CHECK_EQ(conflicts.at(0), "station.callsign");
  CHECK(live.saveMerged(path, base));
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, "K1ABC");
  CHECK_EQ(d.pttBaud, 19200);
  std::remove(path.c_str());
}

TEST_CASE(config_merge_missing_or_garbage_file_writes_live) {
  auto path = tmpPath("merge_missing");
  std::remove(path.c_str());
  Config base;
  base.callsign = "W1AW";
  base.maxRetries = 3;
  Config live = base;
  live.callsign = "K1ABC";
  CHECK(live.saveMerged(path, base));  // file gone: live written in full
  Config d;
  CHECK(d.load(path));
  CHECK_EQ(d.callsign, "K1ABC");
  CHECK_EQ(d.maxRetries, 3);
  struct stat st {};
  CHECK(::stat(path.c_str(), &st) == 0);
  CHECK_EQ(int(st.st_mode & 0777), 0600);
  writeFile(path, "this is not an ini file\n");  // unparseable: same
  CHECK(live.saveMerged(path, base));
  Config e;
  CHECK(e.load(path));
  CHECK_EQ(e.callsign, "K1ABC");
  CHECK_EQ(e.maxRetries, 3);
  std::remove(path.c_str());
}

TEST_CASE(config_merge_unknown_keys_preserved) {
  auto path = tmpPath("merge_unknown");
  writeFile(path, "[modem]\nfuture_knob = 42\nold_knob = 1\n\n[newsection]\nx = y\n");
  Config base;
  CHECK(base.load(path));
  Config live = base;
  live.family = Family::Narrow;
  // Operator, while running: edits one unknown key, removes another, and
  // adds a new section.
  writeFile(path, "[modem]\nfuture_knob = 43\n\n[newsection]\nx = y\n"
                  "[other]\nz = 1\n");
  CHECK(live.saveMerged(path, base));
  Config d;
  CHECK(d.load(path));
  CHECK(d.family == Family::Narrow);
  CHECK_EQ(d.unknownKeys.at("modem.future_knob"), "43");
  CHECK_EQ(d.unknownKeys.at("newsection.x"), "y");
  CHECK_EQ(d.unknownKeys.at("other.z"), "1");
  CHECK(d.unknownKeys.count("modem.old_knob") == 0);
  // Unchanged on disk too: unknown keys simply carry through.
  Config base2 = d;
  CHECK(d.saveMerged(path, base2));
  Config e;
  CHECK(e.load(path));
  CHECK_EQ(e.unknownKeys.size(), 3u);
  std::remove(path.c_str());
}

SKYTEST_MAIN
