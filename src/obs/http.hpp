#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace strata::http {

// Bytes a request head (request line plus headers) may take. The server only
// serves small GETs, so anything longer is refused rather than buffered.
inline constexpr std::size_t kMaxRequestHeadBytes = 8 * 1024;

struct Request {
  std::string method;
  std::string path;     // the target without its query string
  std::string version;  // "HTTP/1.0" or "HTTP/1.1"
  std::optional<std::string> host;
};

// Parses a request head that ends at its blank line ("\r\n\r\n", or "\n\n"
// from lenient clients). Nothing if it is malformed: no request line, no
// HTTP/1.x version, a target that is not an absolute path, or a header line
// whose name is not an HTTP token (which also rejects "Host :" and obsolete
// line folding, either of which could hide a Host header from the check). Header names are
// case-insensitive; a repeated Host makes the request malformed, since two hosts cannot both be
// checked.
[[nodiscard]] std::optional<Request> ParseRequestHead(std::string_view head);

// The offset just past the head's terminating blank line, or nothing if
// `buffer` does not hold a whole head yet.
[[nodiscard]] std::optional<std::size_t> FindEndOfHead(std::string_view buffer);

// Whether a Host header names this machine's loopback interface: localhost,
// 127.0.0.1, or [::1], with or without a port. A server bound to loopback
// refuses other hosts, so a web page that rebinds its own DNS name to
// 127.0.0.1 cannot read the endpoint from the browser.
[[nodiscard]] bool IsLoopbackHost(std::string_view host);

// A complete HTTP/1.1 response that closes the connection. HEAD requests get
// the same headers with no body.
[[nodiscard]] std::string Response(int status, std::string_view content_type, std::string_view body,
                                   bool head_only = false, std::string_view extra_headers = "");

}  // namespace strata::http
