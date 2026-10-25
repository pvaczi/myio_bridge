#include "sd_notify.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sdnotify {

std::string socket_address(const char* env_value) {
    if (!env_value) return {};
    std::string path = env_value;
    // Only absolute paths and the abstract form are valid (same as libsystemd).
    if (path.size() < 2 || (path[0] != '/' && path[0] != '@')) return {};
    if (path[0] == '@') path[0] = '\0';
    return path;
}

std::optional<std::chrono::microseconds>
watchdog_period(const char* usec_env, const char* pid_env, long self_pid) {
    if (!usec_env || !*usec_env) return std::nullopt;
    long long usec = 0;
    const std::string_view u = usec_env;
    const auto [p, ec] = std::from_chars(u.data(), u.data() + u.size(), usec);
    if (ec != std::errc{} || p != u.data() + u.size() || usec <= 0) return std::nullopt;

    if (pid_env && *pid_env) {
        long pid = 0;
        const std::string_view s = pid_env;
        const auto [pp, pec] = std::from_chars(s.data(), s.data() + s.size(), pid);
        if (pec != std::errc{} || pp != s.data() + s.size() || pid != self_pid)
            return std::nullopt;
    }
    return std::chrono::microseconds{usec / 2 > 0 ? usec / 2 : 1};
}

std::optional<std::chrono::microseconds> watchdog_period_from_env() {
#ifdef _WIN32
    return std::nullopt;
#else
    return watchdog_period(std::getenv("WATCHDOG_USEC"), std::getenv("WATCHDOG_PID"),
                           static_cast<long>(::getpid()));
#endif
}

void notify(std::string_view state) {
#ifndef _WIN32
    try {
        const std::string path = socket_address(std::getenv("NOTIFY_SOCKET"));
        if (path.empty()) return;

        sockaddr_un addr{};
        if (path.size() > sizeof(addr.sun_path)) return;
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.data(), path.size());
        // No trailing NUL: the address length is exact (required for the abstract form).
        const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());

        const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        // MSG_DONTWAIT: a full systemd socket buffer must never stall the event loop.
        (void)::sendto(fd, state.data(), state.size(), MSG_DONTWAIT,
                       reinterpret_cast<const sockaddr*>(&addr), len);
        ::close(fd);
    } catch (...) {}
#else
    (void)state;
#endif
}

} // namespace sdnotify
