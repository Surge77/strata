#include "obs/http.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <format>

#include <poll.h>
#include <sys/socket.h>

namespace strata::http {
namespace {

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// Splits off the next line, without its "\n" or "\r\n".
std::string_view NextLine(std::string_view* rest) {
  const std::size_t end = rest->find('\n');
  std::string_view line = rest->substr(0, end);
  rest->remove_prefix(end == std::string_view::npos ? rest->size() : end + 1);
  if (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  return line;
}

// RFC 9110 tchar: what a header name may contain.
bool IsHeaderName(std::string_view text) {
  constexpr std::string_view kSymbols = "!#$%&'*+-.^_`|~";
  return !text.empty() && std::all_of(text.begin(), text.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kSymbols.find(c) != std::string_view::npos;
  });
}

bool IsToken(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return std::isupper(static_cast<unsigned char>(c)) != 0;
  });
}

std::string_view ReasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

}  // namespace

std::optional<std::size_t> FindEndOfHead(std::string_view buffer) {
  const std::size_t crlf = buffer.find("\r\n\r\n");
  const std::size_t lf = buffer.find("\n\n");
  if (crlf == std::string_view::npos && lf == std::string_view::npos) {
    return std::nullopt;
  }
  if (lf == std::string_view::npos || (crlf != std::string_view::npos && crlf < lf)) {
    return crlf + 4;
  }
  return lf + 2;
}

std::optional<Request> ParseRequestHead(std::string_view head) {
  std::string_view rest = head;
  const std::string_view request_line = NextLine(&rest);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t last_space = request_line.rfind(' ');
  if (first_space == std::string_view::npos || first_space == last_space) {
    return std::nullopt;
  }
  const std::string_view method = request_line.substr(0, first_space);
  const std::string_view target =
      request_line.substr(first_space + 1, last_space - first_space - 1);
  const std::string_view version = request_line.substr(last_space + 1);
  if (!IsToken(method) || (version != "HTTP/1.0" && version != "HTTP/1.1") ||
      !target.starts_with('/') || target.find(' ') != std::string_view::npos) {
    return std::nullopt;
  }

  Request request;
  request.method = method;
  request.path = target.substr(0, target.find('?'));
  request.version = version;
  while (!rest.empty()) {
    const std::string_view line = NextLine(&rest);
    if (line.empty()) {
      break;  // the blank line that ends the head
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || !IsHeaderName(line.substr(0, colon))) {
      return std::nullopt;
    }
    if (EqualsIgnoreCase(line.substr(0, colon), "host")) {
      if (request.host.has_value()) {
        return std::nullopt;
      }
      request.host = std::string(Trim(line.substr(colon + 1)));
    }
  }
  return request;
}

bool IsLoopbackHost(std::string_view host) {
  std::string_view name = host;
  if (name.starts_with('[')) {
    const std::size_t close = name.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    const std::string_view after = name.substr(close + 1);
    if (!after.empty() && !after.starts_with(':')) {
      return false;
    }
    return name.substr(0, close + 1) == "[::1]";
  }
  if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
    const std::string_view port = name.substr(colon + 1);
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      return false;
    }
    name = name.substr(0, colon);
  }
  return EqualsIgnoreCase(name, "localhost") || name == "127.0.0.1";
}

std::string Response(int status, std::string_view content_type, std::string_view body,
                     bool head_only, std::string_view extra_headers) {
  // no-store: a stats view is stale the moment it is served. nosniff keeps a
  // browser from reinterpreting JSON as something executable.
  std::string response = std::format(
      "HTTP/1.1 {} {}\r\n"
      "Content-Type: {}\r\n"
      "Content-Length: {}\r\n"
      "Cache-Control: no-store\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Connection: close\r\n"
      "{}\r\n",
      status, ReasonPhrase(status), content_type, body.size(), extra_headers);
  if (!head_only) {
    response.append(body);
  }
  return response;
}

bool WaitFor(int fd, short events, Deadline deadline) {
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return false;
    }
    pollfd ready{.fd = fd, .events = events, .revents = 0};
    const int result = ::poll(&ready, 1, static_cast<int>(left.count()));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result > 0;
  }
}

bool SendAll(int fd, std::string_view data, Deadline deadline) {
  while (!data.empty()) {
    const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      data.remove_prefix(static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    // EWOULDBLOCK is the same value as EAGAIN on Linux, the only target.
    if (sent < 0 && errno == EAGAIN && WaitFor(fd, POLLOUT, deadline)) {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace strata::http
