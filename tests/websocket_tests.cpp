#include "websocket/WebSocketFrame.h"
#include "websocket/WebSocketHandshake.h"
#include "websocket/WebSocketSession.h"
#include "utils/Sha1Base64.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace octane;
using namespace octane::websocket;

void test_sha1_and_base64() {
    // RFC 6455 test key
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string accept = compute_accept_key(key);
    assert(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    std::cout << "[PASS] SHA-1 & Base64 compute_accept_key\n";
}

void test_frame_serialization_and_parsing() {
    // 1. Short unmasked text frame
    std::string payload = "Hello, WebSocket!";
    std::string encoded = serialize_frame(Opcode::Text, payload, true);
    auto opt_hdr = parse_frame_header(encoded);
    assert(opt_hdr.has_value());
    assert(opt_hdr->fin == true);
    assert(opt_hdr->opcode == Opcode::Text);
    assert(opt_hdr->payload_length == payload.size());
    assert(!opt_hdr->masked);
    assert(encoded.substr(opt_hdr->header_size) == payload);

    // 2. Client-masked frame
    std::string masked_encoded = encoded;
    // Set mask bit in byte 1
    masked_encoded[1] = static_cast<char>(static_cast<uint8_t>(masked_encoded[1]) | 0x80);
    // Insert 4-byte mask key: 0x12, 0x34, 0x56, 0x78
    std::array<uint8_t, 4> mask = {0x12, 0x34, 0x56, 0x78};
    std::string mask_str(reinterpret_cast<const char*>(mask.data()), 4);
    masked_encoded.insert(2, mask_str);
    
    // Mask the payload bytes
    for (size_t i = 0; i < payload.size(); ++i) {
        masked_encoded[2 + 4 + i] ^= mask[i % 4];
    }

    auto masked_hdr = parse_frame_header(masked_encoded);
    assert(masked_hdr.has_value());
    assert(masked_hdr->masked == true);
    assert(masked_hdr->mask_key == mask);
    assert(masked_hdr->payload_length == payload.size());

    // Unmask payload
    std::string payload_copy = masked_encoded.substr(masked_hdr->header_size);
    unmask_payload(payload_copy.data(), payload_copy.size(), masked_hdr->mask_key);
    assert(payload_copy == payload);

    // 3. Close frame
    std::string close_frame = serialize_close(CloseCode::NormalClosure, "bye");
    auto close_hdr = parse_frame_header(close_frame);
    assert(close_hdr.has_value());
    assert(close_hdr->opcode == Opcode::Close);

    std::cout << "[PASS] Frame Serialization, Parsing & Unmasking\n";
}

void test_websocket_session() {
    bool opened = false;
    std::string received_msg;
    bool received_binary = false;
    bool closed = false;
    std::string written_data;

    WebSocketConfig config;
    config.on_open = [&](WebSocket&) { opened = true; };
    config.on_message = [&](WebSocket&, std::string_view msg, bool is_binary) {
        received_msg = std::string(msg);
        received_binary = is_binary;
    };
    config.on_close = [&](WebSocket&, uint16_t, std::string_view) { closed = true; };

    WebSocketSession session(
        config,
        [&](std::string frame) { written_data += frame; },
        [&](CloseCode, std::string_view) {}
    );

    session.on_open();
    assert(opened);

    // Feed a masked text frame
    std::string text = "ping-pong-data";
    std::string frame;
    frame.push_back(static_cast<char>(0x81)); // FIN + Text
    frame.push_back(static_cast<char>(0x80 | text.size())); // Masked + len
    std::array<uint8_t, 4> mask = {0xAA, 0xBB, 0xCC, 0xDD};
    frame.append(reinterpret_cast<const char*>(mask.data()), 4);
    for (size_t i = 0; i < text.size(); ++i) {
        frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    }

    session.feed(frame);
    assert(received_msg == text);
    assert(!received_binary);

    // Send text back
    session.send_text("reply");
    assert(!written_data.empty());
    auto resp_hdr = parse_frame_header(written_data);
    assert(resp_hdr.has_value());
    assert(resp_hdr->opcode == Opcode::Text);
    assert(written_data.substr(resp_hdr->header_size) == "reply");

    std::cout << "[PASS] WebSocketSession lifecycle & message exchange\n";
}

int main() {
    test_sha1_and_base64();
    test_frame_serialization_and_parsing();
    test_websocket_session();
    std::cout << "All WebSocket unit tests passed!\n";
    return 0;
}
