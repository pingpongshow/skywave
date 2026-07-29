#include "tcp/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace skw {

namespace {
void setNonBlocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
}  // namespace

TcpServer::~TcpServer() { close(); }

bool TcpServer::open(uint16_t port) {
  close();
  listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_ < 0) return false;
  int one = 1;
  setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listen_, 2) < 0) {
    ::close(listen_);
    listen_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
  boundPort_ = ntohs(addr.sin_port);
  setNonBlocking(listen_);
  return true;
}

void TcpServer::close() {
  dropClient();
  if (listen_ >= 0) {
    ::close(listen_);
    listen_ = -1;
  }
  boundPort_ = 0;
}

void TcpServer::dropClient() {
  if (client_ >= 0) {
    ::close(client_);
    client_ = -1;
  }
}

std::vector<uint8_t> TcpServer::poll() {
  std::vector<uint8_t> rx;
  if (listen_ < 0) return rx;

  // Accept: first connection wins; extras are closed immediately.
  for (;;) {
    int fd = ::accept(listen_, nullptr, nullptr);
    if (fd < 0) break;
    if (client_ < 0) {
      setNonBlocking(fd);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      client_ = fd;
    } else {
      ::close(fd);  // one client per port
    }
  }

  if (client_ < 0) return rx;
  uint8_t buf[4096];
  for (;;) {
    ssize_t n = ::recv(client_, buf, sizeof(buf), 0);
    if (n > 0) {
      rx.insert(rx.end(), buf, buf + n);
      continue;
    }
    if (n == 0) {  // orderly close
      dropClient();
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    dropClient();  // error
    break;
  }
  return rx;
}

void TcpServer::send(const std::vector<uint8_t>& bytes) {
  if (client_ < 0 || bytes.empty()) return;
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::send(client_, bytes.data() + off, bytes.size() - off, 0);
    if (n > 0) {
      off += size_t(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Localhost client not draining: bounded retry via short spin is
      // not acceptable in the worker; drop the laggard instead.
      dropClient();
      return;
    }
    dropClient();
    return;
  }
}

void TcpServer::send(const std::string& s) {
  send(std::vector<uint8_t>(s.begin(), s.end()));
}

void LineBuffer::feed(const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) {
    if (b == '\n') {
      if (!acc_.empty() && acc_.back() == '\r') acc_.pop_back();
      lines_.push_back(std::move(acc_));
      acc_.clear();
    } else {
      acc_.push_back(char(b));
      if (acc_.size() > 4096) acc_.clear();  // oversize: drop
    }
  }
}

std::vector<std::string> LineBuffer::takeLines() {
  auto out = std::move(lines_);
  lines_.clear();
  return out;
}

}  // namespace skw
