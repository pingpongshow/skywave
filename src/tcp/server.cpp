#include "tcp/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
  txBuf_.clear();  // a fresh client must not inherit a dead one's tail
}

std::vector<uint8_t> TcpServer::poll(size_t maxRead) {
  std::vector<uint8_t> rx;
  if (listen_ < 0) return rx;

  // Accept: first connection wins; extras are closed immediately.
  for (;;) {
    int fd = ::accept(listen_, nullptr, nullptr);
    if (fd < 0) break;
    // A client that reconnects at once (the companion app's Connect while
    // connected, or a restart) closes its old socket a moment before the
    // new one arrives, and accept ran BEFORE the read that notices the
    // close - so the newcomer was refused as an "extra" and the app saw
    // a data-socket error. If the current client has already closed (EOF
    // with nothing left unread), drop it and take the newcomer.
    if (client_ >= 0) {
      uint8_t probe;
      const ssize_t n = ::recv(client_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        dropClient();
      }
    }
    if (client_ < 0) {
      setNonBlocking(fd);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
      // A client vanishing mid-send must never SIGPIPE the modem
      // process (found by ST2 fault injection).
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      client_ = fd;
      ++clientGen_;
    } else {
      ::close(fd);  // one client per port
    }
  }

  if (client_ < 0) return rx;
  flush();  // retry anything the socket could not take last time
  if (client_ < 0) return rx;
  if (maxRead == 0) {
    // Backpressure: read nothing, but still notice an orderly close so a
    // vanished client is not held until the backlog drains.
    uint8_t probe;
    const ssize_t n = ::recv(client_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      dropClient();
    }
    return rx;
  }
  uint8_t buf[4096];
  while (rx.size() < maxRead) {
    const size_t want = std::min(sizeof(buf), maxRead - rx.size());
    ssize_t n = ::recv(client_, buf, want, 0);
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
  txBuf_.insert(txBuf_.end(), bytes.begin(), bytes.end());
  flush();
}

// A socket that reports EAGAIN is momentarily full, not broken - a GUI
// companion that pauses for a frame while a bulk transfer lands will do
// this routinely. Dropping the client there cost the application its
// session mid-transfer; instead the remainder is buffered and retried on
// the next poll. The cap is the backstop: a client that genuinely stopped
// reading is disconnected rather than growing the modem's memory without
// bound.
void TcpServer::flush() {
  if (client_ < 0 || txBuf_.empty()) return;
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;  // Linux: no SIGPIPE
#else
  constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE set at accept
#endif
  size_t off = 0;
  while (off < txBuf_.size()) {
    ssize_t n = ::send(client_, txBuf_.data() + off, txBuf_.size() - off,
                       kSendFlags);
    if (n > 0) {
      off += size_t(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    txBuf_.clear();
    dropClient();  // error
    return;
  }
  txBuf_.erase(txBuf_.begin(), txBuf_.begin() + long(off));
  if (txBuf_.size() > kTxBufferCap) {
    txBuf_.clear();
    dropClient();  // client is not reading at all
  }
}

void TcpServer::send(const std::string& s) {
  send(std::vector<uint8_t>(s.begin(), s.end()));
}

void LineBuffer::feed(const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) {
    if (b == '\n') {
      if (dropping_) {           // tail of an oversize line: discard it
        dropping_ = false;
        acc_.clear();
        continue;
      }
      if (!acc_.empty() && acc_.back() == '\r') acc_.pop_back();
      lines_.push_back(std::move(acc_));
      acc_.clear();
    } else if (!dropping_) {
      acc_.push_back(char(b));
      // Oversize line: drop everything up to and including the next
      // newline. Clearing the accumulator alone would let the TAIL of an
      // over-long line be parsed as a command in its own right.
      if (acc_.size() > kMaxLineBytes) {
        acc_.clear();
        dropping_ = true;
      }
    }
  }
}

void LineBuffer::reset() {
  acc_.clear();
  lines_.clear();
  dropping_ = false;
}

std::vector<std::string> LineBuffer::takeLines() {
  auto out = std::move(lines_);
  lines_.clear();
  return out;
}

}  // namespace skw
