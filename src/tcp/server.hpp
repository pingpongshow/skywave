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
  bool open(uint16_t port);
  void close();
  uint16_t boundPort() const { return boundPort_; }

  // Accept/refuse pending connections (one client policy: extras are
  // closed immediately), read whatever the client sent, detect drops.
  // Returns bytes received this poll.
  std::vector<uint8_t> poll();

  bool clientConnected() const { return client_ >= 0; }
  // Queues bytes and sends what the socket will take; the remainder is
  // retried by the next poll(). A client that stops reading entirely is
  // dropped once its backlog passes kTxBufferCap.
  void send(const std::vector<uint8_t>& bytes);
  void send(const std::string& s);
  void dropClient();
  size_t txBacklog() const { return txBuf_.size(); }

  // Enough to ride out a GUI client that pauses for a few frames during a
  // bulk transfer, small enough that a dead client is noticed promptly.
  static constexpr size_t kTxBufferCap = 4u << 20;  // 4 MiB

 private:
  void flush();

  int listen_ = -1;
  int client_ = -1;
  uint16_t boundPort_ = 0;
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

 private:
  std::string acc_;
  std::vector<std::string> lines_;
  bool dropping_ = false;  // inside an oversize line
};

}  // namespace skw
