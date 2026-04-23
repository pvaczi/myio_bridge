#include "async_http_client.hpp"
#include <dsens_parser.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
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

// Extracts JSON content from a raw HTTP response.
// Bypasses Beast's HTTP parser because the myIO server sends non-standard headers.
static std::string extract_json(const std::string& raw, std::string_view host) {
    const auto start = raw.find('{');
    if (start == std::string::npos) {
        if (raw.find("401") != std::string::npos)
            throw std::runtime_error(
                std::format("[{}] HTTP 401 – wrong username or password", host));
        throw std::runtime_error(
            std::format("[{}] No JSON found in response", host));
    }
    const auto end = raw.rfind('}');
    if (end == std::string::npos || end < start)
        throw std::runtime_error(
            std::format("[{}] Malformed JSON in response", host));
    return raw.substr(start, end - start + 1);
}

// ── Constructor ───────────────────────────────────────────────────────────────

AsyncHttpClient::AsyncHttpClient(HttpConfig cfg) : cfg_(std::move(cfg)) {}

// ── Internal GET ──────────────────────────────────────────────────────────────

net::awaitable<std::string> AsyncHttpClient::do_get(std::string_view target) {
    auto ex = co_await net::this_coro::executor;
    beast::tcp_stream stream(ex);

    tcp::resolver resolver(ex);
    const auto endpoints = co_await resolver.async_resolve(
        cfg_.host, std::to_string(cfg_.port), net::use_awaitable);

    stream.expires_after(cfg_.timeout);
    co_await stream.async_connect(endpoints, net::use_awaitable);

    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host,          cfg_.host);
    req.set(http::field::user_agent,    "myio-bridge/1.0");
    req.set(http::field::connection,    "close");
    req.set(http::field::authorization,
            "Basic " + base64_encode(cfg_.user + ":" + cfg_.password));

    stream.expires_after(cfg_.timeout);
    co_await http::async_write(stream, req, net::use_awaitable);

    // Raw read – bypasses Beast HTTP parser
    std::string raw;
    beast::error_code ec;
    co_await net::async_read(
        stream, net::dynamic_buffer(raw),
        net::redirect_error(net::use_awaitable, ec));

    if (ec && ec != net::error::eof && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    beast::error_code sec;
    stream.socket().shutdown(tcp::socket::shutdown_both, sec);

    co_return extract_json(raw, cfg_.host);
}

// ── Internal POST ─────────────────────────────────────────────────────────────

net::awaitable<void> AsyncHttpClient::do_post(std::string_view target,
                                               std::string_view body) {
    auto ex = co_await net::this_coro::executor;
    beast::tcp_stream stream(ex);

    tcp::resolver resolver(ex);
    const auto endpoints = co_await resolver.async_resolve(
        cfg_.host, std::to_string(cfg_.port), net::use_awaitable);

    stream.expires_after(cfg_.timeout);
    co_await stream.async_connect(endpoints, net::use_awaitable);

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host,         cfg_.host);
    req.set(http::field::user_agent,   "myio-bridge/1.0");
    req.set(http::field::connection,   "close");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    req.set(http::field::authorization,
            "Basic " + base64_encode(cfg_.user + ":" + cfg_.password));
    req.body() = std::string(body);
    req.prepare_payload();

    stream.expires_after(cfg_.timeout);
    co_await http::async_write(stream, req, net::use_awaitable);

    // Read and discard the myIO response
    std::string raw;
    beast::error_code ec;
    co_await net::async_read(
        stream, net::dynamic_buffer(raw),
        net::redirect_error(net::use_awaitable, ec));

    beast::error_code sec;
    stream.socket().shutdown(tcp::socket::shutdown_both, sec);
}

// ── Public interface ──────────────────────────────────────────────────────────

net::awaitable<ServerState> AsyncHttpClient::async_poll() {
    const auto json_body = co_await do_get("/sens_out.json");
    co_return DSensOutParser::parse(json_body, cfg_.server_id);
}

net::awaitable<void> AsyncHttpClient::async_send_command(std::string post_body) {
    co_await do_post("/", post_body);
}

} // namespace myio
