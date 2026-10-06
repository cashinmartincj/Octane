#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <array>

namespace octane::utils {

namespace detail {

inline uint32_t rol(uint32_t value, size_t bits) noexcept {
    return (value << bits) | (value >> (32 - bits));
}

class Sha1Context {
public:
    Sha1Context() {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
        count_ = 0;
    }

    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buffer_[count_ & 63] = data[i];
            ++count_;
            if ((count_ & 63) == 0) {
                transform(buffer_);
            }
        }
    }

    void final(std::array<uint8_t, 20>& digest) {
        uint64_t total_bits = count_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while ((count_ & 63) != 56) {
            update(&zero, 1);
        }
        uint8_t len_bytes[8];
        for (int i = 7; i >= 0; --i) {
            len_bytes[7 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
        }
        update(len_bytes, 8);

        for (size_t i = 0; i < 5; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
        }
    }

private:
    void transform(const uint8_t buffer[64]) {
        uint32_t w[80];
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24) |
                   (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(buffer[i * 4 + 3]));
        }
        for (size_t i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];

        for (size_t i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    uint32_t state_[5];
    uint64_t count_{0};
    uint8_t buffer_[64]{};
};

} // namespace detail

/**
 * @brief Computes SHA-1 hash of input bytes.
 * @return 20-byte raw SHA-1 digest.
 */
inline std::array<uint8_t, 20> sha1(std::string_view input) noexcept {
    detail::Sha1Context ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::array<uint8_t, 20> digest{};
    ctx.final(digest);
    return digest;
}

/**
 * @brief Base64 encodes an arbitrary binary buffer.
 */
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = (rem > 1) ? data[i++] : 0;
        uint32_t octet_c = (rem > 2) ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(tbl[(triple >> 18) & 0x3f]);
        out.push_back(tbl[(triple >> 12) & 0x3f]);
        out.push_back(rem > 1 ? tbl[(triple >> 6) & 0x3f] : '=');
        out.push_back(rem > 2 ? tbl[triple & 0x3f] : '=');
    }
    return out;
}

inline std::string base64_encode(std::string_view sv) {
    return base64_encode(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

} // namespace octane::utils
