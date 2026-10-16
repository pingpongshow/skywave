#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "core/config.hpp"
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
  CHECK(!c.callsignValid());  // N0CALL blocks TX
}

TEST_CASE(config_roundtrip_all_keys) {
  Config c;
  c.callsign = "W1AW";
  c.cwId = true;
  c.cwWpm = 25;
  c.inputDevice = 3;
  c.outputGainDb = -2.5;
  c.dataPort = 9200;
  c.bindAddr = "0.0.0.0";
  c.pttMethod = PttMethod::Tcp;
  c.pttTarget = "127.0.0.1:4533";
  c.pttLeadMs = 80;
  c.family = Family::Mid;
  c.ultraEnable = true;
  c.mode = 7;
  c.maxMode = 11;
  c.cpMode = 2;
  c.listen = false;
  c.harq = HarqMode::Ir;
  c.carrierMasking = false;
  c.txWatchdogSec = 90;
  c.cryptoEnabled = true;
  c.cryptoPsk = "aabbcc";
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
  CHECK_EQ(d.bindAddr, "0.0.0.0");
  CHECK(d.pttMethod == PttMethod::Tcp);
  CHECK_EQ(d.pttTarget, "127.0.0.1:4533");
  CHECK_EQ(d.pttLeadMs, 80);
  CHECK(d.family == Family::Mid);
  CHECK(d.ultraEnable);
  CHECK_EQ(d.mode, 7);
  CHECK_EQ(d.maxMode, 11);
  CHECK_EQ(d.cpMode, 2);
  CHECK(!d.listen);
  CHECK(d.harq == HarqMode::Ir);
  CHECK(!d.carrierMasking);
  CHECK_NEAR(d.txWatchdogSec, 90.0, 1e-12);
  CHECK(d.cryptoEnabled);
  CHECK_EQ(d.cryptoPsk, "aabbcc");
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
  CHECK(c.harq == HarqMode::Auto);
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
  // means the highest active mode; numbers clamp into the active range.
  auto path = tmpPath("maxmode");
  {
    std::ofstream f(path);
    f << "[modem]\nmax_mode = auto\n";
  }
  Config c;
  CHECK(c.load(path));
  CHECK_EQ(c.maxMode, kNumModes - 1);
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
  CHECK(callsignValid("W1AW"));
  CHECK(callsignValid("VE3ABC/P"));
  CHECK(!callsignValid("N0CALL"));
  CHECK(!callsignValid("n0call"));
  CHECK(!callsignValid(""));
  CHECK(!callsignValid("AB"));                 // too short
  CHECK(!callsignValid("ABCDEFGHIJKLM"));      // too long
  CHECK(!callsignValid("W1 AW"));              // space
}

SKYTEST_MAIN
