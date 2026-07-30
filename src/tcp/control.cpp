#include "tcp/control.hpp"

#include <cctype>
#include <cstdio>
#include <vector>

namespace skw {
namespace {

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tok;
  std::string cur;
  for (char c : line) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) tok.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) tok.push_back(std::move(cur));
  return tok;
}

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string onOff(bool v) { return v ? "ON" : "OFF"; }

std::optional<bool> parseOnOff(const std::string& s) {
  std::string u = upper(s);
  if (u == "ON") return true;
  if (u == "OFF") return false;
  return std::nullopt;
}

}  // namespace

std::string handleControlLine(const std::string& line, ControlBackend& be) {
  auto tok = tokenize(line);
  if (tok.empty()) return "ERROR:UNKNOWN";
  const std::string verb = upper(tok[0]);
  const size_t argc = tok.size() - 1;

  if (verb == "MYCALL") {
    if (argc == 0) return "MYCALL " + be.myCall();
    return be.setMyCall(upper(tok[1])) ? "MYCALL " + be.myCall()
                                       : "ERROR:BADARG";
  }
  if (verb == "CONNECT") {
    if (argc != 1) return "ERROR:BADARG";
    return be.connectTo(upper(tok[1])) ? "CONNECT " + upper(tok[1])
                                       : "ERROR:STATE";
  }
  if (verb == "DISCONNECT") {
    return be.disconnect() ? "DISCONNECT" : "ERROR:STATE";
  }
  if (verb == "ABORT") {
    be.abortSession();
    return "ABORT";
  }
  if (verb == "LISTEN") {
    if (argc == 0) return "LISTEN " + onOff(be.listening());
    auto v = parseOnOff(tok[1]);
    if (!v) return "ERROR:BADARG";
    return be.setListen(*v) ? "LISTEN " + onOff(*v) : "ERROR:STATE";
  }
  if (verb == "STATUS") {
    if (argc == 1 && upper(tok[1]) == "JSON") {
      return "STATUS " + be.statusJson();
    }
    if (argc != 0) return "ERROR:BADARG";
    return "STATUS " + be.statusLine();
  }
  if (verb == "STATE") return "STATE " + be.fsmState();
  if (verb == "BUSY") return std::string("BUSY ") + (be.busy() ? "1" : "0");
  if (verb == "BUFFER") {
    return "BUFFER " + std::to_string(be.txBufferBytes());
  }
  if (verb == "FAMILY") {
    if (argc == 0) return "FAMILY " + be.family();
    return be.setFamily(upper(tok[1])) ? "FAMILY " + be.family()
                                       : "ERROR:STATE";
  }
  if (verb == "MAXMODE") {
    if (argc == 0) return "MAXMODE M" + std::to_string(be.maxMode());
    std::string a = upper(tok[1]);
    if (a.size() < 2 || a[0] != 'M') return "ERROR:BADARG";
    int m = atoi(a.c_str() + 1);
    if (m < 4 || m > 13) return "ERROR:BADARG";
    return be.setMaxMode(m) ? "MAXMODE M" + std::to_string(m)
                            : "ERROR:STATE";
  }
  if (verb == "MONITOR") {
    if (argc == 0) return "MONITOR " + onOff(be.monitoring());
    auto v = parseOnOff(tok[1]);
    if (!v) return "ERROR:BADARG";
    be.setMonitor(*v);
    return "MONITOR " + onOff(*v);
  }
  if (verb == "TXTEST") {
    if (argc != 1) return "ERROR:BADARG";
    if (upper(tok[1]) == "OFF") return "TXTEST OFF";
    double sec = atof(tok[1].c_str());
    if (sec <= 0.0 || sec > 30.0) return "ERROR:BADARG";
    return be.txTest(sec) ? "TXTEST " + tok[1] : "ERROR:STATE";
  }
  if (verb == "VERSION") return "VERSION " + be.version();

  // --- 4.6 completions ------------------------------------------------
  if (verb == "CQ") return be.cq() ? "CQ" : "ERROR:STATE";
  if (verb == "BEACON") return be.beacon() ? "BEACON" : "ERROR:STATE";
  if (verb == "BCAST") {
    if (argc != 1) return "ERROR:BADARG";
    const std::string& h = tok[1];
    if (h.size() < 2 || (h.size() % 2) != 0) return "ERROR:BADARG";
    std::vector<uint8_t> payload;
    payload.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
      };
      const int hi = nib(h[i]), lo = nib(h[i + 1]);
      if (hi < 0 || lo < 0) return "ERROR:BADARG";
      payload.push_back(uint8_t(hi << 4 | lo));
    }
    return be.bcast(payload) ? "BCAST " + std::to_string(payload.size())
                             : "ERROR:BADARG";
  }
  if (verb == "CWID") {
    if (argc == 0) return "CWID " + onOff(be.cwId());
    auto v = parseOnOff(tok[1]);
    if (!v) return "ERROR:BADARG";
    be.setCwId(*v);
    return "CWID " + onOff(*v);
  }
  if (verb == "MODE") {
    if (argc == 0) return "MODE " + be.modeStr();
    return be.setMode(upper(tok[1])) ? "MODE " + be.modeStr()
                                     : "ERROR:BADARG";
  }
  if (verb == "CPMODE") {
    if (argc == 0) return "CPMODE " + be.cpModeStr();
    return be.setCpMode(upper(tok[1])) ? "CPMODE " + be.cpModeStr()
                                       : "ERROR:BADARG";
  }
  if (verb == "HARQ") {
    if (argc == 0) return "HARQ " + be.harqStr();
    return be.setHarq(upper(tok[1])) ? "HARQ " + be.harqStr()
                                     : "ERROR:BADARG";
  }
  if (verb == "CHANMEM" || verb == "PROBE" || verb == "MASKING" ||
      verb == "RESUME") {
    if (argc == 0) {
      auto v = be.feature(verb);
      return v ? verb + " " + onOff(*v) : "ERROR:STATE";
    }
    auto v = parseOnOff(tok[1]);
    if (!v) return "ERROR:BADARG";
    return be.setFeature(verb, *v) ? verb + " " + onOff(*v)
                                   : "ERROR:STATE";
  }
  if (verb == "NOISEFLOOR") return "NOISEFLOOR " + be.noiseFloor();
  if (verb == "AUDIOLEVEL") return "AUDIOLEVEL " + be.audioLevel();
  if (verb == "FREQINFO") return "FREQINFO " + be.freqInfo();
  if (verb == "PTTMETHOD" || verb == "PTTPORT" || verb == "PTTBAUD" ||
      verb == "PTTLEAD" || verb == "PTTTAIL") {
    const std::string key = verb.substr(3);
    if (argc == 0) return verb + " " + be.pttGet(key);
    // Value passed verbatim: PTTPORT is a case-sensitive device path.
    return be.pttSet(key, tok[1]) ? verb + " " + be.pttGet(key)
                                  : "ERROR:BADARG";
  }
  if (verb == "SECURITY" || verb == "ENCRYPTION" || verb == "ENCRYPTKEY" ||
      verb == "KEYROTATE") {
    if (!be.cryptoSupported()) return "ERROR:UNSUPPORTED";
    if (verb == "SECURITY") return "SECURITY " + be.securityLine();
    if (verb == "ENCRYPTION") {
      if (argc == 0) return "SECURITY " + be.securityLine();
      auto v = parseOnOff(tok[1]);
      if (!v) return "ERROR:BADARG";
      // Refused without a PSK, or while a session is up (SECURITY.md).
      return be.setEncryption(*v) ? "ENCRYPTION " + onOff(*v)
                                  : "ERROR:STATE";
    }
    if (verb == "ENCRYPTKEY") {
      if (argc != 1) return "ERROR:BADARG";
      return be.setPsk(tok[1]) ? "ENCRYPTKEY" : "ERROR:BADARG";
    }
    return be.keyRotate() ? "KEYROTATE" : "ERROR:STATE";
  }
  if (verb == "VOICE" || verb == "VOICECODEC" || verb == "VOICESTART" ||
      verb == "VOICESTOP" || verb == "VOICEPTT" || verb == "VOICESTATUS") {
    // Compiled out (no Codec2): recognized but unavailable, which is a
    // distinct answer from a typo.
    if (!be.voiceSupported()) return "ERROR:UNSUPPORTED";
    if (verb == "VOICE") {
      if (argc == 0) return "VOICE " + be.voiceModeStr();
      return be.setVoiceMode(upper(tok[1])) ? "VOICE " + be.voiceModeStr()
                                            : "ERROR:BADARG";
    }
    if (verb == "VOICECODEC") {
      if (argc == 0) return "VOICECODEC " + be.voiceCodecStr();
      return be.setVoiceCodec(upper(tok[1]))
                 ? "VOICECODEC " + be.voiceCodecStr()
                 : "ERROR:BADARG";
    }
    if (verb == "VOICESTART") {
      return be.voiceStart() ? "VOICESTART" : "ERROR:STATE";
    }
    if (verb == "VOICESTOP") {
      return be.voiceStop() ? "VOICESTOP" : "ERROR:STATE";
    }
    if (verb == "VOICEPTT") {
      if (argc != 1) return "ERROR:BADARG";
      auto v = parseOnOff(tok[1]);
      if (!v) return "ERROR:BADARG";
      return be.voicePtt(*v) ? "VOICEPTT " + onOff(*v) : "ERROR:STATE";
    }
    return "VOICESTATUS " + be.voiceStatusLine();
  }
  return "ERROR:UNKNOWN";
}

std::string evtState(const std::string& s) { return "EVT:STATE " + s; }

std::string evtConnected(const std::string& remoteCall,
                         const std::string& family, int mode) {
  return "EVT:CONNECTED " + remoteCall + " " + family + " M" +
         std::to_string(mode);
}

std::string evtDisconnected(const std::string& reason) {
  return "EVT:DISCONNECTED " + reason;
}

std::string evtMode(int mode, double netBps) {
  char buf[64];
  snprintf(buf, sizeof(buf), "EVT:MODE M%d %.0f", mode, netBps);
  return buf;
}

std::string evtBusy(bool busy) {
  return std::string("EVT:BUSY ") + (busy ? "1" : "0");
}

std::string monLine(const std::string& src, const std::string& dst,
                    uint8_t frameType, const std::string& family, int mode,
                    const std::string& hexPayload) {
  return "MON:" + src + ":" + (dst.empty() ? "*" : dst) + ":" +
         std::to_string(int(frameType)) + ":" + family + ":M" +
         std::to_string(mode) + ":" + hexPayload;
}

}  // namespace skw
