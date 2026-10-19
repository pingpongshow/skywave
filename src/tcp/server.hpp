// Minimal single-client localhost TCP servers (INTERFACES.md): one client
// per port, non-blocking, poll-driven from the owning thread. The data
// port is a raw byte stream; the control port layers a line protocol on
// top (tcp/control.hpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skw {

class TcpServer {
 public:
  TcpServer() = default;
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // Bind + listen on 127.0.0.1:port (port 0 = ephemeral). False on error.
  // Loopback only, by design: there is no bind-address setting.
  bool open(uint16_t port);
  uint16_t boundPort() const { return boundPort_; }

  // Accept/refuse pending connections (one client policy: extras are
  // closed immediately), read what the client sent, detect drops.
  // Returns bytes received this poll - at most `maxRead`. Bytes left
  // unread stay in the kernel socket buffer, so a caller that passes a
  // small (or zero) budget while its own backlog is full makes TCP flow
  // control push back on the client instead of growing modem memory.
  std::vector<uint8_t> poll(size_t maxRead = kDefaultReadCap);

  bool clientConnected() const { return client_ >= 0; }
  // Bumps every time a new client is accepted: per-connection parser
  // state (line buffer, record parser) must be reset when it changes so
  // a dropped client's partial input never prefixes the next client's.
  uint32_t clientGeneration() const { return clientGen_; }
  // Queues bytes and sends what the socket will take; the remainder is
  // retried by the next poll(). A client that stops reading entirely is
  // dropped once its backlog passes kTxBufferCap.
  void send(const std::vector<uint8_t>& bytes);
  void send(const std::string& s);
  size_t txBacklog() const { return txBuf_.size(); }

  // Enough to ride out a GUI client that pauses for a few frames during a
  // bulk transfer, small enough that a dead client is noticed promptly.
  static constexpr size_t kTxBufferCap = 4u << 20;  // 4 MiB
  // Per-poll read budget: far above any interactive rate (the loop polls
  // every ~10 ms), but bounded so one poll can never swallow an unbounded
  // burst.
  static constexpr size_t kDefaultReadCap = 256u << 10;  // 256 KiB

 private:
  void flush();
  void close();       // client and listener (destructor, re-open)
  void dropClient();  // client only (EOF, error, stalled reader)

  int listen_ = -1;
  int client_ = -1;
  uint16_t boundPort_ = 0;
  uint32_t clientGen_ = 0;
  std::vector<uint8_t> txBuf_;  // unsent tail, retried on the next poll
};

// Line splitter for the control port: feed raw bytes, take complete
// '\n'-terminated lines (CR stripped). An oversize line is discarded
// WHOLE - up to and including its terminator - so its tail can never be
// mistaken for a command of its own.
class LineBuffer {
 public:
  static constexpr size_t kMaxLineBytes = 4096;

  void feed(const std::vector<uint8_t>& bytes);
  std::vector<std::string> takeLines();
  // Forget everything buffered (client changed).
  void reset();

 private:
  std::string acc_;
  std::vector<std::string> lines_;
  bool dropping_ = false;  // inside an oversize line
};

}  // namespace skw
