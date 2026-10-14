#include "async_http_client.hpp"
#include "bridge_rules.hpp"
#include <dsens_parser.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>

#include <boost/json.hpp>

#include <charconv>
#include <chrono>
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

namespace {
// The device answered the POST with a non-2xx (or unparsable) status: a
// definitive answer, unlike a transport error, so it is not retried.
struct HttpStatusError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
} // namespace

// Extracts JSON content from a raw HTTP response.
// Bypasses Beast's HTTP parser because the myIO server sends non-standard headers
// (uses bare \n instead of \r\n). Searches only in the body to avoid false
// matches on { or } characters that may appear in HTTP headers.
static std::string extract_json(const std::string& raw, std::string_view host) {
    // Find the end of the HTTP header block (\r\n\r\n or bare \n\n).
    std::size_t body_start = std::string::npos;
    if (const auto pos = raw.find("\r\n\r\n"); pos != std::string::npos)
        body_start = pos + 4;
    else if (const auto pos2 = raw.find("\n\n"); pos2 != std::string::npos)
        body_start = pos2 + 2;

    if (body_start == std::string::npos) {
        if (raw.find("401") != std::string::npos)
            throw std::runtime_error(
                std::format("[{}] HTTP 401 – wrong username or password", host));
        throw std::runtime_error(
            std::format("[{}] No HTTP header terminator found in response", host));
    }

    const auto start = raw.find('{', body_start);
    if (start == std::string::npos)
        throw std::runtime_error(
            std::format("[{}] No JSON found in response body", host));

    const auto end = raw.rfind('}');
    if (end == std::string::npos || end < start)
        throw std::runtime_error(
            std::format("[{}] Malformed JSON in response", host));

    return raw.substr(start, end - start + 1);
}

// ── Constructor ───────────────────────────────────────────────────────────────

AsyncHttpClient::AsyncHttpClient(HttpConfig cfg, const net::any_io_executor& ex)
    : cfg_(std::move(cfg))
    , gate_(ex, 1)
{}

// ── Device gate (async mutex) ─────────────────────────────────────────────────

namespace {
// Releases the gate when the owning coroutine frame is destroyed, so the lock
// is dropped on every path — normal return and exception alike.
struct GateUnlock {
    AsyncHttpClient::gate_t& ch;
    ~GateUnlock() { ch.try_receive([](boost::system::error_code) {}); }
};
} // namespace

net::awaitable<void> AsyncHttpClient::lock_gate() {
    co_await gate_.async_send(boost::system::error_code{}, net::use_awaitable);
}

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
    stream.expires_after(cfg_.timeout);
    co_await net::async_read(
        stream, net::dynamic_buffer(raw, 256 * 1024),
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

    // Read the myIO response; only its status line matters.
    std::string raw;
    beast::error_code ec;
    stream.expires_after(cfg_.timeout);
    co_await net::async_read(
        stream, net::dynamic_buffer(raw, 64 * 1024),
        net::redirect_error(net::use_awaitable, ec));

    if (ec && ec != net::error::eof && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    beast::error_code sec;
    stream.socket().shutdown(tcp::socket::shutdown_both, sec);

    // A rejected command (401, 5xx…) must not be reported as success, else the
    // caller publishes an optimistic state the device never took.
    // Only an explicit status line is judged: the POST response format is not
    // verified on the device (some myIO answers are HTTP/0.9 without a status
    // line), so an empty or status-less response keeps the old "accepted"
    // behaviour instead of failing every command / re-sending it.
    const auto status = bridge::parse_http_status(raw);
    if (!status) co_return;
    if (*status == 401)
        throw HttpStatusError(std::format(
            "[{}] POST: HTTP 401 – wrong username or password", cfg_.host));
    // 3xx is accepted: an embedded form handler may answer with a redirect.
    if (*status < 200 || *status > 399)
        throw HttpStatusError(std::format("[{}] POST rejected: HTTP {}", cfg_.host, *status));
}

// ── Public interface ──────────────────────────────────────────────────────────

net::awaitable<ServerInfo> AsyncHttpClient::async_get_info() {
    namespace json = boost::json;
    co_await lock_gate();
    GateUnlock unlock{gate_};
    const auto body = co_await do_get("/info.json");
    const auto  doc = json::parse(body);
    const auto& obj = doc.as_object().at("info").as_object();

    auto str_or = [&](std::string_view key) -> std::string {
        if (const auto* v = obj.if_contains(key); v && v->is_string())
            return std::string(v->as_string());
        return {};
    };
    auto int_or = [&](std::string_view key) -> int {
        const auto* v = obj.if_contains(key);
        if (!v) return 0;
        if (v->is_int64()) return static_cast<int>(v->as_int64());
        if (v->is_string()) {
            int n = 0;
            const auto s = v->as_string();
            std::from_chars(s.data(), s.data() + s.size(), n);
            return n;
        }
        return 0;
    };

    ServerInfo info;
    info.serial      = str_or("serial");
    info.version     = str_or("version");
    info.cpu         = str_or("CPU");
    info.api         = int_or("api");
    info.relay_count = int_or("RQ");
    info.pca_count   = int_or("PCAQ");
    co_return info;
}

net::awaitable<ServerState> AsyncHttpClient::async_poll() {
    co_await lock_gate();
    GateUnlock unlock{gate_};
    const auto json_body = co_await do_get("/sens_out.json");
    co_return DSensOutParser::parse(json_body, cfg_.server_id);
}

net::awaitable<void> AsyncHttpClient::async_send_command(std::string post_body) {
    // Retry up to 3 times to handle transient W5100 socket drops.
    // Delay uses an async timer so the coroutine yield doesn't block the io_context.
    constexpr int   max_attempts   = 3;
    constexpr auto  retry_delay    = std::chrono::milliseconds(500);

    // Hold the device gate across the retries too — a retry is still traffic
    // to the same single-connection device.
    co_await lock_gate();
    GateUnlock unlock{gate_};

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        bool retry = false;
        try {
            co_await do_post("/", post_body);
            co_return;
        } catch (const HttpStatusError&) {
            // The device answered and refused: retrying only adds W5100 traffic.
            throw;
        } catch (const std::exception&) {
            if (attempt == max_attempts) throw;
            retry = true;
        }
        // co_await must be outside the catch block (C++ coroutine restriction)
        if (retry) {
            net::steady_timer timer(co_await net::this_coro::executor);
            timer.expires_after(retry_delay);
            co_await timer.async_wait(net::use_awaitable);
        }
    }
}

} // namespace myio
