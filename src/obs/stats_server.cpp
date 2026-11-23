#include "strata/stats_server.hpp"

#include "strata/db.hpp"
#include "strata/stats.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "obs/http.hpp"
#include "obs/routes.hpp"

namespace strata {
namespace {

// EWOULDBLOCK is the same value as EAGAIN on Linux, the only target here.

constexpr int kListenBacklog = 16;
constexpr auto kIoTimeout = std::chrono::seconds(2);
constexpr int kAcceptBackoffMs = 100;
constexpr std::string_view kText = "text/plain; charset=utf-8";

Status ErrnoStatus(std::string_view what) {
  return Status::IoError(std::string(what) + ": " + std::strerror(errno));
}

// A file descriptor closed on destruction.
class Fd {
 public:
  explicit Fd(int fd = -1) noexcept : fd_(fd) {}
  ~Fd() { Reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  void Reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_;
};

// A socket address for `address:port`, IPv4 or IPv6.
struct SocketAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_INET;
  bool loopback = false;
};

Result<SocketAddress> ParseAddress(const std::string& address, std::uint16_t port) {
  SocketAddress result;
  sockaddr_in v4{};
  if (::inet_pton(AF_INET, address.c_str(), &v4.sin_addr) == 1) {
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    std::memcpy(&result.storage, &v4, sizeof(v4));
    result.length = sizeof(v4);
    result.family = AF_INET;
    result.loopback = (ntohl(v4.sin_addr.s_addr) >> 24U) == 127U;
    return result;
  }
  sockaddr_in6 v6{};
  if (::inet_pton(AF_INET6, address.c_str(), &v6.sin6_addr) == 1) {
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    std::memcpy(&result.storage, &v6, sizeof(v6));
    result.length = sizeof(v6);
    result.family = AF_INET6;
    // ::1, or an IPv4-mapped 127.x address (::ffff:127.0.0.1).
    result.loopback = IN6_IS_ADDR_LOOPBACK(&v6.sin6_addr) != 0 ||
                      (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr) != 0 && v6.sin6_addr.s6_addr[12] == 127);
    return result;
  }
  return Status::InvalidArgument("not an IPv4 or IPv6 address: " + address);
}

using http::Deadline;
using http::WaitFor;

// The response is sent whole or abandoned at the deadline; either way the
// connection closes next, so the outcome needs no handling.
void SendAll(int fd, std::string_view data, Deadline deadline) {
  static_cast<void>(http::SendAll(fd, data, deadline));
}

}  // namespace

class StatsServer::Impl {
 public:
  Impl(const DB* db, Fd listener, Fd wake_read, Fd wake_write, std::uint16_t port, bool loopback,
       std::vector<StaticFile> files)
      : db_(db),
        listener_(std::move(listener)),
        wake_read_(std::move(wake_read)),
        wake_write_(std::move(wake_write)),
        port_(port),
        loopback_(loopback),
        files_(std::move(files)) {}

  void Run() {
    while (true) {
      std::array<pollfd, 2> fds{pollfd{.fd = listener_.get(), .events = POLLIN, .revents = 0},
                                pollfd{.fd = wake_read_.get(), .events = POLLIN, .revents = 0}};
      if (::poll(fds.data(), fds.size(), -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      if (fds[1].revents != 0) {
        return;  // Stop
      }
      if ((fds[0].revents & POLLIN) == 0) {
        continue;
      }
      // The listener is non-blocking: a client that resets between poll and
      // accept must not leave this thread blocked where Stop cannot reach it.
      Fd client(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (client.get() >= 0) {
        Serve(client.get());
      } else if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
        // Out of descriptors or memory: the listener stays readable, so back
        // off instead of spinning, while still answering Stop.
        pollfd wake{.fd = wake_read_.get(), .events = POLLIN, .revents = 0};
        ::poll(&wake, 1, kAcceptBackoffMs);
      }
    }
  }

  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    const char byte = 0;
    // Nothing to do if this fails: the pipe is empty and ours, so it cannot.
    [[maybe_unused]] const ssize_t written = ::write(wake_write_.get(), &byte, 1);
    thread_.join();
    listener_.Reset();  // refuse new connections once stopped, not only on destruction
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  std::thread thread_;

 private:
  void Serve(int fd) {
    // One deadline for the whole request, not per read: a per-read timeout
    // would let a client that trickles a byte at a time hold the server for
    // as long as it likes.
    const Deadline deadline = std::chrono::steady_clock::now() + kIoTimeout;

    std::string buffer;
    std::array<char, 2048> chunk{};
    while (!http::FindEndOfHead(buffer).has_value()) {
      if (buffer.size() >= http::kMaxRequestHeadBytes) {
        SendAll(fd, http::Response(431, kText, "request head too large\n"), deadline);
        return;
      }
      if (!WaitFor(fd, POLLIN, deadline)) {
        SendAll(fd, http::Response(408, kText, "timed out reading the request\n"),
                std::chrono::steady_clock::now() + kIoTimeout);
        return;
      }
      const ssize_t received = ::recv(fd, chunk.data(), chunk.size(), 0);
      if (received < 0 && (errno == EINTR || errno == EAGAIN)) {
        continue;
      }
      if (received <= 0) {
        return;  // the client went away, or the socket failed
      }
      buffer.append(chunk.data(), static_cast<std::size_t>(received));
    }
    const std::size_t head_end = *http::FindEndOfHead(buffer);
    if (head_end > http::kMaxRequestHeadBytes) {
      SendAll(fd, http::Response(431, kText, "request head too large\n"), deadline);
      return;
    }
    const obs::RouteContext context{.db = db_, .loopback = loopback_, .files = &files_};
    SendAll(fd, obs::HandleRequest(std::string_view(buffer).substr(0, head_end), context),
            deadline);
  }

  const DB* const db_;
  Fd listener_;
  const Fd wake_read_;
  const Fd wake_write_;
  const std::uint16_t port_;
  const bool loopback_;
  const std::vector<StaticFile> files_;
};

Result<std::unique_ptr<StatsServer>> StatsServer::Start(const DB* db,
                                                        const StatsServerOptions& options) {
  if (db == nullptr) {
    return Status::InvalidArgument("StatsServer needs a database");
  }
  STRATA_RETURN_IF_ERROR(obs::ValidateStaticFiles(options.files));
  STRATA_ASSIGN_OR_RETURN(SocketAddress address, ParseAddress(options.bind_address, options.port));
  Fd listener(::socket(address.family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (listener.get() < 0) {
    return ErrnoStatus("socket");
  }
  // Lets a restarted server rebind while the old connections sit in TIME_WAIT.
  const int enable = 1;
  ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address.storage), address.length) !=
      0) {
    return ErrnoStatus("bind " + options.bind_address + ":" + std::to_string(options.port));
  }
  if (::listen(listener.get(), kListenBacklog) != 0) {
    return ErrnoStatus("listen");
  }
  sockaddr_storage bound{};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    return ErrnoStatus("getsockname");
  }
  const std::uint16_t port =
      ntohs(address.family == AF_INET ? reinterpret_cast<const sockaddr_in*>(&bound)->sin_port
                                      : reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);

  // Stop wakes the poll loop through this pipe.
  std::array<int, 2> pipe_fds{-1, -1};
  if (::pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
    return ErrnoStatus("pipe2");
  }
  auto impl = std::make_unique<Impl>(db, std::move(listener), Fd(pipe_fds[0]), Fd(pipe_fds[1]),
                                     port, address.loopback, options.files);
  Impl* const raw = impl.get();
  raw->thread_ = std::thread([raw] { raw->Run(); });
  return std::unique_ptr<StatsServer>(new StatsServer(std::move(impl)));
}

StatsServer::StatsServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StatsServer::~StatsServer() {
  Stop();
}

std::uint16_t StatsServer::port() const noexcept {
  return impl_->port();
}

void StatsServer::Stop() {
  impl_->Stop();
}

}  // namespace strata
