// TCP servers + control protocol (ROADMAP 2.6): scripted command/
// response/notification session against a stub backend, plus real-socket
// loopback for the server plumbing (single-client policy, line framing,
// data-port byte stream).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
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
  std::string version() override { return "skywave-test"; }
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
  be.state = "CONNECTED";
  CHECK(run("TXTEST 5") == "ERROR:STATE");

  // Unknown.
  CHECK(run("FLY ME") == "ERROR:UNKNOWN");
  CHECK(run("") == "ERROR:UNKNOWN");
}

TEST_CASE(notification_formats) {
  CHECK(evtState("CONNECTED") == "EVT:STATE CONNECTED");
  CHECK(evtConnected("K6XYZ", "WIDE", 6) == "EVT:CONNECTED K6XYZ WIDE M6");
  CHECK(evtDisconnected("IDLE_TIMEOUT") == "EVT:DISCONNECTED IDLE_TIMEOUT");
  CHECK(evtMode(8, 1677.0) == "EVT:MODE M8 1677");
  CHECK(evtBusy(true) == "EVT:BUSY 1");
  CHECK(monLine("W1AW", "", 19, "WIDE", 4, "AB12") ==
        "MON:W1AW:*:19:WIDE:M4:AB12");
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

SKYTEST_MAIN
