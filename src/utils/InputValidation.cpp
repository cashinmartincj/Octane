#include "utils/InputValidation.h"

#include <charconv>
#include <limits>

namespace octane::validation {
namespace {

constexpr bool ascii_alpha(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool ascii_digit(unsigned char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr bool ascii_alnum(unsigned char c) noexcept {
    return ascii_alpha(c) || ascii_digit(c);
}

constexpr bool hex_digit(unsigned char c) noexcept {
    return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool valid_domain(std::string_view domain) noexcept {
    if (domain.empty() || domain.size() > 253 || domain.front() == '.' || domain.back() == '.') {
        return false;
    }

    std::size_t label_length = 0;
    for (std::size_t i = 0; i < domain.size(); ++i) {
        const auto c = static_cast<unsigned char>(domain[i]);
        if (c == '.') {
            if (label_length == 0 || label_length > 63 || domain[i - 1] == '-') return false;
            label_length = 0;
            continue;
        }
        if (!ascii_alnum(c) && c != '-') return false;
        if (label_length == 0 && c == '-') return false;
        ++label_length;
    }
    return label_length > 0 && label_length <= 63 && domain.back() != '-';
}

constexpr bool leap_year(unsigned year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

} // namespace

bool is_valid_utf8(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char lead = bytes[i++];
        if (lead <= 0x7f) continue;

        std::uint32_t codepoint = 0;
        std::size_t continuation_count = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            codepoint = lead & 0x1f;
            continuation_count = 1;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            codepoint = lead & 0x0f;
            continuation_count = 2;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            codepoint = lead & 0x07;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (continuation_count > value.size() - i) return false;
        for (std::size_t n = 0; n < continuation_count; ++n) {
            const unsigned char c = bytes[i++];
            if ((c & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (c & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool is_valid_text(std::string_view value, TextRules rules) noexcept {
    if (rules.min_bytes > rules.max_bytes || value.size() < rules.min_bytes ||
        value.size() > rules.max_bytes || !is_valid_utf8(value)) {
        return false;
    }
    if (!rules.allow_leading_or_trailing_space && !value.empty() &&
        (value.front() == ' ' || value.back() == ' ')) {
        return false;
    }
    for (const unsigned char c : value) {
        if (c == 0x7f) return false;
        if (c < 0x20) {
            if (c == '\n' && rules.allow_newlines) continue;
            if (c == '\r' && rules.allow_newlines) continue;
            if (c == '\t' && rules.allow_tabs) continue;
            return false;
        }
    }
    return true;
}

bool is_valid_email(std::string_view value) noexcept {
    if (value.empty() || value.size() > 254) return false;
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at > 64 ||
        at != value.rfind('@') || at + 1 >= value.size()) {
        return false;
    }
    const auto local = value.substr(0, at);
    if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string_view::npos) {
        return false;
    }
    for (const unsigned char c : local) {
        if (ascii_alnum(c)) continue;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'':
            case '*': case '+': case '-': case '/': case '=': case '?':
            case '^': case '_': case '`': case '{': case '|': case '}':
            case '~': case '.':
                continue;
            default:
                return false;
        }
    }
    return valid_domain(value.substr(at + 1));
}

bool is_valid_username(std::string_view value, std::size_t min_bytes,
                       std::size_t max_bytes) noexcept {
    if (min_bytes > max_bytes || value.size() < min_bytes || value.size() > max_bytes) return false;
    for (const unsigned char c : value) {
        if (!ascii_alnum(c) && c != '_' && c != '.' && c != '@' && c != '+' && c != '-') {
            return false;
        }
    }
    return true;
}

bool is_valid_password_input(std::string_view value, std::size_t min_bytes,
                             std::size_t max_bytes) noexcept {
    if (min_bytes > max_bytes || value.size() < min_bytes || value.size() > max_bytes ||
        !is_valid_utf8(value)) {
        return false;
    }
    for (const unsigned char c : value) {
        if (c == 0 || c == '\r' || c == '\n' || c == 0x7f) return false;
    }
    return true;
}

bool is_valid_slug(std::string_view value, std::size_t max_bytes) noexcept {
    if (value.empty() || value.size() > max_bytes || !ascii_alnum(value.front()) ||
        !ascii_alnum(value.back())) {
        return false;
    }
    for (const unsigned char c : value) {
        if (!ascii_alnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

bool is_valid_uuid(std::string_view value) noexcept {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!hex_digit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

bool is_valid_iso_date(std::string_view value) noexcept {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != 4 && i != 7 && !ascii_digit(static_cast<unsigned char>(value[i]))) return false;
    }
    const unsigned year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                          (value[2] - '0') * 10 + (value[3] - '0');
    const unsigned month = (value[5] - '0') * 10 + (value[6] - '0');
    const unsigned day = (value[8] - '0') * 10 + (value[9] - '0');
    if (year == 0 || month == 0 || month > 12 || day == 0) return false;
    static constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned limit = month == 2 && leap_year(year) ? 29 : days[month - 1];
    return day <= limit;
}

bool is_valid_decimal(std::string_view value, std::size_t max_integer_digits,
                      std::size_t max_fraction_digits, bool allow_negative) noexcept {
    if (value.empty() || max_integer_digits == 0) return false;
    std::size_t i = 0;
    if (value.front() == '-') {
        if (!allow_negative || value.size() == 1) return false;
        i = 1;
    } else if (value.front() == '+') {
        return false;
    }
    std::size_t integer_digits = 0;
    std::size_t fraction_digits = 0;
    bool dot = false;
    for (; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == '.' && !dot && max_fraction_digits > 0) {
            dot = true;
            continue;
        }
        if (!ascii_digit(c)) return false;
        if (dot) ++fraction_digits;
        else ++integer_digits;
    }
    return integer_digits > 0 && integer_digits <= max_integer_digits &&
           (!dot || fraction_digits > 0) && fraction_digits <= max_fraction_digits;
}

bool is_valid_hex_colour(std::string_view value) noexcept {
    if (value.size() != 7 || value.front() != '#') return false;
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!hex_digit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool is_valid_phone(std::string_view value, std::size_t max_bytes) noexcept {
    if (value.empty() || value.size() > max_bytes) return false;
    std::size_t digits = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = value[i];
        if (ascii_digit(c)) {
            ++digits;
        } else if (c == '+' && i == 0) {
            continue;
        } else if (c != ' ' && c != '-' && c != '(' && c != ')') {
            return false;
        }
    }
    return digits >= 5 && digits <= 15;
}

// Validates that filenames cannot cause path traversal or inject invalid UTF-8.
// Rejects relative segments (..), path separators (/ and \), colons, and control characters.
bool is_safe_filename(std::string_view value, std::size_t max_bytes) noexcept {
    if (value.empty() || value.size() > max_bytes || value == "." || value == ".." ||
        !is_valid_utf8(value)) {
        return false;
    }
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':') return false;
    }
    return value.find("..") == std::string_view::npos;
}

// Validates 24-hour time of day in either "HH:MM" (5 bytes) or "HH:MM:SS" (8 bytes).
// Used for gym working hours (GymWorkingHours) and booking slot times (GymScheduleSlot).
bool is_valid_time_of_day(std::string_view value) noexcept {
    if (value.size() != 5 && value.size() != 8) return false;
    if (value[2] != ':') return false;
    if (!ascii_digit(static_cast<unsigned char>(value[0])) ||
        !ascii_digit(static_cast<unsigned char>(value[1])) ||
        !ascii_digit(static_cast<unsigned char>(value[3])) ||
        !ascii_digit(static_cast<unsigned char>(value[4]))) {
        return false;
    }
    const unsigned hour = (value[0] - '0') * 10 + (value[1] - '0');
    const unsigned minute = (value[3] - '0') * 10 + (value[4] - '0');
    if (hour > 23 || minute > 59) return false;

    if (value.size() == 8) {
        if (value[5] != ':') return false;
        if (!ascii_digit(static_cast<unsigned char>(value[6])) ||
            !ascii_digit(static_cast<unsigned char>(value[7]))) {
            return false;
        }
        const unsigned second = (value[6] - '0') * 10 + (value[7] - '0');
        if (second > 59) return false;
    }
    return true;
}

// Validates ISO 8601 UTC timestamps (D-044).
// Accepts "YYYY-MM-DDTHH:MM:SSZ", with optional subseconds ".fff" and timezone offsets ("+02:00").
// Date portion is strictly checked against real calendar bounds including leap years.
bool is_valid_iso_timestamp(std::string_view value) noexcept {
    if (value.size() < 20 || !is_valid_iso_date(value.substr(0, 10))) return false;
    if (value[10] != 'T' && value[10] != 't') return false;
    if (value[13] != ':' || value[16] != ':') return false;
    for (std::size_t i : {11, 12, 14, 15, 17, 18}) {
        if (!ascii_digit(static_cast<unsigned char>(value[i]))) return false;
    }
    const unsigned hour = (value[11] - '0') * 10 + (value[12] - '0');
    const unsigned minute = (value[14] - '0') * 10 + (value[15] - '0');
    const unsigned second = (value[17] - '0') * 10 + (value[18] - '0');
    if (hour > 23 || minute > 59 || second > 59) return false;

    std::size_t idx = 19;
    // Optional fraction (.fff)
    if (idx < value.size() && value[idx] == '.') {
        ++idx;
        const std::size_t frac_start = idx;
        while (idx < value.size() && ascii_digit(static_cast<unsigned char>(value[idx]))) {
            ++idx;
        }
        if (idx == frac_start) return false;
    }

    if (idx >= value.size()) return false;

    // UTC "Z" or timezone offset "[+-]HH:MM"
    if (value[idx] == 'Z' || value[idx] == 'z') {
        return idx + 1 == value.size();
    }
    if (value[idx] == '+' || value[idx] == '-') {
        if (idx + 6 != value.size() || value[idx + 3] != ':') return false;
        for (std::size_t i : {idx + 1, idx + 2, idx + 4, idx + 5}) {
            if (!ascii_digit(static_cast<unsigned char>(value[i]))) return false;
        }
        const unsigned tz_hour = (value[idx + 1] - '0') * 10 + (value[idx + 2] - '0');
        const unsigned tz_min = (value[idx + 4] - '0') * 10 + (value[idx + 5] - '0');
        return tz_hour <= 23 && tz_min <= 59;
    }
    return false;
}

// Validates Italian Codice Fiscale (16 characters: 6 alpha, 2 digits, 1 alpha, 2 digits, 1 alpha, 3 alnum, 1 alpha).
// Used for customer profiles, gym memberships, and FattureInCloud synchronization.
bool is_valid_codice_fiscale(std::string_view value) noexcept {
    if (value.size() != 16) return false;
    for (std::size_t i = 0; i < 6; ++i) {
        if (!ascii_alpha(static_cast<unsigned char>(value[i]))) return false;
    }
    if (!ascii_digit(static_cast<unsigned char>(value[6])) ||
        !ascii_digit(static_cast<unsigned char>(value[7]))) {
        return false;
    }
    if (!ascii_alpha(static_cast<unsigned char>(value[8]))) return false;
    if (!ascii_digit(static_cast<unsigned char>(value[9])) ||
        !ascii_digit(static_cast<unsigned char>(value[10]))) {
        return false;
    }
    if (!ascii_alpha(static_cast<unsigned char>(value[11]))) return false;
    for (std::size_t i = 12; i < 15; ++i) {
        if (!ascii_alnum(static_cast<unsigned char>(value[i]))) return false;
    }
    return ascii_alpha(static_cast<unsigned char>(value[15]));
}

// Validates Italian Partita IVA (11 numeric digits).
// Used for gym corporate settings (GymCompanyInfo) and invoice billing data.
bool is_valid_partita_iva(std::string_view value) noexcept {
    if (value.size() != 11) return false;
    for (const unsigned char c : value) {
        if (!ascii_digit(c)) return false;
    }
    return true;
}

// Sanity checks uploaded medical certificate PDF buffers for header integrity and active exploits (D-020, D-063).
// Rejects any file missing the "%PDF-" magic bytes or containing active execution dictionary tokens.
bool is_safe_pdf_header_and_content(std::string_view bytes, std::size_t max_bytes) noexcept {
    if (bytes.size() < 5 || bytes.size() > max_bytes) return false;
    if (bytes.substr(0, 5) != "%PDF-") return false;

    // Scan for dangerous executable or active PDF action tokens
    static constexpr std::string_view dangerous_tokens[] = {
        "/JavaScript",
        "/JS",
        "/Launch",
        "/EmbeddedFiles",
        "/RichMedia"
    };

    for (const auto token : dangerous_tokens) {
        if (bytes.find(token) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

// Prevents CSV / Spreadsheet Formula Injection (CWE-1236, D-028).
// Prohibits strings starting with '=', '+', '-', '@', '\t', or '\r' from executing formulas in Excel/LibreOffice.
bool is_safe_spreadsheet_text(std::string_view value) noexcept {
    if (value.empty()) return true;
    const unsigned char first = static_cast<unsigned char>(value.front());
    if (first == '=' || first == '+' || first == '-' || first == '@' ||
        first == '\t' || first == '\r') {
        return false;
    }
    return value.find('\0') == std::string_view::npos;
}

std::optional<std::int64_t> parse_int64(std::string_view value) noexcept {
    if (value.empty() || value.front() == '+' || value.front() == ' ') return std::nullopt;
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::optional<std::uint64_t> parse_uint64(std::string_view value) noexcept {
    if (value.empty() || value.front() == '+' || value.front() == '-' || value.front() == ' ') {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

} // namespace octane::validation
