// Control-port line protocol (INTERFACES.md): Phase-2 command subset.
// Pure logic: one line in, one response line out (no trailing newline);
// the backend is an abstract view of the modem the worker implements.
// Notifications (EVT:/MON:) are formatted by free functions and pushed by
// the worker whenever state changes - they are not responses.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace skw {

// What the control protocol needs from the modem. Setters return false
// when the value/state is not acceptable (wrong state, bad value).
struct ControlBackend {
  virtual ~ControlBackend() = default;

  virtual std::string myCall() = 0;
  virtual bool setMyCall(const std::string& call) = 0;

  virtual bool connectTo(const std::string& call) = 0;  // false: bad state
  virtual bool disconnect() = 0;
  virtual void abortSession() = 0;
  virtual bool setListen(bool on) = 0;
  virtual bool listening() = 0;

  virtual std::string fsmState() = 0;   // IDLE/LISTENING/...
  virtual std::string statusLine() = 0; // full STATUS payload after verb
  virtual bool busy() = 0;
  virtual size_t txBufferBytes() = 0;

  virtual std::string family() = 0;
  virtual bool setFamily(const std::string& fam) = 0;  // IDLE/LISTENING only
  virtual int maxMode() = 0;
  virtual bool setMaxMode(int mode) = 0;
  virtual bool setMonitor(bool on) = 0;
  virtual bool monitoring() = 0;
  virtual bool txTest(double seconds) = 0;  // false: refused
  virtual std::string version() = 0;
};

// Uppercases the verb, dispatches, returns the single response line.
// Unknown commands: "ERROR:UNKNOWN". Bad arguments: "ERROR:BADARG".
// State refusals: "ERROR:STATE".
std::string handleControlLine(const std::string& line, ControlBackend& be);

// ------------------------------------------------- notification formatting
std::string evtState(const std::string& fsmState);
std::string evtConnected(const std::string& remoteCall,
                         const std::string& family, int mode);
std::string evtDisconnected(const std::string& reason);
std::string evtMode(int mode, double netBps);
std::string evtBusy(bool busy);
std::string monLine(const std::string& src, const std::string& dst,
                    uint8_t frameType, const std::string& family, int mode,
                    const std::string& hexPayload);

}  // namespace skw
