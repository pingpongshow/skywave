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
  void send(const std::vector<uint8_t>& bytes);
  void send(const std::string& s);
  void dropClient();

 private:
  int listen_ = -1;
  int client_ = -1;
  uint16_t boundPort_ = 0;
};

// Line splitter for the control port: feed raw bytes, take complete
// '\n'-terminated lines (CR stripped, oversize lines dropped).
class LineBuffer {
 public:
  void feed(const std::vector<uint8_t>& bytes);
  std::vector<std::string> takeLines();

 private:
  std::string acc_;
  std::vector<std::string> lines_;
};

}  // namespace skw
