// Control-port line protocol (INTERFACES.md): full 4.6 command surface.
// Pure logic: one line in, one response line out (no trailing newline);
// the backend is an abstract view of the modem the worker implements.
// Notifications (EVT:/MON:) are formatted by free functions and pushed by
// the worker whenever state changes - they are not responses.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  virtual void setListen(bool on) = 0;  // always accepted
  virtual bool listening() = 0;

  virtual std::string fsmState() = 0;   // IDLE/LISTENING/...
  virtual std::string statusLine() = 0; // full STATUS payload after verb
  virtual bool busy() = 0;
  virtual size_t txBufferBytes() = 0;

  virtual std::string family() = 0;
  virtual bool setFamily(const std::string& fam) = 0;  // IDLE/LISTENING only
  virtual int maxMode() = 0;
  // False: not one of the band's OFDM modes (kFirstMode..bandMaxMode).
  virtual bool setMaxMode(int mode) = 0;
  virtual void setMonitor(bool on) = 0;  // always accepted
  virtual bool monitoring() = 0;
  virtual bool txTest(double seconds) = 0;  // false: refused
  virtual void txTestStop() = 0;            // TXTEST OFF: end it now
  virtual std::string version() = 0;

  // --- 4.6 completions --------------------------------------------------
  virtual bool cq() = 0;
  virtual bool beacon() = 0;
  virtual bool bcast(const std::vector<uint8_t>& payload) = 0;
  virtual bool cwId() = 0;
  virtual void setCwId(bool on) = 0;
  virtual std::string modeStr() = 0;             // "AUTO(M9)" or "M6"
  virtual bool setMode(const std::string& m) = 0;  // "M<n>" (band's OFDM modes) | "AUTO"
  virtual std::string cpModeStr() = 0;           // AUTO/SHORT/MEDIUM/LONG
  virtual bool setCpMode(const std::string& m) = 0;
  virtual std::string harqStr() = 0;             // CHASE/IR
  virtual bool setHarq(const std::string& m) = 0;
  // Feature toggles: CHANMEM / PROBE / MASKING / RESUME / ULTRA. A
  // feature that cannot be switched off refuses OFF (setFeature false).
  virtual std::optional<bool> feature(const std::string& name) = 0;
  virtual bool setFeature(const std::string& name, bool on) = 0;
  virtual std::string statusJson() = 0;  // one-line JSON object
  virtual std::string noiseFloor() = 0;  // payload after the verb
  virtual std::string audioLevel() = 0;
  virtual std::string freqInfo() = 0;
  // `FREQINFO <hz>`: the companion app reporting the rig's dial frequency
  // (INTERFACES.md "Companion App Split"). Informational - it keys the
  // channel-memory buckets and nothing else. False rejects the value.
  virtual bool setDialFreqHz(double hz) = 0;
  // PTT config: METHOD/PORT/BAUD/LEAD/TAIL (INTERFACES.md).
  virtual std::string pttGet(const std::string& key) = 0;
  virtual bool pttSet(const std::string& key, const std::string& val) = 0;

  // --- security (crypto builds; non-crypto builds return false/absent
  // and the dispatcher answers ERROR:UNSUPPORTED) -----------------------
  virtual bool cryptoSupported() { return false; }
  virtual std::string securityLine() { return ""; }
  virtual bool setEncryption(bool /*on*/) { return false; }
  virtual bool setPsk(const std::string& /*hex*/) { return false; }
  virtual bool keyRotate() { return false; }

  // --- voice (ENABLE_VOICE builds; otherwise ERROR:UNSUPPORTED) ---------
  virtual bool voiceSupported() { return false; }
  virtual std::string voiceModeStr() { return "OFF"; }
  virtual bool setVoiceMode(const std::string& /*m*/) { return false; }
  virtual std::string voiceCodecStr() { return "AUTO"; }
  virtual bool setVoiceCodec(const std::string& /*c*/) { return false; }
  virtual bool voiceStart() { return false; }
  virtual bool voiceStop() { return false; }
  virtual bool voicePtt(bool /*on*/) { return false; }
  virtual std::string voiceStatusLine() { return ""; }
};

// Uppercases the verb, dispatches, returns the single response line.
// Unknown commands: "ERROR:UNKNOWN". Bad arguments: "ERROR:BADARG".
// State refusals: "ERROR:STATE" - including every transmitting command
// (CONNECT, CQ, BEACON, BCAST, TXTEST, VOICESTART, VOICEPTT ON) while the
// callsign is the placeholder (N0CALL or empty).
std::string handleControlLine(const std::string& line, ControlBackend& be);

// ------------------------------------------------- notification formatting
std::string evtState(const std::string& fsmState);
std::string evtConnected(const std::string& remoteCall,
                         const std::string& family, int mode);
std::string evtDisconnected(const std::string& reason);
std::string evtMode(int mode, double netBps);
std::string evtBusy(bool busy);
// Link-quality notification. The modem emits this whenever its measured RX
// SNR moves materially, so a client need not poll STATUS to track the link.
std::string evtSnr(double localDb, double remoteDb, bool remoteValid);
// Fires past the +-40 Hz alignment envelope: the operator must retune.
std::string evtCfoWarn(double shiftHz);
// The TX watchdog force-released PTT after `seconds` of continuous keying.
std::string evtTxWatchdog(double seconds);
// A monitored broadcast (CQ/BEACON/BCAST_DATA) whose fragment chain
// completed. Payload is hex so it survives the line protocol intact.
std::string evtBroadcast(const std::string& src,
                         const std::vector<uint8_t>& payload);
std::string monLine(const std::string& src, const std::string& dst,
                    uint8_t frameType, const std::string& family, int mode,
                    const std::string& hexPayload);

}  // namespace skw
