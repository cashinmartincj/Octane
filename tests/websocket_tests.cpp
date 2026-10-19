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

std::string client_frame(Opcode opcode, std::string_view payload, bool fin = true) {
    auto frame = serialize_frame(opcode, payload, fin);
    const auto hdr = *parse_frame_header(frame);
    const std::array<uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};
    frame[1] = static_cast<char>(static_cast<uint8_t>(frame[1]) | 0x80);
    frame.insert(hdr.header_size, reinterpret_cast<const char*>(mask.data()), mask.size());
    unmask_payload(frame.data() + hdr.header_size + 4, payload.size(), mask);
    return frame;
}

void test_rejection(std::string_view frame, CloseCode expected, size_t limit = 1024) {
    int messages = 0, notifications = 0;
    uint16_t close_code = 0;
    std::string output;
    WebSocketConfig config;
    config.max_payload_bytes = limit;
    config.on_message = [&](auto&, auto, bool) { ++messages; };
    config.on_close = [&](auto&, uint16_t code, auto) { close_code = code; ++notifications; };
    WebSocketSession session(config, [&](std::string data) { output += data; }, [](auto, auto) {});
    session.feed(frame);
    assert(session.should_close_transport());
    assert(close_code == static_cast<uint16_t>(expected));
    assert(messages == 0 && notifications == 1);
    const auto hdr = *parse_frame_header(output);
    assert(hdr.opcode == Opcode::Close && hdr.payload_length <= 125);
    assert(!hdr.masked);
    session.feed(client_frame(Opcode::Text, "ignored"));
    session.transport_closed();
    assert(notifications == 1 && messages == 0);
}

void test_invalid_frames() {
    test_rejection(serialize_frame(Opcode::Text, "unmasked"), CloseCode::ProtocolError);
    for (auto flags : {0x40, 0x20, 0x10}) {
        auto frame = client_frame(Opcode::Text, "x");
        frame[0] = static_cast<char>(static_cast<uint8_t>(frame[0]) | flags);
        test_rejection(frame, CloseCode::ProtocolError);
    }
    for (auto opcode : {3, 4, 5, 6, 7, 11, 12, 13, 14, 15}) {
        auto frame = client_frame(Opcode::Text, "x");
        frame[0] = static_cast<char>(0x80 | opcode);
        test_rejection(frame, CloseCode::ProtocolError);
    }
    auto ping = client_frame(Opcode::Ping, "x");
    ping[0] = 0x09;
    test_rejection(ping, CloseCode::ProtocolError);
    test_rejection(std::string("\x89\xFE", 2), CloseCode::ProtocolError);
    test_rejection(std::string("\x81\xFE\x00\x01", 4), CloseCode::ProtocolError);
    test_rejection(std::string("\x81\xFF\x80\x00\x00\x00\x00\x01\x00\x00", 10), CloseCode::ProtocolError);
    test_rejection(std::string("\x81\xFF\x00\x00\x00\x00\x00\x00\x00\x7F", 10), CloseCode::ProtocolError);
    // Header alone advertises 65536 bytes. Reject before accepting any payload.
    test_rejection(std::string("\x81\xFF\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00", 14), CloseCode::MessageTooBig);
    test_rejection(client_frame(Opcode::Continuation, "x"), CloseCode::ProtocolError);
    test_rejection(client_frame(Opcode::Text, "a", false) + client_frame(Opcode::Text, "b"), CloseCode::ProtocolError);
    test_rejection(client_frame(Opcode::Text, "abc", false) + client_frame(Opcode::Continuation, "de"), CloseCode::MessageTooBig, 4);
    test_rejection(client_frame(Opcode::Close, "x"), CloseCode::ProtocolError);
    for (uint16_t code : {999, 1004, 1005, 1006, 1015, 2000, 5000}) {
        std::string payload{static_cast<char>(code >> 8), static_cast<char>(code)};
        test_rejection(client_frame(Opcode::Close, payload), CloseCode::ProtocolError);
    }
    for (std::string text : {std::string("\xC0\xAF", 2), std::string("\xED\xA0\x80", 3),
                            std::string("\xF4\x90\x80\x80", 4), std::string("\xE2\x82", 2), std::string("\x80", 1)}) {
        test_rejection(client_frame(Opcode::Text, text), CloseCode::InvalidFramePayloadData);
        test_rejection(client_frame(Opcode::Close, std::string("\x03\xE8", 2) + text), CloseCode::InvalidFramePayloadData);
    }
    std::cout << "[PASS] Invalid flags, lengths, masking, fragments, UTF-8, and Close payloads\n";
}

void test_fragmentation_and_control() {
    WebSocketConfig config;
    std::string received, output;
    int messages = 0;
    config.on_message = [&](auto&, auto data, bool binary) { received = data; ++messages; assert(!binary); };
    WebSocketSession session(config, [&](std::string data) { output += data; }, [](auto, auto) {});
    // A Unicode character split between fragments, with interleaved Ping.
    auto frames = client_frame(Opcode::Text, std::string("\xE2", 1), false) +
                  client_frame(Opcode::Ping, "ping") +
                  client_frame(Opcode::Continuation, std::string("\x82\xAC", 2));
    for (char byte : frames) session.feed(std::string_view(&byte, 1));
    assert(messages == 1 && received == std::string("\xE2\x82\xAC", 3));
    assert(output == serialize_frame(Opcode::Pong, "ping"));
    assert(!session.has_pending_input());
    std::cout << "[PASS] Byte-at-a-time input, Unicode fragmentation, and interleaved control frames\n";
}

void test_close_and_callbacks() {
    WebSocketConfig config;
    int notifications = 0;
    std::string output;
    config.on_close = [&](auto&, uint16_t code, auto) { ++notifications; assert(code == 1000); throw 42; };
    WebSocketSession session(config, [&](std::string data) { output += data; }, [](auto, auto) {});
    session.close(CloseCode::NormalClosure, "bye");
    assert(!session.is_open() && session.is_readable() && notifications == 0);
    session.feed(client_frame(Opcode::Close, std::string("\x03\xE8", 2)));
    session.transport_closed();
    assert(notifications == 1 && session.should_close_transport());
    assert(output == serialize_close(CloseCode::NormalClosure, "bye"));

    output.clear();
    config.on_close = [&](auto&, uint16_t code, auto) { assert(code == 1005); };
    WebSocketSession empty(config, [&](std::string data) { output += data; }, [](auto, auto) {});
    empty.feed(client_frame(Opcode::Close, {}));
    assert(output == serialize_frame(Opcode::Close, {}));

    for (int event = 0; event < 4; ++event) {
        config = {};
        uint16_t code = 0;
        config.on_error = [](auto&, auto) { throw 42; };
        config.on_close = [&](auto&, uint16_t value, auto) { code = value; };
        if (event == 0) config.on_open = [](auto&) { throw 42; };
        if (event == 1) config.on_message = [](auto&, auto, bool) { throw 42; };
        if (event == 2) config.on_ping = [](auto&, auto) { throw 42; };
        if (event == 3) config.on_pong = [](auto&, auto) { throw 42; };
        WebSocketSession failing(config, [](auto) {}, [](auto, auto) {});
        failing.on_open();
        if (event) failing.feed(client_frame(event == 1 ? Opcode::Text : event == 2 ? Opcode::Ping : Opcode::Pong, "x"));
        assert(code == 1011 && failing.should_close_transport());
    }
    std::cout << "[PASS] Close handshake, empty Close, and throwing callbacks\n";
}

void test_handshake_validation() {
    const std::string base = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    auto raw = base + "\r\n";
    auto req = HttpParser::parse_headers(raw);
    HttpResponse response;
    WebSocketConfig config;
    assert(prepare_upgrade_response(req, response, config) == 101);
    for (auto key : {"", "abc", "dGhlIHNhbXBsZSBub25jZR==", "dGhlIHNhbXBsZSBub25jZQ=", "dGhlIHNhbXBsZSBub25jZ!=="}) assert(!valid_client_key(key));
    req.http_version = "HTTP/1.0";
    assert(!is_upgrade_request(req));
    req.http_version = "HTTP/1.1";
    req.content_length = 1;
    assert(!is_upgrade_request(req));
    req.content_length = 0;
    req.keep_alive = false;
    assert(!is_upgrade_request(req));
    req.keep_alive = true;
    config.require_origin = true;
    assert(prepare_upgrade_response(req, response, config) == 403);
    raw = base + "Origin: https://app.example\r\n\r\n";
    req = HttpParser::parse_headers(raw);
    assert(prepare_upgrade_response(req, response, config) == 403);
    config.allowed_origins = {"https://app.example"};
    assert(prepare_upgrade_response(req, response, config) == 101);
    config.authorize = [](auto&) { return false; };
    assert(prepare_upgrade_response(req, response, config) == 403);
    config.authorize = [](auto&) -> bool { throw 42; };
    assert(prepare_upgrade_response(req, response, config) == 500);
    for (auto header : {"Sec-WebSocket-Key: abc", "Sec-WebSocket-Version: 12", "Origin: https://evil.example", "Connection: close", "Upgrade: bad"}) {
        raw = base + "Origin: https://app.example\r\n" + header + "\r\n\r\n";
        bool rejected = false;
        try { HttpParser::parse_headers(raw); } catch (const HttpParseError& error) { rejected = error.status == 400; }
        assert(rejected);
    }
    raw = base + "Sec-WebSocket-Protocol: chat, other\r\n\r\n";
    req = HttpParser::parse_headers(raw);
    assert(make_upgrade_response(req, response, "chat"));
    assert(!make_upgrade_response(req, response, "unoffered"));
    config.authorize = {};
    config.require_origin = false;
    config.subprotocol = "chat";
    assert(prepare_upgrade_response(req, response, config) == 101);
    assert(response.headers.at("Sec-WebSocket-Protocol") == "chat");
    config.subprotocol = "unoffered";
    assert(prepare_upgrade_response(req, response, config) == 400);
    assert(!make_upgrade_response(req, response, "chat\r\nInjected: x"));
    std::cout << "[PASS] Handshake, origin, authorization, duplicate headers, and subprotocol validation\n";
}

void test_outbound_validation_and_unmasking() {
    WebSocketConfig config;
    config.max_payload_bytes = 4;
    int writes = 0;
    WebSocketSession session(config, [&](auto) { ++writes; }, [](auto, auto) {});
    auto rejects = [](auto fn) { bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; } assert(rejected); };
    rejects([&] { session.send_ping(std::string(126, 'x')); });
    rejects([&] { session.send_pong(std::string(126, 'x')); });
    rejects([&] { session.send_text(std::string("\x80", 1)); });
    rejects([&] { session.send_binary("12345"); });
    rejects([&] { session.close(CloseCode::NoStatusReceived); });
    rejects([&] { session.close(CloseCode::NormalClosure, std::string(124, 'x')); });
    assert(writes == 0 && session.is_open());
    const std::array<uint8_t, 4> mask{1, 2, 3, 4};
    for (size_t offset = 0; offset < 8; ++offset) {
        std::string data(32, 'x'), expected = data;
        for (size_t i = 0; i < 17; ++i) expected[1+i] ^= mask[(offset+i)%4];
        unmask_payload(data.data()+1, 17, mask, offset);
        assert(data == expected);
    }
    std::cout << "[PASS] Outbound limits and unaligned unmasking\n";
}

int main() {
    test_sha1_and_base64();
    test_frame_serialization_and_parsing();
    test_websocket_session();
    test_invalid_frames();
    test_fragmentation_and_control();
    test_close_and_callbacks();
    test_handshake_validation();
    test_outbound_validation_and_unmasking();
    std::cout << "All WebSocket unit tests passed!\n";
    return 0;
}
