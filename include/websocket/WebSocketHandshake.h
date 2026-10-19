#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <algorithm>
#include "../HttpRequest.h"
#include "../HttpResponse.h"
#include "../HttpParser.h"
#include "../utils/Sha1Base64.h"
#include "WebSocket.h"

namespace octane::websocket {

inline constexpr std::string_view WS_MAGIC_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline bool valid_client_key(std::string_view key) noexcept {
    if (key.size() != 24 || key.substr(22) != "==") return false;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (size_t i = 0; i < 22; ++i) if (digit(key[i]) < 0) return false;
    return (digit(key[21]) & 15) == 0; // Canonical Base64 encoding of exactly 16 bytes.
}

/**
 * @brief Computes the Sec-WebSocket-Accept token for a given client key.
 */
inline std::string compute_accept_key(std::string_view client_key) {
    std::string combined;
    combined.reserve(client_key.size() + WS_MAGIC_GUID.size());
    combined.append(client_key);
    combined.append(WS_MAGIC_GUID);

    auto digest = utils::sha1(combined);
    return utils::base64_encode(digest.data(), digest.size());
}

/**
 * @brief Checks if an incoming HTTP request is a valid WebSocket upgrade request.
 */
inline bool is_upgrade_request(const HttpRequest& req) {
    if (req.method != HttpMethod::GET || req.http_version != "HTTP/1.1" ||
        req.header("Host").empty() || req.content_length != 0 || !req.keep_alive)
        return false;

    std::string_view upgrade = req.header("Upgrade");
    if (!HttpParser::contains_token(upgrade, "websocket")) return false;

    std::string_view connection = req.header("Connection");
    if (!HttpParser::contains_token(connection, "Upgrade")) return false;

    std::string_view version = req.header("Sec-WebSocket-Version");
    if (version != "13") return false;

    std::string_view key = req.header("Sec-WebSocket-Key");
    if (!valid_client_key(key)) return false;

    return true;
}

/**
 * @brief Prepares an HTTP 101 Switching Protocols response for a valid upgrade request.
 */
inline bool make_upgrade_response(const HttpRequest& req, HttpResponse& res, std::string_view subprotocol = {}) {
    if (!is_upgrade_request(req)) return false;

    if (!subprotocol.empty()) {
        bool offered = false;
        auto list = req.header("Sec-WebSocket-Protocol");
        while (!list.empty()) {
            auto end = list.find(',');
            auto item = list.substr(0, end);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
            if (item == subprotocol) offered = true;
            if (end == std::string_view::npos) break;
            list.remove_prefix(end + 1);
        }
        for (unsigned char c : subprotocol)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return false;
        if (!offered) return false;
    }

    std::string_view key = req.header("Sec-WebSocket-Key");
    std::string accept_val = compute_accept_key(key);

    res.status(101)
       .header("Upgrade", "websocket")
       .header("Connection", "Upgrade")
       .header("Sec-WebSocket-Accept", accept_val);

    if (!subprotocol.empty()) {
        res.header("Sec-WebSocket-Protocol", std::string(subprotocol));
    }
    return true;
}

// Runs before upgrade, while request views are valid. Browser origins fail
// closed unless explicitly allowed; authentication belongs in authorize.
inline int prepare_upgrade_response(const HttpRequest& req, HttpResponse& res,
                                    const WebSocketConfig& config) {
    if (!is_upgrade_request(req)) return 400;
    const auto origin = req.header("Origin");
    if (req.headers.contains(std::string_view{"Origin"})) {
        if (origin.empty() || std::find(config.allowed_origins.begin(), config.allowed_origins.end(), origin) == config.allowed_origins.end())
            return 403;
    } else if (config.require_origin) return 403;
    try {
        if (config.authorize && !config.authorize(req)) return 403;
    } catch (...) { return 500; }
    return make_upgrade_response(req, res, config.subprotocol) ? 101 : 400;
}

} // namespace octane::websocket
