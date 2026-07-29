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
  if (verb == "STATUS") return "STATUS " + be.statusLine();
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
