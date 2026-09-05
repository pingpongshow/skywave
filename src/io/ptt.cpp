#include "io/ptt.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/log.hpp"

namespace skw {
namespace {

// --- NONE / VOX -------------------------------------------------------------
// VOX is not a no-op by accident: the rig derives keying from the audio
// itself, so the modem's job is exactly the lead/tail silence padding the
// burst already carries.
class NullPtt final : public PttDriver {
 public:
  explicit NullPtt(const char* name) : name_(name) {}
  bool key() override { return true; }
  bool unkey() override { return true; }
  bool healthy() const override { return true; }
  const char* name() const override { return name_; }

 private:
  const char* name_;
};

// --- RTS / DTR --------------------------------------------------------------
class SerialPtt final : public PttDriver {
 public:
  SerialPtt(const std::string& device, int baud, bool useRts)
      : device_(device), baud_(baud), useRts_(useRts) {
    open();
  }
  ~SerialPtt() override {
    if (fd_ >= 0) {
      lower();
      ::close(fd_);
    }
  }

  bool key() override { return raise(); }
  bool unkey() override { return lower(); }
  bool healthy() const override { return fd_ >= 0; }
  const char* name() const override { return useRts_ ? "RTS" : "DTR"; }
  std::string lastError() const override { return err_; }

 private:
  static speed_t baudConstant(int baud) {
    switch (baud) {
      case 1200: return B1200;
      case 2400: return B2400;
      case 4800: return B4800;
      case 9600: return B9600;
      case 19200: return B19200;
      case 38400: return B38400;
      case 57600: return B57600;
      case 115200: return B115200;
      default: return B38400;
    }
  }

  bool open() {
    if (fd_ >= 0) return true;
    if (device_.empty()) {
      err_ = "no ptt_port configured";
      return false;
    }
    // O_NONBLOCK also stops open() blocking on carrier detect for a port
    // with no device attached.
    fd_ = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
      err_ = device_ + ": " + std::strerror(errno);
      return false;
    }
    // A serial PTT line needs no data traffic, but the port still has to be
    // in a sane mode: raw, no flow control (hardware flow control would
    // fight us for RTS), local mode so the driver does not drop the lines.
    termios tio{};
    if (tcgetattr(fd_, &tio) == 0) {
      cfmakeraw(&tio);
      tio.c_cflag |= (CLOCAL | CREAD);
      tio.c_cflag &= ~unsigned(CRTSCTS);
      const speed_t sp = baudConstant(baud_);
      cfsetispeed(&tio, sp);
      cfsetospeed(&tio, sp);
      tcsetattr(fd_, TCSANOW, &tio);
    }
    // Some drivers assert both lines on open; start from a known-unkeyed
    // state so a crash-restart cannot leave a transmitter running.
    lower();
    err_.clear();
    SKW_INFO("ptt: %s on %s (%d baud)", name(), device_.c_str(), baud_);
    return true;
  }

  void fail(const char* what) {
    err_ = device_ + ": " + what + ": " + std::strerror(errno);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool raise() {
    if (fd_ < 0 && !open()) return false;
    int bits = useRts_ ? TIOCM_RTS : TIOCM_DTR;
    if (ioctl(fd_, TIOCMBIS, &bits) < 0) {
      fail("TIOCMBIS");
      return false;
    }
    return true;
  }

  bool lower() {
    if (fd_ < 0) return false;
    int bits = useRts_ ? TIOCM_RTS : TIOCM_DTR;
    if (ioctl(fd_, TIOCMBIC, &bits) < 0) {
      fail("TIOCMBIC");
      return false;
    }
    return true;
  }

  std::string device_;
  int baud_ = 38400;
  bool useRts_ = true;
  int fd_ = -1;
  std::string err_;
};

// --- TCP --------------------------------------------------------------------
// `KEY\n` / `UNKEY\n` to a companion-app PTT server (INTERFACES.md). The
// socket is non-blocking throughout: this runs on the audio worker thread.
class TcpPtt final : public PttDriver {
 public:
  TcpPtt(const std::string& target, uint16_t defaultPort) {
    const size_t colon = target.rfind(':');
    if (colon != std::string::npos && colon > 0) {
      host_ = target.substr(0, colon);
      port_ = uint16_t(std::atoi(target.c_str() + colon + 1));
    } else {
      host_ = target;
      port_ = defaultPort;
    }
    if (host_.empty()) host_ = "127.0.0.1";
    connectSocket();
  }
  ~TcpPtt() override {
    if (fd_ >= 0) {
      send("UNKEY\n");
      ::close(fd_);
    }
  }

  bool key() override { return send("KEY\n"); }
  bool unkey() override { return send("UNKEY\n"); }
  bool healthy() const override { return fd_ >= 0; }
  const char* name() const override { return "TCP"; }
  std::string lastError() const override { return err_; }

 private:
  bool connectSocket() {
    if (fd_ >= 0) return true;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 ||
        !res) {
      err_ = host_ + ": name resolution failed";
      return false;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
      freeaddrinfo(res);
      err_ = "socket: ";
      err_ += std::strerror(errno);
      return false;
    }
    // Blocking connect to localhost is sub-millisecond; a remote or dead
    // endpoint must not stall the worker, so the socket goes non-blocking
    // first and an in-progress connect is simply left to finish.
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    const int rc = ::connect(fd, res->ai_addr, socklen_t(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS && errno != EALREADY &&
        errno != EISCONN) {
      err_ = host_ + ":" + portStr + ": " + std::strerror(errno);
      ::close(fd);
      return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    fd_ = fd;
    err_.clear();
    SKW_INFO("ptt: TCP keying to %s:%u", host_.c_str(), unsigned(port_));
    return true;
  }

  bool send(const char* line) {
    if (fd_ < 0 && !connectSocket()) return false;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const size_t len = std::strlen(line);
    const ssize_t n = ::send(fd_, line, len, flags);
    if (n == ssize_t(len)) return true;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // A PTT server that is not draining a 6-byte write is broken; treat
      // it as failed rather than buffering keying commands.
      err_ = "ptt server not accepting writes";
      return false;
    }
    err_ = std::string("send: ") + std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  std::string host_;
  uint16_t port_ = 0;
  int fd_ = -1;
  std::string err_;
};

}  // namespace

std::unique_ptr<PttDriver> makePttDriver(PttMethod method,
                                         const std::string& target,
                                         int baud) {
  switch (method) {
    case PttMethod::Rts:
      return std::make_unique<SerialPtt>(target, baud, /*useRts=*/true);
    case PttMethod::Dtr:
      return std::make_unique<SerialPtt>(target, baud, /*useRts=*/false);
    case PttMethod::Vox:
      return std::make_unique<NullPtt>("VOX");
    case PttMethod::Tcp:
      return std::make_unique<TcpPtt>(target, kPttTcpDefaultPort);
    case PttMethod::None:
      break;
  }
  return std::make_unique<NullPtt>("NONE");
}

}  // namespace skw
