#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstring>
#include <optional>

namespace octane::websocket {

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA
};

inline bool is_control_opcode(Opcode op) noexcept {
    return static_cast<uint8_t>(op) >= 0x8;
}

enum class CloseCode : uint16_t {
    NormalClosure           = 1000,
    GoingAway               = 1001,
    ProtocolError           = 1002,
    UnsupportedData         = 1003,
    NoStatusReceived        = 1005,
    AbnormalClosure         = 1006,
    InvalidFramePayloadData = 1007,
    PolicyViolation         = 1008,
    MessageTooBig           = 1009,
    MandatoryExtension      = 1010,
    InternalServerError     = 1011,
    TlsHandshakeFailed      = 1015
};

struct FrameHeader {
    bool fin{true};
    bool rsv1{false};
    bool rsv2{false};
    bool rsv3{false};
    Opcode opcode{Opcode::Text};
    bool masked{false};
    uint64_t payload_length{0};
    std::array<uint8_t, 4> mask_key{0, 0, 0, 0};
    size_t header_size{0};
};

struct ParsedFrame {
    FrameHeader header;
    std::string_view payload;
};

/**
 * @brief Unmasks WebSocket payload in-place using fast 64-bit word XOR.
 */
inline void unmask_payload(char* data, size_t length, const std::array<uint8_t, 4>& mask_key, size_t initial_offset = 0) noexcept {
    if (length == 0) return;

    size_t offset = initial_offset % 4;
    size_t i = 0;

    // Align to 4-byte / mask boundary if offset != 0
    while (offset != 0 && i < length) {
        data[i] ^= mask_key[offset];
        offset = (offset + 1) % 4;
        ++i;
    }

    if (i >= length) return;

    // Fast 64-bit word unmasking
    uint32_t mask32 = (static_cast<uint32_t>(mask_key[0])) |
                      (static_cast<uint32_t>(mask_key[1]) << 8) |
                      (static_cast<uint32_t>(mask_key[2]) << 16) |
                      (static_cast<uint32_t>(mask_key[3]) << 24);
    uint64_t mask64 = (static_cast<uint64_t>(mask32) << 32) | static_cast<uint64_t>(mask32);

    size_t remaining = length - i;
    size_t chunks64 = remaining / 8;
    uint64_t* ptr64 = reinterpret_cast<uint64_t*>(data + i);

    for (size_t c = 0; c < chunks64; ++c) {
        ptr64[c] ^= mask64;
    }

    i += chunks64 * 8;
    offset = 0;
    while (i < length) {
        data[i] ^= mask_key[offset];
        offset = (offset + 1) % 4;
        ++i;
    }
}

/**
 * @brief Attempts to parse a WebSocket frame header from available bytes.
 * @return Parsed FrameHeader or nullopt if more bytes are needed.
 * @throws std::runtime_error on protocol violation.
 */
inline std::optional<FrameHeader> parse_frame_header(std::string_view buffer) {
    if (buffer.size() < 2) return std::nullopt;

    const uint8_t b0 = static_cast<uint8_t>(buffer[0]);
    const uint8_t b1 = static_cast<uint8_t>(buffer[1]);

    FrameHeader header;
    header.fin = (b0 & 0x80) != 0;
    header.rsv1 = (b0 & 0x40) != 0;
    header.rsv2 = (b0 & 0x20) != 0;
    header.rsv3 = (b0 & 0x10) != 0;
    header.opcode = static_cast<Opcode>(b0 & 0x0F);
    header.masked = (b1 & 0x80) != 0;

    uint8_t len_code = b1 & 0x7F;
    size_t pos = 2;

    if (len_code <= 125) {
        header.payload_length = len_code;
    } else if (len_code == 126) {
        if (buffer.size() < pos + 2) return std::nullopt;
        header.payload_length = (static_cast<uint8_t>(buffer[pos]) << 8) |
                                 static_cast<uint8_t>(buffer[pos + 1]);
        pos += 2;
    } else { // 127
        if (buffer.size() < pos + 8) return std::nullopt;
        uint64_t len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<uint8_t>(buffer[pos + i]);
        }
        header.payload_length = len;
        pos += 8;
    }

    if (header.masked) {
        if (buffer.size() < pos + 4) return std::nullopt;
        for (int i = 0; i < 4; ++i) {
            header.mask_key[i] = static_cast<uint8_t>(buffer[pos + i]);
        }
        pos += 4;
    }

    header.header_size = pos;
    return header;
}

/**
 * @brief Serializes a server-to-client WebSocket frame (server-to-client frames are unmasked).
 */
inline std::string serialize_frame(Opcode opcode, std::string_view payload, bool fin = true) {
    std::string out;
    size_t payload_len = payload.size();
    size_t header_len = 2 + (payload_len <= 125 ? 0 : (payload_len <= 0xFFFF ? 2 : 8));
    out.reserve(header_len + payload_len);

    uint8_t b0 = (fin ? 0x80 : 0x00) | (static_cast<uint8_t>(opcode) & 0x0F);
    out.push_back(static_cast<char>(b0));

    if (payload_len <= 125) {
        out.push_back(static_cast<char>(payload_len)); // Mask bit 0
    } else if (payload_len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload_len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((payload_len >> (i * 8)) & 0xFF));
        }
    }

    out.append(payload);
    return out;
}

/**
 * @brief Formats a standard Close frame with code and optional reason.
 */
inline std::string serialize_close(CloseCode code = CloseCode::NormalClosure, std::string_view reason = "") {
    std::string payload;
    uint16_t c = static_cast<uint16_t>(code);
    payload.push_back(static_cast<char>((c >> 8) & 0xFF));
    payload.push_back(static_cast<char>(c & 0xFF));
    payload.append(reason);
    return serialize_frame(Opcode::Close, payload, true);
}

} // namespace octane::websocket
