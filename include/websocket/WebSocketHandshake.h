#pragma once

#include <string>
#include <string_view>
#include <optional>
#include "../HttpRequest.h"
#include "../HttpResponse.h"
#include "../HttpParser.h"
#include "../utils/Sha1Base64.h"

namespace octane::websocket {

inline constexpr std::string_view WS_MAGIC_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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
    if (req.method != HttpMethod::GET) return false;

    std::string_view upgrade = req.header("Upgrade");
    if (!HttpParser::contains_token(upgrade, "websocket")) return false;

    std::string_view connection = req.header("Connection");
    if (!HttpParser::contains_token(connection, "Upgrade")) return false;

    std::string_view version = req.header("Sec-WebSocket-Version");
    if (version != "13") return false;

    std::string_view key = req.header("Sec-WebSocket-Key");
    if (key.empty()) return false;

    return true;
}

/**
 * @brief Prepares an HTTP 101 Switching Protocols response for a valid upgrade request.
 */
inline bool make_upgrade_response(const HttpRequest& req, HttpResponse& res, std::string_view subprotocol = {}) {
    if (!is_upgrade_request(req)) return false;

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

} // namespace octane::websocket
