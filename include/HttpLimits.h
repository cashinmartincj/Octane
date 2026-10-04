#pragma once

/**
 * @file HttpLimits.h
 * @brief Connection thresholds, request size bounds, and timeout configuration for Octane.
 *
 * @details
 * This header defines `octane::HttpLimits`, which configures connection admission thresholds,
 * request header and body size ceilings, idle/read deadlines, and graceful shutdown timeouts.
 *
 * @section usage Usage Example:
 * @code
 * #include "HttpLimits.h"
 *
 * octane::HttpLimits limits;
 * limits.max_header_bytes = 8 * 1024;             // 8 KiB headers
 * limits.max_body_bytes   = 10 * 1024 * 1024;      // 10 MiB body (e.g. for PDF uploads)
 * limits.max_connections  = 10000;                // Max concurrent client connections
 * limits.read_timeout     = std::chrono::seconds(15);
 * limits.write_timeout    = std::chrono::seconds(15);
 * limits.validate(); // Ensure limits are mathematically consistent
 * @endcode
 */

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace octane {

/**
 * @struct HttpLimits
 * @brief Per-connection request bounds, socket timeouts, and server admission policies.
 *
 * @details
 * Values in `HttpLimits` are copied when a server or connection is initialized.
 * These limits protect the server against buffer bloat, Slowloris attacks, and denial of service.
 * Note: These settings cap per-request ingress buffers; they do not restrict application heap
 * allocations or downstream database buffer limits.
 */
struct HttpLimits {
    /// Maximum allowable request-line and header size in bytes, including the final CRLF CRLF (default: 16 KiB).
    std::size_t max_header_bytes = 16 * 1024;

    /// Maximum declared Content-Length in bytes (default: 1 MiB). Requests exceeding this receive HTTP 413.
    std::size_t max_body_bytes = 1024 * 1024;

    /// Maximum number of simultaneously active TCP connections (default: 4096). Excess connections are closed immediately.
    std::size_t max_connections = 4096;

    /// Maximum number of requests allowed over a single persistent keep-alive connection before closing (default: 1000).
    std::size_t max_requests_per_connection = 1000;

    /// Absolute time window allowed to read the complete request line, headers, and body (default: 10 seconds).
    std::chrono::milliseconds read_timeout{10000};

    /// Maximum time allowed to complete an asynchronous socket response write (default: 10 seconds).
    std::chrono::milliseconds write_timeout{10000};

    /// Grace period granted during server shutdown before remaining in-flight connections are forcibly canceled (default: 5 seconds).
    std::chrono::milliseconds shutdown_timeout{5000};

    /**
     * @brief Validates bounds consistency, positive timeout durations, and overflow-safe arithmetic.
     * @throws std::invalid_argument If any timeout is non-positive, connection bounds are zero,
     * or size additions could trigger integer overflow.
     */
    void validate() const {
        if (max_header_bytes < 4 || max_body_bytes >
            std::numeric_limits<std::size_t>::max() - max_header_bytes ||
            !max_connections || !max_requests_per_connection ||
            read_timeout.count() <= 0 || write_timeout.count() <= 0 ||
            shutdown_timeout.count() <= 0)
            throw std::invalid_argument("Invalid HTTP limits configuration");
    }
};

} // namespace octane
