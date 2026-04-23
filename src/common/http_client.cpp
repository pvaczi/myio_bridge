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

std::string http_get(const HttpConfig& cfg, std::string_view target) {
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
    std::string raw;
    beast::error_code ec;
    net::read(stream, net::dynamic_buffer(raw), ec);

    if (ec && ec != net::error::eof && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    // Extract JSON from the raw response
    const auto json_start = raw.find('{');
    if (json_start == std::string::npos) {
        if (raw.find("401") != std::string::npos)
            throw std::runtime_error(
                std::format("[{}] HTTP 401 – wrong username or password", cfg.host));
        throw std::runtime_error(
            std::format("[{}] No JSON found in response", cfg.host));
    }

    const auto json_end = raw.rfind('}');
    if (json_end == std::string::npos || json_end < json_start)
        throw std::runtime_error(
            std::format("[{}] Malformed JSON in response", cfg.host));

    beast::error_code shutdown_ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);

    return raw.substr(json_start, json_end - json_start + 1);
}

} // namespace myio
