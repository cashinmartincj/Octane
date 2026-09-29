#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace octane::validation {

struct TextRules {
    std::size_t min_bytes{0};
    std::size_t max_bytes{4096};
    bool allow_newlines{false};
    bool allow_tabs{false};
    bool allow_leading_or_trailing_space{false};
};

// These functions validate input. They do not escape or modify it. SQL values
// must still be passed to libpqxx as bound parameters.
[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_text(std::string_view value,
                                 TextRules rules = {}) noexcept;
[[nodiscard]] bool is_valid_email(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_username(std::string_view value,
                                     std::size_t min_bytes = 1,
                                     std::size_t max_bytes = 150) noexcept;
[[nodiscard]] bool is_valid_password_input(std::string_view value,
                                           std::size_t min_bytes = 8,
                                           std::size_t max_bytes = 1024) noexcept;
[[nodiscard]] bool is_valid_slug(std::string_view value,
                                 std::size_t max_bytes = 100) noexcept;
[[nodiscard]] bool is_valid_uuid(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_iso_date(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_decimal(std::string_view value,
                                    std::size_t max_integer_digits,
                                    std::size_t max_fraction_digits,
                                    bool allow_negative = false) noexcept;
[[nodiscard]] bool is_valid_hex_colour(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_phone(std::string_view value,
                                  std::size_t max_bytes = 32) noexcept;
[[nodiscard]] bool is_safe_filename(std::string_view value,
                                    std::size_t max_bytes = 255) noexcept;
[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::uint64_t> parse_uint64(std::string_view value) noexcept;

} // namespace octane::validation
