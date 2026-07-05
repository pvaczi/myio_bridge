#include <http_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>

#include <format>
#include <stdexcept>
#include <string>

namespace myio {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using     tcp   = net::ip::tcp;

static std::string base64_encode(std::string_view input) {
    std::string out(beast::detail::base64::encoded_size(input.size()), '\0');
    beast::detail::base64::encode(out.data(), input.data(), input.size());
    return out;
}

static std::string fetch_raw_response(const HttpConfig& cfg, std::string_view target) {
    net::io_context ioc;
    beast::tcp_stream stream(ioc);

    tcp::resolver resolver(ioc);
    const auto endpoints = resolver.resolve(cfg.host, std::to_string(cfg.port));

    stream.expires_after(cfg.timeout);
    stream.connect(endpoints);

    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host,          cfg.host);
    req.set(http::field::user_agent,    "myio-discovery/1.0");
    req.set(http::field::connection,    "close");
    req.set(http::field::authorization,
            "Basic " + base64_encode(cfg.user + ":" + cfg.password));

    stream.expires_after(cfg.timeout);
    http::write(stream, req);

    // Raw socket read – bypasses the Beast HTTP parser because the myIO server
    // sends a non-standard HTTP header (missing CRLF or bad reason phrase).
    // Capped so a misbehaving firmware cannot grow the buffer unbounded.
    std::string raw;
    beast::error_code ec;
    net::read(stream, net::dynamic_buffer(raw, 256 * 1024), ec);

    if (ec && ec != net::error::eof && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    beast::error_code shutdown_ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);

    return raw;
}

// End of the header block. myIO terminates 200 OK header lines with bare LF
// while its 401 response uses CRLF — accept both, whichever comes first.
static std::size_t header_end(const std::string& raw, std::size_t& sep_len) {
    const auto crlf = raw.find("\r\n\r\n");
    const auto lf   = raw.find("\n\n");
    if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
        sep_len = 4;
        return crlf;
    }
    sep_len = 2;
    return lf;
}

// The status code must come from the status line only — a substring search
// over the whole response would misfire on e.g. "Content-Length: 1401".
static bool is_http_401(const std::string& raw) {
    const auto eol = raw.find('\n');
    const std::string_view status_line(
        raw.data(), eol == std::string::npos ? raw.size() : eol);
    return status_line.starts_with("HTTP/") &&
           status_line.find(" 401") != std::string_view::npos;
}

std::string http_get(const HttpConfig& cfg, std::string_view target) {
    const std::string raw = fetch_raw_response(cfg, target);

    if (is_http_401(raw))
        throw std::runtime_error(
            std::format("[{}] HTTP 401 – wrong username or password", cfg.host));

    const auto json_start = raw.find('{');
    if (json_start == std::string::npos)
        throw std::runtime_error(
            std::format("[{}] No JSON found in response", cfg.host));

    const auto json_end = raw.rfind('}');
    if (json_end == std::string::npos || json_end < json_start)
        throw std::runtime_error(
            std::format("[{}] Malformed JSON in response", cfg.host));

    return raw.substr(json_start, json_end - json_start + 1);
}

std::string http_get_raw(const HttpConfig& cfg, std::string_view target) {
    const std::string raw = fetch_raw_response(cfg, target);

    if (is_http_401(raw))
        throw std::runtime_error(
            std::format("[{}] HTTP 401 – wrong username or password", cfg.host));

    std::size_t sep_len = 0;
    const auto sep = header_end(raw, sep_len);
    return (sep != std::string::npos) ? raw.substr(sep + sep_len) : raw;
}

} // namespace myio
