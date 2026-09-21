#pragma once
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace octane {
/**
 * @brief Per-connection request bounds and server admission/shutdown policy.
 * Values are copied when a server or connection is constructed. These limits
 * do not cap application responses or total process memory.
 */
struct HttpLimits {
    /// Maximum request-line/header bytes, including final CRLF CRLF.
    std::size_t max_header_bytes = 16 * 1024;
    /// Maximum declared Content-Length; zero allows only empty bodies.
    std::size_t max_body_bytes = 1024 * 1024;
    /// Maximum active connections; additional accepted sockets are closed.
    std::size_t max_connections = 4096;
    /// Close after writing the response to this many requests.
    std::size_t max_requests_per_connection = 1000;
    /// Absolute deadline for headers and body together; also bounds idle time.
    std::chrono::milliseconds read_timeout{10000};
    /// Deadline for one complete asynchronous response write.
    std::chrono::milliseconds write_timeout{10000};
    /// Grace period before remaining connections are asked to cancel.
    std::chrono::milliseconds shutdown_timeout{5000};

    /**
     * @brief Check usable bounds, positive deadlines, and size-addition safety.
     * @throws std::invalid_argument If the configuration is invalid.
     */
    void validate() const {
        if (max_header_bytes < 4 || max_body_bytes >
            std::numeric_limits<std::size_t>::max() - max_header_bytes ||
            !max_connections || !max_requests_per_connection ||
            read_timeout.count() <= 0 || write_timeout.count() <= 0 ||
            shutdown_timeout.count() <= 0)
            throw std::invalid_argument("Invalid HTTP limits");
    }
};
}
