// TCP servers + control protocol (ROADMAP 2.6): scripted command/
// response/notification session against a stub backend, plus real-socket
// loopback for the server plumbing (single-client policy, line framing,
// data-port byte stream).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "skytest.hpp"
#include "tcp/control.hpp"
#include "tcp/server.hpp"

using namespace skw;

namespace {

struct StubBackend : ControlBackend {
  std::string call = "N0CALL";
  std::string state = "IDLE";
  bool listen = false, mon = false, isBusy = false;
  std::string fam = "WIDE";
  int maxM = 13;
  size_t txbuf = 0;
  std::vector<std::string> log;

  std::string myCall() override { return call; }
  bool setMyCall(const std::string& c) override {
    if (c.size() < 3) return false;
    call = c;
    return true;
  }
  bool connectTo(const std::string& c) override {
    if (state != "IDLE" && state != "LISTENING") return false;
    log.push_back("connect:" + c);
    state = "CALLING";
    return true;
  }
  bool disconnect() override {
    if (state != "CONNECTED") return false;
    state = "DISCONNECTING";
    return true;
  }
  void abortSession() override { state = "IDLE"; }
  bool setListen(bool on) override {
    listen = on;
    if (state == "IDLE" && on) state = "LISTENING";
    if (state == "LISTENING" && !on) state = "IDLE";
    return true;
  }
  bool listening() override { return listen; }
  std::string fsmState() override { return state; }
  std::string statusLine() override {
    return "STATE=" + state + " CALL=" + call + " FAMILY=" + fam;
  }
  bool busy() override { return isBusy; }
  size_t txBufferBytes() override { return txbuf; }
  std::string family() override { return fam; }
  bool setFamily(const std::string& f) override {
    if (state != "IDLE" && state != "LISTENING") return false;
    if (f != "WIDE" && f != "MID" && f != "NARROW" && f != "ULTRA") {
      return false;
    }
    fam = f;
    return true;
  }
  int maxMode() override { return maxM; }
  bool setMaxMode(int m) override {
    maxM = m;
    return true;
  }
  bool setMonitor(bool on) override {
    mon = on;
    return true;
  }
  bool monitoring() override { return mon; }
  bool txTest(double sec) override { return state == "IDLE" && sec <= 30; }
  void txTestStop() override { ++txTestStops; }
  int txTestStops = 0;
  std::string version() override { return "skywave-test"; }
  // 4.6 surface: simple stateful stubs so the dispatcher is testable.
  bool cq() override { return state == "IDLE"; }
  bool beacon() override { return state == "IDLE"; }
  bool bcast(const std::vector<uint8_t>& p) override {
    lastBcast = p;
    return !p.empty() && p.size() <= 4096;
  }
  bool cwId() override { return cw; }
  void setCwId(bool on) override { cw = on; }
  std::string modeStr() override { return modeS; }
  bool setMode(const std::string& m) override {
    if (m == "AUTO") {
      modeS = "AUTO(M8)";
      return true;
    }
    if (m.size() >= 2 && m[0] == 'M') {
      modeS = m;
      return true;
    }
    return false;
  }
  std::string cpModeStr() override { return cpS; }
  bool setCpMode(const std::string& m) override {
    if (m != "AUTO" && m != "SHORT" && m != "MEDIUM" && m != "LONG") {
      return false;
    }
    cpS = m;
    return true;
  }
  std::string harqStr() override { return harqS; }
  bool setHarq(const std::string& m) override {
    if (m != "AUTO" && m != "CHASE" && m != "IR") return false;
    harqS = m;
    return true;
  }
  std::optional<bool> feature(const std::string& name) override {
    return features.count(name) ? std::optional<bool>(features[name])
                                : std::optional<bool>(true);
  }
  bool setFeature(const std::string& name, bool on) override {
    features[name] = on;
    return true;
  }
  std::string statusJson() override { return "{\"state\":\"" + state + "\"}"; }
  std::string noiseFloor() override { return "-98.5"; }
  std::string audioLevel() override { return "RX=-18.2 TX=-12.0"; }
  std::string freqInfo() override { return "CENTER=1500 SHIFT=0.0 CFO=0.0"; }
  double dialHz = -1.0;
  bool setDialFreqHz(double hz) override { dialHz = hz; return true; }
  std::string pttGet(const std::string& key) override {
    return ptt.count(key) ? ptt[key] : "NONE";
  }
  bool pttSet(const std::string& key, const std::string& val) override {
    ptt[key] = val;
    return true;
  }
  std::vector<uint8_t> lastBcast;
  bool cw = false;
  std::string modeS = "AUTO(M8)";
  std::string cpS = "AUTO";
  std::string harqS = "AUTO";
  std::map<std::string, bool> features;
  std::map<std::string, std::string> ptt;
};

int connectLoopback(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string recvSome(int fd, int maxMs = 500) {
  std::string out;
  for (int i = 0; i < maxMs / 10; ++i) {
    char buf[512];
    ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      out.append(buf, size_t(n));
      return out;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return out;
}

}  // namespace

TEST_CASE(control_protocol_scripted_session) {
  StubBackend be;
  auto run = [&](const std::string& in) { return handleControlLine(in, be); };

  // Identity round.
  CHECK(run("MYCALL") == "MYCALL N0CALL");
  CHECK(run("mycall w1aw") == "MYCALL W1AW");  // case-insensitive verb+arg
  CHECK(run("MYCALL xx") == "ERROR:BADARG");

  // Session commands.
  CHECK(run("LISTEN ON") == "LISTEN ON");
  CHECK(run("STATE") == "STATE LISTENING");
  CHECK(run("CONNECT k6xyz") == "CONNECT K6XYZ");
  CHECK(be.log.back() == "connect:K6XYZ");
  CHECK(run("CONNECT AGAIN") == "ERROR:STATE");  // already calling
  CHECK(run("DISCONNECT") == "ERROR:STATE");     // not connected
  be.state = "CONNECTED";
  CHECK(run("DISCONNECT") == "DISCONNECT");
  CHECK(run("ABORT") == "ABORT");
  CHECK(run("STATE") == "STATE IDLE");

  // Status and observability.
  CHECK(run("STATUS") == "STATUS STATE=IDLE CALL=W1AW FAMILY=WIDE");
  CHECK(run("BUSY") == "BUSY 0");
  be.isBusy = true;
  CHECK(run("BUSY") == "BUSY 1");
  be.txbuf = 512;
  CHECK(run("BUFFER") == "BUFFER 512");
  CHECK(run("VERSION") == "VERSION skywave-test");

  // Mode/family.
  CHECK(run("FAMILY") == "FAMILY WIDE");
  CHECK(run("FAMILY narrow") == "FAMILY NARROW");
  be.state = "CONNECTED";
  CHECK(run("FAMILY WIDE") == "ERROR:STATE");  // connected: refused
  be.state = "IDLE";
  CHECK(run("MAXMODE M10") == "MAXMODE M10");
  CHECK(run("MAXMODE M2") == "ERROR:BADARG");
  CHECK(run("MAXMODE") == "MAXMODE M10");

  // Monitor + TXTEST.
  CHECK(run("MONITOR ON") == "MONITOR ON");
  CHECK(run("TXTEST 10") == "TXTEST 10");
  CHECK(run("TXTEST 99") == "ERROR:BADARG");

  // 4.6 surface.
  CHECK(run("MODE") == "MODE AUTO(M8)");
  CHECK(run("MODE M6") == "MODE M6");
  CHECK(run("MODE AUTO") == "MODE AUTO(M8)");
  CHECK(run("CPMODE LONG") == "CPMODE LONG");
  CHECK(run("CPMODE BOGUS") == "ERROR:BADARG");
  CHECK(run("HARQ CHASE") == "HARQ CHASE");
  CHECK(run("CHANMEM OFF") == "CHANMEM OFF");
  CHECK(run("PROBE ON") == "PROBE ON");
  CHECK(run("MASKING OFF") == "MASKING OFF");
  CHECK(run("RESUME ON") == "RESUME ON");
  CHECK(run("CQ") == "CQ");
  CHECK(run("BEACON") == "BEACON");
  CHECK(run("BCAST 48656C6C6F") == "BCAST 5");
  CHECK(be.lastBcast.size() == 5 && be.lastBcast[0] == 'H');
  CHECK(run("BCAST xz") == "ERROR:BADARG");
  CHECK(run("CWID ON") == "CWID ON");
  CHECK(run("STATUS JSON") == "STATUS {\"state\":\"IDLE\"}");
  CHECK(run("NOISEFLOOR") == "NOISEFLOOR -98.5");
  CHECK(run("AUDIOLEVEL") == "AUDIOLEVEL RX=-18.2 TX=-12.0");
  CHECK(run("FREQINFO") == "FREQINFO CENTER=1500 SHIFT=0.0 CFO=0.0");
  CHECK(run("PTTMETHOD RTS") == "PTTMETHOD RTS");
  CHECK(run("PTTLEAD 120") == "PTTLEAD 120");
  CHECK(run("VOICESTATUS") == "ERROR:UNSUPPORTED");
  CHECK(run("SECURITY") == "ERROR:UNSUPPORTED");
  CHECK(run("XYZZY") == "ERROR:UNKNOWN");
  be.state = "CONNECTED";
  CHECK(run("TXTEST 5") == "ERROR:STATE");

  // Unknown.
  CHECK(run("FLY ME") == "ERROR:UNKNOWN");
  CHECK(run("") == "ERROR:UNKNOWN");
}

TEST_CASE(command_surface_complete) {
  // M4 checklist: every command in INTERFACES.md answers with something
  // other than ERROR:UNKNOWN (UNSUPPORTED is legal for gated features).
  StubBackend be;
  const char* cmds[] = {
      "MYCALL",       "CONNECT K6XYZ", "DISCONNECT",  "ABORT",
      "LISTEN ON",    "CQ",            "BEACON",      "BCAST 00",
      "CWID ON",      "FAMILY",        "MODE",        "MAXMODE",
      "CPMODE",       "HARQ",          "CHANMEM",     "PROBE",
      "MASKING",      "RESUME",        "STATUS",      "STATUS JSON",
      "STATE",        "BUSY",          "NOISEFLOOR",  "BUFFER",
      "AUDIOLEVEL",   "MONITOR",       "TXTEST 5",    "VERSION",
      "FREQINFO",     "PTTMETHOD",     "PTTPORT",     "PTTBAUD 9600",
      "PTTLEAD",      "PTTTAIL",       "ENCRYPTION ON", "ENCRYPTKEY 00",
      "KEYROTATE",    "SECURITY",      "VOICE",       "VOICECODEC",
      "VOICESTART",   "VOICESTOP",     "VOICEPTT ON", "VOICESTATUS",
  };
  for (const char* c : cmds) {
    const std::string r = handleControlLine(c, be);
    if (r.rfind("ERROR:UNKNOWN", 0) == 0) {
      std::printf("  UNKNOWN: %s\n", c);
    }
    CHECK(r.rfind("ERROR:UNKNOWN", 0) != 0);
  }
}

TEST_CASE(notification_formats) {
  CHECK(evtState("CONNECTED") == "EVT:STATE CONNECTED");
  CHECK(evtConnected("K6XYZ", "WIDE", 6) == "EVT:CONNECTED K6XYZ WIDE M6");
  CHECK(evtDisconnected("IDLE_TIMEOUT") == "EVT:DISCONNECTED IDLE_TIMEOUT");
  CHECK(evtMode(8, 1677.0) == "EVT:MODE M8 1677");
  CHECK(evtBusy(true) == "EVT:BUSY 1");
  CHECK(monLine("W1AW", "", 19, "WIDE", 4, "AB12") ==
        "MON:W1AW:*:19:WIDE:M4:AB12");
  CHECK(evtSnr(-3.5, 2.0, true) == "EVT:SNR -3.5 2.0");
  CHECK(evtSnr(-3.5, 0.0, false) == "EVT:SNR -3.5 -");
  CHECK(evtCfoWarn(-42.5) == "EVT:CFO_WARN -42.5");
  CHECK(evtTxWatchdog(120.0) == "EVT:TX_WATCHDOG 120.0");
  CHECK(evtBroadcast("K6XYZ", {0xDE, 0xAD}) == "EVT:BROADCAST K6XYZ DEAD");
  CHECK(evtBroadcast("", {}) == "EVT:BROADCAST ? ");
}

// An over-long line must be discarded WHOLE. Clearing the accumulator at
// the cap alone let the TAIL of an oversize line be parsed as a command
// in its own right - a line ending in "...DISCONNECT\n" would execute.
TEST_CASE(line_buffer_drops_oversize_line_entirely) {
  LineBuffer lb;
  std::vector<uint8_t> huge(LineBuffer::kMaxLineBytes + 64, 'A');
  lb.feed(huge);
  CHECK(lb.takeLines().empty());
  // The tail of the oversize line, then its terminator: still nothing.
  lb.feed({'D', 'I', 'S', 'C', 'O', 'N', 'N', 'E', 'C', 'T', '\n'});
  CHECK(lb.takeLines().empty());
  // The NEXT line is a fresh, valid command.
  lb.feed({'S', 'T', 'A', 'T', 'E', '\n'});
  auto lines = lb.takeLines();
  CHECK_EQ(int(lines.size()), 1);
  CHECK(lines[0] == "STATE");
}

TEST_CASE(line_buffer_framing) {
  LineBuffer lb;
  lb.feed({'S', 'T'});
  CHECK(lb.takeLines().empty());
  lb.feed({'A', 'T', 'E', '\r', '\n', 'B', 'U', 'S', 'Y', '\n'});
  auto lines = lb.takeLines();
  CHECK_EQ(int(lines.size()), 2);
  CHECK(lines[0] == "STATE");
  CHECK(lines[1] == "BUSY");
}

TEST_CASE(socket_loopback_control_session) {
  TcpServer srv;
  CHECK(srv.open(0));
  int fd = connectLoopback(srv.boundPort());
  CHECK(fd >= 0);
  // Server sees the client after a poll.
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientConnected());

  StubBackend be;
  LineBuffer lb;
  const char* cmd = "MYCALL W1AW\nSTATE\n";
  ::send(fd, cmd, strlen(cmd), 0);
  std::string replies;
  for (int i = 0; i < 100 && replies.find("STATE") == std::string::npos;
       ++i) {
    lb.feed(srv.poll());
    for (auto& line : lb.takeLines()) {
      srv.send(handleControlLine(line, be) + "\n");
    }
    replies += recvSome(fd, 20);
  }
  CHECK(replies.find("MYCALL W1AW\n") != std::string::npos);
  CHECK(replies.find("STATE IDLE\n") != std::string::npos);

  // Single-client policy: a second connection is closed immediately.
  int fd2 = connectLoopback(srv.boundPort());
  CHECK(fd2 >= 0);
  srv.poll();
  char b;
  ssize_t n = -1;
  for (int i = 0; i < 100; ++i) {
    n = ::recv(fd2, &b, 1, MSG_DONTWAIT);
    if (n == 0) break;  // peer closed us
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    srv.poll();
  }
  CHECK_EQ(int(n), 0);
  ::close(fd2);

  // Client drop is detected; a new client can then attach.
  ::close(fd);
  for (int i = 0; i < 100 && srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(!srv.clientConnected());
  int fd3 = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientConnected());
  ::close(fd3);
}

TEST_CASE(socket_data_port_byte_stream) {
  TcpServer srv;
  CHECK(srv.open(0));
  int fd = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Client -> server bytes arrive intact (incl. 0xFB and zeros).
  std::vector<uint8_t> payload = {0x00, 0xFB, 0x41, 0xFF, 0x00, 0x7E};
  ::send(fd, payload.data(), payload.size(), 0);
  std::vector<uint8_t> got;
  for (int i = 0; i < 100 && got.size() < payload.size(); ++i) {
    auto rx = srv.poll();
    got.insert(got.end(), rx.begin(), rx.end());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(got == payload);
  // Server -> client.
  srv.send(payload);
  std::string back = recvSome(fd);
  CHECK_EQ(int(back.size()), int(payload.size()));
  CHECK(memcmp(back.data(), payload.data(), payload.size()) == 0);
  ::close(fd);
}

// An immediate reconnect must be accepted: the old socket closes a moment
// before the new one arrives, and accept ran before the read that notices
// the close, so the newcomer was refused as an extra client (found by the
// companion app's scripted test: Connect after auto-connect failed).
TEST_CASE(immediate_reconnect_is_accepted) {
  TcpServer srv;
  CHECK(srv.open(0));
  int fd = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientConnected());
  ::close(fd);                                  // no poll in between
  int fd2 = connectLoopback(srv.boundPort());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  srv.poll();                                   // accept + liveness in one
  std::vector<uint8_t> payload = {0x52, 0x45};
  ::send(fd2, payload.data(), payload.size(), 0);
  std::vector<uint8_t> got;
  for (int i = 0; i < 100 && got.size() < payload.size(); ++i) {
    auto rx = srv.poll();
    got.insert(got.end(), rx.begin(), rx.end());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(got == payload);
  srv.send(payload);
  CHECK_EQ(int(recvSome(fd2).size()), int(payload.size()));
  ::close(fd2);
}

// A client that stops reading briefly must NOT be disconnected: the send
// path buffers the remainder and retries it on the next poll. Dropping the
// client on EAGAIN cost a companion app its session mid-transfer whenever
// it paused for a frame during a bulk delivery.
TEST_CASE(slow_client_is_buffered_not_dropped) {
  TcpServer srv;
  CHECK(srv.open(0));
  int fd = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientConnected());

  // Push a lot while the client reads nothing at all. Whether the kernel
  // absorbs it or the server has to buffer it depends on the platform's
  // socket buffers; what must NOT happen either way is a disconnect.
  std::vector<uint8_t> chunk(64 * 1024, 0xA5);
  const int kChunks = 32;  // 2 MiB, comfortably under kTxBufferCap
  for (int i = 0; i < kChunks; ++i) srv.send(chunk);
  CHECK(srv.clientConnected());

  // Everything sent must arrive: a buffered remainder is retried by
  // poll(), not dropped.
  const size_t expect = size_t(kChunks) * chunk.size();
  size_t drained = 0;
  std::vector<uint8_t> sink(64 * 1024);
  for (int i = 0; i < 4000 && drained < expect; ++i) {
    ssize_t n = ::recv(fd, sink.data(), sink.size(), 0);
    if (n > 0) {
      drained += size_t(n);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    srv.poll();
  }
  CHECK(srv.clientConnected());
  CHECK_EQ(int(drained), int(expect));
  CHECK_EQ(int(srv.txBacklog()), 0);
  ::close(fd);
}

// --- review fixes (C5, C6, C17) ----------------------------------------------

TEST_CASE(txtest_off_reaches_the_backend) {
  // C5: TXTEST OFF answered OK without stopping anything.
  StubBackend be;
  CHECK(handleControlLine("TXTEST 10", be) == "TXTEST 10");
  CHECK(handleControlLine("TXTEST off", be) == "TXTEST OFF");
  CHECK_EQ(be.txTestStops, 1);
}

TEST_CASE(line_buffer_reset_drops_partial_line) {
  // C17: a dropped client's unterminated "CONNECT W1A" must not prefix
  // the next client's "STATUS".
  LineBuffer lb;
  const std::string a = "CONNECT W1A";
  lb.feed(std::vector<uint8_t>(a.begin(), a.end()));
  lb.reset();
  const std::string b = "STATUS\n";
  lb.feed(std::vector<uint8_t>(b.begin(), b.end()));
  auto lines = lb.takeLines();
  CHECK_EQ(int(lines.size()), 1);
  if (!lines.empty()) CHECK_EQ(lines[0], std::string("STATUS"));
}

TEST_CASE(client_generation_and_read_budget) {
  TcpServer srv;
  CHECK(srv.open(0));
  const uint32_t g0 = srv.clientGeneration();
  int fd = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientConnected());
  const uint32_t g1 = srv.clientGeneration();
  CHECK(g1 != g0);

  // C6: a zero budget reads nothing (bytes stay queued in the kernel,
  // which is what applies TCP flow control); a small budget caps the
  // read; the rest arrives on later polls intact.
  std::vector<uint8_t> payload(10000);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7);
  ::send(fd, payload.data(), payload.size(), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(srv.poll(0).empty());
  CHECK(srv.clientConnected());
  std::vector<uint8_t> got;
  for (int i = 0; i < 200 && got.size() < payload.size(); ++i) {
    auto rx = srv.poll(1000);
    CHECK(rx.size() <= 1000);
    got.insert(got.end(), rx.begin(), rx.end());
    if (rx.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(got == payload);

  // A paused (zero-budget) poll still notices the client going away.
  ::close(fd);
  for (int i = 0; i < 100 && srv.clientConnected(); ++i) {
    srv.poll(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(!srv.clientConnected());
  int fd2 = connectLoopback(srv.boundPort());
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) {
    srv.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(srv.clientGeneration() != g1);  // new client: parsers reset
  ::close(fd2);
}

SKYTEST_MAIN
